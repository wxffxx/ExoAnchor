// Transitional Agent module: models, history and memory HTTP. Shares the run context until its service API is finalized.
static esp_err_t agent_data_require_browser(httpd_req_t *req)
{
    si_auth_session_context_t session;
    esp_err_t ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, &session);
    if (ret != ESP_OK) {
        return ret;
    }
    if (session.principal != SI_PRINCIPAL_BROWSER) {
        return httpd_resp_send_err(
            req, HTTPD_403_FORBIDDEN,
            "Agent data deletion is available only to an authenticated browser");
    }
    return ESP_OK;
}

static bool agent_run_state_is_active(agent_run_state_t state)
{
    return state == AGENT_RUN_STATE_RUNNING ||
           state == AGENT_RUN_STATE_WAITING_REQUEST ||
           state == AGENT_RUN_STATE_PAUSED;
}

static esp_err_t agent_data_clear_all(si_agent_data_clear_result_t *result)
{
    if (result) {
        memset(result, 0, sizeof(*result));
    }
    if (!s_agent_run_lock ||
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (agent_run_state_is_active(s_agent_run_job.state)) {
        xSemaphoreGive(s_agent_run_lock);
        return ESP_ERR_INVALID_STATE;
    }

    si_agent_data_clear_result_t cleared = {0};
    esp_err_t ret = si_agent_repository_clear_all(&cleared);
    if (ret == ESP_OK) {
        char old_job_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
        strlcpy(old_job_id, s_agent_run_job.job_id, sizeof(old_job_id));
        free(s_agent_run_job.result_json);
        memset(&s_agent_run_job, 0, sizeof(s_agent_run_job));
        si_action_event_buffer_init(&s_agent_run_job.action_events);
        if (old_job_id[0]) {
            si_agent_request_broker_cancel_run(old_job_id);
        }
    }
    xSemaphoreGive(s_agent_run_lock);
    if (result) {
        *result = cleared;
    }
    return ret;
}

static esp_err_t agent_data_clear_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = agent_data_require_browser(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    return si_http_send_text_status(
        req, "503 Service Unavailable",
        "Agent persistence mutation disabled in recovery candidate");
#endif
    char buf[SCRATCH_BUFSIZE] = {0};
    cJSON *root = NULL;
    esp_err_t json_ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (json_ret != ESP_OK) {
        return json_ret;
    }
    const char *confirmation =
        json_string_any(root, "confirm", "confirmation", NULL);
    bool confirmed = confirmation &&
                     strcmp(confirmation, AGENT_DATA_CLEAR_CONFIRMATION) == 0;
    cJSON_Delete(root);
    if (!confirmed) {
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "confirm must be DELETE_ALL_AGENT_DATA");
    }

    si_agent_data_clear_result_t cleared = {0};
    esp_err_t ret = agent_data_clear_all(&cleared);
    if (ret == ESP_ERR_INVALID_STATE) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "stop the active Agent task before deleting conversations and memory");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ret == ESP_OK);
    cJSON_AddBoolToObject(resp, "history_cleared",
                          cleared.history_cleared);
    cJSON_AddBoolToObject(resp, "memory_cleared",
                          cleared.memory_cleared);
    cJSON_AddBoolToObject(resp, "run_checkpoint_cleared",
                          cleared.run_checkpoint_cleared);
    cJSON_AddNumberToObject(resp, "session_count", 0);
    cJSON_AddNumberToObject(resp, "conversation_record_count", 0);
    cJSON_AddNumberToObject(resp, "memory_record_count", 0);
    cJSON_AddBoolToObject(resp, "mcp_exposed", false);
    if (ret != ESP_OK) {
        cJSON_AddStringToObject(
            resp, "status", si_agent_repository_last_error()[0] ?
            si_agent_repository_last_error() : esp_err_to_name(ret));
    } else {
        si_web_log("WARN",
                   "All Agent conversations and persistent memory cleared");
    }
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}

