// Transitional Agent module: background run task and submit lifecycle. Shares the run context until its service API is finalized.
static void __attribute__((unused)) agent_run_start_next_queued(void);
static const char *const AGENT_DEFAULT_COMPLETION_CRITERIA =
    "Return a bounded final response and independently verify every device action.";
#define AGENT_RUN_BROKER_RETIRE_MAX_ATTEMPTS 4U
#define AGENT_TASK_SERVICE_RECOVERY_STACK 8192U
/* Recovery/HIL candidate: Runtime V2 may recover in its low-priority task for
 * diagnostics, but the main HTTP/worker paths never synchronously append or
 * fsync TASKS.LOG. Legacy remains the only executor and is bounded read-only. */
static bool s_agent_task_service_recovery_scheduled;

static void agent_task_service_recovery_task(void *arg)
{
    (void)arg;
    /* Runtime V2 remains a fail-closed shadow. Recovery/storage faults must
     * not take down the legacy Agent manager, KVM, or Web server. */
    esp_err_t ret = si_agent_task_service_recover_reserved();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Agent Task Service degraded; legacy Agent remains available "
                 "with bounded read-only compatibility; writes and leased "
                 "tools fail closed: %s (%s)", esp_err_to_name(ret),
                 si_agent_task_service_last_error());
    } else {
        esp_err_t checkpoint_ret = agent_run_checkpoint_restore();
        if (checkpoint_ret != ESP_OK) {
            ESP_LOGE(TAG,
                     "Legacy checkpoint remains quarantined after shadow "
                     "recovery: %s", esp_err_to_name(checkpoint_ret));
        } else if (s_agent_run_lock &&
                   xSemaphoreTake(s_agent_run_lock,
                                  pdMS_TO_TICKS(300)) == pdTRUE) {
            if (s_agent_run_job.state == AGENT_RUN_STATE_IDLE &&
                !s_agent_run_job.task && !s_agent_run_job.job_id[0]) {
                s_agent_run_job.task_service_error[0] = '\0';
            }
            xSemaphoreGive(s_agent_run_lock);
        }
    }
    vTaskDeleteWithCaps(NULL);
}

static void agent_task_service_prepare_shadow(void)
{
    esp_err_t ret = si_agent_task_service_reserve_recovery();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Agent Task Service mutex unavailable; Web/KVM and legacy "
                 "Agent remain available in degraded mode: %s",
                 esp_err_to_name(ret));
    }
}

static void agent_task_service_start_shadow_async(void)
{
    bool expected = false;
    if (!__atomic_compare_exchange_n(
            &s_agent_task_service_recovery_scheduled, &expected, true,
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (si_agent_task_service_reserve_recovery() != ESP_OK) {
        ESP_LOGE(TAG,
                 "Agent Task Service recovery not scheduled; prepare failed");
        return;
    }
    BaseType_t created = xTaskCreateWithCaps(
        agent_task_service_recovery_task, "si_agent_recover",
        AGENT_TASK_SERVICE_RECOVERY_STACK, NULL, tskIDLE_PRIORITY + 1,
        NULL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGE(TAG,
                 "Agent Task Service recovery task unavailable; Web/KVM "
                 "continues with degraded legacy Agent");
    }
}

static void agent_run_task_arg_free(agent_run_task_arg_t *run)
{
    if (!run) {
        return;
    }
    if (run->message) {
        memset(run->message, 0, strlen(run->message));
        free(run->message);
    }
#if SI_CFG_VIDEO_H264_ENABLED
    if (run->h264_resource_token != 0U) {
        const si_h264_resource_token_t resource_token =
            run->h264_resource_token;
        run->h264_resource_token = 0U;
        if (!si_h264_stream_release_agent_resources(resource_token)) {
            ESP_LOGW(TAG,
                     "ignored stale Agent/H.264 resource token during cleanup");
        }
    }
#endif
    memset(run, 0, sizeof(*run));
    free(run);
}

static bool agent_result_requires_independent_verification(
    const cJSON *result)
{
    if (!result || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                       result, "dry_run"))) {
        return false;
    }
    const cJSON *actions =
        cJSON_GetObjectItemCaseSensitive(result, "actions");
    if (cJSON_IsArray(actions) && cJSON_GetArraySize(actions) > 0) {
        return true;
    }
    const cJSON *calls =
        cJSON_GetObjectItemCaseSensitive(result, "tool_calls");
    const cJSON *call = NULL;
    cJSON_ArrayForEach(call, calls) {
        const char *name = agent_tool_name(call);
        if (!name) {
            return true;
        }
        if (agent_tool_is_ask_user(call)) {
            continue;
        }
        bool lease_required = false;
        char resource[SI_AGENT_REQUEST_RESOURCE_MAX_LEN + 1] = {0};
        char effect[SI_AGENT_REQUEST_EFFECT_MAX_LEN + 1] = {0};
        char verification[SI_AGENT_REQUEST_VERIFY_MAX_LEN + 1] = {0};
        si_agent_request_risk_t risk = agent_tool_request_risk(
            name, call, &lease_required, resource, sizeof(resource),
            effect, sizeof(effect), verification, sizeof(verification));
        if (risk != SI_AGENT_REQUEST_RISK_LOW || lease_required) {
            return true;
        }
    }
    return false;
}

static bool agent_result_execution_ok(esp_err_t execute_ret,
                                      const cJSON *result)
{
    return execute_ret == ESP_OK && result &&
           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "ok")) &&
           !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
               result, "had_tool_failure"));
}

static bool agent_text_sha256(const char *text, char out[65])
{
    if (!text || !text[0] || !out) {
        return false;
    }
    unsigned char digest[32] = {0};
    int hash_result = mbedtls_sha256(
        (const unsigned char *)text, strlen(text), digest, 0);
    if (hash_result != 0) {
        return false;
    }
    for (size_t index = 0; index < sizeof(digest); ++index) {
        snprintf(out + index * 2U, 3U, "%02x", digest[index]);
    }
    memset(digest, 0, sizeof(digest));
    return true;
}

