// Product-facing persistent feature switches and capability visibility.
#if SI_CFG_EMBEDDED_AGENT_ENABLED
static esp_err_t agent_handler(httpd_req_t *req)
{
    si_product_feature_settings_t features;
    si_product_feature_settings_get(&features);
    if (!features.embedded_agent_enabled) {
        return si_http_send_text_status(req, "403 Forbidden",
                                        "embedded Agent disabled in Settings");
    }
    return send_embedded_html(req, www_agent_html_start, www_agent_html_end);
}
#endif

static void add_product_features_json(cJSON *root)
{
    si_product_feature_settings_t settings;
    si_product_feature_settings_get(&settings);
    bool agent_enabled = SI_CFG_EMBEDDED_AGENT_ENABLED &&
                         settings.embedded_agent_enabled;
    cJSON_AddBoolToObject(root, "lan_discovery_enabled",
                          settings.lan_discovery_enabled);
    cJSON_AddBoolToObject(root, "embedded_agent_enabled",
                          agent_enabled);
    cJSON_AddBoolToObject(root, "page_context_enabled",
                          SI_CFG_EMBEDDED_AGENT_ENABLED &&
                              settings.page_context_enabled);
    cJSON_AddBoolToObject(root, "conversation_history_enabled",
                          SI_CFG_EMBEDDED_AGENT_ENABLED &&
                              settings.conversation_history_enabled);
    cJSON_AddBoolToObject(root, "long_term_memory_enabled",
                          SI_CFG_EMBEDDED_AGENT_ENABLED &&
                              settings.long_term_memory_enabled);
    cJSON_AddStringToObject(root, "page_context_scope", "device");
    cJSON_AddBoolToObject(root, "embedded_agent_compiled",
                          SI_CFG_EMBEDDED_AGENT_ENABLED);
    cJSON_AddBoolToObject(root, "auto_update_check_supported", false);
    cJSON_AddBoolToObject(root, "anonymous_diagnostics_supported", false);
    cJSON_AddBoolToObject(root, "stored_in_nvs", true);
}

static esp_err_t settings_product_features_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_capability(
        req, req->method == HTTP_GET ? SI_CAPABILITY_OBSERVE :
                                      SI_CAPABILITY_SETTINGS,
        NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        add_product_features_json(resp);
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                                   "method not allowed");
    }
    char buf[512] = {0};
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) {
        return ret;
    }
    si_product_feature_settings_t settings;
    si_product_feature_settings_get(&settings);
#if SI_CFG_EMBEDDED_AGENT_ENABLED
    bool agent_was_enabled = settings.embedded_agent_enabled;
    bool page_context_was_enabled = settings.page_context_enabled;
#endif
#define READ_FEATURE(name, field)                                            \
    do {                                                                     \
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);          \
        if (item) {                                                          \
            if (!cJSON_IsBool(item)) {                                       \
                cJSON_Delete(root);                                          \
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,       \
                                           name " must be boolean");         \
            }                                                                \
            settings.field = cJSON_IsTrue(item);                             \
        }                                                                    \
    } while (0)
    READ_FEATURE("lan_discovery_enabled", lan_discovery_enabled);
#if SI_CFG_EMBEDDED_AGENT_ENABLED
    READ_FEATURE("embedded_agent_enabled", embedded_agent_enabled);
    READ_FEATURE("page_context_enabled", page_context_enabled);
    READ_FEATURE("conversation_history_enabled", conversation_history_enabled);
    READ_FEATURE("long_term_memory_enabled", long_term_memory_enabled);
#endif
#undef READ_FEATURE
    cJSON_Delete(root);
#if SI_CFG_EMBEDDED_AGENT_ENABLED
    if (agent_was_enabled && !settings.embedded_agent_enabled) {
        (void)automation_request_stop_actor(
            SI_RESOURCE_OPERATION_ACTOR_AGENT,
            "embedded Agent disabled in Settings");
    }
    if (page_context_was_enabled && !settings.page_context_enabled) {
        ret = agent_run_revoke_page_context(&settings);
    } else
#endif
    {
        ret = si_product_feature_settings_set(&settings);
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "save product feature settings");
    cJSON *resp = cJSON_CreateObject();
    add_product_features_json(resp);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}
