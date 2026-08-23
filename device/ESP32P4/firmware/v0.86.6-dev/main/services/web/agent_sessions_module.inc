// Transitional Agent module: session projection and HTTP. Shares the run context until its service API is finalized.
static int agent_session_find(agent_session_summary_t *sessions, uint32_t count,
                              const char *session_id)
{
    if (!sessions || !session_id) {
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (sessions[i].used && strcmp(sessions[i].id, session_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int agent_session_get_slot(agent_session_summary_t *sessions, uint32_t *count,
                                  const char *session_id)
{
    if (!sessions || !count || !session_id) {
        return -1;
    }
    int existing = agent_session_find(sessions, *count, session_id);
    if (existing >= 0) {
        return existing;
    }
    if (*count >= AGENT_SESSION_RETURN_MAX) {
        return -1;
    }
    uint32_t index = *count;
    sessions[index].used = true;
    strlcpy(sessions[index].id, session_id, sizeof(sessions[index].id));
    (*count)++;
    return (int)index;
}

static void agent_session_apply_record(agent_session_summary_t *sessions, uint32_t *count,
                                       const cJSON *entry)
{
    char session_id[AGENT_SESSION_ID_MAX_LEN + 1];
    agent_session_normalize(agent_history_record_session_id(entry), session_id, sizeof(session_id));
    int slot_index = agent_session_get_slot(sessions, count, session_id);
    if (slot_index < 0) {
        return;
    }

    agent_session_summary_t *session = &sessions[slot_index];
    cJSON *stored = cJSON_GetObjectItemCaseSensitive(entry, "stored_ms");
    uint32_t stored_ms = cJSON_IsNumber(stored) ? (uint32_t)stored->valuedouble : 0;
    if (session->created_ms == 0) {
        session->created_ms = stored_ms;
    }
    if (stored_ms >= session->updated_ms) {
        session->updated_ms = stored_ms;
    }

    if (agent_history_record_is_session_control(entry)) {
        cJSON *action = cJSON_GetObjectItemCaseSensitive(entry, "action");
        const char *action_text = cJSON_IsString(action) ? action->valuestring : "create";
        if (action_text && strcasecmp(action_text, "delete") == 0) {
            session->deleted = true;
            return;
        }
        session->deleted = false;
        cJSON *title = cJSON_GetObjectItemCaseSensitive(entry, "title");
        if (cJSON_IsString(title) && title->valuestring && title->valuestring[0]) {
            strlcpy(session->title, title->valuestring, sizeof(session->title));
        }
        return;
    }

    if (!agent_history_record_is_visible(entry)) {
        return;
    }
    session->deleted = false;
    session->message_count++;
    if (session->title[0] == '\0') {
        const char *content = agent_history_record_content(entry);
        agent_session_title_from_content(content, session->title, sizeof(session->title));
    }
}

static void agent_sessions_sort(agent_session_summary_t *sessions, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = i + 1; j < count; j++) {
            if (sessions[j].updated_ms > sessions[i].updated_ms) {
                agent_session_summary_t tmp = sessions[i];
                sessions[i] = sessions[j];
                sessions[j] = tmp;
            }
        }
    }
}

typedef struct {
    agent_session_summary_t *sessions;
    uint32_t *count;
} agent_sessions_collect_ctx_t;

static esp_err_t agent_sessions_collect_cb(cJSON *entry, void *user_ctx)
{
    agent_sessions_collect_ctx_t *ctx = (agent_sessions_collect_ctx_t *)user_ctx;
    if (!ctx || !ctx->sessions || !ctx->count) {
        return ESP_ERR_INVALID_ARG;
    }
    agent_session_apply_record(ctx->sessions, ctx->count, entry);
    return ESP_OK;
}

static esp_err_t __attribute__((unused))
agent_sessions_collect_locked(agent_session_summary_t *sessions,
                                               uint32_t *count)
{
    if (!sessions || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    memset(sessions, 0, sizeof(agent_session_summary_t) * AGENT_SESSION_RETURN_MAX);

    agent_sessions_collect_ctx_t ctx = {
        .sessions = sessions,
        .count = count,
    };
    esp_err_t ret = agent_history_file_for_each_locked(agent_sessions_collect_cb, &ctx, NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    agent_sessions_sort(sessions, *count);
    return ESP_OK;
}

static void __attribute__((unused))
agent_sessions_add_json(cJSON *root, const agent_session_summary_t *sessions,
                                    uint32_t count)
{
    cJSON *array = cJSON_AddArrayToObject(root, "sessions");
    uint32_t visible = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!sessions[i].used || sessions[i].deleted) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", sessions[i].id);
        cJSON_AddStringToObject(item, "session_id", sessions[i].id);
        cJSON_AddStringToObject(item, "title",
                                sessions[i].title[0] ? sessions[i].title : "新会话");
        cJSON_AddNumberToObject(item, "created_ms", sessions[i].created_ms);
        cJSON_AddNumberToObject(item, "updated_ms", sessions[i].updated_ms);
        cJSON_AddNumberToObject(item, "message_count", sessions[i].message_count);
        cJSON_AddItemToArray(array, item);
        visible++;
    }
    cJSON_AddNumberToObject(root, "session_count", visible);
    cJSON_AddBoolToObject(root, "empty_allowed", true);
    cJSON_AddStringToObject(root, "default_session_id", AGENT_SESSION_DEFAULT_ID);
}

static esp_err_t agent_sessions_get_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddArrayToObject(resp, "sessions");
    cJSON_AddNumberToObject(resp, "session_count", 0);
    cJSON_AddBoolToObject(resp, "empty_allowed", true);
    cJSON_AddBoolToObject(resp, "supported", false);
    cJSON_AddStringToObject(resp, "storage", "ram_only_recovery_candidate");
    cJSON_AddStringToObject(resp, "status", "automatic_tf_reads_disabled");
    cJSON_AddStringToObject(resp, "default_session_id",
                            AGENT_SESSION_DEFAULT_ID);
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
#else
    cJSON *resp = cJSON_CreateObject();

    agent_session_summary_t sessions[AGENT_SESSION_RETURN_MAX];
    uint32_t count = 0;
    bool formatted = false;
    esp_err_t ret = ESP_OK;
    if (!s_agent_history_lock ||
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "agent history lock unavailable");
    }
    ret = agent_history_file_ensure_ready(&formatted);
    if (ret == ESP_OK) {
        ret = agent_sessions_collect_locked(sessions, &count);
    }
    xSemaphoreGive(s_agent_history_lock);

    (void)agent_history_status_json(resp, 0, 0, formatted, ret);
    if (ret == ESP_OK) {
        agent_sessions_add_json(resp, sessions, count);
    } else {
        cJSON_AddArrayToObject(resp, "sessions");
        cJSON_AddNumberToObject(resp, "session_count", 0);
        cJSON_AddBoolToObject(resp, "empty_allowed", true);
        cJSON_AddStringToObject(resp, "default_session_id", AGENT_SESSION_DEFAULT_ID);
        cJSON_AddStringToObject(resp, "status", esp_err_to_name(ret));
    }
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
#endif
}

