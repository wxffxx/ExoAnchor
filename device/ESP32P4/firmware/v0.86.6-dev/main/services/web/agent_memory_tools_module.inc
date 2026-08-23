// Transitional Agent module: Memory and history tools. Shares the run context until its service API is finalized.
static bool agent_execute_memory_search_tool(const char *job_id, const cJSON *call,
                                             int index, const char *current_session,
                                             cJSON *results, bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const char *query = agent_tool_query(call);
    const char *requested_type = json_string_any(args, "type", "memory_type", "scope");
    const char *type = agent_memory_type_normalize(requested_type);
    const char *requested_session = json_string_any(args, "session_id", "session", "thread_id");
    char session_id[AGENT_SESSION_ID_MAX_LEN + 1] = {0};
    bool session_filter = false;
    uint32_t limit = agent_search_limit_from_args(args, 8,
                                                  AGENT_MEMORY_SEARCH_RETURN_MAX);

    cJSON *item = cJSON_CreateObject();
    if (!item) {
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddNumberToObject(item, "index", index);
    cJSON_AddStringToObject(item, "tool", "memory_search");
    if (query && query[0]) {
        cJSON_AddStringToObject(item, "query", query);
    }
    cJSON_AddNumberToObject(item, "limit", limit);
    si_product_feature_settings_t product_features;
    si_product_feature_settings_get(&product_features);
    if (!product_features.long_term_memory_enabled) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "long-term memory disabled in Settings");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        agent_run_add_eventf(job_id, "warn",
                             "tool[%d] memory_search blocked · disabled",
                             index + 1);
        return false;
    }

    if (query && strlen(query) > AGENT_SEARCH_QUERY_MAX_LEN) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "memory_search query too large");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] memory_search query too large",
                             index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    if (requested_type && requested_type[0] && !type) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "invalid memory type");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] memory_search invalid type",
                             index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    if (!agent_normalize_optional_session_arg(requested_session, current_session,
                                              session_id, sizeof(session_id),
                                              &session_filter)) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "invalid session_id");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] memory_search invalid session",
                             index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    if (type) {
        cJSON_AddStringToObject(item, "type", type);
    }
    if (session_filter) {
        cJSON_AddStringToObject(item, "session_id", session_id);
    }

    cJSON *records = cJSON_AddArrayToObject(item, "records");
    if (!records) {
        cJSON_Delete(item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    if (!s_agent_history_lock ||
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(1200)) != pdTRUE) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "agent memory lock unavailable");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    bool formatted = false;
    esp_err_t ret = agent_memory_file_ensure_ready(&formatted);
    uint32_t matched = 0;
    uint32_t returned = 0;
    if (ret == ESP_OK) {
        ret = agent_search_records_locked(true, records, query, type, session_id,
                                          session_filter, limit, &matched,
                                          &returned);
    }
    xSemaphoreGive(s_agent_history_lock);

    cJSON_AddBoolToObject(item, "ok", ret == ESP_OK);
    cJSON_AddBoolToObject(item, "formatted", formatted);
    cJSON_AddNumberToObject(item, "matched", matched);
    cJSON_AddNumberToObject(item, "returned", returned);
    if (ret != ESP_OK) {
        cJSON_AddStringToObject(item, "error", s_agent_history_last_error[0] ?
                                s_agent_history_last_error : esp_err_to_name(ret));
        if (all_ok) {
            *all_ok = false;
        }
    }
    cJSON_AddItemToArray(results, item);
    agent_run_add_eventf(job_id, ret == ESP_OK ? "ok" : "bad",
                         "tool[%d] memory_search %s · returned=%u/%u",
                         index + 1, ret == ESP_OK ? "ok" : "failed",
                         returned, matched);
    return true;
}

