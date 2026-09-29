// Transitional Agent module: run HTTP and diagnostics adapters. Shares the run context until its service API is finalized.
static esp_err_t agent_run_handler(httpd_req_t *req)
{
    si_product_feature_settings_t product_features;
    si_product_feature_settings_get(&product_features);
    if (!product_features.embedded_agent_enabled) {
        return si_http_send_text_status(req, "403 Forbidden",
                                        "embedded Agent disabled in Settings");
    }
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char *buf = calloc(1, AGENT_RUN_MAX_BODY);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, AGENT_RUN_MAX_BODY, &root);
    free(buf);
    if (ret != ESP_OK) {
        return ret;
    }

    const char *message = json_string_any(root, "message", "prompt", "input");
    const char *completion_criteria =
        json_string_any(root, "completion_criteria", "done_when", NULL);
    const char *idempotency_key =
        json_string_any(root, "idempotency_key", "request_id", NULL);
    const char *profile_id = json_string_any(root, "profile", "profile_id", NULL);
    const char *model_override = json_string_any(root, "model", "model_name", NULL);
    /* Request fields select conversation continuity only. Authentication is
     * taken exclusively from the already-validated HTTP session context. */
    const char *requested_thread =
        json_string_any(root, "thread_id", "conversation_id", "session_id");
    const char *authority_mode =
        json_string_any(root, "authority_mode", "execution_policy", NULL);
    cJSON *dry_item = cJSON_GetObjectItemCaseSensitive(root, "dry_run");
    cJSON *include_item = cJSON_GetObjectItemCaseSensitive(root, "include_screenshot");
    cJSON *web_item = cJSON_GetObjectItemCaseSensitive(root, "include_web_search");
    if (!cJSON_IsBool(web_item)) {
        web_item = cJSON_GetObjectItemCaseSensitive(root, "allow_web_search");
    }
    if (!cJSON_IsBool(web_item)) {
        web_item = cJSON_GetObjectItemCaseSensitive(root, "web_search");
    }
    bool dry_run = !cJSON_IsBool(dry_item) || cJSON_IsTrue(dry_item);
    bool include_screenshot = cJSON_IsBool(include_item) && cJSON_IsTrue(include_item);
    bool allow_web_search = cJSON_IsBool(web_item) && cJSON_IsTrue(web_item);
    if (authority_mode && strcmp(authority_mode, "policy") == 0) {
        dry_run = false;
    }
    char page_context_json[AGENT_PAGE_CONTEXT_MAX_LEN + 1] = {0};
    char page_context_err[AGENT_RUN_ERROR_MAX_LEN + 1] = {0};
    esp_err_t page_context_ret = agent_page_context_normalize(
        product_features.page_context_enabled ?
            cJSON_GetObjectItemCaseSensitive(root, "page_context") : NULL,
        page_context_json, sizeof(page_context_json),
        page_context_err, sizeof(page_context_err));
    if (page_context_ret != ESP_OK) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   page_context_err[0] ? page_context_err :
                                   "invalid page_context");
    }

    bool accepted = false;
    bool busy = false;
    bool deduplicated = false;
    char submitted_run_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    char err[AGENT_RUN_ERROR_MAX_LEN + 1] = {0};
    si_agent_task_source_t trusted_source = {
        .origin = session.principal == SI_PRINCIPAL_MCP ?
                  SI_AGENT_TASK_ORIGIN_MCP : SI_AGENT_TASK_ORIGIN_WEB,
        .source_account_id = session.session_id,
        .source_actor_id = si_principal_kind_name(session.principal),
        .auth_kind = SI_AGENT_TASK_AUTH_DEVICE_SESSION,
        .principal = session.principal,
        .auth_id = session.session_id,
        .auth_generation = session.generation,
        .authority_ceiling = session.capabilities,
        .history_policy = SI_AGENT_TASK_HISTORY_THREAD,
    };
    esp_err_t submit_ret = agent_run_submit(requested_thread,
                                            &trusted_source, message,
                                            completion_criteria,
                                            idempotency_key,
                                            page_context_json, profile_id,
                                            model_override, dry_run,
                                            include_screenshot, allow_web_search,
                                            &accepted, &busy, &deduplicated,
                                            submitted_run_id,
                                            sizeof(submitted_run_id), err,
                                            sizeof(err));
    if (submit_ret != ESP_OK) {
        cJSON_Delete(root);
        if (submit_ret == ESP_ERR_NOT_ALLOWED) {
            return si_http_send_text_status(
                req, "403 Forbidden",
                err[0] ? err : "embedded Agent disabled in Settings");
        }
        if (submit_ret == ESP_ERR_INVALID_ARG) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       err[0] ? err : "invalid agent run request");
        }
        if (submit_ret == ESP_ERR_INVALID_STATE &&
            strcmp(err, "idempotency_conflict") == 0) {
            httpd_resp_set_status(req, "409 Conflict");
            cJSON *conflict = cJSON_CreateObject();
            cJSON_AddBoolToObject(conflict, "accepted", false);
            cJSON_AddBoolToObject(conflict, "queued", false);
            cJSON_AddBoolToObject(conflict, "busy", busy);
            cJSON_AddBoolToObject(conflict, "deduplicated", false);
            cJSON_AddStringToObject(conflict, "submitted_run_id", "");
            cJSON_AddStringToObject(conflict, "active_run_id",
                                    busy ? submitted_run_id : "");
            cJSON_AddStringToObject(conflict, "conflicting_run_id",
                                    submitted_run_id);
            cJSON_AddStringToObject(conflict, "reason",
                                    "idempotency_conflict");
            esp_err_t send_ret = si_http_send_json(req, conflict);
            cJSON_Delete(conflict);
            return send_ret;
        }
        if (submit_ret == ESP_ERR_INVALID_STATE &&
            strstr(err, "H.264 codec is active") != NULL) {
            return si_http_send_text_status(
                req, "409 Conflict", err);
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   err[0] ? err : esp_err_to_name(submit_ret));
    }
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    if (busy && !deduplicated) {
        /* The returned ID belongs to the already-active Run. Never present it
         * as the receipt for this rejected submission, and do not append a
         * racy full status projection for a potentially replaced Run. */
        httpd_resp_set_status(req, "409 Conflict");
        cJSON_AddBoolToObject(resp, "accepted", false);
        cJSON_AddBoolToObject(resp, "queued", false);
        cJSON_AddBoolToObject(resp, "busy", true);
        cJSON_AddBoolToObject(resp, "deduplicated", false);
        cJSON_AddStringToObject(resp, "submitted_run_id", "");
        cJSON_AddStringToObject(resp, "active_run_id", submitted_run_id);
        cJSON_AddStringToObject(resp, "reason", "agent_run_busy");
        esp_err_t send_ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return send_ret;
    }

    cJSON_AddBoolToObject(resp, "accepted", accepted);
    cJSON_AddBoolToObject(resp, "queued", false);
    cJSON_AddBoolToObject(resp, "deduplicated", deduplicated);
    /* Preserve the atomic submit receipt separately from the newer live-status
     * snapshot, which may already observe completion or replacement. */
    cJSON_AddBoolToObject(resp, "receipt_busy", busy);
    cJSON_AddStringToObject(resp, "submitted_run_id", submitted_run_id);
    cJSON_AddStringToObject(resp, "active_run_id", busy ? submitted_run_id : "");
    if (accepted && !deduplicated) {
        httpd_resp_set_status(req, "202 Accepted");
    }
    bool status_mismatch = false;
    agent_run_add_status_json(resp, false, submitted_run_id,
                              &status_mismatch);
    if (status_mismatch) {
        cJSON_AddBoolToObject(resp, "status_retained", false);
    }
    if (!cJSON_GetObjectItemCaseSensitive(resp, "busy")) {
        cJSON_AddBoolToObject(resp, "busy", busy);
    }
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}

