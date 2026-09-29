// Transitional domain module: Power HTTP. Kept in the web compilation unit until its service contract is independent.
static void add_gpio_map_entry(cJSON *map, const si_power_gpio_map_entry_t *item)
{
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "role", item->role);
    cJSON_AddStringToObject(entry, "label", item->label);
    cJSON_AddStringToObject(entry, "direction", item->direction);
    cJSON_AddNumberToObject(entry, "gpio", item->gpio);
    cJSON_AddBoolToObject(entry, "enabled", item->enabled);
    cJSON_AddBoolToObject(entry, "implemented", item->implemented);
    cJSON_AddBoolToObject(entry, "active_high", item->active_high);
    cJSON_AddStringToObject(entry, "active_level", item->active_high ? "high" : "low");
    cJSON_AddBoolToObject(entry, "active", item->active);
    cJSON_AddBoolToObject(entry, "configurable", item->configurable);
    cJSON_AddBoolToObject(entry, "required", item->required);
    cJSON_AddStringToObject(entry, "state", item->state);
    cJSON_AddItemToArray(map, entry);
}

static void add_power_json(cJSON *root)
{
    si_power_status_t st;
    si_power_get_status(&st);

    cJSON_AddBoolToObject(root, "enabled", st.enabled);
    cJSON_AddBoolToObject(root, "initialized", st.initialized);
    cJSON_AddBoolToObject(root, "available", st.enabled && st.initialized);
    cJSON_AddBoolToObject(root, "busy", st.busy);
    cJSON_AddBoolToObject(root, "active_high", st.active_high);
    cJSON_AddStringToObject(root, "active_level", st.active_high ? "high" : "low");
    cJSON_AddNumberToObject(root, "default_press_ms", st.default_press_ms);
    cJSON_AddNumberToObject(root, "force_off_ms", st.force_off_ms);
    cJSON_AddNumberToObject(root, "action_count", st.action_count);
    cJSON_AddStringToObject(root, "last_action", st.last_action);
    cJSON_AddStringToObject(root, "last_error", st.last_error);

    cJSON *buttons = cJSON_AddObjectToObject(root, "buttons");
    cJSON *power = cJSON_AddObjectToObject(buttons, "power");
    cJSON_AddNumberToObject(power, "gpio", st.power_button_gpio);
    cJSON_AddStringToObject(power, "label", "Power button");
    cJSON_AddBoolToObject(power, "available", st.enabled && st.initialized && st.power_button_gpio >= 0);
    cJSON_AddNumberToObject(power, "press_ms", st.default_press_ms);

    cJSON *reset = cJSON_AddObjectToObject(buttons, "reset");
    cJSON_AddNumberToObject(reset, "gpio", st.reset_button_gpio);
    cJSON_AddStringToObject(reset, "label", "Reset button");
    cJSON_AddBoolToObject(reset, "available", st.enabled && st.initialized && st.reset_button_gpio >= 0);
    cJSON_AddNumberToObject(reset, "press_ms", st.default_press_ms);

    cJSON *detect = cJSON_AddObjectToObject(root, "detect");
    cJSON *power_detect = cJSON_AddObjectToObject(detect, "power");
    cJSON_AddNumberToObject(power_detect, "gpio", st.power_detect_gpio);
    cJSON_AddBoolToObject(power_detect, "supported", st.power_detect_supported);
    cJSON_AddBoolToObject(power_detect, "active", st.power_on);
    cJSON_AddBoolToObject(power_detect, "active_high", st.power_detect_active_high);
    cJSON_AddStringToObject(power_detect, "active_level", st.power_detect_active_high ? "high" : "low");
    cJSON_AddStringToObject(power_detect, "state",
                            st.power_detect_supported ? (st.power_on ? "on" : "off") : "not-wired");
    cJSON *standby = cJSON_AddObjectToObject(detect, "standby");
    cJSON_AddNumberToObject(standby, "gpio", st.standby_detect_gpio);
    cJSON_AddBoolToObject(standby, "supported", st.standby_detect_supported);
    cJSON_AddBoolToObject(standby, "active", st.standby_on);
    cJSON_AddBoolToObject(standby, "active_high", st.standby_detect_active_high);
    cJSON_AddStringToObject(standby, "active_level", st.standby_detect_active_high ? "high" : "low");
    cJSON_AddStringToObject(standby, "state",
                            st.standby_detect_supported ? (st.standby_on ? "on" : "off") : "not-wired");

    cJSON *locator = cJSON_AddObjectToObject(root, "locator");
    cJSON_AddNumberToObject(locator, "gpio", st.locator_gpio);
    cJSON_AddBoolToObject(locator, "supported", st.locator_supported);
    cJSON_AddBoolToObject(locator, "active", st.locator_on);
    cJSON_AddBoolToObject(locator, "active_high", st.locator_active_high);
    cJSON_AddStringToObject(locator, "state",
                            st.locator_supported ? (st.locator_on ? "on" : "off") : "not-wired");

    cJSON *map = cJSON_AddArrayToObject(root, "gpio_map");
    for (size_t i = 0; i < st.gpio_map_count && i < SI_POWER_GPIO_MAP_MAX; i++) {
        add_gpio_map_entry(map, &st.gpio_map[i]);
    }
}