static esp_err_t agent_api_models_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (req->method != HTTP_GET) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }

    char query[128] = {0};
    char profile_id[AGENT_API_PROFILE_ID_MAX_LEN + 1] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "profile", profile_id, sizeof(profile_id));
    }

    agent_api_profile_t profile;
    esp_err_t ret = agent_api_get_profile(profile_id[0] ? profile_id : NULL, &profile);
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid agent api profile");
    }
    if (!profile.endpoint[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "agent api endpoint not configured");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "profile", profile.id);
    cJSON_AddStringToObject(resp, "provider", profile.provider);
    cJSON_AddStringToObject(resp, "endpoint", profile.endpoint);
    cJSON_AddStringToObject(resp, "selected_model", profile.model);
    cJSON_AddStringToObject(resp, "source", "configured");
    cJSON *models = cJSON_AddArrayToObject(resp, "models");
    if (profile.model[0]) {
        cJSON_AddItemToArray(models, cJSON_CreateString(profile.model));
    }
    (void)agent_add_builtin_models(models, profile.provider);

    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t agent_history_status_json(cJSON *resp, size_t used_bytes,
                                           uint32_t record_count, bool formatted,
                                           esp_err_t storage_status)
{
    si_storage_tf_status_t tf_status;
    si_storage_get_tf_status(&tf_status);
    bool supported = storage_status == ESP_OK && tf_status.mounted;
    cJSON_AddBoolToObject(resp, "supported", supported);
    cJSON_AddBoolToObject(resp, "formatted", formatted);
    cJSON_AddStringToObject(resp, "storage", "tf_card");
    cJSON_AddStringToObject(resp, "partition", "tf_card");
    cJSON_AddStringToObject(resp, "path", AGENT_HISTORY_TF_FILE);
    cJSON_AddBoolToObject(resp, "tf_mounted", tf_status.mounted);
    cJSON_AddNumberToObject(resp, "capacity_bytes", tf_status.total_bytes);
    cJSON_AddNumberToObject(resp, "free_bytes", tf_status.free_bytes);
    cJSON_AddNumberToObject(resp, "used_bytes", used_bytes);
    cJSON_AddNumberToObject(resp, "record_count", record_count);
    cJSON_AddNumberToObject(resp, "max_record_bytes", AGENT_HISTORY_MAX_RECORD_BYTES);
    cJSON_AddNumberToObject(resp, "return_max", AGENT_HISTORY_RETURN_MAX);
    if (storage_status != ESP_OK) {
        cJSON_AddStringToObject(resp, "error", s_agent_history_last_error[0] ?
                                s_agent_history_last_error :
                                (tf_status.last_error[0] ? tf_status.last_error :
                                 esp_err_to_name(storage_status)));
    }
    return ESP_OK;
}

static esp_err_t agent_history_get_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddArrayToObject(resp, "records");
    cJSON_AddBoolToObject(resp, "supported", false);
    cJSON_AddStringToObject(resp, "storage", "ram_only_recovery_candidate");
    cJSON_AddStringToObject(resp, "status", "automatic_tf_reads_disabled");
    cJSON_AddNumberToObject(resp, "record_count", 0);
    cJSON_AddNumberToObject(resp, "returned", 0);
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
#else
    char query[128] = {0};
    char requested_session[AGENT_SESSION_ID_MAX_LEN + 1] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "session_id", requested_session,
                                    sizeof(requested_session));
        if (requested_session[0] == '\0') {
            (void)httpd_query_key_value(query, "session", requested_session,
                                        sizeof(requested_session));
        }
    }
    char session_id[AGENT_SESSION_ID_MAX_LEN + 1];
    agent_session_normalize(requested_session, session_id, sizeof(session_id));

    cJSON *resp = cJSON_CreateObject();
    cJSON *records = cJSON_AddArrayToObject(resp, "records");

    if (!s_agent_history_lock ||
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "agent history lock unavailable");
    }

    bool formatted = false;
    size_t used_bytes = 0;
    uint32_t total_record_count = 0;
    uint32_t record_count = 0;
    uint32_t returned = 0;
    esp_err_t ret = agent_history_file_ensure_ready(&formatted);
    if (ret == ESP_OK) {
        ret = agent_history_file_stats_locked(&used_bytes, &total_record_count);
        if (ret == ESP_OK) {
            ret = agent_history_file_count_visible_locked(session_id, &record_count);
        }
    }
    if (ret == ESP_OK) {
        uint32_t skip = record_count > AGENT_HISTORY_RETURN_MAX ?
                        record_count - AGENT_HISTORY_RETURN_MAX : 0;
        ret = agent_history_file_add_records_json_locked(records, skip,
                                                         session_id, &returned);
    }
    xSemaphoreGive(s_agent_history_lock);

    (void)agent_history_status_json(resp, used_bytes, record_count, formatted, ret);
    cJSON_AddStringToObject(resp, "session_id", session_id);
    cJSON_AddNumberToObject(resp, "total_record_count", total_record_count);
    cJSON_AddNumberToObject(resp, "returned", returned);
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
#endif
}