static esp_err_t agent_run_control_handler(
    httpd_req_t *req, agent_run_control_action_t action)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (req->content_len <= 0 || req->content_len >= 192) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "exact run_id required");
    }
    char body[192] = {0};
    cJSON *root = NULL;
    esp_err_t parse_ret = si_http_recv_json(req, body, sizeof(body), &root);
    if (parse_ret != ESP_OK) {
        return parse_ret;
    }
    const cJSON *run_id =
        cJSON_GetObjectItemCaseSensitive(root, "run_id");
    if (!cJSON_IsString(run_id) || !run_id->valuestring[0] ||
        strnlen(run_id->valuestring, AGENT_RUN_JOB_ID_MAX_LEN + 1U) >
            AGENT_RUN_JOB_ID_MAX_LEN) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "exact run_id required");
    }
    char expected_job[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    strlcpy(expected_job, run_id->valuestring, sizeof(expected_job));
    cJSON_Delete(root);
    char controlled_job[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    bool shadow_degraded = false;
    esp_err_t task_ret = agent_run_control_exact(
        action, expected_job, controlled_job, sizeof(controlled_job),
        &shadow_degraded);
    bool accepted = task_ret == ESP_OK;
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "accepted", accepted);
    cJSON_AddBoolToObject(resp, "runtime_shadow_degraded",
                          shadow_degraded);
    cJSON_AddStringToObject(
        resp, "control",
        action == AGENT_RUN_CONTROL_PAUSE ? "pause" :
        action == AGENT_RUN_CONTROL_RESUME ? "resume" : "abort");
    if (accepted) {
        cJSON_AddStringToObject(resp, "controlled_run_id", expected_job);
    }
    if (!accepted) {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(task_ret));
    }
    if (task_ret == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "409 Conflict");
        cJSON_AddStringToObject(resp, "reason", "run_id_mismatch");
        cJSON_AddStringToObject(resp, "run_id", expected_job);
    } else {
        bool status_mismatch = false;
        agent_run_add_status_json(resp, false, expected_job,
                                  &status_mismatch);
        if (status_mismatch) {
            cJSON_AddBoolToObject(resp, "status_retained", false);
        }
    }
    esp_err_t ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t agent_run_pause_handler(httpd_req_t *req)
{
    return agent_run_control_handler(req, AGENT_RUN_CONTROL_PAUSE);
}

static esp_err_t agent_run_resume_handler(httpd_req_t *req)
{
    return agent_run_control_handler(req, AGENT_RUN_CONTROL_RESUME);
}

static esp_err_t agent_run_abort_handler(httpd_req_t *req)
{
    return agent_run_control_handler(req, AGENT_RUN_CONTROL_ABORT);
}

static esp_err_t agent_run_cancel_handler(httpd_req_t *req)
{
    return agent_run_abort_handler(req);
}

