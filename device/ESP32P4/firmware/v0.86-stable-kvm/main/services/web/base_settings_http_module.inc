// Shared assets, authentication and settings HTTP adapters.
static uint32_t fps_value_to_x100(double fps)
{
    return fps <= 0.0 ? 0 : (uint32_t)(fps * 100.0 + 0.5);
}

static void add_session_settings_json(cJSON *root)
{
    si_session_settings_t settings;
    si_session_settings_get(&settings);
    cJSON_AddBoolToObject(root, "auto_logout_enabled", settings.auto_logout_enabled);
    cJSON_AddNumberToObject(root, "auto_logout_minutes", settings.auto_logout_minutes);
    cJSON_AddNumberToObject(root, "min_minutes", SI_SESSION_LOGOUT_MIN_MINUTES);
    cJSON_AddNumberToObject(root, "max_minutes", SI_SESSION_LOGOUT_MAX_MINUTES);
    cJSON_AddBoolToObject(root, "stored_in_nvs", true);
}

static esp_err_t send_embedded_asset(httpd_req_t *req, const char *type,
                                     const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t len = (size_t)(end - start);
    if (len > 0 && start[len - 1] == '\0') {
        len--;
    }
    return httpd_resp_send(req, (const char *)start, len);
}

static esp_err_t send_embedded_html(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
    return send_embedded_asset(req, "text/html; charset=utf-8", start, end);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return send_embedded_html(req, www_index_html_start, www_index_html_end);
}

static esp_err_t kvm_handler(httpd_req_t *req)
{
    return send_embedded_html(req, www_kvm_html_start, www_kvm_html_end);
}

static esp_err_t ui_core_css_handler(httpd_req_t *req)
{
    return send_embedded_asset(req, "text/css; charset=utf-8",
                               www_ui_core_css_start, www_ui_core_css_end);
}

static esp_err_t ui_core_js_handler(httpd_req_t *req)
{
    return send_embedded_asset(req, "application/javascript; charset=utf-8",
                               www_ui_core_js_start, www_ui_core_js_end);
}

static esp_err_t ui_shell_css_handler(httpd_req_t *req)
{
    return send_embedded_asset(req, "text/css; charset=utf-8",
                               www_ui_shell_css_start, www_ui_shell_css_end);
}

static esp_err_t ui_shell_js_handler(httpd_req_t *req)
{
    return send_embedded_asset(req, "application/javascript; charset=utf-8",
                               www_ui_shell_js_start, www_ui_shell_js_end);
}

static esp_err_t settings_handler(httpd_req_t *req)
{
    return send_embedded_html(req, www_settings_html_start, www_settings_html_end);
}

