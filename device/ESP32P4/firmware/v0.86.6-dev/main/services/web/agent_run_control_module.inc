// Cooperative pause, resume, and cancellation checkpoints for a background Run.
typedef enum {
    AGENT_RUN_CONTROL_PAUSE = 0,
    AGENT_RUN_CONTROL_RESUME,
    AGENT_RUN_CONTROL_ABORT,
} agent_run_control_action_t;

static bool agent_run_control_state_active(agent_run_state_t state)
{
    return state == AGENT_RUN_STATE_RUNNING ||
           state == AGENT_RUN_STATE_WAITING_REQUEST ||
           state == AGENT_RUN_STATE_PAUSED;
}

static si_capability_t agent_tool_required_runtime_capability(
    const char *name)
{
    if (!name) return SI_CAPABILITY_AGENT_RUN;
    if (strcmp(name, "uart_status") == 0 ||
        strcmp(name, "uart_read") == 0 ||
        strcmp(name, "uart_write") == 0 ||
        strcmp(name, "uart_auth") == 0 ||
        strcmp(name, "uart_baud") == 0) return SI_CAPABILITY_UART;
    if (strcmp(name, "ssh_exec") == 0 ||
        strcmp(name, "console_login") == 0 ||
        strcmp(name, "check_service") == 0 ||
        strcmp(name, "verify_port") == 0 ||
        strcmp(name, "ensure_package") == 0) return SI_CAPABILITY_SSH;
    if (strcmp(name, "memory_search") == 0 ||
        strcmp(name, "history_search") == 0 ||
        strcmp(name, "memory_write") == 0) return SI_CAPABILITY_STORAGE;
    if (strcmp(name, "power_action") == 0) return SI_CAPABILITY_POWER;
    if (strcmp(name, "video_control") == 0)
        return SI_CAPABILITY_VIDEO_CONTROL;
    if (strcmp(name, "web_search") == 0 ||
        strcmp(name, "observe_status") == 0 ||
        strcmp(name, "observe_video_status") == 0 ||
        strcmp(name, "observe_hid_status") == 0 ||
        strcmp(name, "host_display") == 0 ||
        strcmp(name, "boot_key_sequence") == 0)
        return SI_CAPABILITY_OBSERVE;
    return SI_CAPABILITY_AGENT_RUN;
}

