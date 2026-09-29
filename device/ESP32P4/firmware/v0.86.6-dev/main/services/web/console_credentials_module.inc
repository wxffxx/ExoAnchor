// Target-host Console credential maintenance. Passwords never leave this local
// settings boundary and are never returned by the HTTP API.
#define CONSOLE_CREDENTIALS_MAX_BODY 512

static void add_console_credentials_json(
    cJSON *root, const si_console_credentials_status_t *status)
{
    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddBoolToObject(root, "configured", status->configured);
    cJSON_AddStringToObject(root, "username", status->username);
    cJSON_AddBoolToObject(root, "password_configured",
                          status->password_configured);
    cJSON_AddStringToObject(root, "scope", "target_console");
    cJSON_AddStringToObject(root, "credential_ref", "console://default");
    cJSON_AddBoolToObject(root, "agent_kvm_login",
                          status->configured &&
                          status->password_configured);
    cJSON_AddStringToObject(root, "keyboard_layout", "us_ascii");
    cJSON_AddStringToObject(
        root, "password_policy",
        "local_secret_not_returned_or_sent_to_agent_api");
}

static esp_err_t send_console_credentials_status(httpd_req_t *req, bool ok)
{
    si_console_credentials_status_t status;
    esp_err_t ret = si_console_credentials_get_status(&status);
    if (ret != ESP_OK) {
        return si_http_send_text_status(req, "500 Internal Server Error",
                                        esp_err_to_name(ret));
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    if (ok) {
        cJSON_AddBoolToObject(resp, "ok", true);
    }
    add_console_credentials_json(resp, &status);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static void clear_console_password_json(cJSON *root)
{
    cJSON *password =
        cJSON_GetObjectItemCaseSensitive(root, "password");
    if (cJSON_IsString(password) && password->valuestring) {
        si_secret_store_clear(password->valuestring,
                              strlen(password->valuestring));
    }
}

static esp_err_t settings_console_credentials_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (req->method == HTTP_GET) {
        return send_console_credentials_status(req, false);
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                                   "method not allowed");
    }
    if (automation_agent_run_active() || si_control_lease_agent_active()) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "stop the active Agent/HID operation before changing Console credentials");
    }
    if (req->content_len <= 0 ||
        req->content_len > CONSOLE_CREDENTIALS_MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "json body required");
    }

    char *buf = calloc(1, CONSOLE_CREDENTIALS_MAX_BODY);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret =
        si_http_recv_json(req, buf, CONSOLE_CREDENTIALS_MAX_BODY, &root);
    if (ret != ESP_OK) {
        si_secret_store_clear(buf, CONSOLE_CREDENTIALS_MAX_BODY);
        free(buf);
        return ret;
    }

    const cJSON *clear_item =
        cJSON_GetObjectItemCaseSensitive(root, "clear");
    if (cJSON_IsTrue(clear_item)) {
        clear_console_password_json(root);
        cJSON_Delete(root);
        si_secret_store_clear(buf, CONSOLE_CREDENTIALS_MAX_BODY);
        free(buf);
        ret = si_console_credentials_clear();
        if (ret != ESP_OK) {
            return si_http_send_text_status(
                req, "500 Internal Server Error", esp_err_to_name(ret));
        }
        return send_console_credentials_status(req, true);
    }

    const char *username =
        json_string_any(root, "username", "user", "login");
    const char *password =
        json_string_any(root, "password", "console_password", NULL);
    const cJSON *clear_password_item =
        cJSON_GetObjectItemCaseSensitive(root, "clear_password");
    bool clear_password = cJSON_IsTrue(clear_password_item);
    bool password_present = password && password[0] != '\0';
    char username_copy[SI_CONSOLE_USERNAME_MAX_LEN] = {0};
    char password_copy[SI_CONSOLE_PASSWORD_MAX_LEN] = {0};

    if (!username || !username[0] ||
        strlen(username) >= sizeof(username_copy)) {
        clear_console_password_json(root);
        cJSON_Delete(root);
        si_secret_store_clear(buf, CONSOLE_CREDENTIALS_MAX_BODY);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "username required");
    }
    strlcpy(username_copy, username, sizeof(username_copy));
    if (password_present) {
        if (strlen(password) >= sizeof(password_copy)) {
            clear_console_password_json(root);
            cJSON_Delete(root);
            si_secret_store_clear(buf, CONSOLE_CREDENTIALS_MAX_BODY);
            free(buf);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "password too long");
        }
        strlcpy(password_copy, password, sizeof(password_copy));
    }

    clear_console_password_json(root);
    cJSON_Delete(root);
    si_secret_store_clear(buf, CONSOLE_CREDENTIALS_MAX_BODY);
    free(buf);

    ret = si_console_credentials_save(
        username_copy, password_present ? password_copy : NULL,
        password_present && !clear_password, clear_password);
    si_secret_store_clear(password_copy, sizeof(password_copy));
    si_secret_store_clear(username_copy, sizeof(username_copy));
    if (ret == ESP_ERR_INVALID_ARG || ret == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid Console credentials");
    }
    if (ret != ESP_OK) {
        return si_http_send_text_status(req, "500 Internal Server Error",
                                        esp_err_to_name(ret));
    }
    return send_console_credentials_status(req, true);
}