static esp_err_t auth_login_handler(httpd_req_t *req)
{
    char buf[SCRATCH_BUFSIZE] = {0};
    cJSON *resp = cJSON_CreateObject();
    bool auth_enabled = si_auth_is_enabled();
    bool ok = !auth_enabled;
    if (!ok) {
        cJSON *root = NULL;
        esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
        if (ret != ESP_OK) {
            cJSON_Delete(resp);
            return ret;
        }
        cJSON *username = root ? cJSON_GetObjectItemCaseSensitive(root, "username") : NULL;
        cJSON *password = root ? cJSON_GetObjectItemCaseSensitive(root, "password") : NULL;
        ok = cJSON_IsString(username) && cJSON_IsString(password) &&
             si_auth_credentials_match(username->valuestring, password->valuestring);
        cJSON_Delete(root);
    }

    if (!ok) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid credentials");
    }

    si_auth_record_login();
    si_auth_status_t auth_status;
    si_auth_get_status(&auth_status);
    cJSON_AddStringToObject(resp, "token", auth_status.token);
    cJSON_AddStringToObject(resp, "type", "bearer");
    cJSON_AddNumberToObject(resp, "login_count", auth_status.login_count);
    cJSON_AddBoolToObject(resp, "enabled", auth_status.enabled);
    cJSON_AddStringToObject(resp, "username", auth_status.username);
    cJSON_AddBoolToObject(resp, "local_password", !auth_status.using_default);
    cJSON_AddBoolToObject(resp, "local_credentials", !auth_status.using_default);
    cJSON_AddBoolToObject(resp, "using_default",
                          auth_status.using_default && auth_status.enabled);
    cJSON_AddBoolToObject(resp, "must_change_credentials",
                          auth_status.using_default && auth_status.enabled);
    esp_err_t ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t auth_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    si_auth_status_t auth_status;
    si_auth_get_status(&auth_status);
    cJSON_AddBoolToObject(root, "enabled", auth_status.enabled);
    cJSON_AddStringToObject(root, "username", auth_status.username);
    cJSON_AddStringToObject(root, "default_username", si_auth_default_username());
    cJSON_AddBoolToObject(root, "local_password", !auth_status.using_default);
    cJSON_AddBoolToObject(root, "local_credentials", !auth_status.using_default);
    cJSON_AddBoolToObject(root, "using_default",
                          auth_status.using_default && auth_status.enabled);
    cJSON_AddBoolToObject(root, "must_change_credentials",
                          auth_status.using_default && auth_status.enabled);
    cJSON_AddBoolToObject(root, "token_valid", si_http_check_auth(req));
    cJSON_AddNumberToObject(root, "login_count", auth_status.login_count);
    cJSON_AddNumberToObject(root, "username_max_length", SI_AUTH_USERNAME_MAX_LEN);
    cJSON_AddNumberToObject(root, "min_length", SI_AUTH_PASSWORD_MIN_LEN);
    cJSON_AddNumberToObject(root, "max_length", SI_AUTH_PASSWORD_MAX_LEN);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static const char *json_string_any(const cJSON *obj, const char *first, const char *second, const char *third)
{
    const char *names[] = {first, second, third};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!names[i]) {
            continue;
        }
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, names[i]);
        if (cJSON_IsString(item)) {
            return item->valuestring;
        }
    }
    return NULL;
}