static bool agent_run_captured_capability(
    const char *job_id, si_capability_t required)
{
    /* Refresh generation/current capabilities immediately before the effect,
     * then enforce the immutable source ceiling for this exact tool. */
    if (!agent_run_ensure_live_source(job_id) || !s_agent_run_lock ||
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    bool allowed = strcmp(s_agent_run_job.job_id, job_id) == 0 &&
        agent_run_control_state_active(s_agent_run_job.state) &&
        si_capability_set_has(s_agent_run_job.source_authority_ceiling,
                              required);
    xSemaphoreGive(s_agent_run_lock);
    return allowed;
}

static bool agent_run_source_binding_live(
    si_agent_task_origin_t origin, si_agent_task_auth_kind_t auth_kind,
    si_principal_kind_t principal, const char *auth_id,
    uint32_t auth_generation, si_capability_set_t authority_ceiling)
{
    if (!si_capability_set_has(authority_ceiling,
                               SI_CAPABILITY_AGENT_RUN)) {
        return false;
    }
    if (origin == SI_AGENT_TASK_ORIGIN_SYSTEM &&
        auth_kind == SI_AGENT_TASK_AUTH_SYSTEM &&
        principal == SI_PRINCIPAL_SYSTEM) {
        return true;
    }
    /* QQ/delegation sources have no generation-aware live resolver in this
     * candidate and therefore fail closed instead of borrowing Web state. */
    if (auth_kind != SI_AGENT_TASK_AUTH_DEVICE_SESSION ||
        (origin != SI_AGENT_TASK_ORIGIN_WEB &&
         origin != SI_AGENT_TASK_ORIGIN_MCP)) {
        return false;
    }
    si_auth_session_context_t live = {0};
    bool valid = si_auth_session_get_live_context(
                     auth_id, auth_generation, &live) &&
                 live.authenticated && live.principal == principal &&
                 live.generation == auth_generation &&
                 strcmp(live.session_id, auth_id) == 0 &&
                 si_capability_set_has(live.capabilities,
                                       SI_CAPABILITY_AGENT_RUN) &&
                 (live.capabilities & authority_ceiling) ==
                     authority_ceiling;
    memset(&live, 0, sizeof(live));
    return valid;
}

static bool agent_run_ensure_live_source(const char *job_id)
{
    if (!job_id || !job_id[0] || !s_agent_run_lock ||
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    bool same_active = strcmp(s_agent_run_job.job_id, job_id) == 0 &&
        agent_run_control_state_active(s_agent_run_job.state) &&
        !s_agent_run_job.cancel_requested;
    si_agent_task_origin_t origin = s_agent_run_job.source_origin;
    si_agent_task_auth_kind_t auth_kind =
        s_agent_run_job.source_auth_kind;
    si_principal_kind_t principal = s_agent_run_job.source_principal;
    uint32_t auth_generation = s_agent_run_job.source_auth_generation;
    si_capability_set_t authority_ceiling =
        s_agent_run_job.source_authority_ceiling;
    char auth_id[SI_AGENT_TASK_SOURCE_ID_MAX_LEN + 1U] = {0};
    strlcpy(auth_id, s_agent_run_job.source_auth_id, sizeof(auth_id));
    xSemaphoreGive(s_agent_run_lock);
    if (!same_active) {
        return false;
    }
    bool live = agent_run_source_binding_live(
        origin, auth_kind, principal, auth_id, auth_generation,
        authority_ceiling);
    memset(auth_id, 0, sizeof(auth_id));
    if (live) {
        return true;
    }

    bool shadow_bound = false;
    uint32_t revoke_epoch = 0U;
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(s_agent_run_job.job_id, job_id) == 0 &&
            agent_run_control_state_active(s_agent_run_job.state)) {
            s_agent_run_job.pause_requested = false;
            s_agent_run_job.cancel_requested = true;
            if (++s_agent_run_job.control_epoch == 0U) {
                s_agent_run_job.control_epoch++;
            }
            revoke_epoch = s_agent_run_job.control_epoch;
            shadow_bound = s_agent_run_job.task_service_bound;
            s_agent_run_job.task_service_control_pending = shadow_bound;
            strlcpy(s_agent_run_job.detail,
                    "source session revoked; local fail-stop applied",
                    sizeof(s_agent_run_job.detail));
            strlcpy(s_agent_run_job.error,
                    "source authorization is no longer live",
                    sizeof(s_agent_run_job.error));
            s_agent_run_job.updated_ms = si_monotonic_ms();
        }
        xSemaphoreGive(s_agent_run_lock);
    }
    /* Fail-stop and exact owner cleanup precede the best-effort journal. */
    si_agent_request_broker_cancel_run(job_id);
    agent_run_release_hid_control(job_id);
#if !AGENT_RUNTIME_RECOVERY_RAM_ONLY
    si_video_control_release_agent();
    (void)si_video_control_apply();
#endif
    if (shadow_bound) {
        esp_err_t journal_ret = si_agent_task_service_cancel(job_id);
        if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (strcmp(s_agent_run_job.job_id, job_id) == 0) {
                if (s_agent_run_job.control_epoch == revoke_epoch) {
                    s_agent_run_job.task_service_control_pending = false;
                }
                if (journal_ret != ESP_OK) {
                    s_agent_run_job.task_service_bound = false;
                    snprintf(s_agent_run_job.task_service_error,
                             sizeof(s_agent_run_job.task_service_error),
                             "auth revoke storage_failure: %s",
                             esp_err_to_name(journal_ret));
                }
            }
            xSemaphoreGive(s_agent_run_lock);
        }
    }
    return false;
}