static esp_err_t agent_append_session_control(const char *session_id, const char *action,
                                              const char *title)
{
    cJSON *entry = cJSON_CreateObject();
    if (!entry) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(entry, "kind", "session");
    cJSON_AddStringToObject(entry, "action", action ? action : "create");
    cJSON_AddStringToObject(entry, "session_id", session_id);
    if (title && title[0]) {
        cJSON_AddStringToObject(entry, "title", title);
    }
    cJSON_AddNumberToObject(entry, "stored_ms", si_monotonic_ms());
    char *payload = cJSON_PrintUnformatted(entry);
    cJSON_Delete(entry);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    if (!s_agent_history_lock ||
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        free(payload);
        return ESP_ERR_TIMEOUT;
    }
    bool formatted = false;
    size_t used_bytes = 0;
    uint32_t record_count = 0;
    esp_err_t ret = agent_history_file_ensure_ready(&formatted);
    if (ret == ESP_OK) {
        ret = agent_history_file_append_locked(payload, strlen(payload),
                                               &used_bytes, &record_count);
    }
    xSemaphoreGive(s_agent_history_lock);
    free(payload);
    return ret;
}

static esp_err_t agent_sessions_post_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    return si_http_send_text_status(
        req, "503 Service Unavailable",
        "session persistence disabled in recovery candidate");