static esp_err_t settings_password_handler(httpd_req_t *req)
{
    char buf[SCRATCH_BUFSIZE] = {0};
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) {
        return ret;
    }

    const char *current_username = json_string_any(root, "current_username", "currentUsername", "old_username");
    const char *current = json_string_any(root, "current_password", "currentPassword", "old_password");
    const char *username = json_string_any(root, "username", "new_username", "newUsername");
    const char *password = json_string_any(root, "new_password", "newPassword", "password");
    si_auth_status_t auth_status;
    si_auth_get_status(&auth_status);
    if (!username) {
        username = auth_status.username;
    }
    bool authorized = si_http_check_auth(req) || !auth_status.enabled ||
                      (current && si_auth_credentials_match(current_username, current));
    if (!authorized) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    }

    ret = si_auth_validate_username(username);
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "username must be 1..32 printable ASCII characters");
    }

    ret = si_auth_validate_password(password);
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "password must be 6..64 printable ASCII characters");
    }

    ret = si_auth_set_credentials(username, password);
    cJSON_Delete(root);
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
    }

    si_web_log("INFO", "Local web credentials updated");
    si_auth_get_status(&auth_status);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "token", auth_status.token);
    cJSON_AddStringToObject(resp, "type", "bearer");
    cJSON_AddStringToObject(resp, "username", auth_status.username);
    cJSON_AddBoolToObject(resp, "local_password", true);
    cJSON_AddBoolToObject(resp, "local_credentials", true);
    cJSON_AddBoolToObject(resp, "using_default", false);
    cJSON_AddBoolToObject(resp, "must_change_credentials", false);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t settings_device_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    char device_label[SI_DEVICE_LABEL_MAX_LEN + 1];
    si_device_label_get(device_label, sizeof(device_label));

    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "label", device_label);
        cJSON_AddStringToObject(resp, "default_label", SI_DEVICE_LABEL_DEFAULT);
        cJSON_AddNumberToObject(resp, "max_length", SI_DEVICE_LABEL_MAX_LEN);
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }

    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }
    char buf[SCRATCH_BUFSIZE] = {0};
    cJSON *root = NULL;
    esp_err_t json_ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (json_ret != ESP_OK) {
        return json_ret;
    }
    const char *label = root ? json_string_any(root, "label", "device_label", "name") : NULL;
    if (!label) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "label is required");
    }

    esp_err_t ret = si_device_label_set(label);
    cJSON_Delete(root);
    if (ret == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "label must be 1..48 bytes");
    }
    if (ret == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "label contains invalid characters");
    }
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
    }

    si_web_log("INFO", "Device label updated");
    si_device_label_get(device_label, sizeof(device_label));
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "label", device_label);
    cJSON_AddStringToObject(resp, "default_label", SI_DEVICE_LABEL_DEFAULT);
    cJSON_AddNumberToObject(resp, "max_length", SI_DEVICE_LABEL_MAX_LEN);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t settings_session_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        add_session_settings_json(resp);
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }

    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }

    si_session_settings_t settings;
    si_session_settings_get(&settings);

    char buf[192] = {0};
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) {
        return ret;
    }

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "auto_logout_enabled");
    if (!enabled) {
        enabled = cJSON_GetObjectItemCaseSensitive(root, "auto_logout");
    }
    if (!enabled) {
        enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    }
    if (enabled) {
        if (!cJSON_IsBool(enabled)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "auto_logout_enabled must be boolean");
        }
        settings.auto_logout_enabled = cJSON_IsTrue(enabled);
    }

    cJSON *minutes_item = cJSON_GetObjectItemCaseSensitive(root, "auto_logout_minutes");
    if (!minutes_item) {
        minutes_item = cJSON_GetObjectItemCaseSensitive(root, "minutes");
    }
    if (!minutes_item) {
        minutes_item = cJSON_GetObjectItemCaseSensitive(root, "idle_minutes");
    }
    if (minutes_item) {
        if (!cJSON_IsNumber(minutes_item)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "auto_logout_minutes must be numeric");
        }
        if (minutes_item->valueint < (int)SI_SESSION_LOGOUT_MIN_MINUTES ||
            minutes_item->valueint > (int)SI_SESSION_LOGOUT_MAX_MINUTES) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "auto_logout_minutes must be 1..1440");
        }
        settings.auto_logout_minutes = (uint32_t)minutes_item->valueint;
    }
    cJSON_Delete(root);

    ret = si_session_settings_set(&settings);
    if (ret == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "auto_logout_minutes must be 1..1440");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "save session settings");

    cJSON *resp = cJSON_CreateObject();
    add_session_settings_json(resp);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static void system_reset_everything_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(650));
    ESP_LOGW(TAG, "Reset everything requested: erasing default NVS partition");
    esp_err_t ret = nvs_flash_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS deinit before reset failed: %s", esp_err_to_name(ret));
    }
    ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS erase failed during reset everything: %s", esp_err_to_name(ret));
    }
    esp_restart();
}

static esp_err_t settings_system_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "reset_everything_available", true);
        cJSON_AddStringToObject(resp, "confirm_phrase", "RESET EVERYTHING");
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }

    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }

    char buf[192] = {0};
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) {
        return ret;
    }
    cJSON *reset = cJSON_GetObjectItemCaseSensitive(root, "reset_everything");
    cJSON *confirm = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    bool confirmed = cJSON_IsTrue(reset) &&
                     cJSON_IsString(confirm) &&
                     strcmp(confirm->valuestring, "RESET EVERYTHING") == 0;
    cJSON_Delete(root);
    if (!confirmed) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "reset_everything and confirm phrase required");
    }

    si_web_log("WARN", "Reset everything requested");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "erasing settings and rebooting");
    cJSON_AddBoolToObject(resp, "rebooting", true);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    (void)xTaskCreate(system_reset_everything_task, "si_reset_all", 3072, NULL,
                      tskIDLE_PRIORITY + 4, NULL);
    return ret;
}