static esp_err_t agent_run_steer_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    char buf[SCRATCH_BUFSIZE] = {0};
    cJSON *root = NULL;
    esp_err_t json_ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (json_ret != ESP_OK) {
        return json_ret;
    }
    const char *message = json_string_any(root, "message", "input", "steer");
    const char *requested_run =
        json_string_any(root, "run_id", "job_id", NULL);
    if (!requested_run || !requested_run[0] ||
        strnlen(requested_run, AGENT_RUN_JOB_ID_MAX_LEN + 1U) >
            AGENT_RUN_JOB_ID_MAX_LEN) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "exact run_id required");
    }
    if (!message || !message[0] ||
        strlen(message) > AGENT_RUN_STEER_MAX_LEN) {
        cJSON_Delete(root);
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "steer message must be 1..512 bytes");
    }
    bool accepted = false;
    char run_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    uint32_t revision = 0;
    esp_err_t steer_ret = ESP_ERR_INVALID_STATE;
    bool active = false;
    bool shadow_bound = false;
    bool run_mismatch = false;
    char expected_run[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    strlcpy(expected_run, requested_run, sizeof(expected_run));
    if (agent_run_start() == ESP_OK && s_agent_run_lock &&
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(300)) == pdTRUE) {
        active =
            s_agent_run_job.state == AGENT_RUN_STATE_RUNNING ||
            s_agent_run_job.state == AGENT_RUN_STATE_WAITING_REQUEST ||
            s_agent_run_job.state == AGENT_RUN_STATE_PAUSED;
        bool run_matches =
            strcmp(expected_run, s_agent_run_job.job_id) == 0;
        run_mismatch = !run_matches;
        if (run_mismatch) {
            steer_ret = ESP_ERR_NOT_FOUND;
        }
        if (active && run_matches) {
            strlcpy(run_id, s_agent_run_job.job_id, sizeof(run_id));
            shadow_bound = s_agent_run_job.task_service_bound &&
                           !s_agent_run_job.task_service_control_pending;
        }
        xSemaphoreGive(s_agent_run_lock);
    }
    si_agent_task_service_snapshot_t *task_snapshot = NULL;
    if (active && run_id[0]) {
        /* Task/Event journal I/O never runs under the Web Run lock. */
        steer_ret = shadow_bound ?
            si_agent_task_service_steer(run_id, message) : ESP_OK;
        if (steer_ret == ESP_OK && shadow_bound) {
            task_snapshot = agent_alloc(sizeof(*task_snapshot));
            if (task_snapshot) {
                memset(task_snapshot, 0, sizeof(*task_snapshot));
                if (si_agent_task_service_snapshot(task_snapshot) != ESP_OK) {
                    free(task_snapshot);
                    task_snapshot = NULL;
                }
            }
        }
        if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(300)) == pdTRUE) {
            bool same_active =
                strcmp(s_agent_run_job.job_id, run_id) == 0 &&
                agent_run_control_state_active(s_agent_run_job.state);
            if (same_active && steer_ret != ESP_OK &&
                s_agent_run_job.task_service_bound) {
                s_agent_run_job.task_service_bound = false;
                snprintf(s_agent_run_job.task_service_error,
                         sizeof(s_agent_run_job.task_service_error),
                         "steer storage_failure; shadow demoted: %s",
                         esp_err_to_name(steer_ret));
                shadow_bound = false;
                steer_ret = ESP_OK;
            }
            if (same_active && steer_ret == ESP_OK) {
                if (task_snapshot &&
                    task_snapshot->runtime.has_active_turn &&
                    strcmp(task_snapshot->runtime.active_turn.run_attempt
                               .run_attempt_id, run_id) == 0) {
                    revision = task_snapshot->runtime.active_turn
                                   .intent_revision;
                    s_agent_run_job.intent_revision = revision;
                    s_agent_run_job.plan_version =
                        task_snapshot->runtime.active_turn.plan_version;
                } else {
                    revision = ++s_agent_run_job.intent_revision;
                    s_agent_run_job.plan_version++;
                }
                strlcpy(s_agent_run_job.pending_steer, message,
                        sizeof(s_agent_run_job.pending_steer));
                s_agent_run_job.updated_ms = si_monotonic_ms();
                strlcpy(
                    s_agent_run_job.detail,
                    shadow_bound ?
                    "已持久记录补充要求；将在下一个安全检查点更新计划" :
                    "Runtime V2 degraded；补充要求将在本地安全检查点生效",
                    sizeof(s_agent_run_job.detail));
                accepted = true;
            } else if (steer_ret == ESP_OK) {
                steer_ret = ESP_ERR_INVALID_STATE;
            }
            xSemaphoreGive(s_agent_run_lock);
        } else {
            steer_ret = ESP_ERR_TIMEOUT;
        }
    }
    free(task_snapshot);
    cJSON_Delete(root);
    if (accepted) {
        agent_run_add_action_eventf(
            run_id, NULL, SI_ACTION_CHANNEL_AGENT,
            SI_ACTION_PHASE_OUTPUT_RECEIVED, false, true,
            "steer accepted · intent revision=%" PRIu32, revision);
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "accepted", accepted);
    cJSON_AddStringToObject(resp, "control", "steer");
    if (accepted) {
        cJSON_AddStringToObject(resp, "controlled_run_id", expected_run);
    }
    cJSON_AddNumberToObject(resp, "intent_revision", revision);
    if (!accepted) {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(steer_ret));
    }
    if (run_mismatch || steer_ret == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "409 Conflict");
        cJSON_AddStringToObject(resp, "reason", "run_id_mismatch");
        cJSON_AddStringToObject(resp, "run_id", expected_run);
    } else {
        bool status_mismatch = false;
        agent_run_add_status_json(resp, false, expected_run,
                                  &status_mismatch);
        if (status_mismatch) {
            cJSON_AddBoolToObject(resp, "status_retained", false);
        }
    }
    esp_err_t ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static bool agent_run_add_event_json(cJSON *events, const si_action_event_t *event)
{
    if (!events || !event) {
        return false;
    }
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return false;
    }
    bool complete =
        cJSON_AddNumberToObject(item, "schema_version",
                                event->schema_version) != NULL &&
        cJSON_AddNumberToObject(item, "seq", event->seq) != NULL &&
        cJSON_AddNumberToObject(item, "ms", event->timestamp_ms) != NULL &&
        cJSON_AddStringToObject(item, "run_id", event->run_id) != NULL &&
        cJSON_AddStringToObject(item, "action_id", event->action_id) != NULL &&
        cJSON_AddStringToObject(item, "actor",
                               si_action_actor_name(event->actor)) != NULL &&
        cJSON_AddStringToObject(item, "channel",
                               si_action_channel_name(event->channel)) != NULL &&
        cJSON_AddStringToObject(item, "phase",
                               si_action_phase_name(event->phase)) != NULL &&
        cJSON_AddBoolToObject(item, "redacted", event->redacted) != NULL &&
        cJSON_AddBoolToObject(item, "background", event->background) != NULL &&
        cJSON_AddStringToObject(
            item, "kind", si_action_phase_legacy_kind(event->phase)) != NULL &&
        cJSON_AddStringToObject(item, "text", event->summary) != NULL;
    if (!complete || !cJSON_AddItemToArray(events, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

typedef struct {
    const char *run_id;
    uint32_t after_seq;
    si_action_event_t *events;
    size_t capacity;
    size_t count;
    uint32_t latest_seq;
    bool history_lost;
} agent_persisted_event_read_ctx_t;

static esp_err_t __attribute__((unused)) agent_persisted_event_read_cb(
    cJSON *entry, void *user_ctx)
{
    agent_persisted_event_read_ctx_t *ctx =
        (agent_persisted_event_read_ctx_t *)user_ctx;
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(entry, "kind");
    const cJSON *run_id = cJSON_GetObjectItemCaseSensitive(entry, "run_id");
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(entry, "seq");
    if (!ctx || !cJSON_IsString(kind) ||
        strcmp(kind->valuestring, "action_event_v1") != 0 ||
        !cJSON_IsString(run_id) || strcmp(run_id->valuestring, ctx->run_id) != 0 ||
        !cJSON_IsNumber(seq) || seq->valuedouble <= 0 ||
        seq->valuedouble > UINT32_MAX) {
        return ESP_OK;
    }
    uint32_t event_seq = (uint32_t)seq->valuedouble;
    if (event_seq > ctx->latest_seq) {
        ctx->latest_seq = event_seq;
    }
    if (event_seq <= ctx->after_seq) {
        return ESP_OK;
    }

    si_action_event_t event = {0};
    event.schema_version = SI_ACTION_EVENT_SCHEMA_VERSION;
    event.seq = event_seq;
    const cJSON *ms = cJSON_GetObjectItemCaseSensitive(entry, "ms");
    const cJSON *actor = cJSON_GetObjectItemCaseSensitive(entry, "actor_code");
    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(entry, "channel_code");
    const cJSON *phase = cJSON_GetObjectItemCaseSensitive(entry, "phase_code");
    const cJSON *action_id =
        cJSON_GetObjectItemCaseSensitive(entry, "action_id");
    const cJSON *summary =
        cJSON_GetObjectItemCaseSensitive(entry, "summary");
    const cJSON *redacted =
        cJSON_GetObjectItemCaseSensitive(entry, "redacted");
    const cJSON *background =
        cJSON_GetObjectItemCaseSensitive(entry, "background");
    event.timestamp_ms = cJSON_IsNumber(ms) ? (uint32_t)ms->valuedouble : 0;
    event.actor = cJSON_IsNumber(actor) ?
                  (si_action_actor_t)actor->valueint : SI_ACTION_ACTOR_AGENT;
    event.channel = cJSON_IsNumber(channel) ?
                    (si_action_channel_t)channel->valueint :
                    SI_ACTION_CHANNEL_AGENT;
    event.phase = cJSON_IsNumber(phase) ?
                  (si_action_phase_t)phase->valueint :
                  SI_ACTION_PHASE_STARTED;
    event.redacted = cJSON_IsTrue(redacted);
    event.background = cJSON_IsTrue(background);
    strlcpy(event.run_id, ctx->run_id, sizeof(event.run_id));
    if (cJSON_IsString(action_id)) {
        strlcpy(event.action_id, action_id->valuestring,
                sizeof(event.action_id));
    }
    if (cJSON_IsString(summary)) {
        strlcpy(event.summary, summary->valuestring, sizeof(event.summary));
    }
    if (ctx->count < ctx->capacity) {
        ctx->events[ctx->count++] = event;
    } else {
        memmove(ctx->events, ctx->events + 1,
                sizeof(*ctx->events) * (ctx->capacity - 1U));
        ctx->events[ctx->capacity - 1U] = event;
        ctx->history_lost = true;
    }
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) agent_read_persisted_events(
    const char *run_id, uint32_t after_seq, si_action_event_t *events,
    size_t capacity, size_t *count_out, uint32_t *latest_seq_out,
    bool *history_lost_out)
{
    if (!run_id || !run_id[0] || !events || capacity == 0 ||
        !count_out || !latest_seq_out || !history_lost_out ||
        !s_agent_history_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    agent_persisted_event_read_ctx_t ctx = {
        .run_id = run_id,
        .after_seq = after_seq,
        .events = events,
        .capacity = capacity,
    };
    if (xSemaphoreTake(s_agent_history_lock,
                       pdMS_TO_TICKS(1500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = agent_history_file_ensure_ready(NULL);
    if (ret == ESP_OK) {
        ret = agent_history_file_for_each_locked(
            agent_persisted_event_read_cb, &ctx, NULL);
    }
    xSemaphoreGive(s_agent_history_lock);
    *count_out = ctx.count;
    *latest_seq_out = ctx.latest_seq;
    *history_lost_out = ctx.history_lost ||
        (ctx.count > 0 && after_seq > 0 &&
         ctx.events[0].seq > after_seq + 1U);
    return ret;
}

typedef struct {
    const char *run_id;
    cJSON *items;
    size_t count;
    size_t capacity;
    uint64_t next_after_seq;
    bool truncated;
} agent_task_event_replay_ctx_t;

static esp_err_t __attribute__((unused)) agent_task_event_replay_json(
    const si_agent_event_record_t *record, void *user_ctx)
{
    agent_task_event_replay_ctx_t *ctx =
        (agent_task_event_replay_ctx_t *)user_ctx;
    if (!record || !ctx || !ctx->items) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->run_id && ctx->run_id[0] &&
        strcmp(record->run_id, ctx->run_id) != 0) {
        return ESP_OK;
    }
    if (ctx->count >= ctx->capacity) {
        ctx->truncated = true;
        return ESP_OK;
    }
    cJSON *item = cJSON_CreateObject();
    if (!item) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(item, "schema_version", SI_AGENT_EVENT_SCHEMA);
    cJSON_AddNumberToObject(item, "seq", (double)record->seq);
    cJSON_AddStringToObject(item, "thread_id", record->thread_id);
    cJSON_AddStringToObject(item, "turn_id", record->turn_id);
    cJSON_AddStringToObject(item, "run_id", record->run_id);
    cJSON_AddStringToObject(item, "step_id", record->step_id);
    cJSON_AddStringToObject(item, "request_id", record->request_id);
    cJSON_AddStringToObject(item, "action_id", record->action_id);
    cJSON_AddStringToObject(item, "artifact_id", record->artifact_id);
    cJSON_AddStringToObject(item, "type", record->event_type);
    cJSON_AddNumberToObject(item, "server_time_ms",
                            (double)record->server_time_ms);
    cJSON_AddNumberToObject(item, "monotonic_time_ms",
                            (double)record->monotonic_time_ms);
    cJSON_AddStringToObject(item, "payload_hash", record->payload_hash);
    cJSON_AddStringToObject(item, "previous_hash", record->previous_hash);
    cJSON_AddStringToObject(item, "event_hash", record->event_hash);
    cJSON *payload = record->payload ?
        cJSON_Duplicate(record->payload, true) : cJSON_CreateObject();
    if (!payload) {
        cJSON_Delete(item);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(item, "payload", payload);
    cJSON_AddItemToArray(ctx->items, item);
    ctx->count++;
    ctx->next_after_seq = record->seq;
    return ESP_OK;
}

static esp_err_t agent_run_events_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    char query[128] = {0};
    char value[24] = {0};
    char requested_job[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    uint64_t task_after_seq = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "after_seq", value,
                                  sizeof(value)) != ESP_OK) {
            (void)httpd_query_key_value(query, "since_seq", value,
                                        sizeof(value));
        }
        if (value[0]) {
            char *end = NULL;
            errno = 0;
            unsigned long long parsed = strtoull(value, &end, 10);
            if (errno != 0 || !end || *end != '\0') {
                return httpd_resp_send_err(
                    req, HTTPD_400_BAD_REQUEST, "invalid after_seq");
            }
            task_after_seq = (uint64_t)parsed;
        }
        (void)httpd_query_key_value(query, "job_id", requested_job,
                                    sizeof(requested_job));
    }

    si_action_event_t *events =
        calloc(SI_ACTION_EVENT_CAPACITY, sizeof(*events));
    if (!events) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "event replay alloc failed");
    }
    if (agent_run_start() != ESP_OK ||
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(300)) != pdTRUE) {
        free(events);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "agent run unavailable");
    }
    uint32_t available_latest_seq = 0;
    uint32_t after_seq = task_after_seq > UINT32_MAX ?
        UINT32_MAX : (uint32_t)task_after_seq;
    bool history_lost = false;
    char job_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    strlcpy(job_id, requested_job[0] ? requested_job :
            s_agent_run_job.job_id, sizeof(job_id));
    bool current_run = job_id[0] &&
                       strcmp(job_id, s_agent_run_job.job_id) == 0;
    size_t count = 0;
    if (current_run) {
        count = si_action_event_buffer_read_after(
            &s_agent_run_job.action_events, after_seq, events,
            SI_ACTION_EVENT_CAPACITY, &available_latest_seq, &history_lost);
    }
    agent_run_state_t state = s_agent_run_job.state;
    xSemaphoreGive(s_agent_run_lock);
    /* HTTP event reads are RAM-only. A gap or prior run is reported instead
     * of synchronously scanning HIST.LOG/TASKS.LOG on the Web task. */
    bool persisted = false;
    history_lost = history_lost || (job_id[0] && !current_run);

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        free(events);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "event replay response alloc failed");
    }
    cJSON_AddNumberToObject(resp, "schema_version", SI_ACTION_EVENT_SCHEMA_VERSION);
    cJSON_AddStringToObject(resp, "job_id", job_id);
    cJSON_AddStringToObject(resp, "state",
                            current_run ? agent_run_state_name(state) :
                            "retained");
    cJSON_AddNumberToObject(resp, "after_seq", after_seq);
    cJSON_AddNumberToObject(resp, "latest_seq", available_latest_seq);
    cJSON_AddBoolToObject(resp, "history_lost", history_lost);
    cJSON_AddBoolToObject(resp, "persisted", persisted);
    cJSON_AddStringToObject(resp, "replay_scope", "current_run_ram_ring_no_tf_scan");
    cJSON *items = cJSON_AddArrayToObject(resp, "events");
    if (!items) {
        free(events);
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "event response alloc failed");
    }
    uint32_t next_after_seq = after_seq;
    bool truncated = false;
    for (size_t i = 0; i < count; i++) {
        if (!agent_run_add_event_json(items, &events[i])) {
            truncated = true;
            break;
        }
        next_after_seq = events[i].seq;
    }
    free(events);
    cJSON_AddNumberToObject(resp, "next_after_seq", next_after_seq);
    cJSON_AddBoolToObject(resp, "truncated", truncated);

    cJSON *task_items = cJSON_AddArrayToObject(resp, "task_events");
    if (!task_items) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "task event response alloc failed");
    }
    si_agent_task_service_snapshot_t *task_snapshot = NULL;
    esp_err_t task_ret = ESP_ERR_NOT_SUPPORTED;