static esp_err_t agent_history_post_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    return si_http_send_text_status(
        req, "503 Service Unavailable",
        "history persistence disabled in recovery candidate");
#endif
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char *buf = calloc(1, AGENT_HISTORY_MAX_BODY);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, AGENT_HISTORY_MAX_BODY, &root);
    free(buf);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "history record must be object");
    }

    cJSON *entry = cJSON_Duplicate(root, true);
    cJSON_Delete(root);
    if (!entry) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "history record alloc failed");
    }
    if (!cJSON_GetObjectItemCaseSensitive(entry, "kind")) {
        cJSON_AddStringToObject(entry, "kind", "message");
    }
    cJSON *session_item = cJSON_GetObjectItemCaseSensitive(entry, "session_id");
    char session_id[AGENT_SESSION_ID_MAX_LEN + 1];
    if (cJSON_IsString(session_item) && session_item->valuestring &&
        !agent_session_id_valid(session_item->valuestring)) {
        cJSON_Delete(entry);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid session_id");
    }
    agent_session_normalize(cJSON_IsString(session_item) ? session_item->valuestring : NULL,
                            session_id, sizeof(session_id));
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "session_id");
    cJSON_AddStringToObject(entry, "session_id", session_id);
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "stored_ms");
    cJSON_AddNumberToObject(entry, "stored_ms", si_monotonic_ms());
    char *payload = cJSON_PrintUnformatted(entry);
    cJSON_Delete(entry);
    if (!payload) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "history payload alloc failed");
    }
    size_t payload_len = strlen(payload);
    if (payload_len == 0 || payload_len > AGENT_HISTORY_MAX_RECORD_BYTES) {
        free(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "history record too large");
    }

    if (!s_agent_history_lock ||
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        free(payload);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "agent history lock unavailable");
    }

    bool formatted = false;
    size_t used_bytes = 0;
    uint32_t record_count = 0;
    ret = agent_history_file_ensure_ready(&formatted);
    if (ret == ESP_OK) {
        ret = agent_history_file_append_locked(payload, payload_len,
                                               &used_bytes, &record_count);
    }
    xSemaphoreGive(s_agent_history_lock);
    free(payload);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ret == ESP_OK);
    (void)agent_history_status_json(resp, used_bytes, record_count, formatted, ret);
    if (ret != ESP_OK) {
        cJSON_AddStringToObject(resp, "status", esp_err_to_name(ret));
    }
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}

static esp_err_t agent_history_clear_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = agent_data_require_browser(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    return si_http_send_text_status(
        req, "503 Service Unavailable",
        "history persistence disabled in recovery candidate");
#endif

    if (automation_agent_run_active()) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "stop the active Agent task before clearing history");
    }

    bool cleared = false;
    esp_err_t ret = si_agent_repository_clear_history(&cleared);
    if (ret == ESP_OK) {
        si_web_log("INFO", "Agent history cleared");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ret == ESP_OK);
    (void)agent_history_status_json(resp, 0, 0, cleared, ret);
    if (ret != ESP_OK) {
        cJSON_AddStringToObject(resp, "status", esp_err_to_name(ret));
    }
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}

