// /api/settings/agent-tools: tool policy, skills, web search and the
// embedded Agent autonomy level.
static void agent_autonomy_add_settings_json(cJSON *root)
{
    si_agent_autonomy_level_t autonomy = si_agent_autonomy_get();
    cJSON_AddNumberToObject(root, "autonomy_level", autonomy);
    cJSON_AddStringToObject(root, "autonomy_label",
                            si_agent_autonomy_level_label(autonomy));
    cJSON_AddNumberToObject(root, "autonomy_max_level", SI_AGENT_AUTONOMY_MAX);
    cJSON_AddNumberToObject(root, "autonomy_auto_through",
                            si_agent_autonomy_auto_through(autonomy));
}

/* NULL when the optional autonomy_level field is absent or acceptable. */
static const char *agent_autonomy_request_error(
    const cJSON *autonomy, const si_auth_session_context_t *session,
    httpd_err_code_t *status)
{
    if (!autonomy) {
        return NULL;
    }
    if (!cJSON_IsNumber(autonomy) ||
        autonomy->valuedouble != (double)autonomy->valueint ||
        !si_agent_autonomy_level_valid(autonomy->valueint)) {
        *status = HTTPD_400_BAD_REQUEST;
        return "autonomy_level must be an integer 0..4; L5 is not offered";
    }
    /* Raising what the Agent may do on its own is an operator decision. */
    if (!session || session->principal != SI_PRINCIPAL_BROWSER) {
        *status = HTTPD_403_FORBIDDEN;
        return "browser operator required to change Agent autonomy";
    }
    return NULL;
}