#if !AGENT_RUNTIME_RECOVERY_RAM_ONLY
    task_snapshot = agent_alloc(sizeof(*task_snapshot));
    task_ret = task_snapshot ?
        si_agent_task_service_snapshot(task_snapshot) : ESP_ERR_NO_MEM;
#endif
    uint64_t task_latest_seq = task_ret == ESP_OK ?
        task_snapshot->latest_event_seq : 0U;
    si_agent_event_recovery_status_t task_recovery_status =
        task_ret == ESP_OK ? task_snapshot->recovery_status :
                             SI_AGENT_EVENT_RECOVERY_IO_ERROR;
    free(task_snapshot);
    cJSON_AddStringToObject(resp, "task_event_schema_version",
                            SI_AGENT_EVENT_SCHEMA);
    cJSON_AddNumberToObject(resp, "task_after_seq", (double)task_after_seq);
    cJSON_AddNumberToObject(resp, "task_latest_seq",
                            (double)task_latest_seq);
    cJSON_AddNumberToObject(resp, "task_next_after_seq",
                            (double)task_after_seq);
    cJSON_AddNumberToObject(resp, "task_event_count", 0);
    cJSON_AddBoolToObject(resp, "task_truncated",
                          task_after_seq < task_latest_seq);
    cJSON_AddBoolToObject(resp, "task_history_lost",
                          task_after_seq < task_latest_seq);
    cJSON_AddStringToObject(resp, "task_status",
                            esp_err_to_name(task_ret));
    cJSON_AddStringToObject(
        resp, "task_recovery_status",
        si_agent_event_recovery_status_name(task_recovery_status));
    cJSON_AddStringToObject(resp, "task_replay_scope",
                            "ram_cursor_only_no_tf_scan");
    esp_err_t ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static bool web_debug_diag_append_b64(char *out,
                                      size_t out_size,
                                      const char *trusted_prefix,
                                      const char *value,
                                      size_t max_bytes)
{
    if (!out || out_size == 0 || !trusted_prefix) {
        return false;
    }
    si_diagnostics_text_builder_t builder = {
        .out = out,
        .capacity = out_size,
        .length = strnlen(out, out_size),
    };
    if (builder.length >= out_size) {
        return false;
    }
    const char *text = value ? value : "";
    size_t length = strlen(text);
    if (max_bytes > 0 && length > max_bytes) {
        length = max_bytes;
    }
    return si_diagnostics_text_append_base64_line(
        &builder, trusted_prefix, text, length);
}

