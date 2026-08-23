// Shared short-lived ownership for Agent/MCP terminal operations.  A browser
// WebSocket stays connected as an observer while its writes are fail-closed.
static esp_err_t ssh_ws_send_terminal_json(const char *json, size_t len);
static esp_err_t ssh_ws_send_terminal_output(const void *data, size_t len);
#if SI_CFG_UART_TERMINAL_ENABLED && defined(CONFIG_HTTPD_WS_SUPPORT)
static esp_err_t uart_ws_send_terminal_json(const char *json, size_t len);
#endif

static SemaphoreHandle_t s_terminal_control_lock;
static si_terminal_control_state_t s_terminal_control;

static const char *terminal_channel_name(si_terminal_channel_t channel)
{
    return channel == SI_TERMINAL_CHANNEL_UART ? "uart" : "ssh";
}

static const char *terminal_actor_name(si_terminal_actor_t actor)
{
    return actor == SI_TERMINAL_ACTOR_MCP ? "mcp" :
           actor == SI_TERMINAL_ACTOR_AGENT ? "agent" : "none";
}

static esp_err_t terminal_control_start(void)
{
    if (s_terminal_control_lock) return ESP_OK;
    s_terminal_control_lock = xSemaphoreCreateMutex();
    if (!s_terminal_control_lock) return ESP_ERR_NO_MEM;
    si_terminal_control_state_init(&s_terminal_control);
    return ESP_OK;
}

