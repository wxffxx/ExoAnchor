// Transitional Agent API profile settings HTTP adapter.
static void add_agent_api_capabilities_json(cJSON *parent,
                                            const char *provider,
                                            const char *model)
{
    si_model_capabilities_t capabilities;
    si_model_capabilities_resolve(provider, model, &capabilities);
    cJSON *item = cJSON_AddObjectToObject(parent, "capabilities");
    if (!item) {
        return;
    }
    cJSON_AddBoolToObject(item, "text_input", capabilities.text_input);
    cJSON_AddBoolToObject(item, "image_input", capabilities.image_input);
    cJSON_AddBoolToObject(item, "video_input", capabilities.video_input);
    cJSON_AddBoolToObject(item, "base64_image_input",
                          capabilities.base64_image_input);
    cJSON_AddBoolToObject(item, "public_image_url",
                          capabilities.public_image_url);
    cJSON_AddBoolToObject(item, "native_tools", capabilities.native_tools);
    cJSON_AddBoolToObject(item, "structured_json",
                          capabilities.structured_json);
    cJSON_AddBoolToObject(item, "streaming", capabilities.streaming);
    cJSON_AddBoolToObject(item, "reasoning", capabilities.reasoning);
    cJSON_AddBoolToObject(item, "preserve_assistant_message",
                          capabilities.preserve_assistant_message);
    cJSON_AddBoolToObject(item, "fixed_sampling_parameters",
                          capabilities.fixed_sampling_parameters);
    if (capabilities.default_reasoning_effort) {
        cJSON_AddStringToObject(item, "default_reasoning_effort",
                                capabilities.default_reasoning_effort);
    }
}

static void add_agent_api_profile_json(cJSON *array,
                                       const agent_api_profile_t *profile)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", profile->id);
    cJSON_AddStringToObject(item, "label", profile->label);
    cJSON_AddStringToObject(item, "provider", profile->provider);
    cJSON_AddStringToObject(item, "api_provider", profile->provider);
    cJSON_AddStringToObject(item, "endpoint", profile->endpoint);
    cJSON_AddStringToObject(item, "model", profile->model);
    cJSON_AddBoolToObject(item, "api_key_configured",
                          profile->api_key_configured);
    add_agent_api_capabilities_json(item, profile->provider, profile->model);
    cJSON_AddItemToArray(array, item);
}

static void add_agent_api_json(cJSON *root)
{
    agent_api_profile_t active;
    if (agent_api_get_profile(NULL, &active) != ESP_OK) {
        memset(&active, 0, sizeof(active));
    }
    cJSON_AddStringToObject(root, "active_profile", active.id);
    cJSON_AddStringToObject(root, "label", active.label);
    cJSON_AddStringToObject(root, "provider", active.provider);
    cJSON_AddStringToObject(root, "api_provider", active.provider);
    cJSON_AddStringToObject(root, "endpoint", active.endpoint);
    cJSON_AddStringToObject(root, "model", active.model);
    cJSON_AddBoolToObject(root, "api_key_configured",
                          active.api_key_configured);
    add_agent_api_capabilities_json(root, active.provider, active.model);
    cJSON *profiles = cJSON_AddArrayToObject(root, "profiles");
    for (size_t i = 0; i < si_agent_api_profile_count(); i++) {
        agent_api_profile_t profile;
        if (agent_api_get_profile(si_agent_api_profile_id_at(i), &profile) ==
            ESP_OK) {
            add_agent_api_profile_json(profiles, &profile);
        }
    }
    cJSON_AddBoolToObject(root, "stored_in_nvs", true);
    cJSON_AddNumberToObject(root, "profile_count", AGENT_API_PROFILE_COUNT);
    cJSON_AddNumberToObject(root, "provider_max_length",
                            AGENT_API_PROVIDER_MAX_LEN);
    cJSON_AddNumberToObject(root, "endpoint_max_length",
                            AGENT_API_ENDPOINT_MAX_LEN);
    cJSON_AddNumberToObject(root, "model_max_length",
                            AGENT_API_MODEL_MAX_LEN);
    cJSON_AddNumberToObject(root, "api_key_max_length",
                            AGENT_API_SECRET_MAX_LEN);
}

static esp_err_t settings_agent_api_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        add_agent_api_json(resp);
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                                   "method not allowed");
    }
    (void)automation_request_stop_actor(
        SI_RESOURCE_OPERATION_ACTOR_AGENT,
        "Agent API profile changed in Settings");

    char buf[1024] = {0};
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) {
        return ret;
    }

    const char *profile_id =
        json_string_any(root, "profile", "profile_id", "id");
    char profile_buf[AGENT_API_PROFILE_ID_MAX_LEN + 1];
    agent_api_profile_t active;
    ret = agent_api_get_profile(NULL, &active);
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return si_http_send_text_status(
            req, "500 Internal Server Error", esp_err_to_name(ret));
    }
    strlcpy(profile_buf,
            profile_id && profile_id[0] ? profile_id : active.id,
            sizeof(profile_buf));
    agent_api_profile_t current;
    ret = agent_api_get_profile(profile_buf, &current);
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid agent api profile");
    }
    const char *label =
        json_string_any(root, "label", "name", "profile_label");
    const char *provider =
        json_string_any(root, "provider", "api_provider", NULL);
    const char *endpoint =
        json_string_any(root, "endpoint", "api_base", "base_url");
    const char *model =
        json_string_any(root, "model", "model_name", NULL);
    cJSON *api_key_item = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    cJSON *clear_item =
        cJSON_GetObjectItemCaseSensitive(root, "clear_api_key");
    bool clear_api_key = cJSON_IsTrue(clear_item);
    bool api_key_present =
        cJSON_IsString(api_key_item) && api_key_item->valuestring &&
        api_key_item->valuestring[0] != '\0';

    char label_buf[sizeof(current.label)];
    char provider_buf[sizeof(current.provider)];
    char endpoint_buf[sizeof(current.endpoint)];
    char model_buf[sizeof(current.model)];
    strlcpy(label_buf, label ? label : current.label, sizeof(label_buf));
    strlcpy(provider_buf, provider ? provider : current.provider,
            sizeof(provider_buf));
    strlcpy(endpoint_buf, endpoint ? endpoint : current.endpoint,
            sizeof(endpoint_buf));
    strlcpy(model_buf, model ? model : current.model, sizeof(model_buf));

    ret = agent_api_save_profile(
        profile_buf, label_buf, provider_buf, endpoint_buf, model_buf,
        api_key_present ? api_key_item->valuestring : "",
        api_key_present, clear_api_key);
    if (api_key_present) {
        memset(api_key_item->valuestring, 0,
               strlen(api_key_item->valuestring));
    }
    cJSON_Delete(root);

    if (ret == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "agent api field too long or empty");
    }
    if (ret == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid agent api field");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "save agent api settings");

    cJSON *resp = cJSON_CreateObject();
    add_agent_api_json(resp);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}