/* Caller holds s_agent_run_lock. Resume and steer are forward-progress
 * controls, so UART may issue them only for a Run admitted by this exact
 * bounded diagnostics source. Pause/abort remain cross-source fail-stop. */
static bool web_debug_agent_run_has_diagnostics_source_locked(void)
{
    const si_diagnostics_agent_source_identity_t observed = {
        .origin = s_agent_run_job.source_origin,
        .auth_kind = s_agent_run_job.source_auth_kind,
        .principal = s_agent_run_job.source_principal,
        .auth_id = s_agent_run_job.source_auth_id,
        .auth_generation = s_agent_run_job.source_auth_generation,
        .authority_ceiling = s_agent_run_job.source_authority_ceiling,
    };
    const si_diagnostics_agent_source_identity_t required = {
        .origin = SI_AGENT_TASK_ORIGIN_SYSTEM,
        .auth_kind = SI_AGENT_TASK_AUTH_SYSTEM,
        .principal = SI_PRINCIPAL_SYSTEM,
        .auth_id = "device-diagnostics",
        .auth_generation = 1U,
        .authority_ceiling =
            SI_CAPABILITY_OBSERVE | SI_CAPABILITY_AGENT_RUN,
    };
    return si_diagnostics_agent_source_identity_matches(
        &observed, &required);
}

static esp_err_t web_debug_agent_run_require_diagnostics_source(
    const char *run_id)
{
    esp_err_t start_ret = agent_run_start();
    if (start_ret != ESP_OK || !s_agent_run_lock) {
        return start_ret == ESP_OK ? ESP_ERR_INVALID_STATE : start_ret;
    }
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(300)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result =
        strcmp(run_id, s_agent_run_job.job_id) != 0 ? ESP_ERR_NOT_FOUND :
        web_debug_agent_run_has_diagnostics_source_locked() ? ESP_OK :
        ESP_ERR_NOT_ALLOWED;
    xSemaphoreGive(s_agent_run_lock);
    return result;
}

static void web_debug_agent_run_start(const char *session_id,
                                      const char *message,
                                      const char *profile,
                                      const char *model,
                                      bool dry_run,
                                      bool include_screenshot,
                                      bool allow_web_search,
                                      char *out,
                                      size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    bool accepted = false;
    bool busy = false;
    bool deduplicated = false;
    char job_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    char err[AGENT_RUN_ERROR_MAX_LEN + 1] = {0};
    si_agent_task_source_t diagnostics_source = {
        .origin = SI_AGENT_TASK_ORIGIN_SYSTEM,
        .source_account_id = "device",
        .source_actor_id = "diagnostics",
        .auth_kind = SI_AGENT_TASK_AUTH_SYSTEM,
        .principal = SI_PRINCIPAL_SYSTEM,
        .auth_id = "device-diagnostics",
        .auth_generation = 1U,
        .authority_ceiling =
            SI_CAPABILITY_OBSERVE | SI_CAPABILITY_AGENT_RUN,
        .history_policy = SI_AGENT_TASK_HISTORY_THREAD,
    };
    esp_err_t ret = agent_run_submit(session_id, &diagnostics_source,
                                     message, NULL, NULL, NULL,
                                     profile, model, dry_run,
                                     include_screenshot, allow_web_search,
                                     &accepted, &busy, &deduplicated,
                                     job_id, sizeof(job_id), err, sizeof(err));
    char submitted_run_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    char active_run_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    si_diagnostics_agent_submit_receipt_ids(
        accepted, busy, deduplicated, job_id,
        submitted_run_id, sizeof(submitted_run_id),
        active_run_id, sizeof(active_run_id));
    si_diagnostics_text_builder_t builder;
    si_diagnostics_text_builder_init(&builder, out, out_size);
    (void)si_diagnostics_text_appendf(
        &builder,
        "agent-run ret=%s accepted=%d busy=%d deduplicated=%d"
        " job=%s run_id=%s submitted_run_id=%s active_run_id=%s"
        " mode=%s screenshot=%s web_search=%s\n",
        esp_err_to_name(ret), accepted, busy, deduplicated,
        submitted_run_id, submitted_run_id, submitted_run_id, active_run_id,
        dry_run ? "dry-run" : "execute",
        include_screenshot ? "on" : "off",
        allow_web_search ? "on" : "off");
    (void)web_debug_diag_append_b64(
        out, out_size, "session_b64=",
        session_id && session_id[0] ? session_id : "default", 0);
    (void)web_debug_diag_append_b64(
        out, out_size, "profile_b64=",
        profile && profile[0] ? profile : "(active)", 0);
    (void)web_debug_diag_append_b64(
        out, out_size, "model_b64=",
        model && model[0] ? model : "(profile default)", 0);
    if (err[0]) {
        (void)web_debug_diag_append_b64(
            out, out_size, "error_b64=", err, AGENT_RUN_ERROR_MAX_LEN);
    }
}