static esp_err_t agent_run_control_exact(
    agent_run_control_action_t action, const char *expected_job,
    char *controlled_job, size_t controlled_job_size,
    bool *shadow_degraded_out)
{
    if (controlled_job && controlled_job_size > 0) {
        controlled_job[0] = '\0';
    }
    if (shadow_degraded_out) {
        *shadow_degraded_out = false;
    }
    esp_err_t start_ret = agent_run_start();
    if (start_ret != ESP_OK || !s_agent_run_lock) {
        return start_ret == ESP_OK ? ESP_ERR_INVALID_STATE : start_ret;
    }
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(300)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!agent_run_control_state_active(s_agent_run_job.state) ||
        !s_agent_run_job.job_id[0]) {
        xSemaphoreGive(s_agent_run_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (expected_job && expected_job[0] &&
        strcmp(expected_job, s_agent_run_job.job_id) != 0) {
        xSemaphoreGive(s_agent_run_lock);
        return ESP_ERR_NOT_FOUND;
    }

    char exact_job[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    strlcpy(exact_job, s_agent_run_job.job_id, sizeof(exact_job));
    bool shadow_bound = s_agent_run_job.task_service_bound;
    if (action == AGENT_RUN_CONTROL_RESUME &&
        s_agent_run_job.task_service_control_pending) {
        xSemaphoreGive(s_agent_run_lock);
        return ESP_ERR_TIMEOUT;
    }
    uint32_t exact_epoch = ++s_agent_run_job.control_epoch;
    if (exact_epoch == 0U) {
        exact_epoch = ++s_agent_run_job.control_epoch;
    }
    if (shadow_bound) {
        s_agent_run_job.task_service_control_pending = true;
    }
    /* Pause and abort are local fail-stop operations. Publish their flags
     * before any journal I/O so a slow/recovering TF card cannot let the
     * provider, ask_user wait, or worker continue. A bound resume stays paused
     * until its journal transition succeeds or that shadow is demoted. */
    if (action == AGENT_RUN_CONTROL_PAUSE) {
        s_agent_run_job.pause_requested = true;
        strlcpy(
            s_agent_run_job.detail,
            "pause requested; waiting for the next safe checkpoint",
            sizeof(s_agent_run_job.detail));
    } else if (action == AGENT_RUN_CONTROL_ABORT) {
        s_agent_run_job.pause_requested = false;
        s_agent_run_job.cancel_requested = true;
        strlcpy(
            s_agent_run_job.detail,
            "abort requested; waiting for the next safe I/O checkpoint",
            sizeof(s_agent_run_job.detail));
    } else if (!shadow_bound) {
        s_agent_run_job.pause_requested = false;
        strlcpy(
            s_agent_run_job.detail,
            "resume requested locally in Runtime V2 degraded mode",
            sizeof(s_agent_run_job.detail));
    }
    s_agent_run_job.updated_ms = si_monotonic_ms();
    if (controlled_job && controlled_job_size > 0) {
        strlcpy(controlled_job, exact_job, controlled_job_size);
    }
    xSemaphoreGive(s_agent_run_lock);

    if (action == AGENT_RUN_CONTROL_ABORT) {
        /* Cleanup is deliberately ahead of journal I/O too. Exact owner-token
         * release cannot revoke a later boot-key/KVM owner. */
        si_agent_request_broker_cancel_run(exact_job);
        agent_run_release_hid_control(exact_job);
#if !AGENT_RUNTIME_RECOVERY_RAM_ONLY
        si_video_control_release_agent();
        (void)si_video_control_apply();
#endif
    }
    if (!shadow_bound) {
        return ESP_OK;
    }

    /* Never hold the Web Run lock across Task Service/Event Store I/O. */
    esp_err_t journal_ret =
        action == AGENT_RUN_CONTROL_PAUSE ?
            si_agent_task_service_pause(exact_job) :
        action == AGENT_RUN_CONTROL_RESUME ?
            si_agent_task_service_resume(exact_job) :
            si_agent_task_service_cancel(exact_job);
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(300)) != pdTRUE) {
        /* Pause/abort are already fail-stopped. A resume remains paused and is
         * safely retryable if this exact post-journal update cannot publish. */
        return action == AGENT_RUN_CONTROL_RESUME ?
            ESP_ERR_TIMEOUT : ESP_OK;
    }
    bool same_job = strcmp(s_agent_run_job.job_id, exact_job) == 0;
    bool still_active = agent_run_control_state_active(s_agent_run_job.state);
    bool same_transition =
        same_job && s_agent_run_job.control_epoch == exact_epoch;
    bool shadow_degraded = false;
    if (same_transition) {
        s_agent_run_job.task_service_control_pending = false;
    }
    bool journal_failure_degrades =
        journal_ret != ESP_OK &&
        !(action == AGENT_RUN_CONTROL_RESUME &&
          journal_ret == ESP_ERR_INVALID_STATE);
    if (journal_failure_degrades && same_job &&
        s_agent_run_job.task_service_bound) {
        const char *service_error = si_agent_task_service_last_error();
        s_agent_run_job.task_service_bound = false;
        snprintf(
            s_agent_run_job.task_service_error,
            sizeof(s_agent_run_job.task_service_error),
            "control storage_failure; shadow demoted: %s%s%s",
            esp_err_to_name(journal_ret),
            service_error && service_error[0] ? ": " : "",
            service_error && service_error[0] ? service_error : "");
        shadow_degraded = true;
    }
    esp_err_t result = ESP_OK;
    if (action == AGENT_RUN_CONTROL_RESUME) {
        if (!same_transition || !still_active ||
            s_agent_run_job.cancel_requested) {
            result = ESP_ERR_INVALID_STATE;
        } else if (journal_ret == ESP_OK || shadow_degraded) {
            /* Journal success preserves the binding. Any journal failure has
             * first demoted this exact job, so local-only resume is explicit. */
            s_agent_run_job.pause_requested = false;
            strlcpy(
                s_agent_run_job.detail,
                shadow_degraded ?
                    "resume requested locally; Runtime V2 shadow degraded" :
                    "resume requested; leaving cooperative checkpoint",
                sizeof(s_agent_run_job.detail));
            s_agent_run_job.updated_ms = si_monotonic_ms();
        } else {
            /* A duplicate/invalid durable resume is rejected without
             * demoting a healthy shadow or releasing the local pause. */
            result = journal_ret;
        }
    }
    if (shadow_degraded_out) {
        *shadow_degraded_out = shadow_degraded;
    }
    xSemaphoreGive(s_agent_run_lock);
    if (shadow_degraded) {
        ESP_LOGE(TAG,
                 "Agent %s shadow transition failed for %s; exact job "
                 "demoted while local control remains safe: %s",
                 action == AGENT_RUN_CONTROL_PAUSE ? "pause" :
                 action == AGENT_RUN_CONTROL_RESUME ? "resume" : "abort",
                 exact_job, esp_err_to_name(journal_ret));
    }
    return result;
}