#endif
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }
    char title[AGENT_SESSION_TITLE_MAX_LEN + 1] = {0};
    if (req->content_len > 0) {
        char *buf = calloc(1, AGENT_HISTORY_MAX_BODY);
        if (!buf) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request alloc failed");
        }
        cJSON *root = NULL;
        esp_err_t json_ret = si_http_recv_json(req, buf, AGENT_HISTORY_MAX_BODY, &root);
        free(buf);
        if (json_ret != ESP_OK) {
            return json_ret;
        }
        cJSON *title_item = cJSON_GetObjectItemCaseSensitive(root, "title");
        if (cJSON_IsString(title_item) && title_item->valuestring) {
            strlcpy(title, title_item->valuestring, sizeof(title));
        }
        cJSON_Delete(root);
    }
    if (title[0] == '\0') {
        strlcpy(title, "新会话", sizeof(title));
    }

    char session_id[AGENT_SESSION_ID_MAX_LEN + 1];
    snprintf(session_id, sizeof(session_id), "s%08" PRIx32, esp_random());
    esp_err_t ret = agent_append_session_control(session_id, "create", title);
    if (ret == ESP_ERR_NOT_FOUND) {
        return si_http_send_text_status(req, "409 Conflict", "TF card not mounted");
    }
    if (ret == ESP_ERR_NO_MEM) {
        return si_http_send_text_status(req, "507 Insufficient Storage", "agent history storage full");
    }
    if (ret != ESP_OK) {
        return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "id", session_id);
    cJSON_AddStringToObject(resp, "session_id", session_id);
    cJSON_AddStringToObject(resp, "title", title);
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}

static esp_err_t agent_sessions_delete_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    return si_http_send_text_status(
        req, "503 Service Unavailable",
        "session persistence disabled in recovery candidate");
#endif
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }
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
    cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (!cJSON_IsString(session) || !agent_session_id_valid(session->valuestring)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "valid session_id required");
    }
    char session_id[AGENT_SESSION_ID_MAX_LEN + 1];
    strlcpy(session_id, session->valuestring, sizeof(session_id));
    cJSON_Delete(root);

    if (!s_agent_run_lock ||
        xSemaphoreTake(s_agent_run_lock,
                       pdMS_TO_TICKS(200)) != pdTRUE) {
        return si_http_send_text_status(
            req, "503 Service Unavailable",
            "Agent run state unavailable");
    }
    bool active_session =
        agent_run_control_state_active(s_agent_run_job.state) &&
        strcmp(s_agent_run_job.session_id, session_id) == 0;
    xSemaphoreGive(s_agent_run_lock);
    if (active_session) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "stop the active Agent task before deleting its Thread");
    }

    ret = agent_append_session_control(session_id, "delete", NULL);
    if (ret == ESP_ERR_NOT_FOUND) {
        return si_http_send_text_status(req, "409 Conflict", "TF card not mounted");
    }
    if (ret == ESP_ERR_NO_MEM) {
        return si_http_send_text_status(req, "507 Insufficient Storage", "agent history storage full");
    }
    if (ret != ESP_OK) {
        return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "session_id", session_id);
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}

static const char *agent_response_content(cJSON *api_root)
{
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(api_root, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice ? cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
    cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
    if (cJSON_IsString(content) && content->valuestring) {
        return content->valuestring;
    }
    return json_string_any(api_root, "message", "output", "content");
}

static cJSON *agent_response_message(cJSON *api_root)
{
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(api_root, "choices");
    cJSON *choice =
        cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message =
        choice ? cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
    return cJSON_IsObject(message) ? message : NULL;
}

static cJSON *agent_parse_json_from_text(const char *text)
{
    if (!text) {
        return NULL;
    }
    cJSON *root = cJSON_Parse(text);
    if (root) {
        return root;
    }

    const char *start = strchr(text, '{');
    const char *end = strrchr(text, '}');
    if (!start || !end || end <= start) {
        return NULL;
    }
    size_t len = (size_t)(end - start + 1);
    char *copy = agent_alloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    root = cJSON_Parse(copy);
    free(copy);
    return root;
}

static const cJSON *agent_tool_args(const cJSON *call)
{
    if (!cJSON_IsObject(call)) {
        return NULL;
    }
    const char *names[] = {"args", "arguments", "input", "parameters"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(call, names[i]);
        if (cJSON_IsObject(item)) {
            return item;
        }
    }
    const cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
    if (cJSON_IsObject(fn)) {
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            const cJSON *item = cJSON_GetObjectItemCaseSensitive(fn, names[i]);
            if (cJSON_IsObject(item)) {
                return item;
            }
        }
    }
    return call;
}

static const char *agent_tool_name(const cJSON *call)
{
    const char *name = json_string_any4(call, "tool", "name", "type", "action");
    if (name && name[0]) {
        return name;
    }
    const cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
    if (cJSON_IsObject(fn)) {
        name = json_string_any(fn, "name", "tool", NULL);
        if (name && name[0]) {
            return name;
        }
    }
    return NULL;
}