static esp_err_t agent_memory_status_json(cJSON *resp, size_t used_bytes,
                                          uint32_t record_count, bool formatted,
                                          esp_err_t storage_status)
{
    si_storage_tf_status_t tf_status;
    si_storage_get_tf_status(&tf_status);
    bool supported = storage_status == ESP_OK && tf_status.mounted;
    cJSON_AddBoolToObject(resp, "supported", supported);
    cJSON_AddBoolToObject(resp, "formatted", formatted);
    cJSON_AddStringToObject(resp, "storage", "tf_card");
    cJSON_AddStringToObject(resp, "partition", "tf_card");
    cJSON_AddStringToObject(resp, "path", AGENT_MEMORY_TF_FILE);
    cJSON_AddBoolToObject(resp, "tf_mounted", tf_status.mounted);
    cJSON_AddNumberToObject(resp, "capacity_bytes", tf_status.total_bytes);
    cJSON_AddNumberToObject(resp, "free_bytes", tf_status.free_bytes);
    cJSON_AddNumberToObject(resp, "used_bytes", used_bytes);
    cJSON_AddNumberToObject(resp, "record_count", record_count);
    cJSON_AddNumberToObject(resp, "return_max", AGENT_MEMORY_RETURN_MAX);
    cJSON_AddNumberToObject(resp, "context_max", AGENT_MEMORY_CONTEXT_MAX);
    cJSON *types = cJSON_AddArrayToObject(resp, "types");
    if (types) {
        const char *allowed[] = {
            "fact", "host_state", "service_deployment", "user_decision",
            "todo", "task_summary", "manual_note"
        };
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
            cJSON_AddItemToArray(types, cJSON_CreateString(allowed[i]));
        }
    }
    if (storage_status != ESP_OK) {
        cJSON_AddStringToObject(resp, "error", s_agent_history_last_error[0] ?
                                s_agent_history_last_error :
                                (tf_status.last_error[0] ? tf_status.last_error :
                                 esp_err_to_name(storage_status)));
    }
    return ESP_OK;
}

static esp_err_t agent_memory_get_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddArrayToObject(resp, "records");
    cJSON_AddBoolToObject(resp, "supported", false);
    cJSON_AddStringToObject(resp, "storage", "ram_only_recovery_candidate");
    cJSON_AddStringToObject(resp, "status", "automatic_tf_reads_disabled");
    cJSON_AddNumberToObject(resp, "record_count", 0);
    cJSON_AddNumberToObject(resp, "returned", 0);
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
#else
    cJSON *resp = cJSON_CreateObject();
    cJSON *records = cJSON_AddArrayToObject(resp, "records");

    if (!s_agent_history_lock ||
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "agent memory lock unavailable");
    }

    bool formatted = false;
    size_t used_bytes = 0;
    uint32_t record_count = 0;
    uint32_t returned = 0;
    esp_err_t ret = agent_memory_file_ensure_ready(&formatted);
    if (ret == ESP_OK) {
        ret = agent_memory_file_stats_locked(&used_bytes, &record_count);
    }
    if (ret == ESP_OK) {
        uint32_t skip = record_count > AGENT_MEMORY_RETURN_MAX ?
                        record_count - AGENT_MEMORY_RETURN_MAX : 0;
        ret = agent_memory_file_add_records_json_locked(records, skip, &returned);
    }
    xSemaphoreGive(s_agent_history_lock);

    (void)agent_memory_status_json(resp, used_bytes, record_count, formatted, ret);
    cJSON_AddNumberToObject(resp, "returned", returned);
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
#endif
}

static esp_err_t agent_memory_post_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    return si_http_send_text_status(
        req, "503 Service Unavailable",
        "memory persistence disabled in recovery candidate");