static void web_debug_agent_run_status(const char *expected_run_id,
                                       char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (agent_run_start() != ESP_OK || !s_agent_run_lock) {
        diag_append(out, out_size, "agent state=unavailable\n");
        return;
    }

    agent_run_state_t state = AGENT_RUN_STATE_IDLE;
    bool cancel_requested = false;
    bool recovered_after_reboot = false;
    bool dry_run = true;
    bool include_screenshot = false;
    bool allow_web_search = false;
    uint32_t started_ms = 0;
    uint32_t updated_ms = 0;
    uint32_t finished_ms = 0;
    uint32_t event_seq = 0;
    char job_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    char session_id[AGENT_SESSION_ID_MAX_LEN + 1] = {0};
    char profile[AGENT_API_PROFILE_ID_MAX_LEN + 1] = {0};
    char model[AGENT_API_MODEL_MAX_LEN + 1] = {0};
    char stage[AGENT_RUN_STAGE_MAX_LEN + 1] = {0};
    char detail[AGENT_RUN_DETAIL_MAX_LEN + 1] = {0};
    char error[AGENT_RUN_ERROR_MAX_LEN + 1] = {0};
    char *result_json = NULL;

    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(300)) != pdTRUE) {
        diag_append(out, out_size, "agent state=unavailable lock=timeout\n");
        return;
    }
    if (expected_run_id && expected_run_id[0] &&
        strcmp(expected_run_id, s_agent_run_job.job_id) != 0) {
        diag_append(out, out_size,
                    "agent-status found=0 run_id=%s current=%s error=run_id_mismatch\n",
                    expected_run_id,
                    s_agent_run_job.job_id[0] ? s_agent_run_job.job_id : "-");
        xSemaphoreGive(s_agent_run_lock);
        return;
    }
    state = s_agent_run_job.state;
    cancel_requested = s_agent_run_job.cancel_requested;
    recovered_after_reboot = s_agent_run_job.recovered_after_reboot;
    dry_run = s_agent_run_job.dry_run;
    include_screenshot = s_agent_run_job.include_screenshot;
    allow_web_search = s_agent_run_job.allow_web_search;
    started_ms = s_agent_run_job.started_ms;
    updated_ms = s_agent_run_job.updated_ms;
    finished_ms = s_agent_run_job.finished_ms;
    event_seq = s_agent_run_job.action_events.latest_seq;
    strlcpy(job_id, s_agent_run_job.job_id, sizeof(job_id));
    strlcpy(session_id, s_agent_run_job.session_id, sizeof(session_id));
    strlcpy(profile, s_agent_run_job.profile, sizeof(profile));
    strlcpy(model, s_agent_run_job.model, sizeof(model));
    strlcpy(stage, s_agent_run_job.stage, sizeof(stage));
    strlcpy(detail, s_agent_run_job.detail, sizeof(detail));
    strlcpy(error, s_agent_run_job.error, sizeof(error));
    if (s_agent_run_job.result_json) {
        result_json = strdup(s_agent_run_job.result_json);
    }
    xSemaphoreGive(s_agent_run_lock);

    uint32_t now_ms = si_monotonic_ms();
    uint32_t elapsed_ms = started_ms ? (finished_ms ? finished_ms : now_ms) - started_ms : 0;
    diag_append(out, out_size,
                "agent state=%s running=%d cancel=%d recovered=%d job=%s run_id=%s\n",
                agent_run_state_name(state),
                state == AGENT_RUN_STATE_RUNNING ||
                state == AGENT_RUN_STATE_WAITING_REQUEST ||
                state == AGENT_RUN_STATE_PAUSED,
                cancel_requested, recovered_after_reboot,
                job_id[0] ? job_id : "-",
                job_id[0] ? job_id : "-");
    diag_append(out, out_size,
                "mode=%s screenshot=%s web_search=%s elapsed=%" PRIu32
                "ms event_seq=%" PRIu32 "\n",
                dry_run ? "dry-run" : "execute",
                include_screenshot ? "on" : "off",
                allow_web_search ? "on" : "off",
                elapsed_ms, event_seq);
    (void)web_debug_diag_append_b64(
        out, out_size, "session_b64=", session_id, sizeof(session_id) - 1U);
    (void)web_debug_diag_append_b64(
        out, out_size, "profile_b64=", profile, sizeof(profile) - 1U);
    (void)web_debug_diag_append_b64(
        out, out_size, "model_b64=", model, sizeof(model) - 1U);
    (void)web_debug_diag_append_b64(
        out, out_size, "stage_b64=", stage, sizeof(stage) - 1U);
    (void)web_debug_diag_append_b64(
        out, out_size, "detail_b64=", detail, sizeof(detail) - 1U);
    if (updated_ms) {
        diag_append(out, out_size, "updated_ago=%" PRIu32 "ms\n",
                    (uint32_t)(now_ms - updated_ms));
    }
    if (error[0]) {
        (void)web_debug_diag_append_b64(
            out, out_size, "error_b64=", error, sizeof(error) - 1U);
    }

    if (result_json) {
        cJSON *root = cJSON_Parse(result_json);
        if (root) {
            bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"));
            const cJSON *tool_executed_item =
                cJSON_GetObjectItemCaseSensitive(root, "tool_executed");
            const cJSON *hid_executed_item =
                cJSON_GetObjectItemCaseSensitive(root, "hid_executed");
            const cJSON *cloud_status_item =
                cJSON_GetObjectItemCaseSensitive(root, "cloud_status");
            int tool_executed = cJSON_IsNumber(tool_executed_item) ?
                                tool_executed_item->valueint : 0;
            int hid_executed = cJSON_IsNumber(hid_executed_item) ?
                               hid_executed_item->valueint : 0;
            int cloud_status = cJSON_IsNumber(cloud_status_item) ?
                               cloud_status_item->valueint : 0;
            diag_append(out, out_size,
                        "result ok=%d cloud=%d tools=%d hid=%d\n",
                        ok, cloud_status, tool_executed, hid_executed);
            cJSON *report = cJSON_GetObjectItemCaseSensitive(root, "final_report");
            const char *message = NULL;
            if (cJSON_IsObject(report)) {
                message = json_string_any(report, "message", "summary", "reply");
            }
            if (!message || !message[0]) {
                message = json_string_any(root, "message", "summary", "reply");
            }
            if (message && message[0]) {
                (void)web_debug_diag_append_b64(
                    out, out_size, "assistant_b64=", message, 700U);
            }
            const char *result_error = json_string_any(root, "error", "reason", NULL);
            if (result_error && result_error[0]) {
                (void)web_debug_diag_append_b64(
                    out, out_size, "result_error_b64=", result_error, 700U);
            }
            cJSON_Delete(root);
        } else {
            diag_append(out, out_size, "result_json=parse_failed\n");
        }
        free(result_json);
    }
}

