// Managed KVM-credential to SSH-target binding. Included after ssh_module.inc.
static bool ssh_bootstrap_host_valid(const char *host)
{
    if (!host || !host[0] || strlen(host) >= SI_SSH_HOST_MAX_LEN) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)host; *p; p++) {
        if (iscntrl(*p) || isspace(*p)) {
            return false;
        }
    }
    return true;
}

/*
 * Bind an SSH target to credentials already captured by the supervised KVM
 * console flow. The password stays inside the device secret stores: it is
 * neither accepted in this request nor returned in the response.
 */
static esp_err_t ssh_target_from_console_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t ret = si_http_require_action(
        req, SI_CAPABILITY_SSH, "ssh_target_from_console",
        SI_AUTHZ_RISK_HIGH, &session);
    if (ret != ESP_OK) {
        return ret;
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(
            req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }

    si_terminal_control_status_t active_terminal = {0};
    if (automation_agent_run_active() ||
        (terminal_control_get(SI_TERMINAL_CHANNEL_SSH, &active_terminal) &&
         active_terminal.active)) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "stop the active Agent/MCP SSH operation before changing its target");
    }
    if (req->content_len <= 0 || req->content_len > SSH_TARGET_MAX_BODY) {
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char body[SSH_TARGET_MAX_BODY + 1] = {0};
    cJSON *root = NULL;
    ret = si_http_recv_json(req, body, sizeof(body), &root);
    if (ret != ESP_OK) {
        return ret;
    }

    const char *host = json_string_any(root, "host", "ip", NULL);
    const char *credential_ref = json_string_any(
        root, "credential_ref", "credentials", NULL);
    const char *observed_username = json_string_any(
        root, "observed_username", "uart_username", NULL);
    const cJSON *port_item = json_number_any(root, "port", "ssh_port");
    const cJSON *timeout_item = json_number_any(
        root, "timeout_ms", "timeout");
    const cJSON *sudo_item = cJSON_GetObjectItemCaseSensitive(
        root, "reuse_for_sudo");
    bool reuse_for_sudo = !cJSON_IsFalse(sudo_item);
    int port_value = cJSON_IsNumber(port_item) ? port_item->valueint :
                                                SSH_TARGET_DEFAULT_PORT;
    int timeout_value = cJSON_IsNumber(timeout_item) ? timeout_item->valueint :
                                                      SSH_TARGET_DEFAULT_TIMEOUT_MS;

    if (!ssh_bootstrap_host_valid(host)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST, "valid UART-observed host required");
    }
    if (!credential_ref || strcmp(credential_ref, "console://default") != 0) {
        cJSON_Delete(root);
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "credential_ref must be console://default");
    }
    if (!observed_username || !observed_username[0] ||
        strlen(observed_username) >= SI_SSH_USERNAME_MAX_LEN) {
        cJSON_Delete(root);
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "UART-observed username required");
    }
    if (port_value < 1 || port_value > 65535) {
        cJSON_Delete(root);
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST, "port must be 1..65535");
    }
    if (timeout_value < 5000) {
        timeout_value = 5000;
    } else if (timeout_value > (int)SSH_TARGET_MAX_TIMEOUT_MS) {
        timeout_value = (int)SSH_TARGET_MAX_TIMEOUT_MS;
    }

    si_console_credentials_status_t console = {0};
    ret = si_console_credentials_get_status(&console);
    if (ret != ESP_OK || !console.configured ||
        !console.password_configured || !console.username[0] ||
        strlen(console.username) >= SI_SSH_USERNAME_MAX_LEN) {
        cJSON_Delete(root);
        return si_http_send_text_status(
            req, "409 Conflict",
            "KVM console credentials are not configured for SSH reuse");
    }
    if (strcmp(console.username, observed_username) != 0) {
        cJSON_Delete(root);
        return si_http_send_text_status(
            req, "409 Conflict",
            "UART user does not match the KVM console credential user");
    }

    char password[SI_CONSOLE_PASSWORD_MAX_LEN] = {0};
    ret = si_console_credentials_get_password(password, sizeof(password));
    if (ret != ESP_OK || !password[0] ||
        strlen(password) >= SI_SSH_PASSWORD_MAX_LEN) {
        ssh_secure_clear(password, sizeof(password));
        cJSON_Delete(root);
        return si_http_send_text_status(
            req, "409 Conflict", "KVM console password is unavailable");
    }

    ssh_target_settings_t target;
    ssh_target_defaults(&target);
    target.configured = true;
    strlcpy(target.host, host, sizeof(target.host));
    target.port = (uint16_t)port_value;
    strlcpy(target.username, console.username, sizeof(target.username));
    strlcpy(target.auth_method, "password", sizeof(target.auth_method));
    target.timeout_ms = (uint32_t)timeout_value;
    cJSON_Delete(root);

    ret = ssh_target_save(&target, false);
    if (ret == ESP_OK) {
        ret = ssh_target_save_password(password, false);
    }
    if (ret == ESP_OK) {
        ret = reuse_for_sudo ?
            ssh_target_save_sudo_password(password, false) :
            ssh_target_save_sudo_password(NULL, true);
    }
    ssh_secure_clear(password, sizeof(password));
    if (ret != ESP_OK) {
        return si_http_send_text_status(
            req, "500 Internal Server Error", esp_err_to_name(ret));
    }

    ssh_target_settings_t saved;
    ret = ssh_target_load(&saved);
    if (ret != ESP_OK) {
        return si_http_send_text_status(
            req, "500 Internal Server Error", esp_err_to_name(ret));
    }
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddStringToObject(response, "credential_ref", "console://default");
    cJSON_AddBoolToObject(response, "credential_copied_locally", true);
    cJSON_AddBoolToObject(response, "secret_returned", false);
    add_ssh_target_settings_json(response, &saved);
    ret = si_http_send_json(req, response);
    cJSON_Delete(response);
    return ret;
}