#endif
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char *buf = calloc(1, AGENT_HISTORY_MAX_BODY);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, AGENT_HISTORY_MAX_BODY, &root);
    free(buf);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "memory record must be object");
    }

    cJSON *entry = cJSON_Duplicate(root, true);
    cJSON_Delete(root);
    if (!entry) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory record alloc failed");
    }
    if (!cJSON_GetObjectItemCaseSensitive(entry, "kind")) {
        cJSON_AddStringToObject(entry, "kind", "memory");
    }
    if (!cJSON_GetObjectItemCaseSensitive(entry, "scope")) {
        cJSON_AddStringToObject(entry, "scope", "manual_note");
    }
    const char *requested_type = json_string_any(entry, "type", "memory_type", NULL);
    if (!requested_type || !requested_type[0]) {
        requested_type = json_string_any(entry, "scope", "kind", NULL);
    }
    const char *normalized_type = agent_memory_type_normalize(requested_type);
    if (requested_type && requested_type[0] && !normalized_type) {
        cJSON_Delete(entry);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "type must be fact, host_state, service_deployment, user_decision, todo, task_summary, or manual_note");
    }
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "type");
    cJSON_AddStringToObject(entry, "type", normalized_type ? normalized_type : "manual_note");
    if (!cJSON_GetObjectItemCaseSensitive(entry, "source")) {
        cJSON_AddStringToObject(entry, "source", "api");
    }
    cJSON *content = cJSON_GetObjectItemCaseSensitive(entry, "content");
    if (!cJSON_IsString(content) || !content->valuestring || !content->valuestring[0]) {
        content = cJSON_GetObjectItemCaseSensitive(entry, "text");
    }
    if (!cJSON_IsString(content) || !content->valuestring || !content->valuestring[0]) {
        cJSON_Delete(entry);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "content required");
    }
    cJSON *session_item = cJSON_GetObjectItemCaseSensitive(entry, "session_id");
    char session_id[AGENT_SESSION_ID_MAX_LEN + 1];
    if (cJSON_IsString(session_item) && session_item->valuestring &&
        !agent_session_id_valid(session_item->valuestring)) {
        cJSON_Delete(entry);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid session_id");
    }
    agent_session_normalize(cJSON_IsString(session_item) ? session_item->valuestring : NULL,
                            session_id, sizeof(session_id));
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "session_id");
    cJSON_AddStringToObject(entry, "session_id", session_id);
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "stored_ms");
    cJSON_AddNumberToObject(entry, "stored_ms", si_monotonic_ms());
    char *payload = cJSON_PrintUnformatted(entry);
    cJSON_Delete(entry);
    if (!payload) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory payload alloc failed");
    }
    size_t payload_len = strlen(payload);
    if (payload_len == 0 || payload_len > AGENT_HISTORY_MAX_RECORD_BYTES) {
        free(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "memory record too large");
    }

    if (!s_agent_history_lock ||
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        free(payload);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "agent memory lock unavailable");
    }

    bool formatted = false;
    size_t used_bytes = 0;
    uint32_t record_count = 0;
    ret = agent_memory_file_ensure_ready(&formatted);
    if (ret == ESP_OK) {
        ret = agent_memory_file_append_locked(payload, payload_len,
                                             &used_bytes, &record_count);
    }
    xSemaphoreGive(s_agent_history_lock);
    free(payload);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ret == ESP_OK);
    (void)agent_memory_status_json(resp, used_bytes, record_count, formatted, ret);
    if (ret != ESP_OK) {
        cJSON_AddStringToObject(resp, "status", esp_err_to_name(ret));
    }
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}

static esp_err_t agent_memory_clear_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = agent_data_require_browser(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    return si_http_send_text_status(
        req, "503 Service Unavailable",
        "memory persistence disabled in recovery candidate");
#endif

    if (automation_agent_run_active()) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "stop the active Agent task before clearing memory");
    }

    bool cleared = false;
    esp_err_t ret = si_agent_repository_clear_memory(&cleared);
    if (ret == ESP_OK) {
        si_web_log("INFO", "Agent memory cleared");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ret == ESP_OK);
    (void)agent_memory_status_json(resp, 0, 0, cleared, ret);
    if (ret != ESP_OK) {
        cJSON_AddStringToObject(resp, "status", esp_err_to_name(ret));
    }
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}