static bool agent_execute_history_search_tool(const char *job_id, const cJSON *call,
                                              int index, const char *current_session,
                                              cJSON *results, bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const char *query = agent_tool_query(call);
    const char *requested_session = json_string_any(args, "session_id", "session", "thread_id");
    char session_id[AGENT_SESSION_ID_MAX_LEN + 1] = {0};
    bool session_filter = false;
    uint32_t limit = agent_search_limit_from_args(args, 10,
                                                  AGENT_HISTORY_SEARCH_RETURN_MAX);

    cJSON *item = cJSON_CreateObject();
    if (!item) {
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddNumberToObject(item, "index", index);
    cJSON_AddStringToObject(item, "tool", "history_search");
    if (query && query[0]) {
        cJSON_AddStringToObject(item, "query", query);
    }
    cJSON_AddNumberToObject(item, "limit", limit);

    if (query && strlen(query) > AGENT_SEARCH_QUERY_MAX_LEN) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "history_search query too large");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    if (!agent_normalize_optional_session_arg(requested_session, current_session,
                                              session_id, sizeof(session_id),
                                              &session_filter)) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "invalid session_id");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    if (session_filter) {
        cJSON_AddStringToObject(item, "session_id", session_id);
    } else {
        cJSON_AddStringToObject(item, "session_scope", "all");
    }

    cJSON *records = cJSON_AddArrayToObject(item, "records");
    if (!records) {
        cJSON_Delete(item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    if (!s_agent_history_lock ||
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(1200)) != pdTRUE) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "agent history lock unavailable");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    bool formatted = false;
    esp_err_t ret = agent_history_file_ensure_ready(&formatted);
    uint32_t matched = 0;
    uint32_t returned = 0;
    if (ret == ESP_OK) {
        ret = agent_search_records_locked(false, records, query, NULL, session_id,
                                          session_filter, limit, &matched,
                                          &returned);
    }
    xSemaphoreGive(s_agent_history_lock);

    cJSON_AddBoolToObject(item, "ok", ret == ESP_OK);
    cJSON_AddBoolToObject(item, "formatted", formatted);
    cJSON_AddNumberToObject(item, "matched", matched);
    cJSON_AddNumberToObject(item, "returned", returned);
    if (ret != ESP_OK) {
        cJSON_AddStringToObject(item, "error", s_agent_history_last_error[0] ?
                                s_agent_history_last_error : esp_err_to_name(ret));
        if (all_ok) {
            *all_ok = false;
        }
    }
    cJSON_AddItemToArray(results, item);
    agent_run_add_eventf(job_id, ret == ESP_OK ? "ok" : "bad",
                         "tool[%d] history_search %s · returned=%u/%u",
                         index + 1, ret == ESP_OK ? "ok" : "failed",
                         returned, matched);
    return true;
}

static bool agent_execute_memory_write_tool(const char *job_id, const cJSON *call,
                                            int index, bool dry_run,
                                            const char *current_session,
                                            cJSON *results, bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const char *content = json_string_any(args, "content", "text", "message");
    const char *requested_type = json_string_any(args, "type", "memory_type", "scope");
    const char *type = agent_memory_type_normalize(requested_type);
    const char *requested_session = json_string_any(args, "session_id", "session", "thread_id");
    char session_id[AGENT_SESSION_ID_MAX_LEN + 1] = {0};
    bool session_filter = false;

    cJSON *item = cJSON_CreateObject();
    if (!item) {
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddNumberToObject(item, "index", index);
    cJSON_AddStringToObject(item, "tool", "memory_write");
    cJSON_AddBoolToObject(item, "dry_run", dry_run);
    si_product_feature_settings_t product_features;
    si_product_feature_settings_get(&product_features);
    if (!product_features.long_term_memory_enabled) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "long-term memory disabled in Settings");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        agent_run_add_eventf(job_id, "warn",
                             "tool[%d] memory_write blocked · disabled",
                             index + 1);
        return false;
    }

    if (!content || !content[0]) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "memory_write content required");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    if (strlen(content) > AGENT_HISTORY_CONTENT_STORE_MAX) {
        cJSON_AddBoolToObject(item, "content_truncated", true);
    }
    if (!type) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "memory_write type required");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    if (requested_session && requested_session[0]) {
        if (!agent_normalize_optional_session_arg(requested_session, current_session,
                                                  session_id, sizeof(session_id),
                                                  &session_filter) ||
            !session_filter) {
            cJSON_AddBoolToObject(item, "ok", false);
            cJSON_AddStringToObject(item, "error", "invalid session_id");
            cJSON_AddItemToArray(results, item);
            if (all_ok) {
                *all_ok = false;
            }
            return false;
        }
    } else {
        agent_session_normalize(current_session, session_id, sizeof(session_id));
        session_filter = true;
    }

    cJSON_AddStringToObject(item, "type", type);
    cJSON_AddStringToObject(item, "session_id", session_id);
    if (dry_run) {
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddStringToObject(item, "status", "dry-run");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "cmd", "tool[%d] memory_write dry-run · %s",
                             index + 1, type);
        return false;
    }

    cJSON *entry = cJSON_CreateObject();
    if (!entry) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "memory entry alloc failed");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddStringToObject(entry, "kind", "memory");
    cJSON_AddStringToObject(entry, "scope", type);
    cJSON_AddStringToObject(entry, "type", type);
    cJSON_AddStringToObject(entry, "source", "agent_tool");
    cJSON_AddStringToObject(entry, "session_id", session_id);
    agent_history_add_limited_content(entry, content);
    cJSON_AddNumberToObject(entry, "stored_ms", si_monotonic_ms());
    esp_err_t ret = agent_memory_append_entry(entry);
    cJSON_Delete(entry);

    cJSON_AddBoolToObject(item, "ok", ret == ESP_OK);
    if (ret != ESP_OK) {
        cJSON_AddStringToObject(item, "error", s_agent_history_last_error[0] ?
                                s_agent_history_last_error : esp_err_to_name(ret));
        if (all_ok) {
            *all_ok = false;
        }
    }
    cJSON_AddItemToArray(results, item);
    agent_run_add_eventf(job_id, ret == ESP_OK ? "ok" : "bad",
                         "tool[%d] memory_write %s · type=%s",
                         index + 1, ret == ESP_OK ? "ok" : "failed", type);
    return ret == ESP_OK;
}