static bool agent_run_control_point(const char *job_id)
{
    bool announced_pause = false;
    uint32_t lock_waits = 0;
    for (;;) {
        if (!agent_run_ensure_live_source(job_id)) {
            return false;
        }
        if (!s_agent_run_lock) {
            return false;
        }
        if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (++lock_waits % 50U == 0U) {
                ESP_LOGW(TAG, "Agent control point is waiting for the Run lock");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        lock_waits = 0;
        if (!job_id || strcmp(s_agent_run_job.job_id, job_id) != 0 ||
            s_agent_run_job.cancel_requested) {
            xSemaphoreGive(s_agent_run_lock);
            return false;
        }
        if (!s_agent_run_job.pause_requested) {
            bool was_paused =
                s_agent_run_job.state == AGENT_RUN_STATE_PAUSED;
            if (was_paused) {
                s_agent_run_job.state = AGENT_RUN_STATE_RUNNING;
                strlcpy(s_agent_run_job.stage, "继续运行",
                        sizeof(s_agent_run_job.stage));
                strlcpy(s_agent_run_job.detail,
                        "已从安全检查点恢复后台执行",
                        sizeof(s_agent_run_job.detail));
                s_agent_run_job.updated_ms = si_monotonic_ms();
            }
            xSemaphoreGive(s_agent_run_lock);
            if (was_paused) {
                agent_run_add_action_eventf(
                    job_id, NULL, SI_ACTION_CHANNEL_AGENT,
                    SI_ACTION_PHASE_STARTED, false, true,
                    "run resumed at cooperative checkpoint");
            }
            return true;
        }
        if (s_agent_run_job.state != AGENT_RUN_STATE_PAUSED) {
            s_agent_run_job.state = AGENT_RUN_STATE_PAUSED;
            strlcpy(s_agent_run_job.stage, "已暂停",
                    sizeof(s_agent_run_job.stage));
            strlcpy(s_agent_run_job.detail,
                    "后台任务停在安全检查点，等待 resume 或 abort",
                    sizeof(s_agent_run_job.detail));
            s_agent_run_job.updated_ms = si_monotonic_ms();
        }
        xSemaphoreGive(s_agent_run_lock);
        if (!announced_pause) {
            announced_pause = true;
            agent_run_add_action_eventf(
                job_id, NULL, SI_ACTION_CHANNEL_AGENT,
                SI_ACTION_PHASE_WAITING_APPROVAL, false, true,
                "run paused at cooperative checkpoint");
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* Acquire the Run lock for a context-bearing provider request. On success the
 * lock deliberately remains held until agent_cloud_post_json() performs its
 * first HTTP step; Settings revocation therefore cannot return before the
 * request is either stripped of context or actually in flight. */
static bool agent_run_page_context_request_lock(const char *job_id)
{
    /* This check is immediately before the first provider slice. The cloud
     * transport keeps the short Run-lock context fence only for that first
     * slice, then refreshes the source on every subsequent slice. */
    if (!agent_run_ensure_live_source(job_id) || !s_agent_run_lock ||
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }
    if (strcmp(s_agent_run_job.job_id, job_id) == 0 &&
        !s_agent_run_job.cancel_requested &&
        agent_run_page_context_allowed()) {
        return true;
    }
    xSemaphoreGive(s_agent_run_lock);
    return false;
}