void si_web_add_agent_tools_settings_json(cJSON *root)
{
    char mode[16] = "observe";
    char *policy = heap_caps_calloc(1, AGENT_TOOLS_POLICY_MAX_LEN + 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!policy) {
        policy = calloc(1, AGENT_TOOLS_POLICY_MAX_LEN + 1);
    }
    char *skills = heap_caps_calloc(1, AGENT_TOOLS_SKILLS_MAX_LEN + 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!skills) {
        skills = calloc(1, AGENT_TOOLS_SKILLS_MAX_LEN + 1);
    }

    (void)agent_tools_load_string(AGENT_TOOLS_MODE_KEY, "observe",
                                  mode, sizeof(mode));
    if (!agent_tools_mode_valid(mode)) {
        strlcpy(mode, "observe", sizeof(mode));
    }
    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddNumberToObject(root, "policy_schema_version", 2);
    cJSON_AddStringToObject(root, "settings_endpoint", "/api/settings/agent-tools");
    cJSON_AddStringToObject(root, "default_mode", mode);
    cJSON_AddNumberToObject(root, "policy_max_length", AGENT_TOOLS_POLICY_MAX_LEN);
    cJSON_AddNumberToObject(root, "skills_max_length", AGENT_TOOLS_SKILLS_MAX_LEN);

    agent_autonomy_add_settings_json(root);
    cJSON *modes = cJSON_AddArrayToObject(root, "modes");
    cJSON_AddItemToArray(modes, cJSON_CreateString("observe"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("supervised"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("autonomous"));

    if (policy) {
        (void)agent_tools_load_string(AGENT_TOOLS_POLICY_KEY, AGENT_TOOLS_POLICY_DEFAULT,
                                      policy, AGENT_TOOLS_POLICY_MAX_LEN + 1);
        cJSON_AddStringToObject(root, "policy_json", policy);
        cJSON *parsed = cJSON_Parse(policy);
        if (parsed) {
            cJSON_AddItemToObject(root, "policy", parsed);
        }
    }
    if (skills) {
        (void)agent_tools_load_string(AGENT_TOOLS_SKILLS_KEY, AGENT_TOOLS_SKILLS_DEFAULT,
                                      skills, AGENT_TOOLS_SKILLS_MAX_LEN + 1);
#if SI_CFG_EMBEDDED_AGENT_ENABLED
        cJSON *parsed = agent_skills_parse_with_defaults(skills, AGENT_TOOLS_SKILLS_MAX_LEN + 1);
#else
        cJSON *parsed = cJSON_Parse(skills);
        if (!cJSON_IsArray(parsed)) { cJSON_Delete(parsed); strlcpy(skills, "[]", AGENT_TOOLS_SKILLS_MAX_LEN + 1); parsed = cJSON_CreateArray(); }
#endif
        cJSON_AddStringToObject(root, "skills_json", skills);
        if (parsed) {
            cJSON_AddItemToObject(root, "skills", parsed);
        }
    }
    agent_web_search_settings_t web_search;
    agent_web_search_defaults(&web_search);
    (void)agent_web_search_load(&web_search);
    cJSON *web = cJSON_AddObjectToObject(root, "web_search");
    if (web) {
        cJSON_AddBoolToObject(web, "enabled", web_search.enabled);
        cJSON_AddStringToObject(web, "provider", "qwen_chat_search");
        cJSON_AddStringToObject(web, "profile", web_search.profile);
        cJSON_AddStringToObject(web, "endpoint", web_search.endpoint);
        cJSON_AddStringToObject(web, "model", web_search.model);
        cJSON_AddStringToObject(web, "strategy", web_search.strategy);
        cJSON_AddBoolToObject(web, "api_key_configured",
                              web_search.api_key_configured);
        cJSON_AddBoolToObject(web, "available",
                              agent_web_search_available(&web_search));
        cJSON_AddStringToObject(web, "api_key_source",
                                web_search.api_key_configured ?
                                "web_search" : "agent_profile");
        cJSON_AddStringToObject(web, "default_endpoint_source",
                                "empty endpoint uses selected API profile base");
    }
    free(policy);
    free(skills);
}

static esp_err_t settings_agent_tools_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        si_web_add_agent_tools_settings_json(resp);
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }

    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }
    if (req->content_len <= 0 || req->content_len > AGENT_TOOLS_MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char *buf = calloc(1, AGENT_TOOLS_MAX_BODY);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, AGENT_TOOLS_MAX_BODY, &root);
    free(buf);
    if (ret != ESP_OK) {
        return ret;
    }
    cJSON *reset = cJSON_GetObjectItemCaseSensitive(root, "reset_default");
    if (!reset) {
        reset = cJSON_GetObjectItemCaseSensitive(root, "reset");
    }
    if (cJSON_IsTrue(reset)) {
        cJSON_Delete(root);
        (void)automation_request_stop_all(
            "Agent/MCP tool policy reset in Settings");
        ret = agent_tools_reset_settings();
        if (ret != ESP_OK) {
            return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
        }
        cJSON *resp = cJSON_CreateObject();
        si_web_add_agent_tools_settings_json(resp);
        esp_err_t send_ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return send_ret;
    }

    const cJSON *autonomy = cJSON_GetObjectItemCaseSensitive(root, "autonomy_level");
    httpd_err_code_t autonomy_status = HTTPD_400_BAD_REQUEST;
    const char *autonomy_error =
        agent_autonomy_request_error(autonomy, &session, &autonomy_status);
    if (autonomy_error) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, autonomy_status, autonomy_error);
    }
    const char *mode = json_string_any(root, "default_mode", "mode", NULL);
    const char *policy = json_string_any(root, "policy_json", "policy", NULL);
    const char *skills = json_string_any(root, "skills_json", "skills", NULL);
    const cJSON *web_search = cJSON_GetObjectItemCaseSensitive(root, "web_search");
    bool web_search_present = cJSON_IsObject(web_search);
    const cJSON *web_enabled = web_search_present ?
                                cJSON_GetObjectItemCaseSensitive(web_search, "enabled") : NULL;
    const cJSON *web_clear_key = web_search_present ?
                                 cJSON_GetObjectItemCaseSensitive(web_search, "clear_api_key") : NULL;
    const char *web_profile = web_search_present ?
                              json_string_any(web_search, "profile", "api_profile", "profile_id") : NULL;
    const char *web_endpoint = web_search_present ?
                               json_string_any(web_search, "endpoint", "api_base", "base_url") : NULL;
    const char *web_model = web_search_present ?
                            json_string_any(web_search, "model", "model_name", NULL) : NULL;
    const char *web_strategy = web_search_present ?
                               json_string_any(web_search, "strategy", "search_strategy", NULL) : NULL;
    const char *web_api_key = web_search_present ?
                              json_string_any(web_search, "api_key", "key", "secret") : NULL;
    if (mode && !agent_tools_mode_valid(mode)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "default_mode must be observe, supervised, or autonomous");
    }
    if (policy) {
        ret = agent_tools_validate_policy_json(policy);
        if (ret == ESP_ERR_INVALID_SIZE) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "policy_json too large");
        }
        if (ret != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(
                req, HTTPD_400_BAD_REQUEST,
                "policy_json must contain boolean Agent/MCP switches and valid permission/risk values");
        }
    }
    if (skills) {
        ret = agent_tools_validate_json_text(skills, AGENT_TOOLS_SKILLS_MAX_LEN, true);
        if (ret == ESP_ERR_INVALID_SIZE) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "skills_json too large");
        }
        if (ret != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "skills_json must be a JSON array");
        }
    }
    if (web_search_present) {
        if (web_enabled && !cJSON_IsBool(web_enabled)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "web_search.enabled must be boolean");
        }
        if (web_profile && agent_api_profile_slot(web_profile) < 0) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "web_search.profile must be a known API profile");
        }
        if (web_endpoint &&
            agent_api_validate_endpoint(web_endpoint, true) != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "web_search.endpoint must use https");
        }
        if (web_model &&
            agent_api_validate_text(web_model, AGENT_API_MODEL_MAX_LEN, false) != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "web_search.model invalid or too large");
        }
        if (web_strategy && !agent_web_search_strategy_valid(web_strategy)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "web_search.strategy must be agent, agent_max, turbo, or max");
        }
        if (web_api_key && web_api_key[0] &&
            agent_api_validate_text(web_api_key, AGENT_API_SECRET_MAX_LEN, false) != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "web_search.api_key invalid or too large");
        }
    }

    si_agent_tools_update_t update = {
        .mode = mode,
        .policy = policy,
        .skills = skills,
        .web_present = web_search_present,
        .web_enabled_present = cJSON_IsBool(web_enabled),
        .web_enabled = cJSON_IsTrue(web_enabled),
        .web_profile = web_profile,
        .web_endpoint = web_endpoint,
        .web_model = web_model,
        .web_strategy = web_strategy,
        .web_api_key = web_api_key,
        .clear_web_api_key = cJSON_IsTrue(web_clear_key),
    };
    (void)automation_request_stop_all(
        "Agent/MCP tool policy changed in Settings");
    ret = si_agent_tools_update(&update);
    if (ret == ESP_OK && autonomy) {
        ret = si_agent_autonomy_set(
            (si_agent_autonomy_level_t)autonomy->valueint);
    }
    if (web_api_key && web_api_key[0]) {
        memset((char *)web_api_key, 0, strlen(web_api_key));
    }
    cJSON_Delete(root);

    if (ret != ESP_OK) {
        return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
    }

    cJSON *resp = cJSON_CreateObject();
    si_web_add_agent_tools_settings_json(resp);
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}