static char *agent_build_web_search_request(const char *model, const char *query,
                                            const char *strategy)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    if (!messages) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", model && model[0] ?
                            model : AGENT_WEB_SEARCH_MODEL_DEFAULT);
    cJSON_AddNumberToObject(root, "temperature", 0.1);
    cJSON_AddBoolToObject(root, "enable_search", true);
    (void)strategy;

    agent_add_chat_text(messages, "system",
                        "你是 ExoAnchor 的联网搜索工具。请基于实时搜索结果回答查询，"
                        "输出简洁中文摘要；不要调用设备工具，不要编造来源。"
                        "如果搜索无法确认，请明确说明无法确认。",
                        0);
    agent_add_chat_text(messages, "user", query ? query : "", 0);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static void agent_add_limited_tool_output(cJSON *item, const char *output)
{
    const char *text = output ? output : "";
    size_t len = strlen(text);
    if (len <= AGENT_TOOL_OUTPUT_MAX) {
        cJSON_AddBoolToObject(item, "truncated", false);
        cJSON_AddStringToObject(item, "output", text);
        return;
    }

    char *copy = agent_alloc(AGENT_TOOL_OUTPUT_MAX + 16);
    if (!copy) {
        cJSON_AddBoolToObject(item, "truncated", true);
        cJSON_AddStringToObject(item, "output", "[tool output allocation failed]");
        return;
    }
    snprintf(copy, AGENT_TOOL_OUTPUT_MAX + 16, "%.*s\n[truncated]",
             (int)AGENT_TOOL_OUTPUT_MAX, text);
    cJSON_AddBoolToObject(item, "truncated", true);
    cJSON_AddStringToObject(item, "output", copy);
    free(copy);
}

static void agent_array_add_duplicate(cJSON *array, const cJSON *item)
{
    if (!array || !item) {
        return;
    }
    cJSON *copy = cJSON_Duplicate(item, true);
    if (copy) {
        cJSON_AddItemToArray(array, copy);
    }
}

static void agent_array_extend_duplicate(cJSON *dst, const cJSON *src)
{
    if (!cJSON_IsArray(dst) || !cJSON_IsArray(src)) {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, src) {
        agent_array_add_duplicate(dst, item);
    }
}