static bool agent_run_finalize_task_service(const char *run_id,
                                            bool task_service_bound,
                                            esp_err_t execute_ret,
                                            bool aborted,
                                            cJSON *result,
                                            char *error)
{
    const bool legacy_ok = agent_result_execution_ok(execute_ret, result);
    agent_json_set_string(result, "runtime_shadow",
                          task_service_bound ? "active" : "degraded");
    agent_json_set_bool(result, "runtime_shadow_verified", false);
    cJSON_AddBoolToObject(result, "runtime_shadow_affects_legacy_outcome",
                          false);
    if (aborted) {
        if (task_service_bound) {
            (void)si_agent_task_service_cancel(run_id);
        }
        return false;
    }
    if (!legacy_ok) {
        if (task_service_bound) {
            esp_err_t terminal_result =
                si_agent_task_service_set_step_status(
                    run_id, AGENT_RUNTIME_V2_STEP_ID, SI_AGENT_STEP_FAILED);
            if (terminal_result == ESP_OK) {
                (void)si_agent_task_service_fail(
                    run_id, error && error[0] ? error : "executor_failed");
            }
        }
        agent_json_set_string(result, "runtime_shadow",
                              task_service_bound ? "failed" : "degraded");
        return false;
    }

    cJSON *verification =
        cJSON_AddObjectToObject(result, "completion_verification");
    if (!task_service_bound) {
        if (verification) {
            cJSON_AddStringToObject(verification, "status", "unverified");
            cJSON_AddStringToObject(
                verification, "reason",
                "Task journal is degraded; legacy read-only result is not Runtime V2 evidence");
            cJSON_AddBoolToObject(verification, "shadow_only", true);
        }
        return true;
    }

    if (agent_result_requires_independent_verification(result)) {
        /* This is a defensive fallback. The candidate dispatcher rejects all
         * mutation/HID execution before effect. If a future compatibility path
         * reaches here, Runtime V2 records unknown and cannot self-verify it. */
        esp_err_t unknown_result =
            si_agent_task_service_record_step_verification(
                run_id, AGENT_RUNTIME_V2_STEP_ID,
                SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN, NULL);
        agent_json_set_string(result, "runtime_shadow",
                              unknown_result == ESP_OK ?
                                  "outcome_unknown" : "storage_failure");
        if (verification) {
            cJSON_AddStringToObject(verification, "status",
                                    unknown_result == ESP_OK ?
                                        "outcome_unknown" : "storage_failure");
            cJSON_AddStringToObject(
                verification, "reason",
                unknown_result == ESP_OK ?
                    "mutation has no typed manager readback" :
                    "unknown action outcome was not durably recorded");
            cJSON_AddBoolToObject(verification, "blind_retry_forbidden",
                                  true);
            cJSON_AddBoolToObject(verification, "shadow_only", true);
        }
        return true;
    }

    const cJSON *message = cJSON_GetObjectItemCaseSensitive(result, "message");
    const char *final_response =
        cJSON_IsString(message) && message->valuestring ?
            message->valuestring : NULL;
    size_t final_response_bytes = final_response ? strlen(final_response) : 0U;
    char artifact_hash[SI_AGENT_VERIFY_HASH_MAX_LEN + 1U] = {0};
    si_agent_completion_artifact_t artifact = {0};
    si_agent_verification_result_t verdict = {
        .status = SI_AGENT_VERIFY_INVALID,
        .accepted_index = SIZE_MAX,
    };
    esp_err_t artifact_result =
        final_response_bytes > 0U &&
        final_response_bytes <= SI_AGENT_TASK_COMPLETION_ARTIFACT_MAX_BYTES &&
        agent_text_sha256(final_response, artifact_hash) ?
        si_agent_task_service_record_readonly_completion_artifact(
            run_id, AGENT_RUNTIME_V2_STEP_ID, artifact_hash,
            (uint32_t)final_response_bytes, &artifact) :
        ESP_ERR_INVALID_RESPONSE;
    esp_err_t completion_result = artifact_result;
    if (artifact_result == ESP_OK) {
        completion_result = si_agent_task_service_complete_readonly(
            run_id, AGENT_RUNTIME_V2_STEP_ID, &artifact, &verdict);
    }
    if (verification) {
        cJSON_AddStringToObject(verification, "status",
                                completion_result == ESP_OK ?
                                    "passed" :
                                artifact_result == ESP_OK ?
                                    "response_delivered" : "unverified");
        cJSON_AddStringToObject(
            verification, "reason",
            completion_result == ESP_OK ? verdict.reason :
            artifact_result == ESP_OK ?
                "durable final response recorded; observation criteria still requires independent evidence" :
                "durable final-response artifact unavailable");
        cJSON_AddBoolToObject(verification, "shadow_only", true);
        if (artifact.artifact_id[0]) {
            cJSON_AddStringToObject(verification, "evidence_id",
                                    artifact.artifact_id);
            cJSON_AddStringToObject(verification, "artifact_hash",
                                    artifact.artifact_hash);
            cJSON_AddStringToObject(verification, "evidence_kind",
                                    "artifact");
        }
    }
    if (completion_result == ESP_OK) {
        agent_json_set_string(result, "runtime_shadow", "verified");
        agent_json_set_bool(result, "runtime_shadow_verified", true);
    } else {
        /* No device effect occurred, so a missing shadow artifact must not
         * downgrade the compatibility result. Close the shadow turn without
         * pretending that completion was verified. */
        (void)si_agent_task_service_interrupt(
            run_id, artifact_result == ESP_OK ?
                "observation_evidence_unavailable" :
                "shadow_completion_artifact_unavailable");
        agent_json_set_string(result, "runtime_shadow", "unverified");
    }
    return true;
}

