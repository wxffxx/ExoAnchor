// Persisted Agent/MCP delegation level shared by KVM and Terminal.
static void si_web_add_access_mode_json(cJSON *root)
{
    si_agent_access_mode_t mode = si_agent_access_mode_get();
    cJSON_AddStringToObject(root, "schema", "exoanchor.access_mode.v1");
    cJSON_AddStringToObject(root, "mode", si_agent_access_mode_name(mode));
    cJSON_AddBoolToObject(root, "stored_in_nvs", true);
    cJSON_AddBoolToObject(root, "secret_export_allowed", false);
    cJSON *modes = cJSON_AddArrayToObject(root, "modes");
    if (!modes) return;
    const struct {
        const char *value;
        const char *label;
        const char *description;
    } definitions[] = {
        {"manual", "手动授权", "每个会产生副作用的动作都等待浏览器确认"},
        {"assisted", "替我审批", "自动批准低、中风险动作，高风险动作仍需确认"},
        {"full", "完全访问", "在既定策略和控制租约内自动批准全部风险等级"},
    };
    for (size_t i = 0; i < sizeof(definitions) / sizeof(definitions[0]); i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) continue;
        cJSON_AddStringToObject(item, "value", definitions[i].value);
        cJSON_AddStringToObject(item, "label", definitions[i].label);
        cJSON_AddStringToObject(item, "description", definitions[i].description);
        cJSON_AddItemToArray(modes, item);
    }
}

static esp_err_t settings_access_mode_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret = si_http_require_capability(
        req, req->method == HTTP_GET ? SI_CAPABILITY_OBSERVE :
                                      SI_CAPABILITY_SETTINGS,
        &session);
    if (auth_ret != ESP_OK) return auth_ret;
    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        if (!resp) return ESP_ERR_NO_MEM;
        si_web_add_access_mode_json(resp);
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }
    if (req->method != HTTP_POST)
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                                   "method not allowed");
    if (session.principal != SI_PRINCIPAL_BROWSER)
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "browser operator required");
    char body[192] = {0};
    cJSON *request = NULL;
    esp_err_t ret = si_http_recv_json(req, body, sizeof(body), &request);
    if (ret != ESP_OK) return ret;
    const cJSON *mode_item =
        cJSON_GetObjectItemCaseSensitive(request, "mode");
    const char *mode = cJSON_IsString(mode_item) ? mode_item->valuestring : NULL;
    if (!si_agent_access_mode_valid(mode)) {
        cJSON_Delete(request);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "mode must be manual, assisted, or full");
    }
    si_agent_access_mode_t value = !strcasecmp(mode, "full") ?
        SI_AGENT_ACCESS_FULL : !strcasecmp(mode, "assisted") ?
        SI_AGENT_ACCESS_ASSISTED : SI_AGENT_ACCESS_MANUAL;
    cJSON_Delete(request);
    (void)automation_request_stop_all(
        "Agent/MCP access mode changed by browser operator");
    ret = si_agent_access_mode_set(value);
    if (ret != ESP_OK)
        return si_http_send_text_status(req, "500 Internal Server Error",
                                        esp_err_to_name(ret));
    cJSON *resp = cJSON_CreateObject();
    if (!resp) return ESP_ERR_NO_MEM;
    si_web_add_access_mode_json(resp);
    cJSON_AddBoolToObject(resp, "automation_stopped", true);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}