static void agent_json_set_number(cJSON *obj, const char *name, double value)
{
    if (!obj || !name) {
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(obj, name);
    cJSON_AddNumberToObject(obj, name, value);
}

static void agent_json_set_bool(cJSON *obj, const char *name, bool value)
{
    if (!obj || !name) {
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(obj, name);
    cJSON_AddBoolToObject(obj, name, value);
}

static void agent_json_set_string(cJSON *obj, const char *name, const char *value)
{
    if (!obj || !name) {
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(obj, name);
    cJSON_AddStringToObject(obj, name, value ? value : "");
}

static void agent_split_model_actions(const cJSON *model_json, const cJSON *actions,
                                      cJSON *tool_calls, cJSON *hid_actions)
{
    cJSON *declared_tools = cJSON_GetObjectItemCaseSensitive(model_json, "tool_calls");
    if (!cJSON_IsArray(declared_tools)) {
        declared_tools = cJSON_GetObjectItemCaseSensitive(model_json, "tools");
    }
    if (!cJSON_IsArray(declared_tools)) {
        declared_tools = cJSON_GetObjectItemCaseSensitive(model_json, "commands");
    }
    if (cJSON_IsArray(declared_tools)) {
        const cJSON *call = NULL;
        cJSON_ArrayForEach(call, declared_tools) {
            agent_array_add_duplicate(tool_calls, call);
        }
    }

    if (!cJSON_IsArray(actions)) {
        return;
    }
    const cJSON *action = NULL;
    cJSON_ArrayForEach(action, actions) {
        if (agent_tool_is_runtime_call(action)) {
            agent_array_add_duplicate(tool_calls, action);
        } else {
            agent_array_add_duplicate(hid_actions, action);
        }
    }
}

static void agent_add_tool_error(cJSON *results, int index, const char *tool,
                                 const char *message)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return;
    }
    cJSON_AddNumberToObject(item, "index", index);
    cJSON_AddStringToObject(item, "tool", tool ? tool : "unknown");
    cJSON_AddBoolToObject(item, "ok", false);
    cJSON_AddStringToObject(item, "error", message ? message : "tool error");
    cJSON_AddItemToArray(results, item);
}

static void agent_run_add_eventf(const char *job_id, const char *kind,
                                 const char *fmt, ...);
static void agent_run_set_stage(const char *job_id, const char *stage,
                                const char *detail);

typedef struct {
    const char *job_id;
    char action_id[SI_ACTION_EVENT_ACTION_ID_MAX + 1U];
    int index;
    uint32_t last_emit_ms;
    si_terminal_control_token_t terminal_control;
} agent_ssh_event_ctx_t;

static void agent_compact_event_chunk(const char *data, size_t len,
                                      char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!data || len == 0) {
        return;
    }
    size_t pos = 0;
    bool last_space = false;
    for (size_t i = 0; i < len && pos + 1 < out_size; i++) {
        unsigned char ch = (unsigned char)data[i];
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
        if (ch < 32 || ch == 127) {
            continue;
        }
        if (ch == ' ') {
            if (last_space) {
                continue;
            }
            last_space = true;
        } else {
            last_space = false;
        }
        out[pos++] = (char)ch;
    }
    out[pos] = '\0';
}

static void agent_ssh_output_event_cb(const char *data, size_t len, void *user_ctx)
{
    agent_ssh_event_ctx_t *ctx = (agent_ssh_event_ctx_t *)user_ctx;
    if (!ctx) {
        return;
    }
    terminal_control_mirror_ssh_output(&ctx->terminal_control, data, len);
    uint32_t now = si_monotonic_ms();
    if (ctx->last_emit_ms && now - ctx->last_emit_ms < 900) {
        return;
    }
    char text[160];
    agent_compact_event_chunk(data, len, text, sizeof(text));
    if (!text[0]) {
        return;
    }
    ctx->last_emit_ms = now;
    bool redacted = agent_page_context_sensitive(text);
    agent_run_add_action_eventf(ctx->job_id, ctx->action_id,
                                SI_ACTION_CHANNEL_SSH,
                                SI_ACTION_PHASE_OUTPUT_RECEIVED,
                                redacted, true,
                                redacted ?
                                "tool[%d] output · sensitive content redacted" :
                                "tool[%d] output · %s",
                                ctx->index + 1, text);
}