static esp_err_t power_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    cJSON *root = cJSON_CreateObject();
    add_power_json(root);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t power_gpio_map_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (req->method == HTTP_GET) {
        cJSON *root = cJSON_CreateObject();
        add_power_json(root);
        esp_err_t ret = si_http_send_json(req, root);
        cJSON_Delete(root);
        return ret;
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }

    char buf[1536] = {0};
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) {
        return ret;
    }

    cJSON *reset = cJSON_GetObjectItemCaseSensitive(root, "reset");
    if (cJSON_IsTrue(reset)) {
        cJSON_Delete(root);
        ret = si_power_reset_gpio_map(true);
    } else {
        cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "gpio_map");
        if (!cJSON_IsArray(items)) {
            items = cJSON_GetObjectItemCaseSensitive(root, "map");
        }
        if (!cJSON_IsArray(items)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gpio_map array required");
        }

        si_power_gpio_update_t updates[SI_POWER_GPIO_MAP_MAX] = {0};
        size_t count = 0;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, items) {
            if (count >= SI_POWER_GPIO_MAP_MAX) {
                cJSON_Delete(root);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too many gpio entries");
            }
            cJSON *role = cJSON_GetObjectItemCaseSensitive(item, "role");
            cJSON *gpio = cJSON_GetObjectItemCaseSensitive(item, "gpio");
            cJSON *active_high = cJSON_GetObjectItemCaseSensitive(item, "active_high");
            if (!cJSON_IsString(role) || !cJSON_IsNumber(gpio)) {
                cJSON_Delete(root);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "role and gpio required");
            }
            updates[count].role = role->valuestring;
            updates[count].gpio = gpio->valueint;
            if (cJSON_IsBool(active_high)) {
                updates[count].active_high = cJSON_IsTrue(active_high);
                updates[count].active_high_set = true;
            }
            count++;
        }
        ret = si_power_set_gpio_map(updates, count, true);
        cJSON_Delete(root);
    }

    if (ret == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid gpio map");
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        return si_http_send_text_status(req, "409 Conflict", "power control busy or not initialized");
    }
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gpio map not supported");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "update gpio map");

    si_web_log("INFO", "GPIO map updated");
    cJSON *resp = cJSON_CreateObject();
    add_power_json(resp);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t power_action_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char buf[192] = {0};
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) {
        return ret;
    }
    const char *action = json_string_any(root, "action", "command", "button");
    cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration_ms");
    uint32_t duration_ms = cJSON_IsNumber(duration) ? (uint32_t)duration->valueint : 0;

    if (!action) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "action required");
    }
    if (strcasecmp(action, "power") == 0 || strcasecmp(action, "press_power") == 0) {
        ret = si_power_press_power(duration_ms);
    } else if (strcasecmp(action, "reset") == 0 || strcasecmp(action, "press_reset") == 0) {
        ret = si_power_press_reset(duration_ms);
    } else if (strcasecmp(action, "force_off") == 0 || strcasecmp(action, "forceoff") == 0) {
        ret = si_power_force_off(duration_ms);
    } else if (strcasecmp(action, "locator_on") == 0 || strcasecmp(action, "identify_on") == 0) {
        ret = si_power_set_locator(true);
    } else if (strcasecmp(action, "locator_off") == 0 || strcasecmp(action, "identify_off") == 0) {
        ret = si_power_set_locator(false);
    } else if (strcasecmp(action, "locator_toggle") == 0 || strcasecmp(action, "identify") == 0) {
        ret = si_power_toggle_locator();
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }
    cJSON_Delete(root);

    if (ret == ESP_ERR_NOT_SUPPORTED) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported power action or gpio not configured");
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        return si_http_send_text_status(req, "409 Conflict", "power control busy or not initialized");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "power action");

    char msg[64];
    snprintf(msg, sizeof(msg), "Power action: %s", action);
    si_web_log("INFO", msg);
    return power_status_handler(req);
}