static void web_debug_agent_run_events(const char *expected_run_id,
                                       uint32_t since_seq,
                                       uint32_t *latest_seq,
                                       bool *running,
                                       char *out,
                                       size_t out_size)
{
    if (latest_seq) {
        *latest_seq = since_seq;
    }
    if (running) {
        *running = false;
    }
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    si_action_event_t *events =
        agent_alloc(sizeof(*events) * SI_ACTION_EVENT_CAPACITY);
    if (!events) {
        diag_append(out, out_size,
                    "agent-events error=no_memory\n"
                    "agent-events-trailer run_id=%s next_after_seq=%" PRIu32
                    " available_latest_seq=%" PRIu32
                    " truncated=0 history_lost=0 oldest_seq=0 dropped=0 running=0\n",
                    expected_run_id && expected_run_id[0] ?
                        expected_run_id : "-",
                    since_seq, since_seq);
        return;
    }
    memset(events, 0,
           sizeof(*events) * SI_ACTION_EVENT_CAPACITY);
    if (agent_run_start() != ESP_OK || !s_agent_run_lock) {
        diag_append(out, out_size,
                    "agent-events error=runtime_unavailable\n"
                    "agent-events-trailer run_id=%s next_after_seq=%" PRIu32
                    " available_latest_seq=%" PRIu32
                    " truncated=0 history_lost=0 oldest_seq=0 dropped=0 running=0\n",
                    expected_run_id && expected_run_id[0] ?
                        expected_run_id : "-",
                    since_seq, since_seq);
        free(events);
        return;
    }
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(300)) != pdTRUE) {
        diag_append(out, out_size,
                    "agent-events error=lock_timeout\n"
                    "agent-events-trailer run_id=%s next_after_seq=%" PRIu32
                    " available_latest_seq=%" PRIu32
                    " truncated=0 history_lost=0 oldest_seq=0 dropped=0 running=0\n",
                    expected_run_id && expected_run_id[0] ?
                        expected_run_id : "-",
                    since_seq, since_seq);
        free(events);
        return;
    }
    if (expected_run_id && expected_run_id[0] &&
        strcmp(expected_run_id, s_agent_run_job.job_id) != 0) {
        diag_append(out, out_size,
                    "agent-events error=run_id_mismatch run_id=%s\n"
                    "agent-events-trailer run_id=%s next_after_seq=%" PRIu32
                    " available_latest_seq=%" PRIu32
                    " truncated=0 history_lost=0 oldest_seq=0 dropped=0 running=0\n",
                    expected_run_id, expected_run_id,
                    since_seq, since_seq);
        xSemaphoreGive(s_agent_run_lock);
        free(events);
        return;
    }
    char run_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    strlcpy(run_id, s_agent_run_job.job_id, sizeof(run_id));
    bool active = s_agent_run_job.state == AGENT_RUN_STATE_RUNNING ||
                  s_agent_run_job.state == AGENT_RUN_STATE_WAITING_REQUEST ||
                  s_agent_run_job.state == AGENT_RUN_STATE_PAUSED;
    uint32_t available_latest_seq =
        s_agent_run_job.action_events.latest_seq;
    uint32_t dropped = s_agent_run_job.action_events.dropped;
    uint32_t count = s_agent_run_job.action_events.count;
    uint32_t start =
        (s_agent_run_job.action_events.write_pos +
         SI_ACTION_EVENT_CAPACITY - count) % SI_ACTION_EVENT_CAPACITY;
    uint32_t oldest_seq = count > 0 ?
        s_agent_run_job.action_events.events[start].seq : 0U;
    bool history_lost = count > 0 && oldest_seq > 1U &&
                        since_seq < oldest_seq - 1U;
    uint32_t ignored_latest = since_seq;
    bool ignored_history_lost = false;
    size_t event_count = si_action_event_buffer_read_after(
        &s_agent_run_job.action_events, since_seq, events,
        SI_ACTION_EVENT_CAPACITY, &ignored_latest,
        &ignored_history_lost);
    xSemaphoreGive(s_agent_run_lock);

    if (!si_diagnostics_run_id_valid(run_id)) {
        diag_append(out, out_size,
                    "agent-events error=no_run\n"
                    "agent-events-trailer run_id=- next_after_seq=%" PRIu32
                    " available_latest_seq=%" PRIu32
                    " truncated=0 history_lost=0 oldest_seq=0 dropped=0 running=0\n",
                    since_seq, since_seq);
        free(events);
        return;
    }
    si_diagnostics_agent_event_page_t page = {
        .run_id = run_id,
        .after_seq = since_seq,
        .available_latest_seq = available_latest_seq,
        .oldest_seq = oldest_seq,
        .dropped = dropped,
        .running = active,
        .history_lost = history_lost || ignored_history_lost,
        .events = events,
        .event_count = event_count,
    };
    uint32_t next_after_seq = since_seq;
    bool truncated = false;
    bool formatted = si_diagnostics_format_agent_event_page(
        &page, out, out_size, &next_after_seq, &truncated);
    free(events);
    (void)truncated;
    if (formatted) {
        if (latest_seq) {
            *latest_seq = next_after_seq;
        }
        if (running) {
            *running = active;
        }
    }
}

static void web_debug_agent_run_control(agent_run_control_action_t action,
                                        const char *action_name,
                                        const char *run_id,
                                        char *out,
                                        size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!run_id || !run_id[0]) {
        diag_append(out, out_size, "exact run_id required\n");
        return;
    }
    char controlled_job[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    bool shadow_degraded = false;
    esp_err_t ret = action == AGENT_RUN_CONTROL_RESUME ?
        web_debug_agent_run_require_diagnostics_source(run_id) : ESP_OK;
    if (ret == ESP_OK) {
        ret = agent_run_control_exact(
            action, run_id, controlled_job, sizeof(controlled_job),
            &shadow_degraded);
    }
    diag_append(out, out_size,
                "agent-%s accepted=%d run_id=%s controlled=%s shadow=%s result=%s%s\n",
                action_name,
                ret == ESP_OK,
                run_id,
                controlled_job[0] ? controlled_job : "-",
                shadow_degraded ? "degraded" : "stable",
                esp_err_to_name(ret),
                ret == ESP_ERR_NOT_FOUND ? " reason=run_id_mismatch" :
                ret == ESP_ERR_NOT_ALLOWED ? " reason=cross_source_denied" : "");
}

static void web_debug_agent_run_pause(const char *run_id,
                                      char *out, size_t out_size)
{
    web_debug_agent_run_control(AGENT_RUN_CONTROL_PAUSE, "pause", run_id,
                                out, out_size);
}

static void web_debug_agent_run_resume(const char *run_id,
                                       char *out, size_t out_size)
{
    web_debug_agent_run_control(AGENT_RUN_CONTROL_RESUME, "resume", run_id,
                                out, out_size);
}

static void web_debug_agent_run_cancel(const char *run_id,
                                       char *out, size_t out_size)
{
    web_debug_agent_run_control(AGENT_RUN_CONTROL_ABORT, "cancel", run_id,
                                out, out_size);
}

static void web_debug_agent_run_abort(const char *run_id,
                                      char *out, size_t out_size)
{
    web_debug_agent_run_control(AGENT_RUN_CONTROL_ABORT, "abort", run_id,
                                out, out_size);
}