static void agent_run_task(void *arg)
{
    agent_run_task_arg_t *run = (agent_run_task_arg_t *)arg;
    cJSON *result = NULL;
    char error[AGENT_RUN_ERROR_MAX_LEN + 1] = {0};
    agent_run_set_stage(run->job_id, "后台任务启动", "设备端 Agent run 已脱离页面运行");
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (agent_run_control_point(run->job_id)) {
        ret = agent_run_execute_to_json(
            run->job_id, run->session_id, run->message,
            run->page_context_json, run->profile, run->model,
            run->dry_run, run->include_screenshot,
            run->allow_web_search, &result, error, sizeof(error));
    } else {
        strlcpy(error, "run aborted", sizeof(error));
    }
    char *result_json = NULL;
    if (!result) {
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "session_id", run->session_id);
        cJSON_AddStringToObject(result, "profile", run->profile);
        cJSON_AddStringToObject(result, "model", run->model);
        cJSON_AddBoolToObject(result, "dry_run", run->dry_run);
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(
            result, "error",
            error[0] ? error :
            (ret != ESP_OK ? esp_err_to_name(ret) :
                             "agent execution failed"));
        cJSON_AddStringToObject(result, "message", "");
        cJSON_AddItemToObject(result, "trace", cJSON_CreateArray());
        cJSON_AddItemToObject(result, "tool_calls", cJSON_CreateArray());
        cJSON_AddItemToObject(result, "actions", cJSON_CreateArray());
        cJSON_AddItemToObject(result, "tool_results", cJSON_CreateArray());
        cJSON_AddItemToObject(result, "results", cJSON_CreateArray());
    }
    if (!agent_result_execution_ok(ret, result)) {
        if (!error[0] || strcmp(error, "ESP_OK") == 0) {
            si_agent_result_failure_error(
                ret, result, error, sizeof(error));
        }
        const char *result_error =
            si_agent_result_nonempty_string(result, "error");
        if (!result_error || strcmp(result_error, "ESP_OK") == 0) {
            agent_json_set_string(result, "error",
                                  error[0] ? error :
                                  "agent execution failed");
        }
    }
    cJSON_AddStringToObject(result, "job_id", run->job_id);
    bool aborted = false;
    bool live_task_service_bound = false;
    uint32_t binding_waits = 0U;
    while (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        if (++binding_waits % 25U == 0U) {
            ESP_LOGW(TAG,
                     "Agent finalizer is waiting for the live shadow binding");
        }
    }
    bool same_job = strcmp(s_agent_run_job.job_id, run->job_id) == 0;
    aborted = !same_job || s_agent_run_job.cancel_requested;
    /* Control may demote a bound shadow after a durable append failure. The
     * worker's launch argument is historical; finalization must use this
     * exact job's live binding and never impersonate the failed shadow. */
    live_task_service_bound =
        same_job && s_agent_run_job.task_service_bound &&
        !s_agent_run_job.task_service_control_pending;
    xSemaphoreGive(s_agent_run_lock);
    bool legacy_completed = agent_run_finalize_task_service(
        run->job_id, live_task_service_bound, ret, aborted, result, error);
    esp_err_t history_ret =
        agent_history_store_run_turn(run, result, aborted);
    if (history_ret != ESP_OK && history_ret != ESP_ERR_NOT_SUPPORTED &&
        history_ret != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "Agent turn history unavailable for %s: %s",
                 run->job_id, esp_err_to_name(history_ret));
    }
    si_agent_task_service_snapshot_t *task_snapshot =
        live_task_service_bound ? agent_alloc(sizeof(*task_snapshot)) : NULL;
    bool task_snapshot_ok = task_snapshot &&
        si_agent_task_service_snapshot(task_snapshot) == ESP_OK;
    bool same_active_task =
        task_snapshot_ok &&
        task_snapshot->runtime.has_active_turn &&
        strcmp(task_snapshot->runtime.active_turn.run_attempt.run_attempt_id,
               run->job_id) == 0;
    bool durable_task_terminal =
        same_active_task && task_snapshot->storage_healthy &&
        si_agent_turn_status_is_terminal(
            task_snapshot->runtime.active_turn.status);
    free(task_snapshot);
    uint32_t finalize_waits = 0;
    while (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        if (++finalize_waits % 25U == 0U) {
            ESP_LOGW(TAG, "Agent run completion is waiting for the Run lock");
        }
    }
    /* page_context is transient model input. Status already exposes the
     * independently gated job field, so never persist a duplicate in result. */
    cJSON_DeleteItemFromObjectCaseSensitive(result, "page_context");
    result_json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (strcmp(s_agent_run_job.job_id, run->job_id) == 0) {
        aborted = aborted || s_agent_run_job.cancel_requested;
        free(s_agent_run_job.result_json);
        s_agent_run_job.result_json = result_json;
        result_json = NULL;
        s_agent_run_job.state =
            aborted ? AGENT_RUN_STATE_ABORTED :
            (legacy_completed ? AGENT_RUN_STATE_DONE :
                                AGENT_RUN_STATE_FAILED);
        s_agent_run_job.finished_ms = si_monotonic_ms();
        s_agent_run_job.updated_ms = s_agent_run_job.finished_ms;
        /* Keep the worker handle published until Broker, HID, lease, video,
         * and shadow retirement are all complete. agent_run_submit() treats
         * any non-NULL handle as busy, so a new run cannot be started and
         * then accidentally released by this old worker's cleanup. */
        strlcpy(s_agent_run_job.stage,
                aborted ? "已中止" :
                (legacy_completed ? "完成" : "失败"),
                sizeof(s_agent_run_job.stage));
        strlcpy(s_agent_run_job.detail, "", sizeof(s_agent_run_job.detail));
        strlcpy(s_agent_run_job.error,
                aborted ? "run aborted" :
                (legacy_completed ? "" :
                 (error[0] ? error :
                  (ret != ESP_OK ? esp_err_to_name(ret) :
                                   "agent execution failed"))),
                sizeof(s_agent_run_job.error));
    }
    xSemaphoreGive(s_agent_run_lock);
    free(result_json);
    if (durable_task_terminal) {
        esp_err_t retire_ret = ESP_FAIL;
        for (uint32_t retire_attempt = 1U;
             retire_attempt <= AGENT_RUN_BROKER_RETIRE_MAX_ATTEMPTS;
             ++retire_attempt) {
            retire_ret =
                si_agent_request_broker_terminalize_and_retire_run(
                    run->job_id);
            if (retire_ret == ESP_OK) {
                break;
            }
            if (retire_attempt == 1U) {
                ESP_LOGW(TAG,
                         "Agent request terminalize/retire waiting for %s: %s",
                         run->job_id, esp_err_to_name(retire_ret));
            }
            if (retire_attempt < AGENT_RUN_BROKER_RETIRE_MAX_ATTEMPTS) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        if (retire_ret != ESP_OK) {
            /* Stale Broker records remain fail-closed and are diagnosable;
             * never keep the internal-stack worker alive forever on a cleanup lock. */
            ESP_LOGE(TAG,
                     "Agent request terminalize/retire gave up for %s after "
                     "%u attempts: %s",
                     run->job_id,
                     (unsigned)AGENT_RUN_BROKER_RETIRE_MAX_ATTEMPTS,
                     esp_err_to_name(retire_ret));
        }
    }
    agent_run_release_hid_control(run->job_id);
#if !AGENT_RUNTIME_RECOVERY_RAM_ONLY
    si_video_control_release_agent();
    (void)si_video_control_apply();
#endif
#if AGENT_RUNTIME_V2_SHADOW_BINDING_ENABLED
    agent_run_start_next_queued();
#endif
    uint32_t cleanup_waits = 0U;
    while (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        if (++cleanup_waits % 25U == 0U) {
            ESP_LOGW(TAG,
                     "Agent run cleanup is waiting to release the Run slot");
        }
    }
    if (strcmp(s_agent_run_job.job_id, run->job_id) == 0) {
        s_agent_run_job.task = NULL;
    }
    xSemaphoreGive(s_agent_run_lock);
    ESP_LOGI(TAG, "Agent run task internal stack high-water=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    agent_run_task_arg_free(run);
    vTaskDeleteWithCaps(NULL);
}

static bool agent_run_terminalize_launch_failure_locked(
    const char *job_id, const char *failure)
{
    if (!job_id || !job_id[0] ||
        strcmp(s_agent_run_job.job_id, job_id) != 0 ||
        s_agent_run_job.task != NULL ||
        s_agent_run_job.state != AGENT_RUN_STATE_RUNNING) {
        return false;
    }
    uint32_t now = si_monotonic_ms();
    s_agent_run_job.state = AGENT_RUN_STATE_FAILED;
    s_agent_run_job.task = NULL;
    s_agent_run_job.finished_ms = now;
    s_agent_run_job.updated_ms = now;
    strlcpy(s_agent_run_job.stage, "启动失败",
            sizeof(s_agent_run_job.stage));
    strlcpy(s_agent_run_job.detail, failure ? failure : "executor launch failed",
            sizeof(s_agent_run_job.detail));
    strlcpy(s_agent_run_job.error, failure ? failure : "executor launch failed",
            sizeof(s_agent_run_job.error));
    return true;
}

static bool agent_run_terminalize_launch_failure_exact(
    const char *job_id, const char *failure)
{
    if (!s_agent_run_lock || !job_id || !job_id[0]) {
        return false;
    }
    uint32_t waits = 0U;
    while (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        if (++waits % 25U == 0U) {
            ESP_LOGW(TAG,
                     "Agent launch failure is waiting to terminalize %s",
                     job_id);
        }
    }
    bool terminalized =
        agent_run_terminalize_launch_failure_locked(job_id, failure);
    xSemaphoreGive(s_agent_run_lock);
    return terminalized;
}

static bool agent_run_terminalize_prelaunch_abort_locked(
    const char *job_id)
{
    if (!job_id || strcmp(s_agent_run_job.job_id, job_id) != 0 ||
        s_agent_run_job.task != NULL ||
        s_agent_run_job.state != AGENT_RUN_STATE_RUNNING) {
        return false;
    }
    uint32_t now = si_monotonic_ms();
    s_agent_run_job.state = AGENT_RUN_STATE_ABORTED;
    s_agent_run_job.task = NULL;
    s_agent_run_job.task_service_control_pending = false;
    s_agent_run_job.finished_ms = now;
    s_agent_run_job.updated_ms = now;
    strlcpy(s_agent_run_job.stage, "已中止",
            sizeof(s_agent_run_job.stage));
    strlcpy(s_agent_run_job.detail,
            "abort accepted before executor launch",
            sizeof(s_agent_run_job.detail));
    strlcpy(s_agent_run_job.error, "run aborted",
            sizeof(s_agent_run_job.error));
    return true;
}

static esp_err_t agent_run_publish_shadow_binding_exact(
    agent_run_task_arg_t *task_arg, const char *shadow_error,
    char *err, size_t err_size)
{
    if (!task_arg || !task_arg->job_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        const char *failure = "executor binding publish lock timeout";
        if (err && err_size > 0) {
            strlcpy(err, failure, err_size);
        }
        (void)agent_run_terminalize_launch_failure_exact(
            task_arg->job_id, failure);
        return ESP_ERR_TIMEOUT;
    }
    bool exact = !s_agent_run_job.task &&
        strcmp(s_agent_run_job.job_id, task_arg->job_id) == 0 &&
        s_agent_run_job.state == AGENT_RUN_STATE_RUNNING;
    if (!exact) {
        xSemaphoreGive(s_agent_run_lock);
        if (err && err_size > 0) {
            strlcpy(err, "executor binding no longer owns Run", err_size);
        }
        return ESP_ERR_INVALID_STATE;
    }
    /* Publish the bound-capable BINDING state before launch. Controls that
     * arrive after this point journal against the exact shadow; controls that
     * arrived earlier are reconciled below before a worker can start. */
    s_agent_run_job.task_service_bound = task_arg->task_service_bound;
    s_agent_run_job.task_service_control_pending = false;
    strlcpy(s_agent_run_job.task_service_error,
            task_arg->task_service_bound ? "" :
            (shadow_error ? shadow_error : "Runtime V2 unavailable"),
            sizeof(s_agent_run_job.task_service_error));
    strlcpy(s_agent_run_job.detail,
            task_arg->task_service_bound ?
            "Legacy 主路径已绑定 Runtime V2 shadow，正在启动" :
            "Runtime V2 degraded；仅有界只读兼容可用，写入/租约工具已关闭",
            sizeof(s_agent_run_job.detail));
    xSemaphoreGive(s_agent_run_lock);
    return ESP_OK;
}

static esp_err_t agent_run_reconcile_prelaunch_control(
    agent_run_task_arg_t *task_arg, char *shadow_error,
    size_t shadow_error_size, uint32_t *control_epoch_out,
    bool *aborted_out)
{
    if (!task_arg || !control_epoch_out || !aborted_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *aborted_out = false;
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    bool exact = !s_agent_run_job.task &&
        strcmp(s_agent_run_job.job_id, task_arg->job_id) == 0 &&
        s_agent_run_job.state == AGENT_RUN_STATE_RUNNING;
    if (!exact) {
        xSemaphoreGive(s_agent_run_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t control_epoch = s_agent_run_job.control_epoch;
    bool cancel_requested = s_agent_run_job.cancel_requested;
    bool pause_requested = s_agent_run_job.pause_requested;
    *control_epoch_out = control_epoch;
    task_arg->task_service_bound = s_agent_run_job.task_service_bound;
    if (!cancel_requested && !pause_requested) {
        xSemaphoreGive(s_agent_run_lock);
        return ESP_OK;
    }
    if (!task_arg->task_service_bound) {
        if (cancel_requested) {
            *aborted_out = agent_run_terminalize_prelaunch_abort_locked(
                task_arg->job_id);
        }
        xSemaphoreGive(s_agent_run_lock);
        return *aborted_out ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    s_agent_run_job.task_service_control_pending = true;
    xSemaphoreGive(s_agent_run_lock);

    /* Reconcile controls accepted while Runtime V2 was still binding. No Run
     * lock is held across the Task/Event journal operation. */
    esp_err_t journal_ret = cancel_requested ?
        si_agent_task_service_cancel(task_arg->job_id) :
        si_agent_task_service_pause(task_arg->job_id);
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    exact = !s_agent_run_job.task &&
        strcmp(s_agent_run_job.job_id, task_arg->job_id) == 0 &&
        s_agent_run_job.state == AGENT_RUN_STATE_RUNNING;
    bool same_transition =
        exact && s_agent_run_job.control_epoch == control_epoch;
    if (same_transition) {
        s_agent_run_job.task_service_control_pending = false;
    }
    if (journal_ret != ESP_OK && exact) {
        task_arg->task_service_bound = false;
        s_agent_run_job.task_service_bound = false;
        snprintf(shadow_error, shadow_error_size,
                 "prelaunch control storage_failure: %s",
                 esp_err_to_name(journal_ret));
        strlcpy(s_agent_run_job.task_service_error, shadow_error,
                sizeof(s_agent_run_job.task_service_error));
    }
    if (same_transition && cancel_requested) {
        *aborted_out = agent_run_terminalize_prelaunch_abort_locked(
            task_arg->job_id);
    }
    xSemaphoreGive(s_agent_run_lock);
    return *aborted_out ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t agent_run_launch_execution(
    agent_run_task_arg_t *task_arg, const char *shadow_error,
    bool *aborted_out, char *err, size_t err_size)
{
    char launch_job_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    if (task_arg) {
        strlcpy(launch_job_id, task_arg->job_id, sizeof(launch_job_id));
    }
    if (aborted_out) {
        *aborted_out = false;
    }
    if (!task_arg || !task_arg->job_id[0] || !task_arg->session_id[0] ||
        !task_arg->message || !task_arg->message[0]) {
        const char *failure = "invalid agent executor launch arguments";
        if (err && err_size > 0) {
            strlcpy(err, failure, err_size);
        }
        (void)agent_run_terminalize_launch_failure_exact(
            launch_job_id, failure);
        agent_run_task_arg_free(task_arg);
        return ESP_ERR_INVALID_ARG;
    }

#if SI_CFG_VIDEO_H264_ENABLED
    si_h264_resource_token_t resource_token = 0U;
    esp_err_t resource_ret =
        si_h264_stream_claim_agent_resources(&resource_token);
    if (resource_ret != ESP_OK) {
        const char *failure =
            "H.264 codec is active or releasing resources; retry Agent run";
        if (err && err_size > 0) {
            strlcpy(err, failure, err_size);
        }
        (void)agent_run_terminalize_launch_failure_exact(
            launch_job_id, failure);
        agent_run_task_arg_free(task_arg);
        return ESP_ERR_INVALID_STATE;
    }
    task_arg->h264_resource_token = resource_token;
#endif

    char launch_shadow_error[AGENT_RUN_ERROR_MAX_LEN + 1] = {0};
    strlcpy(launch_shadow_error, shadow_error ? shadow_error : "",
            sizeof(launch_shadow_error));
    for (uint32_t reconcile_attempt = 0U; reconcile_attempt < 4U;
         ++reconcile_attempt) {
        uint32_t reconciled_epoch = 0U;
        bool prelaunch_aborted = false;
        esp_err_t reconcile_ret = agent_run_reconcile_prelaunch_control(
            task_arg, launch_shadow_error, sizeof(launch_shadow_error),
            &reconciled_epoch, &prelaunch_aborted);
        if (prelaunch_aborted) {
            if (aborted_out) {
                *aborted_out = true;
            }
            if (err && err_size > 0) {
                strlcpy(err, "run aborted before executor launch", err_size);
            }
            agent_run_task_arg_free(task_arg);
            return ESP_ERR_INVALID_STATE;
        }
        if (reconcile_ret != ESP_OK) {
            const char *failure = reconcile_ret == ESP_ERR_TIMEOUT ?
                "executor binding/launch lock timeout" :
                "executor launch no longer owns the published Run";
            if (err && err_size > 0) {
                strlcpy(err, failure, err_size);
            }
            (void)agent_run_terminalize_launch_failure_exact(
                launch_job_id, failure);
            agent_run_task_arg_free(task_arg);
            return reconcile_ret;
        }

        if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
            const char *failure = "executor binding/launch lock timeout";
            if (err && err_size > 0) {
                strlcpy(err, failure, err_size);
            }
            /* A published RUNNING slot must never be orphaned when the
             * bounded acquisition above times out. */
            (void)agent_run_terminalize_launch_failure_exact(
                launch_job_id, failure);
            agent_run_task_arg_free(task_arg);
            return ESP_ERR_TIMEOUT;
        }
        if (s_agent_run_job.control_epoch != reconciled_epoch) {
            xSemaphoreGive(s_agent_run_lock);
            continue;
        }
        if (s_agent_run_job.task ||
            strcmp(s_agent_run_job.job_id, launch_job_id) != 0 ||
            s_agent_run_job.state != AGENT_RUN_STATE_RUNNING ||
            s_agent_run_job.cancel_requested) {
            const char *failure =
                "executor launch no longer owns the published Run";
            (void)agent_run_terminalize_launch_failure_locked(
                launch_job_id, failure);
            xSemaphoreGive(s_agent_run_lock);
            if (err && err_size > 0) {
                strlcpy(err, failure, err_size);
            }
            agent_run_task_arg_free(task_arg);
            return ESP_ERR_INVALID_STATE;
        }

        /* A control storage failure may have demoted this exact job after the
         * caller published BINDING. Never overwrite that live demotion with a
         * stale task-argument snapshot. */
        task_arg->task_service_bound = s_agent_run_job.task_service_bound;
        s_agent_run_job.task_service_control_pending = false;

        size_t internal_free =
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t internal_largest = heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t psram_free =
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        BaseType_t ok = xTaskCreateWithCaps(
            agent_run_task, "si_agent_run", AGENT_RUN_TASK_STACK, task_arg,
            tskIDLE_PRIORITY + 3, &s_agent_run_job.task,
            AGENT_RUN_TASK_MEMORY_CAPS);
        if (ok != pdPASS) {
            const char *failure =
                "create agent run task failed (internal stack unavailable)";
            s_agent_run_job.task = NULL;
            (void)agent_run_terminalize_launch_failure_locked(
                launch_job_id, failure);
            xSemaphoreGive(s_agent_run_lock);
            if (err && err_size > 0) {
                strlcpy(err, failure, err_size);
            }
            agent_run_task_arg_free(task_arg);
            ESP_LOGE(TAG,
                     "Agent executor task allocation failed: stack=%u "
                     "internal_free=%u internal_largest=%u psram_free=%u",
                     (unsigned)AGENT_RUN_TASK_STACK, (unsigned)internal_free,
                     (unsigned)internal_largest, (unsigned)psram_free);
            return ESP_ERR_NO_MEM;
        }
        xSemaphoreGive(s_agent_run_lock);
        ESP_LOGI(TAG,
                 "Agent executor started run=%s stack=%u internal_free=%u "
                 "internal_largest=%u psram_free=%u",
                 launch_job_id, (unsigned)AGENT_RUN_TASK_STACK,
                 (unsigned)internal_free, (unsigned)internal_largest,
                 (unsigned)psram_free);
        return ESP_OK;
    }

    const char *failure =
        "executor controls changed repeatedly during binding";
    if (err && err_size > 0) {
        strlcpy(err, failure, err_size);
    }
    (void)agent_run_terminalize_launch_failure_exact(
        launch_job_id, failure);
    agent_run_task_arg_free(task_arg);
    return ESP_ERR_TIMEOUT;
}

static void __attribute__((unused)) agent_run_start_next_queued(void)
{
    si_agent_task_execution_t *next = agent_alloc(sizeof(*next));
    if (!next) {
        ESP_LOGE(TAG, "retire Agent shadow failed: execution alloc unavailable");
        return;
    }
    for (size_t attempts = 0;
         attempts < SI_AGENT_TASK_PENDING_CAPACITY; ++attempts) {
        memset(next, 0, sizeof(*next));
        bool started = false;
        esp_err_t ret = si_agent_task_service_advance(next, &started);
        if (ret != ESP_OK || !started) {
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "retire Agent shadow failed: %s",
                         esp_err_to_name(ret));
            }
            break;
        }
        /* Legacy is the only execution owner in this candidate. A queue can
         * only be residue from an older V2 image; close it without execution. */
        (void)si_agent_task_service_fail(
            next->run_id, "shadow_queue_execution_disabled");
    }
    free(next);
}

/* Caller holds s_agent_run_lock. This mirrors the Task Service idempotency
 * scope for the legacy-primary executor. A retry may reuse an exact Run only
 * when both its opaque key and its trusted source/thread binding match. */
static bool agent_run_idempotency_scope_matches_locked(
    const si_agent_task_source_t *source, const char *thread_id,
    const char *idempotency_key)
{
    return source && thread_id && idempotency_key && idempotency_key[0] &&
           s_agent_run_job.job_id[0] &&
           strcmp(s_agent_run_job.idempotency_key, idempotency_key) == 0 &&
           strcmp(s_agent_run_job.session_id, thread_id) == 0 &&
           s_agent_run_job.source_origin == source->origin &&
           s_agent_run_job.source_auth_kind == source->auth_kind &&
           s_agent_run_job.source_principal == source->principal &&
           s_agent_run_job.source_auth_generation == source->auth_generation &&
           s_agent_run_job.source_authority_ceiling == source->authority_ceiling &&
           strcmp(s_agent_run_job.source_auth_id,
                  source->auth_id ? source->auth_id : "") == 0;
}

/* Hash every request-controlled executor input in a deterministic key order.
 * The immutable fingerprint remains valid even if runtime safety revokes the
 * mutable page-context projection later. Trusted source and Thread are bound
 * separately by agent_run_idempotency_scope_matches_locked(). */
static bool agent_run_idempotency_fingerprint(
    const char *message, const char *completion_criteria,
    const char *page_context_json, const char *profile_id,
    const char *model_override, bool dry_run, bool include_screenshot,
    bool allow_web_search, char out[65])
{
    if (!message || !completion_criteria || !out) {
        return false;
    }
    cJSON *canonical = cJSON_CreateObject();
    bool complete = canonical &&
        cJSON_AddStringToObject(canonical, "message", message) &&
        cJSON_AddStringToObject(canonical, "completion_criteria",
                                completion_criteria) &&
        cJSON_AddStringToObject(canonical, "profile",
                                profile_id ? profile_id : "") &&
        cJSON_AddStringToObject(canonical, "model",
                                model_override ? model_override : "") &&
        cJSON_AddBoolToObject(canonical, "dry_run", dry_run) &&
        cJSON_AddBoolToObject(canonical, "include_screenshot",
                              include_screenshot) &&
        cJSON_AddBoolToObject(canonical, "allow_web_search",
                              allow_web_search) &&
        cJSON_AddStringToObject(canonical, "page_context_json",
                                page_context_json ? page_context_json : "");
    char *serialized = complete ? cJSON_PrintUnformatted(canonical) : NULL;
    cJSON_Delete(canonical);
    bool hashed = serialized && agent_text_sha256(serialized, out);
    free(serialized);
    return hashed;
}

static esp_err_t agent_run_submit(const char *thread_id_in,
                                  const si_agent_task_source_t *source,
                                  const char *message,
                                  const char *completion_criteria,
                                  const char *idempotency_key_in,
                                  const char *page_context_json,
                                  const char *profile_id,
                                  const char *model_override,
                                  bool dry_run,
                                  bool include_screenshot,
                                  bool allow_web_search,
                                  bool *accepted_out,
                                  bool *busy_out,
                                  bool *deduplicated_out,
                                  char *job_id_out,
                                  size_t job_id_out_size,
                                  char *err,
                                  size_t err_size)
{
    if (accepted_out) {
        *accepted_out = false;
    }
    if (busy_out) {
        *busy_out = false;
    }
    if (deduplicated_out) {
        *deduplicated_out = false;
    }
    if (job_id_out && job_id_out_size > 0) {
        job_id_out[0] = '\0';
    }
    if (err && err_size > 0) {
        err[0] = '\0';
    }

    /* The persistent product switch is an execution boundary, not just a Web
     * navigation preference. Enforce it at the shared submit owner so HTTP,
     * diagnostics, and future adapters cannot start a new local Agent run
     * after Settings disables the runtime. Existing work is not cancelled. */
    si_product_feature_settings_t product_features;
    si_product_feature_settings_get(&product_features);
    if (!product_features.embedded_agent_enabled) {
        if (err && err_size > 0) {
            strlcpy(err, "embedded Agent disabled in Settings", err_size);
        }
        return ESP_ERR_NOT_ALLOWED;
    }

    esp_err_t ret = agent_run_start();
    if (ret != ESP_OK) {
        if (err && err_size > 0) {
            strlcpy(err, esp_err_to_name(ret), err_size);
        }
        return ret;
    }
    if (!message || message[0] == '\0' || !source ||
        !si_capability_set_has(source->authority_ceiling,
                               SI_CAPABILITY_AGENT_RUN)) {
        if (err && err_size > 0) {
            strlcpy(err, !message || message[0] == '\0' ?
                    "message required" : "Agent Run capability required",
                    err_size);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (strnlen(message, AGENT_RUN_GOAL_MAX_LEN + 1U) >
        AGENT_RUN_GOAL_MAX_LEN) {
        if (err && err_size > 0) {
            strlcpy(err, "message must be 1..512 bytes", err_size);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!agent_run_source_binding_live(
            source->origin, source->auth_kind, source->principal,
            source->auth_id, source->auth_generation,
            source->authority_ceiling)) {
        if (err && err_size > 0) {
            strlcpy(err,
                    "Agent source authorization is not live or unsupported",
                    err_size);
        }
        return ESP_ERR_INVALID_STATE;
    }
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    /* Prompt builders still contain broad device diagnostics. Until every
     * context field is capability-projected, admit only the exact full-cap
     * browser session used by the first HIL candidate. SYSTEM, MCP, QQ, and
     * partial-capability sources fail closed before any device context is
     * assembled or sent to a provider. */
    if (source->origin != SI_AGENT_TASK_ORIGIN_WEB ||
        source->auth_kind != SI_AGENT_TASK_AUTH_DEVICE_SESSION ||
        source->principal != SI_PRINCIPAL_BROWSER ||
        source->authority_ceiling != SI_CAPABILITIES_WEB_DEFAULT) {
        if (err && err_size > 0) {
            strlcpy(err,
                    "recovery candidate admits full-cap browser sessions only",
                    err_size);
        }
        return ESP_ERR_INVALID_STATE;
    }
    /* Screenshot capture owns the video-control lease. Keep it outside the
     * first recovery HIL just like every other leased path. */
    if (include_screenshot) {
        if (err && err_size > 0) {
            strlcpy(err,
                    "screenshot capture is unavailable in the RAM-only recovery candidate",
                    err_size);
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    if (thread_id_in && thread_id_in[0] &&
        !agent_session_id_valid(thread_id_in)) {
        if (err && err_size > 0) {
            strlcpy(err, "invalid thread_id", err_size);
        }
        return ESP_ERR_INVALID_ARG;
    }

    char thread_id[AGENT_SESSION_ID_MAX_LEN + 1];
    agent_session_normalize(thread_id_in, thread_id, sizeof(thread_id));
    bool page_context_enabled = si_page_context_is_enabled();
    const char *effective_page_context =
        page_context_enabled && agent_run_page_context_allowed() &&
        page_context_json ?
        page_context_json : "";
    if ((include_screenshot || allow_web_search ||
         effective_page_context[0]) &&
        !si_capability_set_has(source->authority_ceiling,
                               SI_CAPABILITY_OBSERVE)) {
        if (err && err_size > 0) {
            strlcpy(err,
                    "OBSERVE capability required for screenshot, page "
                    "context, or web search",
                    err_size);
        }
        return ESP_ERR_INVALID_STATE;
    }
    const char *effective_completion_criteria =
        completion_criteria && completion_criteria[0] ?
        completion_criteria : AGENT_DEFAULT_COMPLETION_CRITERIA;
    if (strnlen(effective_completion_criteria,
                SI_AGENT_TASK_CRITERIA_MAX_LEN + 1U) >
            SI_AGENT_TASK_CRITERIA_MAX_LEN ||
        (profile_id &&
         strnlen(profile_id, AGENT_API_PROFILE_ID_MAX_LEN + 1U) >
             AGENT_API_PROFILE_ID_MAX_LEN) ||
        (model_override &&
         strnlen(model_override, AGENT_API_MODEL_MAX_LEN + 1U) >
             AGENT_API_MODEL_MAX_LEN) ||
        strnlen(effective_page_context, AGENT_PAGE_CONTEXT_MAX_LEN + 1U) >
            AGENT_PAGE_CONTEXT_MAX_LEN) {
        if (err && err_size > 0) {
            strlcpy(err, "agent run request field exceeds its bounded limit",
                    err_size);
        }
        return ESP_ERR_INVALID_ARG;
    }
    char generated_idempotency[SI_AGENT_TASK_IDEMPOTENCY_MAX_LEN + 1U] = {0};
    const char *idempotency_key = idempotency_key_in;
    if (!idempotency_key || !idempotency_key[0]) {
        snprintf(generated_idempotency, sizeof(generated_idempotency),
                 "web-%016" PRIx64 "-%08" PRIx32,
                 s_server_boot_id, esp_random());
        idempotency_key = generated_idempotency;
    }
    if (strnlen(idempotency_key, SI_AGENT_TASK_IDEMPOTENCY_MAX_LEN + 1U) >
        SI_AGENT_TASK_IDEMPOTENCY_MAX_LEN) {
        if (err && err_size > 0) {
            strlcpy(err, "idempotency_key must be 1..64 bytes", err_size);
        }
        return ESP_ERR_INVALID_ARG;
    }
    char idempotency_fingerprint[65] = {0};
    if (!agent_run_idempotency_fingerprint(
            message, effective_completion_criteria,
            effective_page_context, profile_id, model_override,
            dry_run, include_screenshot, allow_web_search,
            idempotency_fingerprint)) {
        if (err && err_size > 0) {
            strlcpy(err, "agent run fingerprint alloc failed", err_size);
        }
        return ESP_ERR_NO_MEM;
    }

    agent_run_task_arg_t *task_arg = agent_alloc(sizeof(*task_arg));
    const size_t message_bytes = strlen(message) + 1U;
    char *message_copy = agent_alloc(message_bytes);
    if (!task_arg || !message_copy) {
        free(task_arg);
        free(message_copy);
        if (err && err_size > 0) {
            strlcpy(err, "agent run alloc failed", err_size);
        }
        return ESP_ERR_NO_MEM;
    }
    memset(task_arg, 0, sizeof(*task_arg));
    memcpy(message_copy, message, message_bytes);

    char run_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    char turn_id[AGENT_TURN_ID_MAX_LEN + 1] = {0};
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        agent_run_task_arg_free(task_arg);
        free(message_copy);
        return ESP_ERR_TIMEOUT;
    }
    bool existing_active =
        s_agent_run_job.state == AGENT_RUN_STATE_RUNNING ||
        s_agent_run_job.state == AGENT_RUN_STATE_WAITING_REQUEST ||
        s_agent_run_job.state == AGENT_RUN_STATE_PAUSED ||
        s_agent_run_job.task;
    if (agent_run_idempotency_scope_matches_locked(
            source, thread_id, idempotency_key)) {
        if (strcmp(s_agent_run_job.idempotency_fingerprint,
                   idempotency_fingerprint) != 0) {
            if (busy_out) *busy_out = existing_active;
            if (job_id_out && job_id_out_size > 0) {
                strlcpy(job_id_out, s_agent_run_job.job_id,
                        job_id_out_size);
            }
            if (err && err_size > 0) {
                strlcpy(err, "idempotency_conflict", err_size);
            }
            xSemaphoreGive(s_agent_run_lock);
            agent_run_task_arg_free(task_arg);
            free(message_copy);
            return ESP_ERR_INVALID_STATE;
        }
        if (accepted_out) *accepted_out = true;
        if (busy_out) *busy_out = existing_active;
        if (deduplicated_out) *deduplicated_out = true;
        if (job_id_out && job_id_out_size > 0) {
            strlcpy(job_id_out, s_agent_run_job.job_id, job_id_out_size);
        }
        xSemaphoreGive(s_agent_run_lock);
        agent_run_task_arg_free(task_arg);
        free(message_copy);
        return ESP_OK;
    }
    if (existing_active) {
        if (busy_out) *busy_out = true;
        if (job_id_out && job_id_out_size > 0) {
            strlcpy(job_id_out, s_agent_run_job.job_id, job_id_out_size);
        }
        xSemaphoreGive(s_agent_run_lock);
        agent_run_task_arg_free(task_arg);
        free(message_copy);
        return ESP_OK;
    }
    uint32_t run_sequence = ++s_agent_run_seq;
    snprintf(run_id, sizeof(run_id), "run-%08" PRIx32 "-%08" PRIx32,
             (uint32_t)s_server_boot_id, run_sequence);
    snprintf(turn_id, sizeof(turn_id), "turn-%08" PRIx32 "-%08" PRIx32,
             esp_random(), run_sequence);

    free(s_agent_run_job.result_json);
    memset(&s_agent_run_job, 0, sizeof(s_agent_run_job));
    strlcpy(s_agent_run_job.job_id, run_id,
            sizeof(s_agent_run_job.job_id));
    strlcpy(s_agent_run_job.turn_id, turn_id,
            sizeof(s_agent_run_job.turn_id));
    strlcpy(s_agent_run_job.session_id, thread_id,
            sizeof(s_agent_run_job.session_id));
    s_agent_run_job.source_origin = source->origin;
    s_agent_run_job.source_auth_kind = source->auth_kind;
    s_agent_run_job.source_principal = source->principal;
    s_agent_run_job.source_auth_generation = source->auth_generation;
    s_agent_run_job.source_authority_ceiling = source->authority_ceiling;
    strlcpy(s_agent_run_job.source_auth_id,
            source->auth_id ? source->auth_id : "",
            sizeof(s_agent_run_job.source_auth_id));
    strlcpy(s_agent_run_job.idempotency_key, idempotency_key,
            sizeof(s_agent_run_job.idempotency_key));
    strlcpy(s_agent_run_job.idempotency_fingerprint,
            idempotency_fingerprint,
            sizeof(s_agent_run_job.idempotency_fingerprint));
    strlcpy(s_agent_run_job.goal, message, sizeof(s_agent_run_job.goal));
    s_agent_run_job.intent_revision = 1U;
    s_agent_run_job.plan_version = 1U;
    __atomic_store_n(
        &s_agent_run_page_context_generation,
        __atomic_load_n(&s_agent_page_context_generation, __ATOMIC_ACQUIRE),
        __ATOMIC_RELEASE);
    strlcpy(s_agent_run_job.page_context_json, effective_page_context,
            sizeof(s_agent_run_job.page_context_json));
    strlcpy(s_agent_run_job.profile, profile_id ? profile_id : "",
            sizeof(s_agent_run_job.profile));
    strlcpy(s_agent_run_job.model, model_override ? model_override : "",
            sizeof(s_agent_run_job.model));
    s_agent_run_job.dry_run = dry_run;
    s_agent_run_job.include_screenshot = include_screenshot;
    s_agent_run_job.allow_web_search = allow_web_search;
    s_agent_run_job.state = AGENT_RUN_STATE_RUNNING;
    s_agent_run_job.started_ms = si_monotonic_ms();
    s_agent_run_job.updated_ms = s_agent_run_job.started_ms;
    strlcpy(s_agent_run_job.stage, "排队启动",
            sizeof(s_agent_run_job.stage));
    strlcpy(s_agent_run_job.detail, "Runtime V2 shadow 正在绑定",
            sizeof(s_agent_run_job.detail));
    si_action_event_buffer_init(&s_agent_run_job.action_events);

    strlcpy(task_arg->job_id, run_id, sizeof(task_arg->job_id));
    strlcpy(task_arg->turn_id, turn_id, sizeof(task_arg->turn_id));
    strlcpy(task_arg->session_id, thread_id, sizeof(task_arg->session_id));
    task_arg->source_origin = source->origin;
    task_arg->source_auth_kind = source->auth_kind;
    task_arg->source_principal = source->principal;
    task_arg->source_auth_generation = source->auth_generation;
    task_arg->source_authority_ceiling = source->authority_ceiling;
    strlcpy(task_arg->source_auth_id,
            source->auth_id ? source->auth_id : "",
            sizeof(task_arg->source_auth_id));
    strlcpy(task_arg->page_context_json, effective_page_context,
            sizeof(task_arg->page_context_json));
    strlcpy(task_arg->profile, profile_id ? profile_id : "",
            sizeof(task_arg->profile));
    strlcpy(task_arg->model, model_override ? model_override : "",
            sizeof(task_arg->model));
    task_arg->dry_run = dry_run;
    task_arg->include_screenshot = include_screenshot;
    task_arg->allow_web_search = allow_web_search;
    task_arg->message = message_copy;
    xSemaphoreGive(s_agent_run_lock);

    bool task_service_bound = false;
    char shadow_error[AGENT_RUN_ERROR_MAX_LEN + 1] = {0};
#if AGENT_RUNTIME_V2_SHADOW_BINDING_ENABLED
    si_agent_task_source_t trusted_source = *source;
    trusted_source.source_conversation_id = thread_id;
    si_agent_task_submit_t task_input = {
        .thread_id = thread_id,
        .turn_id = turn_id,
        .run_id = run_id,
        .goal = message,
        .completion_criteria = effective_completion_criteria,
        /* This adapter owns the fixed completion contract. In the read-only
         * cutover a bounded response artifact may complete a run only when
         * the executed result contains no mutation or HID action. The
         * finalizer independently reclassifies the result before invoking
         * the artifact verifier, so user/model text cannot self-declare a
         * mutation complete. Screenshot-assisted runs still require a fresh
         * observation and therefore remain fail closed. */
        .completion_criteria_kind = include_screenshot ?
            SI_AGENT_TASK_CRITERIA_OBSERVATION :
            SI_AGENT_TASK_CRITERIA_DELIVERABLE,
        .idempotency_key = idempotency_key,
        .profile = profile_id,
        .model = model_override,
        .page_context_json = effective_page_context,
        .dry_run = dry_run,
        .include_screenshot = include_screenshot,
        .allow_web_search = allow_web_search,
        .source = trusted_source,
    };
    si_agent_task_receipt_t receipt = {0};
    ret = si_agent_task_service_submit(&task_input, &receipt);
    task_service_bound =
        ret == ESP_OK && receipt.accepted && receipt.started &&
        !receipt.queued && !receipt.deduplicated &&
        strcmp(receipt.run_id, run_id) == 0 &&
        strcmp(receipt.turn_id, turn_id) == 0;
    if (task_service_bound) {
        ret = agent_run_prepare_task_service(run_id);
        task_service_bound = ret == ESP_OK;
    }
    if (!task_service_bound) {
        if (ret == ESP_OK) {
            strlcpy(shadow_error,
                    "shadow identity or active binding rejected",
                    sizeof(shadow_error));
        } else {
            strlcpy(shadow_error, si_agent_task_service_last_error(),
                    sizeof(shadow_error));
        }
        if (!shadow_error[0]) {
            strlcpy(shadow_error, esp_err_to_name(ret),
                    sizeof(shadow_error));
        }
        ESP_LOGE(TAG,
                 "Runtime V2 shadow unavailable for %s; legacy model loop "
                 "continues with bounded read-only compatibility; writes "
                 "and leased tools fail closed: %s",
                 run_id, shadow_error);
    }
#else
    ret = ESP_ERR_NOT_SUPPORTED;
    strlcpy(shadow_error,
            "Runtime V2 binding disabled in recovery candidate; background "
            "recovery is diagnostic only",
            sizeof(shadow_error));
    ESP_LOGW(TAG,
             "Runtime V2 shadow binding disabled for %s; legacy model loop "
             "continues with bounded read-only compatibility",
             run_id);
#endif
    task_arg->task_service_bound = task_service_bound;
    ret = agent_run_publish_shadow_binding_exact(
        task_arg, shadow_error, err, err_size);
    if (ret != ESP_OK) {
        if (task_service_bound) {
            (void)si_agent_task_service_fail(run_id,
                                             "binding_publish_failed");
        }
        agent_run_task_arg_free(task_arg);
        return ret;
    }
    bool launch_aborted = false;
    ret = agent_run_launch_execution(
        task_arg, shadow_error, &launch_aborted, err, err_size);
    if (ret != ESP_OK) {
        if (task_service_bound && !launch_aborted) {
            (void)si_agent_task_service_fail(run_id,
                                             "executor_start_failed");
        }
        if (err && err_size > 0 && !err[0]) {
            strlcpy(err, esp_err_to_name(ret), err_size);
        }
        return ret;
    }
    if (accepted_out) *accepted_out = true;
    if (job_id_out && job_id_out_size > 0) {
        strlcpy(job_id_out, run_id, job_id_out_size);
    }
    return ESP_OK;
}

static esp_err_t agent_run_revoke_page_context(
    const si_product_feature_settings_t *settings)
{
    if (!settings || settings->page_context_enabled) {
        return ESP_ERR_INVALID_ARG;
    }
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    /* Runtime feature mutation would synchronously touch checkpoint/settings
     * storage. Keep the first recovery/HIL candidate bounded: the caller may
     * retry after rebooting with a fixed configuration. */
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = agent_run_start();
    if (ret != ESP_OK) {
        return ret;
    }
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Revoke volatile projection data and advance the admission generation in
     * one short RAM-only critical section. Task/Event journal, TF checkpoint,
     * and settings persistence must never run while holding the Run lock. */
    char *revoked_result_json = s_agent_run_job.result_json;
    s_agent_run_job.result_json = NULL;
    memset(s_agent_run_job.page_context_json, 0,
           sizeof(s_agent_run_job.page_context_json));
    (void)__atomic_add_fetch(&s_agent_page_context_generation, 1U,
                             __ATOMIC_ACQ_REL);
    xSemaphoreGive(s_agent_run_lock);

    if (revoked_result_json) {
        memset(revoked_result_json, 0, strlen(revoked_result_json));
        free(revoked_result_json);
    }
    esp_err_t shadow_ret = ESP_ERR_NOT_SUPPORTED;
#if AGENT_RUNTIME_V2_SHADOW_BINDING_ENABLED
    shadow_ret = si_agent_task_service_revoke_page_context();
#endif
    if (shadow_ret != ESP_OK) {
        ESP_LOGW(TAG, "Runtime V2 shadow context revoke degraded: %s",
                 esp_err_to_name(shadow_ret));
        if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            snprintf(s_agent_run_job.task_service_error,
                     sizeof(s_agent_run_job.task_service_error),
                     "context revoke storage_failure: %s",
                     esp_err_to_name(shadow_ret));
            xSemaphoreGive(s_agent_run_lock);
        }
    }
    ret = si_agent_repository_run_checkpoint_revoke_page_context();
    if (ret == ESP_OK) {
        ret = si_product_feature_settings_set(settings);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Persist page-context revocation failed: %s",
                 esp_err_to_name(ret));
    }
    return ret;
#endif
}