static bool terminal_control_get(si_terminal_channel_t channel,
                                 si_terminal_control_status_t *status)
{
    if (!status || !s_terminal_control_lock ||
        xSemaphoreTake(s_terminal_control_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool ok = si_terminal_control_state_get(&s_terminal_control, channel,
                                            status);
    xSemaphoreGive(s_terminal_control_lock);
    return ok;
}

static void terminal_control_add_fields(
    cJSON *item, si_terminal_channel_t channel,
    const si_terminal_control_status_t *status)
{
    cJSON_AddStringToObject(item, "channel", terminal_channel_name(channel));
    cJSON_AddBoolToObject(item, "active", status->active);
    cJSON_AddBoolToObject(item, "manual_input_enabled", !status->active);
    cJSON_AddBoolToObject(item, "cancel_requested",
                          status->cancel_requested);
    cJSON_AddBoolToObject(item, "can_stop", status->active);
    cJSON_AddNumberToObject(item, "generation", status->generation);
    cJSON_AddNumberToObject(item, "started_ms", status->started_ms);
    cJSON_AddStringToObject(item, "actor",
                            terminal_actor_name(status->actor));
    cJSON_AddStringToObject(item, "operation_id", status->operation_id);
    cJSON_AddStringToObject(item, "run_id", status->run_id);
    cJSON_AddStringToObject(item, "label", status->label);
}

static void terminal_control_add_status_json(cJSON *root,
                                             si_terminal_channel_t channel)
{
    si_terminal_control_status_t status = {0};
    cJSON *item = cJSON_AddObjectToObject(root, "automation");
    if (!item) return;
    if (!terminal_control_get(channel, &status)) {
        cJSON_AddBoolToObject(item, "available", false);
        return;
    }
    cJSON_AddBoolToObject(item, "available", true);
    terminal_control_add_fields(item, channel, &status);
}

static char *terminal_control_state_json(si_terminal_channel_t channel)
{
    si_terminal_control_status_t status = {0};
    if (!terminal_control_get(channel, &status)) return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "type", "terminal.control");
    terminal_control_add_fields(root, channel, &status);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t terminal_control_send_json(si_terminal_channel_t channel,
                                            const char *json, size_t len)
{
    if (channel == SI_TERMINAL_CHANNEL_SSH)
        return ssh_ws_send_terminal_json(json, len);
#if SI_CFG_UART_TERMINAL_ENABLED && defined(CONFIG_HTTPD_WS_SUPPORT)
    return uart_ws_send_terminal_json(json, len);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void terminal_control_publish_state(si_terminal_channel_t channel)
{
    char *json = terminal_control_state_json(channel);
    if (!json) return;
    (void)terminal_control_send_json(channel, json, strlen(json));
    free(json);
}

static bool terminal_control_begin(
    si_terminal_channel_t channel, si_terminal_actor_t actor,
    const char *operation_id, const char *run_id, const char *label,
    si_terminal_control_token_t *token)
{
    if (!s_terminal_control_lock ||
        xSemaphoreTake(s_terminal_control_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool ok = si_terminal_control_state_begin(
        &s_terminal_control, channel, actor, operation_id, run_id, label,
        si_monotonic_ms(), token);
    xSemaphoreGive(s_terminal_control_lock);
    if (ok) terminal_control_publish_state(channel);
    return ok;
}

static bool terminal_control_finish(
    const si_terminal_control_token_t *token)
{
    if (!token || !s_terminal_control_lock ||
        xSemaphoreTake(s_terminal_control_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool ok = si_terminal_control_state_finish(&s_terminal_control, token);
    xSemaphoreGive(s_terminal_control_lock);
    if (ok) terminal_control_publish_state(token->channel);
    return ok;
}

static bool terminal_control_cancel_requested(
    const si_terminal_control_token_t *token)
{
    if (!token || !s_terminal_control_lock ||
        xSemaphoreTake(s_terminal_control_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return true;
    bool cancelled = si_terminal_control_state_cancel_requested(
        &s_terminal_control, token);
    xSemaphoreGive(s_terminal_control_lock);
    return cancelled;
}

static bool terminal_control_manual_input_allowed(
    si_terminal_channel_t channel)
{
    if (!s_terminal_control_lock ||
        xSemaphoreTake(s_terminal_control_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool allowed = si_terminal_control_state_manual_input_allowed(
        &s_terminal_control, channel);
    xSemaphoreGive(s_terminal_control_lock);
    return allowed;
}

static bool terminal_control_text_sensitive(const char *text)
{
    if (!text) return false;
    static const char *const needles[] = {
        "password", "passwd", "api_key", "apikey", "token=",
        "authorization:", "bearer ", "private key", "secret=",
    };
    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
        if (strcasestr(text, needles[i])) return true;
    }
    return false;
}

static void terminal_control_publish_input(
    const si_terminal_control_token_t *token, const char *text,
    bool append_enter, bool redacted)
{
    if (!token || terminal_control_cancel_requested(token)) return;
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "terminal.input");
    cJSON_AddStringToObject(root, "channel",
                            terminal_channel_name(token->channel));
    cJSON_AddNumberToObject(root, "generation", token->generation);
    cJSON_AddBoolToObject(root, "append_enter", append_enter);
    cJSON_AddBoolToObject(root, "redacted", redacted);
    if (!redacted) cJSON_AddStringToObject(root, "text", text ? text : "");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;
    (void)terminal_control_send_json(token->channel, json, strlen(json));
    free(json);
}

static void terminal_control_mirror_ssh_output(
    const si_terminal_control_token_t *token, const char *data, size_t len)
{
    if (!token || !data || len == 0 ||
        terminal_control_cancel_requested(token)) return;
    (void)ssh_ws_send_terminal_output(data, len);
}

static void terminal_ssh_exec_output_cb(const char *data, size_t len,
                                        void *user_ctx)
{
    terminal_control_mirror_ssh_output(
        (const si_terminal_control_token_t *)user_ctx, data, len);
}

static bool terminal_ssh_exec_cancel_cb(void *user_ctx)
{
    return terminal_control_cancel_requested(
        (const si_terminal_control_token_t *)user_ctx);
}

static esp_err_t terminal_control_stop_handler(httpd_req_t *req)
{
    char body[256];
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, body, sizeof(body), &root);
    if (ret != ESP_OK) return ret;
    const cJSON *channel_item =
        cJSON_GetObjectItemCaseSensitive(root, "channel");
    const cJSON *generation_item =
        cJSON_GetObjectItemCaseSensitive(root, "generation");
    bool uart = cJSON_IsString(channel_item) &&
                strcmp(channel_item->valuestring, "uart") == 0;
    bool ssh = cJSON_IsString(channel_item) &&
               strcmp(channel_item->valuestring, "ssh") == 0;
    double generation_value = cJSON_IsNumber(generation_item) ?
                              generation_item->valuedouble : 0.0;
    bool generation_valid = generation_value >= 1.0 &&
                            generation_value <= (double)UINT32_MAX &&
                            generation_value ==
                                (double)(uint32_t)generation_value;
    uint32_t generation = generation_valid ?
                          (uint32_t)generation_value : 0U;
    cJSON_Delete(root);
    if ((!uart && !ssh) || generation == 0U)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "channel and generation required");
    si_auth_session_context_t session = {0};
    ret = si_http_require_action(
        req, uart ? SI_CAPABILITY_UART : SI_CAPABILITY_SSH,
        "terminal_stop", SI_AUTHZ_RISK_HIGH, &session);
    if (ret != ESP_OK) return ret;
    if (session.principal != SI_PRINCIPAL_BROWSER)
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "browser operator required");
    si_terminal_channel_t channel = uart ? SI_TERMINAL_CHANNEL_UART :
                                           SI_TERMINAL_CHANNEL_SSH;
    if (xSemaphoreTake(s_terminal_control_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    bool stopped = si_terminal_control_state_request_cancel(
        &s_terminal_control, channel, generation);
    xSemaphoreGive(s_terminal_control_lock);
    if (!stopped)
        return si_http_send_text_status(req, "409 Conflict",
                                        "terminal operation changed or ended");
    terminal_control_publish_state(channel);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "accepted", true);
    terminal_control_add_status_json(response, channel);
    ret = si_http_send_json(req, response);
    cJSON_Delete(response);
    return ret;
}