static void web_debug_agent_run_steer(const char *expected_run_id,
                                      const char *message,
                                      char *out,
                                      size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!expected_run_id || !expected_run_id[0]) {
        diag_append(out, out_size, "exact run_id required\n");
        return;
    }
    if (!message || !message[0] ||
        strlen(message) > AGENT_RUN_STEER_MAX_LEN) {
        diag_append(out, out_size,
                    "agent-steer accepted=0 run_id=%s result=%s error=message_must_be_1_to_%d_bytes\n",
                    expected_run_id, esp_err_to_name(ESP_ERR_INVALID_ARG),
                    AGENT_RUN_STEER_MAX_LEN);
        return;
    }

    bool active = false;
    bool run_matches = false;
    bool source_matches = false;
    bool shadow_bound = false;
    bool shadow_degraded = false;
    uint32_t revision = 0;
    esp_err_t steer_ret = agent_run_start();
    if (steer_ret == ESP_OK && s_agent_run_lock &&
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(300)) == pdTRUE) {
        active = agent_run_control_state_active(s_agent_run_job.state);
        run_matches = active &&
            strcmp(expected_run_id, s_agent_run_job.job_id) == 0;
        if (run_matches) {
            source_matches =
                web_debug_agent_run_has_diagnostics_source_locked();
            shadow_bound = s_agent_run_job.task_service_bound &&
                           !s_agent_run_job.task_service_control_pending;
        }
        xSemaphoreGive(s_agent_run_lock);
        steer_ret = !run_matches ?
                    (active ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE) :
                    source_matches ? ESP_OK : ESP_ERR_NOT_ALLOWED;
    } else if (steer_ret == ESP_OK) {
        steer_ret = ESP_ERR_TIMEOUT;
    }

    si_agent_task_service_snapshot_t *task_snapshot = NULL;
    if (steer_ret == ESP_OK) {
        /* Match the HTTP steer durability boundary without granting the
         * diagnostics SYSTEM principal any approval authority. */
        steer_ret = shadow_bound ?
            si_agent_task_service_steer(expected_run_id, message) : ESP_OK;
        if (steer_ret == ESP_OK && shadow_bound) {
            task_snapshot = agent_alloc(sizeof(*task_snapshot));
            if (task_snapshot) {
                memset(task_snapshot, 0, sizeof(*task_snapshot));
                if (si_agent_task_service_snapshot(task_snapshot) != ESP_OK) {
                    free(task_snapshot);
                    task_snapshot = NULL;
                }
            }
        }
        if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(300)) == pdTRUE) {
            bool same_active =
                strcmp(s_agent_run_job.job_id, expected_run_id) == 0 &&
                agent_run_control_state_active(s_agent_run_job.state) &&
                web_debug_agent_run_has_diagnostics_source_locked();
            if (same_active && steer_ret != ESP_OK &&
                s_agent_run_job.task_service_bound) {
                s_agent_run_job.task_service_bound = false;
                snprintf(s_agent_run_job.task_service_error,
                         sizeof(s_agent_run_job.task_service_error),
                         "steer storage_failure; shadow demoted: %s",
                         esp_err_to_name(steer_ret));
                shadow_bound = false;
                shadow_degraded = true;
                steer_ret = ESP_OK;
            }
            if (same_active && steer_ret == ESP_OK) {
                if (task_snapshot &&
                    task_snapshot->runtime.has_active_turn &&
                    strcmp(task_snapshot->runtime.active_turn.run_attempt
                               .run_attempt_id, expected_run_id) == 0) {
                    revision = task_snapshot->runtime.active_turn
                                   .intent_revision;
                    s_agent_run_job.intent_revision = revision;
                    s_agent_run_job.plan_version =
                        task_snapshot->runtime.active_turn.plan_version;
                } else {
                    revision = ++s_agent_run_job.intent_revision;
                    s_agent_run_job.plan_version++;
                }
                strlcpy(s_agent_run_job.pending_steer, message,
                        sizeof(s_agent_run_job.pending_steer));
                s_agent_run_job.updated_ms = si_monotonic_ms();
                strlcpy(
                    s_agent_run_job.detail,
                    shadow_bound ?
                    "已持久记录 UART 补充要求；将在下一个安全检查点更新计划" :
                    "Runtime V2 degraded；UART 补充要求将在本地安全检查点生效",
                    sizeof(s_agent_run_job.detail));
            } else if (steer_ret == ESP_OK) {
                steer_ret = ESP_ERR_INVALID_STATE;
            }
            xSemaphoreGive(s_agent_run_lock);
        } else {
            steer_ret = ESP_ERR_TIMEOUT;
        }
    }
    free(task_snapshot);

    if (steer_ret == ESP_OK) {
        agent_run_add_action_eventf(
            expected_run_id, NULL, SI_ACTION_CHANNEL_AGENT,
            SI_ACTION_PHASE_OUTPUT_RECEIVED, false, true,
            "UART steer accepted · intent revision=%" PRIu32, revision);
    }
    diag_append(out, out_size,
                "agent-steer accepted=%d run_id=%s revision=%" PRIu32
                " shadow=%s result=%s%s\n",
                steer_ret == ESP_OK, expected_run_id, revision,
                shadow_degraded ? "degraded" : "stable",
                esp_err_to_name(steer_ret),
                steer_ret == ESP_ERR_NOT_FOUND ?
                    " reason=run_id_mismatch" :
                steer_ret == ESP_ERR_NOT_ALLOWED ?
                    " reason=cross_source_denied" : "");
}

static void web_debug_agent_run_result(const char *expected_run_id,
                                       size_t offset,
                                       size_t max_bytes,
                                       char *out,
                                       size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!expected_run_id || !expected_run_id[0]) {
        diag_append(out, out_size,
                    "agent-result ready=0 ok=0 error=exact_run_id_required\n");
        return;
    }
    size_t requested = max_bytes > 0 ? max_bytes :
        SI_DIAGNOSTICS_RESULT_DEFAULT_CHUNK;
    if (requested > SI_DIAGNOSTICS_RESULT_MAX_CHUNK) {
        requested = SI_DIAGNOSTICS_RESULT_MAX_CHUNK;
    }
    if (out_size <= SI_DIAGNOSTICS_RESULT_FRAMING_RESERVE) {
        diag_append(out, out_size,
                    "agent-result ready=0 ok=0 run_id=%s error=output_too_small\n",
                    expected_run_id);
        return;
    }
    size_t fit_raw =
        ((out_size - SI_DIAGNOSTICS_RESULT_FRAMING_RESERVE) / 4U) * 3U;
    if (requested > fit_raw) {
        requested = fit_raw;
    }
    if (requested == 0) {
        diag_append(out, out_size,
                    "agent-result ready=0 ok=0 run_id=%s error=output_too_small\n",
                    expected_run_id);
        return;
    }
    /* Allocate before taking the Run lock. The lock only protects identity,
     * length validation, and one bounded memcpy. */
    unsigned char *chunk = agent_alloc(requested);
    if (!chunk) {
        diag_append(out, out_size,
                    "agent-result ready=0 ok=0 run_id=%s error=no_memory\n",
                    expected_run_id);
        return;
    }
    if (agent_run_start() != ESP_OK || !s_agent_run_lock) {
        diag_append(out, out_size,
                    "agent-result ready=0 ok=0 run_id=%s error=runtime_unavailable\n",
                    expected_run_id);
        free(chunk);
        return;
    }
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(300)) != pdTRUE) {
        diag_append(out, out_size,
                    "agent-result ready=0 ok=0 run_id=%s error=lock_timeout\n",
                    expected_run_id);
        free(chunk);
        return;
    }
    if (strcmp(expected_run_id, s_agent_run_job.job_id) != 0) {
        diag_append(out, out_size,
                    "agent-result ready=0 ok=0 run_id=%s current=%s error=run_id_mismatch\n",
                    expected_run_id,
                    s_agent_run_job.job_id[0] ? s_agent_run_job.job_id : "-");
        xSemaphoreGive(s_agent_run_lock);
        free(chunk);
        return;
    }
    if (!s_agent_run_job.result_json) {
        diag_append(out, out_size,
                    "agent-result ready=0 ok=0 run_id=%s state=%s error=result_not_ready\n",
                    expected_run_id,
                    agent_run_state_name(s_agent_run_job.state));
        xSemaphoreGive(s_agent_run_lock);
        free(chunk);
        return;
    }

    size_t total = strlen(s_agent_run_job.result_json);
    si_diagnostics_result_chunk_plan_t plan = {0};
    if (!si_diagnostics_result_chunk_plan(
            total, offset, requested, out_size, &plan)) {
        diag_append(out, out_size,
                    "agent-result ready=0 ok=0 run_id=%s offset=%zu total=%zu error=invalid_chunk_range\n",
                    expected_run_id, offset, total);
        xSemaphoreGive(s_agent_run_lock);
        free(chunk);
        return;
    }
    if (plan.chunk_bytes > 0) {
        memcpy(chunk, s_agent_run_job.result_json + plan.offset,
               plan.chunk_bytes);
    }
    xSemaphoreGive(s_agent_run_lock);

    uint32_t chunk_crc32 =
        si_diagnostics_crc32(chunk, plan.chunk_bytes);
    si_diagnostics_text_builder_t builder;
    si_diagnostics_text_builder_init(&builder, out, out_size);
    bool header_ok = si_diagnostics_text_appendf(
        &builder,
        "agent-result ready=1 ok=1 run_id=%s offset=%zu bytes=%zu total=%zu"
        " next=%zu eof=%d encoding=base64 crc32=%08" PRIx32 "\n",
        expected_run_id, plan.offset, plan.chunk_bytes, total,
        plan.next_offset, plan.eof, chunk_crc32);
    bool data_ok = header_ok && si_diagnostics_text_append_base64_line(
        &builder, "data=", chunk, plan.chunk_bytes);
    free(chunk);
    if (!data_ok) {
        out[0] = '\0';
        diag_append(out, out_size,
                    "agent-result ready=0 ok=0 run_id=%s offset=%zu total=%zu error=format_failed\n",
                    expected_run_id, offset, total);
    }
}
