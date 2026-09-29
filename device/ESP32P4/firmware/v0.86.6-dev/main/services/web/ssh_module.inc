// Transitional domain module: SSH settings, HTTP and WebSocket. Kept in the web compilation unit until its service contract is independent.
static void ssh_secure_clear(void *data, size_t len)
{
    static void *(*const volatile memset_fn)(void *, int, size_t) = memset;
    if (data && len > 0) {
        (void)memset_fn(data, 0, len);
    }
}

static void ssh_ws_delete_connect_json(cJSON *root)
{
    if (!root) {
        return;
    }
    static const char *const secret_keys[] = {
        "password", "pass", "key_passphrase", "passphrase",
    };
    for (size_t i = 0; i < sizeof(secret_keys) / sizeof(secret_keys[0]); i++) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, secret_keys[i]);
        if (cJSON_IsString(item) && item->valuestring) {
            ssh_secure_clear(item->valuestring, strlen(item->valuestring));
        }
    }
    cJSON_Delete(root);
}

static bool ssh_target_auth_method_valid(const char *auth_method)
{
    return auth_method &&
           (strcasecmp(auth_method, "auto") == 0 ||
            strcasecmp(auth_method, "key") == 0 ||
            strcasecmp(auth_method, "stored_key") == 0 ||
            strcasecmp(auth_method, "publickey") == 0 ||
            strcasecmp(auth_method, "password") == 0);
}

static const char *ssh_target_normalize_auth_method(const char *auth_method)
{
    if (!auth_method || !auth_method[0]) {
        return "auto";
    }
    if (strcasecmp(auth_method, "stored_key") == 0 ||
        strcasecmp(auth_method, "publickey") == 0) {
        return "key";
    }
    if (strcasecmp(auth_method, "key") == 0) {
        return "key";
    }
    if (strcasecmp(auth_method, "password") == 0) {
        return "password";
    }
    return "auto";
}

static void ssh_target_defaults(ssh_target_settings_t *settings)
{
    if (!settings) {
        return;
    }
    memset(settings, 0, sizeof(*settings));
    settings->port = SSH_TARGET_DEFAULT_PORT;
    settings->timeout_ms = SSH_TARGET_DEFAULT_TIMEOUT_MS;
    strlcpy(settings->auth_method, "auto", sizeof(settings->auth_method));
}

static esp_err_t ssh_secret_is_configured(const char *namespace_name,
                                          const char *key,
                                          bool *configured_out)
{
    if (!configured_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *configured_out = false;
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, namespace_name);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = si_secret_store_is_configured(&store, key, configured_out);
    }
    si_settings_store_close(&store);
    return ret;
}

static esp_err_t ssh_secret_save(const char *namespace_name, const char *key,
                                 const char *value, bool clear,
                                 size_t max_size)
{
    if (!clear && (!value || !value[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!clear && strlen(value) >= max_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(&store, namespace_name);
    if (ret == ESP_OK) {
        ret = clear ? si_secret_store_erase(&store, key) :
                      si_secret_store_set_string(&store, key, value);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}

static esp_err_t ssh_secret_load(const char *namespace_name, const char *key,
                                 char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, namespace_name);
    if (ret == ESP_OK) {
        ret = si_secret_store_get_string(&store, key, out, out_size);
    } else {
        si_secret_store_clear(out, out_size);
    }
    si_settings_store_close(&store);
    return ret;
}

static esp_err_t ssh_target_load(ssh_target_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    ssh_target_defaults(settings);

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret =
        si_settings_store_open_read(&store, SSH_TARGET_NAMESPACE);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = si_settings_store_get_string(
        &store, SSH_TARGET_HOST_KEY, settings->host,
        sizeof(settings->host));
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) goto done;

    ret = si_settings_store_get_string(
        &store, SSH_TARGET_USERNAME_KEY, settings->username,
        sizeof(settings->username));
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) goto done;

    char auth_method[SSH_TARGET_AUTH_MAX_LEN + 1] = {0};
    ret = si_settings_store_get_string(
        &store, SSH_TARGET_AUTH_METHOD_KEY, auth_method,
        sizeof(auth_method));
    if (ret == ESP_OK && ssh_target_auth_method_valid(auth_method)) {
        strlcpy(settings->auth_method, ssh_target_normalize_auth_method(auth_method),
                sizeof(settings->auth_method));
    } else if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) goto done;

    uint16_t port = 0;
    ret = si_settings_store_get_u16(&store, SSH_TARGET_PORT_KEY, &port);
    if (ret == ESP_OK && port > 0) {
        settings->port = port;
    } else if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) goto done;

    uint32_t timeout_ms = 0;
    ret = si_settings_store_get_u32(
        &store, SSH_TARGET_TIMEOUT_MS_KEY, &timeout_ms);
    if (ret == ESP_OK && timeout_ms > 0) {
        if (timeout_ms < 5000U) {
            timeout_ms = 5000U;
        } else if (timeout_ms > SSH_TARGET_MAX_TIMEOUT_MS) {
            timeout_ms = SSH_TARGET_MAX_TIMEOUT_MS;
        }
        settings->timeout_ms = timeout_ms;
    } else if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) goto done;
    ret = ESP_OK;
done:
    si_settings_store_close(&store);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        return ret;
    }
    ret = ssh_secret_is_configured(
        SSH_TARGET_NAMESPACE, SSH_TARGET_PASSWORD_KEY,
        &settings->password_configured);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ssh_secret_is_configured(
        SSH_TARGET_NAMESPACE, SSH_TARGET_SUDO_PASSWORD_KEY,
        &settings->sudo_password_configured);
    if (ret != ESP_OK) {
        return ret;
    }
    settings->configured = settings->host[0] != '\0' && settings->username[0] != '\0';
    return ESP_OK;
}

static esp_err_t ssh_target_save(const ssh_target_settings_t *settings, bool clear)
{
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret =
        si_settings_store_open_write(&store, SSH_TARGET_NAMESPACE);
    if (ret != ESP_OK) {
        return ret;
    }

    if (clear) {
        ret = si_settings_store_erase_all(&store);
    } else if (!settings || !settings->host[0] || !settings->username[0]) {
        ret = ESP_ERR_INVALID_ARG;
    } else {
        ret = si_settings_store_set_string(
            &store, SSH_TARGET_HOST_KEY, settings->host);
        if (ret == ESP_OK) {
            ret = si_settings_store_set_u16(
                &store, SSH_TARGET_PORT_KEY,
                settings->port ? settings->port : SSH_TARGET_DEFAULT_PORT);
        }
        if (ret == ESP_OK) {
            ret = si_settings_store_set_string(
                &store, SSH_TARGET_USERNAME_KEY, settings->username);
        }
        if (ret == ESP_OK) {
            ret = si_settings_store_set_string(
                &store, SSH_TARGET_AUTH_METHOD_KEY,
                ssh_target_normalize_auth_method(settings->auth_method));
        }
        if (ret == ESP_OK) {
            uint32_t timeout_ms = settings->timeout_ms ? settings->timeout_ms :
                                  SSH_TARGET_DEFAULT_TIMEOUT_MS;
            if (timeout_ms < 5000U) {
                timeout_ms = 5000U;
            } else if (timeout_ms > SSH_TARGET_MAX_TIMEOUT_MS) {
                timeout_ms = SSH_TARGET_MAX_TIMEOUT_MS;
            }
            ret = si_settings_store_set_u32(
                &store, SSH_TARGET_TIMEOUT_MS_KEY, timeout_ms);
        }
    }

    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}

static esp_err_t ssh_target_save_password(const char *password, bool clear)
{
    return ssh_secret_save(
        SSH_TARGET_NAMESPACE, SSH_TARGET_PASSWORD_KEY, password,
        clear || !password || !password[0], SI_SSH_PASSWORD_MAX_LEN);
}

static esp_err_t ssh_target_save_sudo_password(const char *password, bool clear)
{
    return ssh_secret_save(
        SSH_TARGET_NAMESPACE, SSH_TARGET_SUDO_PASSWORD_KEY, password,
        clear || !password || !password[0], SI_SSH_PASSWORD_MAX_LEN);
}

static esp_err_t ssh_target_load_password(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ssh_secret_load(
        SSH_TARGET_NAMESPACE, SSH_TARGET_PASSWORD_KEY, out, out_size);
}

static esp_err_t ssh_target_load_sudo_password(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ssh_secret_load(
        SSH_TARGET_NAMESPACE, SSH_TARGET_SUDO_PASSWORD_KEY, out, out_size);
}

static bool ssh_target_apply_password(char *dst, size_t dst_size,
                                      const char *provided_password,
                                      bool *using_stored_password)
{
    if (using_stored_password) {
        *using_stored_password = false;
    }
    if (!dst || dst_size == 0) {
        return false;
    }
    dst[0] = '\0';
    if (provided_password && provided_password[0]) {
        strlcpy(dst, provided_password, dst_size);
        return true;
    }

    char stored_password[SI_SSH_PASSWORD_MAX_LEN] = {0};
    esp_err_t ret = ssh_target_load_password(stored_password, sizeof(stored_password));
    if (ret == ESP_OK && stored_password[0]) {
        strlcpy(dst, stored_password, dst_size);
        memset(stored_password, 0, sizeof(stored_password));
        if (using_stored_password) {
            *using_stored_password = true;
        }
        return true;
    }
    memset(stored_password, 0, sizeof(stored_password));
    return false;
}

static bool ssh_target_apply_sudo_password(char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return false;
    }
    dst[0] = '\0';
    char stored_password[SI_SSH_PASSWORD_MAX_LEN] = {0};
    esp_err_t ret = ssh_target_load_sudo_password(stored_password,
                                                  sizeof(stored_password));
    if (ret == ESP_OK && stored_password[0]) {
        strlcpy(dst, stored_password, dst_size);
        memset(stored_password, 0, sizeof(stored_password));
        return true;
    }
    memset(stored_password, 0, sizeof(stored_password));
    return false;
}

static bool ssh_command_word_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static bool ssh_command_has_sudo_token(const char *command)
{
    if (!command) {
        return false;
    }
    for (const char *p = command; *p; ++p) {
        if ((p == command || !ssh_command_word_char(p[-1])) &&
            strncmp(p, "sudo", 4) == 0 &&
            !ssh_command_word_char(p[4])) {
            return true;
        }
    }
    return false;
}

static bool shell_single_quote(const char *input, char *out, size_t out_size)
{
    if (!input || !out || out_size < 3) {
        return false;
    }
    size_t used = 0;
    out[used++] = '\'';
    for (const char *p = input; *p; ++p) {
        if (*p == '\'') {
            const char *escaped = "'\\''";
            size_t len = strlen(escaped);
            if (used + len >= out_size) {
                return false;
            }
            memcpy(out + used, escaped, len);
            used += len;
        } else {
            if (used + 1 >= out_size) {
                return false;
            }
            out[used++] = *p;
        }
    }
    if (used + 2 > out_size) {
        return false;
    }
    out[used++] = '\'';
    out[used] = '\0';
    return true;
}

static bool ssh_prepare_sudo_command(const char *command,
                                     const char *sudo_password,
                                     si_ssh_exec_config_t *config,
                                     bool *sudo_applied)
{
    if (sudo_applied) {
        *sudo_applied = false;
    }
    if (!command || !config) {
        return false;
    }
    if (!sudo_password || !sudo_password[0] ||
        !ssh_command_has_sudo_token(command)) {
        strlcpy(config->command, command, sizeof(config->command));
        return true;
    }

    char quoted[SI_SSH_COMMAND_MAX_LEN] = {0};
    if (!shell_single_quote(command, quoted, sizeof(quoted))) {
        return false;
    }

    int written = snprintf(config->command, sizeof(config->command),
                           "sudo -S -p '' sh -lc %s", quoted);
    if (written < 0 || (size_t)written >= sizeof(config->command)) {
        return false;
    }
    snprintf(config->stdin_data, sizeof(config->stdin_data), "%s\n",
             sudo_password);
    config->close_stdin_after_write = true;
    if (sudo_applied) {
        *sudo_applied = true;
    }
    return true;
}

static bool ssh_key_string_exists(const char *key);

static void add_ssh_target_settings_json(cJSON *root, const ssh_target_settings_t *settings)
{
    ssh_target_settings_t loaded;
    if (!settings) {
        ssh_target_load(&loaded);
        settings = &loaded;
    }
    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddBoolToObject(root, "configured", settings->configured);
    cJSON_AddStringToObject(root, "host", settings->host);
    cJSON_AddNumberToObject(root, "port", settings->port);
    cJSON_AddStringToObject(root, "username", settings->username);
    cJSON_AddStringToObject(root, "auth_method", settings->auth_method);
    cJSON_AddNumberToObject(root, "timeout_ms", settings->timeout_ms);
    cJSON_AddBoolToObject(root, "private_key_configured",
                          ssh_key_string_exists(SSH_KEY_PRIVATE_KEY));
    cJSON_AddBoolToObject(root, "password_configured", settings->password_configured);
    cJSON_AddBoolToObject(root, "sudo_password_configured",
                          settings->sudo_password_configured);
    cJSON_AddStringToObject(root, "password_storage", "device_local_secret_store");
    cJSON_AddStringToObject(root, "password_policy", "local_only_not_sent_to_agent_api");
    cJSON_AddStringToObject(root, "sudo_password_policy",
                            "local_only_used_for_sudo_stdin_not_sent_to_agent_api");
}

static esp_err_t settings_ssh_target_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    if (req->method == HTTP_GET) {
        ssh_target_settings_t settings;
        esp_err_t ret = ssh_target_load(&settings);
        if (ret != ESP_OK) {
            return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
        }
        cJSON *resp = cJSON_CreateObject();
        add_ssh_target_settings_json(resp, &settings);
        ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }

    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
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
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char *buf = calloc(1, SSH_TARGET_MAX_BODY);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, SSH_TARGET_MAX_BODY, &root);
    free(buf);
    if (ret != ESP_OK) {
        return ret;
    }

    cJSON *clear_item = cJSON_GetObjectItemCaseSensitive(root, "clear");
    if (cJSON_IsTrue(clear_item)) {
        cJSON_Delete(root);
        ret = ssh_target_save(NULL, true);
        if (ret != ESP_OK) {
            return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
        }
        ssh_target_settings_t settings;
        ssh_target_load(&settings);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", true);
        add_ssh_target_settings_json(resp, &settings);
        esp_err_t send_ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return send_ret;
    }

    const char *host = json_string_any(root, "host", "ip", "target");
    const char *username = json_string_any(root, "username", "user", "login");
    const char *password = json_string_any(root, "password", "ssh_password", "target_password");
    const char *sudo_password = json_string_any(root, "sudo_password", "sudo_pass", "root_password");
    const char *auth_method = json_string_any(root, "auth_method", "auth", NULL);
    const cJSON *clear_password_item = cJSON_GetObjectItemCaseSensitive(root, "clear_password");
    const cJSON *password_clear_item = cJSON_GetObjectItemCaseSensitive(root, "password_clear");
    const cJSON *clear_sudo_password_item = cJSON_GetObjectItemCaseSensitive(root, "clear_sudo_password");
    const cJSON *sudo_password_clear_item = cJSON_GetObjectItemCaseSensitive(root, "sudo_password_clear");
    const cJSON *port_item = json_number_any(root, "port", "ssh_port");
    const cJSON *timeout_item = json_number_any(root, "timeout_ms", "timeout");
    bool clear_password = cJSON_IsTrue(clear_password_item) ||
                          cJSON_IsTrue(password_clear_item);
    bool clear_sudo_password = cJSON_IsTrue(clear_sudo_password_item) ||
                               cJSON_IsTrue(sudo_password_clear_item);
    char password_update[SI_SSH_PASSWORD_MAX_LEN] = {0};
    char sudo_password_update[SI_SSH_PASSWORD_MAX_LEN] = {0};

    if (!host || !host[0] || strlen(host) >= SI_SSH_HOST_MAX_LEN ||
        !username || !username[0] || strlen(username) >= SI_SSH_USERNAME_MAX_LEN) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "host and username required");
    }
    if (password && password[0]) {
        if (strlen(password) >= SI_SSH_PASSWORD_MAX_LEN) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "password too long");
        }
        if (!clear_password) {
            strlcpy(password_update, password, sizeof(password_update));
        }
    }
    if (sudo_password && sudo_password[0]) {
        if (strlen(sudo_password) >= SI_SSH_PASSWORD_MAX_LEN) {
            memset(password_update, 0, sizeof(password_update));
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "sudo password too long");
        }
        if (!clear_sudo_password) {
            strlcpy(sudo_password_update, sudo_password,
                    sizeof(sudo_password_update));
        }
    }
    if (auth_method && !ssh_target_auth_method_valid(auth_method)) {
        memset(password_update, 0, sizeof(password_update));
        memset(sudo_password_update, 0, sizeof(sudo_password_update));
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "auth_method must be auto, key, or password");
    }

    int port_value = port_item && port_item->valueint > 0 ? port_item->valueint :
                     SSH_TARGET_DEFAULT_PORT;
    if (port_value < 1 || port_value > 65535) {
        memset(password_update, 0, sizeof(password_update));
        memset(sudo_password_update, 0, sizeof(sudo_password_update));
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "port must be 1..65535");
    }
    int timeout_value = timeout_item && timeout_item->valueint > 0 ? timeout_item->valueint :
                        SSH_TARGET_DEFAULT_TIMEOUT_MS;
    if (timeout_value < 5000) {
        timeout_value = 5000;
    } else if (timeout_value > (int)SSH_TARGET_MAX_TIMEOUT_MS) {
        timeout_value = (int)SSH_TARGET_MAX_TIMEOUT_MS;
    }

    ssh_target_settings_t settings;
    ssh_target_defaults(&settings);
    settings.configured = true;
    strlcpy(settings.host, host, sizeof(settings.host));
    settings.port = (uint16_t)port_value;
    strlcpy(settings.username, username, sizeof(settings.username));
    strlcpy(settings.auth_method, ssh_target_normalize_auth_method(auth_method),
            sizeof(settings.auth_method));
    settings.timeout_ms = (uint32_t)timeout_value;
    cJSON_Delete(root);

    ret = ssh_target_save(&settings, false);
    if (ret == ESP_OK && clear_password) {
        ret = ssh_target_save_password(NULL, true);
    } else if (ret == ESP_OK && password_update[0]) {
        ret = ssh_target_save_password(password_update, false);
    }
    if (ret == ESP_OK && clear_sudo_password) {
        ret = ssh_target_save_sudo_password(NULL, true);
    } else if (ret == ESP_OK && sudo_password_update[0]) {
        ret = ssh_target_save_sudo_password(sudo_password_update, false);
    }
    memset(password_update, 0, sizeof(password_update));
    memset(sudo_password_update, 0, sizeof(sudo_password_update));
    if (ret != ESP_OK) {
        return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
    }

    ssh_target_settings_t saved_settings;
    ret = ssh_target_load(&saved_settings);
    if (ret != ESP_OK) {
        return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    add_ssh_target_settings_json(resp, &saved_settings);
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}

static bool ssh_key_string_exists(const char *key)
{
    bool configured = false;
    return ssh_secret_is_configured(
               SSH_KEY_NAMESPACE, key, &configured) == ESP_OK &&
           configured;
}

bool si_web_ssh_target_available(void)
{
    ssh_target_settings_t target;
    ssh_target_defaults(&target);
    return ssh_target_load(&target) == ESP_OK &&
           target.configured &&
           (target.password_configured ||
            ssh_key_string_exists(SSH_KEY_PRIVATE_KEY));
}

static esp_err_t ssh_key_load_string(const char *key, char *out, size_t out_size)
{
    if (!key || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ssh_secret_load(SSH_KEY_NAMESPACE, key, out, out_size);
}

static bool ssh_private_key_looks_valid(const char *text)
{
    if (!text || !text[0]) {
        return false;
    }
    return strstr(text, "BEGIN OPENSSH PRIVATE KEY") ||
           (strstr(text, "BEGIN ") && strstr(text, "PRIVATE KEY"));
}

static bool ssh_public_key_looks_valid(const char *text)
{
    if (!text || !text[0]) {
        return true;
    }
    return strncmp(text, "ssh-", 4) == 0 ||
           strncmp(text, "ecdsa-", 6) == 0 ||
           strncmp(text, "sk-", 3) == 0;
}

static esp_err_t ssh_key_apply_to_exec(si_ssh_exec_config_t *config,
                                       const char *passphrase_override)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ssh_key_load_string(SSH_KEY_PRIVATE_KEY,
                                        config->private_key,
                                        sizeof(config->private_key));
    if (ret != ESP_OK || !config->private_key[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    (void)ssh_key_load_string(SSH_KEY_PUBLIC_KEY,
                              config->public_key,
                              sizeof(config->public_key));
    if (passphrase_override && passphrase_override[0]) {
        strlcpy(config->key_passphrase, passphrase_override,
                sizeof(config->key_passphrase));
    } else {
        (void)ssh_key_load_string(SSH_KEY_PASSPHRASE_KEY,
                                  config->key_passphrase,
                                  sizeof(config->key_passphrase));
    }
    config->use_private_key = true;
    return ESP_OK;
}

static esp_err_t ssh_key_apply_to_shell(si_ssh_shell_config_t *config,
                                        const char *passphrase_override)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ssh_key_load_string(SSH_KEY_PRIVATE_KEY,
                                        config->private_key,
                                        sizeof(config->private_key));
    if (ret != ESP_OK || !config->private_key[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    (void)ssh_key_load_string(SSH_KEY_PUBLIC_KEY,
                              config->public_key,
                              sizeof(config->public_key));
    if (passphrase_override && passphrase_override[0]) {
        strlcpy(config->key_passphrase, passphrase_override,
                sizeof(config->key_passphrase));
    } else {
        (void)ssh_key_load_string(SSH_KEY_PASSPHRASE_KEY,
                                  config->key_passphrase,
                                  sizeof(config->key_passphrase));
    }
    config->use_private_key = true;
    return ESP_OK;
}

static void add_ssh_key_settings_json(cJSON *root)
{
    bool private_configured = ssh_key_string_exists(SSH_KEY_PRIVATE_KEY);
    bool public_configured = ssh_key_string_exists(SSH_KEY_PUBLIC_KEY);
    bool passphrase_configured = ssh_key_string_exists(SSH_KEY_PASSPHRASE_KEY);
    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddBoolToObject(root, "private_key_configured", private_configured);
    cJSON_AddBoolToObject(root, "public_key_configured", public_configured);
    cJSON_AddBoolToObject(root, "passphrase_configured", passphrase_configured);
    cJSON_AddNumberToObject(root, "private_key_max_length", SI_SSH_PRIVATE_KEY_MAX_LEN - 1);
    cJSON_AddNumberToObject(root, "public_key_max_length", SI_SSH_PUBLIC_KEY_MAX_LEN - 1);
    cJSON_AddNumberToObject(root, "passphrase_max_length", SI_SSH_KEY_PASSPHRASE_MAX_LEN - 1);
    cJSON_AddStringToObject(root, "auth_method", private_configured ? "key" : "password");
    char public_key[SI_SSH_PUBLIC_KEY_MAX_LEN] = {0};
    if (public_configured &&
        ssh_key_load_string(SSH_KEY_PUBLIC_KEY, public_key, sizeof(public_key)) == ESP_OK) {
        cJSON_AddStringToObject(root, "public_key", public_key);
    } else {
        cJSON_AddStringToObject(root, "public_key", "");
    }
}

static esp_err_t settings_ssh_key_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        add_ssh_key_settings_json(resp);
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }

    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }
    si_terminal_control_status_t active_terminal = {0};
    if (automation_agent_run_active() ||
        (terminal_control_get(SI_TERMINAL_CHANNEL_SSH, &active_terminal) &&
         active_terminal.active)) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "stop the active Agent/MCP SSH operation before changing its key");
    }
    if (req->content_len <= 0 || req->content_len > SSH_KEY_MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char *buf = calloc(1, SSH_KEY_MAX_BODY);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, SSH_KEY_MAX_BODY, &root);
    free(buf);
    if (ret != ESP_OK) {
        return ret;
    }

    cJSON *clear_item = cJSON_GetObjectItemCaseSensitive(root, "clear");
    cJSON *private_item = cJSON_GetObjectItemCaseSensitive(root, "private_key");
    cJSON *public_item = cJSON_GetObjectItemCaseSensitive(root, "public_key");
    cJSON *passphrase_item = cJSON_GetObjectItemCaseSensitive(root, "passphrase");

    const char *private_key = cJSON_IsString(private_item) ? private_item->valuestring : NULL;
    const char *public_key = cJSON_IsString(public_item) ? public_item->valuestring : NULL;
    const char *passphrase = cJSON_IsString(passphrase_item) ? passphrase_item->valuestring : NULL;

    if (private_key && private_key[0]) {
        if (strlen(private_key) >= SI_SSH_PRIVATE_KEY_MAX_LEN ||
            !ssh_private_key_looks_valid(private_key)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "private_key must be an OpenSSH/PEM private key");
        }
    }
    if (public_key && public_key[0]) {
        if (strlen(public_key) >= SI_SSH_PUBLIC_KEY_MAX_LEN ||
            !ssh_public_key_looks_valid(public_key)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "public_key must be an OpenSSH public key");
        }
    }
    if (passphrase && strlen(passphrase) >= SI_SSH_KEY_PASSPHRASE_MAX_LEN) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "passphrase too long");
    }

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ret = si_settings_store_open_write(&store, SSH_KEY_NAMESPACE);
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
    }

    if (cJSON_IsTrue(clear_item)) {
        ret = si_secret_store_erase(&store, SSH_KEY_PRIVATE_KEY);
        if (ret == ESP_OK) {
            ret = si_secret_store_erase(&store, SSH_KEY_PUBLIC_KEY);
        }
        if (ret == ESP_OK) {
            ret = si_secret_store_erase(&store, SSH_KEY_PASSPHRASE_KEY);
        }
    } else {
        if (ret == ESP_OK && private_key && private_key[0]) {
            ret = si_secret_store_set_string(
                &store, SSH_KEY_PRIVATE_KEY, private_key);
        }
        if (ret == ESP_OK && public_key) {
            if (public_key[0]) {
                ret = si_secret_store_set_string(
                    &store, SSH_KEY_PUBLIC_KEY, public_key);
            } else {
                ret = si_secret_store_erase(&store, SSH_KEY_PUBLIC_KEY);
            }
        }
        if (ret == ESP_OK && passphrase) {
            if (passphrase[0]) {
                ret = si_secret_store_set_string(
                    &store, SSH_KEY_PASSPHRASE_KEY, passphrase);
            } else {
                ret = si_secret_store_erase(
                    &store, SSH_KEY_PASSPHRASE_KEY);
            }
        }
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);

    if (private_item && cJSON_IsString(private_item) && private_item->valuestring) {
        memset(private_item->valuestring, 0, strlen(private_item->valuestring));
    }
    if (passphrase_item && cJSON_IsString(passphrase_item) && passphrase_item->valuestring) {
        memset(passphrase_item->valuestring, 0, strlen(passphrase_item->valuestring));
    }
    cJSON_Delete(root);

    if (ret != ESP_OK) {
        return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    add_ssh_key_settings_json(resp);
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}

static esp_err_t ssh_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddStringToObject(root, "backend", "libssh2_esp");
    cJSON_AddBoolToObject(root, "password_auth", true);
    cJSON_AddBoolToObject(root, "public_key_auth", true);
    cJSON_AddBoolToObject(root, "private_key_configured",
                          ssh_key_string_exists(SSH_KEY_PRIVATE_KEY));
    cJSON_AddBoolToObject(root, "pty", true);
    cJSON_AddBoolToObject(root, "host_key_check", true);
    cJSON_AddStringToObject(root, "host_key_policy", "tofu-sha256");
    cJSON_AddNumberToObject(root, "max_timeout_ms", SSH_TARGET_MAX_TIMEOUT_MS);
    ssh_target_settings_t target;
    if (ssh_target_load(&target) == ESP_OK) {
        cJSON *target_json = cJSON_AddObjectToObject(root, "target");
        add_ssh_target_settings_json(target_json, &target);
    }
    terminal_control_add_status_json(root, SI_TERMINAL_CHANNEL_SSH);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t ssh_exec_handler(httpd_req_t *req)
{
    int64_t started_us = esp_timer_get_time();
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret =
        si_http_require_action(req, SI_CAPABILITY_SSH, "ssh_exec",
                               SI_AUTHZ_RISK_MEDIUM, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char *buf = calloc(1, 2048);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, 2048, &root);
    free(buf);
    if (ret != ESP_OK) {
        return ret;
    }

    const char *host = json_string_any(root, "host", "ip", "target");
    const char *username = json_string_any(root, "username", "user", "login");
    const char *password = json_string_any(root, "password", "pass", NULL);
    const char *command = json_string_any(root, "command", "cmd", "input");
    const char *auth_method = json_string_any(root, "auth_method", "auth", NULL);
    const char *key_passphrase = json_string_any(root, "key_passphrase", "passphrase", NULL);
    const cJSON *port_item = json_number_any(root, "port", "ssh_port");
    const cJSON *timeout_item = json_number_any(root, "timeout_ms", "timeout");

    ssh_target_settings_t target;
    ret = ssh_target_load(&target);
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
    }
    bool using_stored_target = false;
    if ((!host || !host[0]) && target.host[0]) {
        host = target.host;
        using_stored_target = true;
    }
    if ((!username || !username[0]) && target.username[0]) {
        username = target.username;
        using_stored_target = true;
    }
    if ((!auth_method || !auth_method[0]) && target.auth_method[0]) {
        auth_method = target.auth_method;
    }

    if (!host || !host[0] || !username || !username[0] || !command || !command[0]) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "ssh target not configured; host, username and command required");
    }

    si_ssh_exec_config_t config = {0};
    strlcpy(config.host, host, sizeof(config.host));
    config.port = port_item && port_item->valueint > 0 ? (uint16_t)port_item->valueint :
                  (target.port ? target.port : SSH_TARGET_DEFAULT_PORT);
    strlcpy(config.username, username, sizeof(config.username));
    bool using_stored_password = false;
    bool has_password = ssh_target_apply_password(config.password, sizeof(config.password),
                                                  password, &using_stored_password);
    char sudo_password_buf[SI_SSH_PASSWORD_MAX_LEN] = {0};
    bool sudo_password_available = ssh_target_apply_sudo_password(sudo_password_buf,
                                                                  sizeof(sudo_password_buf));
    bool sudo_applied = false;
    if (!ssh_prepare_sudo_command(command,
                                  sudo_password_available ? sudo_password_buf : NULL,
                                  &config, &sudo_applied)) {
        cJSON_Delete(root);
        memset(&config, 0, sizeof(config));
        memset(sudo_password_buf, 0, sizeof(sudo_password_buf));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "ssh command too long after sudo wrapping");
    }
    memset(sudo_password_buf, 0, sizeof(sudo_password_buf));
    bool use_key = auth_method && (strcasecmp(auth_method, "key") == 0 ||
                                   strcasecmp(auth_method, "stored_key") == 0 ||
                                   strcasecmp(auth_method, "publickey") == 0);
    bool auto_key = (!auth_method || strcasecmp(auth_method, "auto") == 0) &&
                    !has_password;
    if (use_key || auto_key) {
        ret = ssh_key_apply_to_exec(&config, key_passphrase);
        if (ret != ESP_OK && use_key) {
            cJSON_Delete(root);
            memset(&config, 0, sizeof(config));
            return si_http_send_text_status(req, "409 Conflict", "ssh private key not configured");
        }
        if (ret != ESP_OK && auto_key) {
            cJSON_Delete(root);
            memset(&config, 0, sizeof(config));
            return si_http_send_text_status(req, "409 Conflict",
                                    "ssh credential not configured in Settings");
        }
    } else if (!has_password && auth_method &&
               strcasecmp(auth_method, "password") == 0) {
        cJSON_Delete(root);
        memset(&config, 0, sizeof(config));
        return si_http_send_text_status(req, "409 Conflict",
                                "ssh password not configured in Settings");
    }
    int timeout_value = timeout_item && timeout_item->valueint > 0 ? timeout_item->valueint :
                        (int)(target.timeout_ms ? target.timeout_ms :
                              SSH_TARGET_DEFAULT_TIMEOUT_MS);
    if (timeout_value < 5000) {
        timeout_value = 5000;
    } else if (timeout_value > (int)SSH_TARGET_MAX_TIMEOUT_MS) {
        timeout_value = (int)SSH_TARGET_MAX_TIMEOUT_MS;
    }
    config.timeout_ms = (uint32_t)timeout_value;
    si_terminal_control_token_t control = {0};
    bool automated = session.principal == SI_PRINCIPAL_MCP;
    if (automated && !terminal_control_begin(
            SI_TERMINAL_CHANNEL_SSH, SI_TERMINAL_ACTOR_MCP, "ssh_exec",
            NULL, "执行 SSH 命令", &control)) {
        cJSON_Delete(root);
        ssh_secure_clear(&config, sizeof(config));
        return si_http_send_text_status(req, "409 Conflict",
                                        "SSH automation already active");
    }
    if (automated) terminal_control_publish_input(
        &control, command, true, terminal_control_text_sensitive(command));
    cJSON_Delete(root);

    char *output = heap_caps_calloc(1, 8193, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output) {
        output = calloc(1, 8193);
    }
    if (!output) {
        if (automated) (void)terminal_control_finish(&control);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "output alloc failed");
    }

    si_ssh_exec_result_t result = {
        .output = output,
        .output_size = 8193,
        .exit_status = -1,
    };
    if (automated) {
        config.output_cb = terminal_ssh_exec_output_cb;
        config.output_user_ctx = &control;
        config.cancel_cb = terminal_ssh_exec_cancel_cb;
        config.cancel_user_ctx = &control;
    }
    ret = si_ssh_exec(&config, &result);
    if (automated) (void)terminal_control_finish(&control);
    int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    if (elapsed_ms > 1000) {
        ESP_LOGI(TAG, "SSH exec %s@%s:%u took %" PRId64 " ms status=%d ret=%s",
                 config.username, config.host, config.port, elapsed_ms,
                 result.exit_status, esp_err_to_name(ret));
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ret == ESP_OK && result.ok);
    cJSON_AddStringToObject(resp, "backend", "libssh2_esp");
    cJSON_AddStringToObject(resp, "auth_method", config.use_private_key ? "key" : "password");
    cJSON_AddStringToObject(resp, "credential_source",
                            config.use_private_key ? "stored_key" :
                            (using_stored_password ? "settings_password" : "request_password"));
    cJSON_AddBoolToObject(resp, "sudo_password_configured", target.sudo_password_configured);
    cJSON_AddBoolToObject(resp, "sudo_applied", sudo_applied);
    cJSON_AddStringToObject(resp, "target_source", using_stored_target ? "settings" : "request");
    cJSON_AddStringToObject(resp, "host", config.host);
    cJSON_AddNumberToObject(resp, "port", config.port);
    cJSON_AddStringToObject(resp, "username", config.username);
    cJSON_AddNumberToObject(resp, "exit_status", result.exit_status);
    cJSON_AddNumberToObject(resp, "ssh_rc", result.ssh_rc);
    cJSON_AddNumberToObject(resp, "elapsed_ms", result.elapsed_ms ? result.elapsed_ms : elapsed_ms);
    cJSON_AddBoolToObject(resp, "truncated", result.truncated);
    cJSON_AddStringToObject(resp, "output", result.output ? result.output : "");
    if (result.error[0]) {
        cJSON_AddStringToObject(resp, "error", result.error);
    }
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    memset(config.password, 0, sizeof(config.password));
    memset(config.stdin_data, 0, sizeof(config.stdin_data));
    memset(config.public_key, 0, sizeof(config.public_key));
    memset(config.private_key, 0, sizeof(config.private_key));
    memset(config.key_passphrase, 0, sizeof(config.key_passphrase));
    free(output);
    return send_ret;
}

#ifdef CONFIG_HTTPD_WS_SUPPORT
typedef struct {
    httpd_handle_t handle;
    int fd;
    si_ssh_shell_t *shell;
} ssh_ws_ctx_t;

static SemaphoreHandle_t s_ssh_ws_lock;
static httpd_handle_t s_ssh_ws_handle;
static int s_ssh_ws_fd = -1;

typedef struct {
    httpd_handle_t handle;
    int fd;
    size_t len;
    uint8_t payload[];
} ssh_ws_send_job_t;

#define SSH_WS_CLEANUP_TASK_STACK 4096

static void ssh_ws_send_work(void *arg)
{
    ssh_ws_send_job_t *job = (ssh_ws_send_job_t *)arg;
    if (!job) {
        return;
    }
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = job->payload,
        .len = job->len,
    };
    (void)httpd_ws_send_frame_async(job->handle, job->fd, &frame);
    free(job);
}

static esp_err_t ssh_ws_send_async(httpd_handle_t handle, int fd, const void *data, size_t len)
{
    if (!handle || fd < 0 || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ssh_ws_send_job_t *job = malloc(sizeof(*job) + len);
    if (!job) {
        return ESP_ERR_NO_MEM;
    }
    job->handle = handle;
    job->fd = fd;
    job->len = len;
    memcpy(job->payload, data, len);
    esp_err_t ret = httpd_queue_work(handle, ssh_ws_send_work, job);
    if (ret != ESP_OK) {
        free(job);
    }
    return ret;
}

static esp_err_t ssh_ws_send_terminal_json(const char *json, size_t len)
{
    httpd_handle_t handle = NULL;
    int fd = -1;
    if (!json || len == 0 || !s_ssh_ws_lock ||
        xSemaphoreTake(s_ssh_ws_lock, pdMS_TO_TICKS(20)) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    handle = s_ssh_ws_handle;
    fd = s_ssh_ws_fd;
    xSemaphoreGive(s_ssh_ws_lock);
    return handle && fd >= 0 ? ssh_ws_send_async(handle, fd, json, len) :
                              ESP_ERR_NOT_FOUND;
}

static esp_err_t ssh_ws_send_terminal_output(const void *data, size_t len)
{
    return ssh_ws_send_terminal_json((const char *)data, len);
}

static esp_err_t ssh_ws_send_str(ssh_ws_ctx_t *ctx, const char *text)
{
    return text ? ssh_ws_send_async(ctx->handle, ctx->fd, text, strlen(text)) : ESP_ERR_INVALID_ARG;
}

static esp_err_t ssh_ws_output_cb(const uint8_t *data, size_t len, void *user_ctx)
{
    ssh_ws_ctx_t *ctx = (ssh_ws_ctx_t *)user_ctx;
    if (!ctx || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ssh_ws_send_async(ctx->handle, ctx->fd, data, len);
}

static void ssh_ws_ctx_cleanup_task(void *arg)
{
    ssh_ws_ctx_t *ctx = (ssh_ws_ctx_t *)arg;
    if (ctx) {
        if (ctx->shell) {
            si_ssh_shell_stop(ctx->shell);
            ctx->shell = NULL;
        }
        free(ctx);
    }
    vTaskDeleteWithCaps(NULL);
}

static void ssh_ws_ctx_free(void *ptr)
{
    ssh_ws_ctx_t *ctx = (ssh_ws_ctx_t *)ptr;
    if (!ctx) {
        return;
    }
    if (s_ssh_ws_lock &&
        xSemaphoreTake(s_ssh_ws_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_ssh_ws_handle == ctx->handle && s_ssh_ws_fd == ctx->fd) {
            s_ssh_ws_handle = NULL;
            s_ssh_ws_fd = -1;
        }
        xSemaphoreGive(s_ssh_ws_lock);
    }
    if (ctx->shell) {
        /*
         * Session cleanup can wait for a network select/SSH handshake. Never
         * make the single main HTTP task wait here, otherwise closing Terminal
         * can stall page navigation and every API on the device.
         */
        BaseType_t task_ok = xTaskCreateWithCaps(
            ssh_ws_ctx_cleanup_task, "si_ssh_cleanup",
            SSH_WS_CLEANUP_TASK_STACK, ctx, tskIDLE_PRIORITY + 2, NULL,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (task_ok == pdPASS) {
            return;
        }
        ESP_LOGE(TAG, "SSH cleanup task unavailable; using synchronous cleanup");
        si_ssh_shell_stop(ctx->shell);
        ctx->shell = NULL;
    }
    free(ctx);
}

static esp_err_t ssh_ws_start_shell(ssh_ws_ctx_t *ctx, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ssh_ws_send_str(ctx, "\r\n[ssh: connect json required]\r\n");
    }

    const char *host = json_string_any(root, "host", "ip", "target");
    const char *username = json_string_any(root, "username", "user", "login");
    const char *password = json_string_any(root, "password", "pass", NULL);
    const char *auth_method = json_string_any(root, "auth_method", "auth", NULL);
    const char *key_passphrase = json_string_any(root, "key_passphrase", "passphrase", NULL);
    const cJSON *port_item = json_number_any(root, "port", "ssh_port");
    const cJSON *timeout_item = json_number_any(root, "timeout_ms", "timeout");
    const cJSON *cols_item = json_number_any(root, "cols", "columns");
    const cJSON *rows_item = json_number_any(root, "rows", "height");

    ssh_target_settings_t target;
    esp_err_t ret = ssh_target_load(&target);
    if (ret != ESP_OK) {
        ssh_ws_delete_connect_json(root);
        char msg[80];
        snprintf(msg, sizeof(msg), "\r\n[ssh: target load failed: %s]\r\n",
                 esp_err_to_name(ret));
        return ssh_ws_send_str(ctx, msg);
    }
    if ((!host || !host[0]) && target.host[0]) {
        host = target.host;
    }
    if ((!username || !username[0]) && target.username[0]) {
        username = target.username;
    }
    if ((!auth_method || !auth_method[0]) && target.auth_method[0]) {
        auth_method = target.auth_method;
    }

    if (!host || !host[0] || !username || !username[0]) {
        ssh_ws_delete_connect_json(root);
        return ssh_ws_send_str(ctx, "\r\n[ssh: target not configured]\r\n");
    }

    si_ssh_shell_config_t config = {0};
    strlcpy(config.host, host, sizeof(config.host));
    config.port = port_item && port_item->valueint > 0 ? (uint16_t)port_item->valueint :
                  (target.port ? target.port : SSH_TARGET_DEFAULT_PORT);
    strlcpy(config.username, username, sizeof(config.username));
    bool using_stored_password = false;
    bool has_password = ssh_target_apply_password(config.password, sizeof(config.password),
                                                  password, &using_stored_password);
    (void)using_stored_password;
    bool use_key = auth_method && (strcasecmp(auth_method, "key") == 0 ||
                                   strcasecmp(auth_method, "stored_key") == 0 ||
                                   strcasecmp(auth_method, "publickey") == 0);
    bool auto_key = (!auth_method || strcasecmp(auth_method, "auto") == 0) &&
                    !has_password;
    if (use_key || auto_key) {
        esp_err_t key_ret = ssh_key_apply_to_shell(&config, key_passphrase);
        if (key_ret != ESP_OK && use_key) {
            ssh_ws_delete_connect_json(root);
            ssh_secure_clear(&config, sizeof(config));
            return ssh_ws_send_str(ctx, "\r\n[ssh: private key not configured]\r\n");
        }
        if (key_ret != ESP_OK && auto_key) {
            ssh_ws_delete_connect_json(root);
            ssh_secure_clear(&config, sizeof(config));
            return ssh_ws_send_str(ctx, "\r\n[ssh: credential not configured in Settings]\r\n");
        }
    } else if (!has_password && auth_method &&
               strcasecmp(auth_method, "password") == 0) {
        ssh_ws_delete_connect_json(root);
        ssh_secure_clear(&config, sizeof(config));
        return ssh_ws_send_str(ctx, "\r\n[ssh: password not configured in Settings]\r\n");
    }
    int timeout_value = timeout_item && timeout_item->valueint > 0 ? timeout_item->valueint :
                        (int)(target.timeout_ms ? target.timeout_ms :
                              SSH_TARGET_DEFAULT_TIMEOUT_MS);
    if (timeout_value < 5000) {
        timeout_value = 5000;
    } else if (timeout_value > (int)SSH_TARGET_MAX_TIMEOUT_MS) {
        timeout_value = (int)SSH_TARGET_MAX_TIMEOUT_MS;
    }
    config.timeout_ms = (uint32_t)timeout_value;
    config.cols = cols_item && cols_item->valueint > 0 ? (uint16_t)cols_item->valueint : 120;
    config.rows = rows_item && rows_item->valueint > 0 ? (uint16_t)rows_item->valueint : 32;
    ssh_ws_delete_connect_json(root);

    ret = si_ssh_shell_start(&config, ssh_ws_output_cb, ctx, &ctx->shell);
    ssh_secure_clear(&config, sizeof(config));
    if (ret != ESP_OK) {
        char msg[80];
        snprintf(msg, sizeof(msg), "\r\n[ssh: start failed: %s]\r\n", esp_err_to_name(ret));
        return ssh_ws_send_str(ctx, msg);
    }
    return ssh_ws_send_str(ctx, "\r\n[ssh: connecting]\r\n");
}

static esp_err_t ssh_ws_handler(httpd_req_t *req)
{
    ssh_ws_ctx_t *ctx = (ssh_ws_ctx_t *)req->sess_ctx;
    if (!ctx) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ssh_ws_ctx_free(ctx);
        req->sess_ctx = NULL;
        req->free_ctx = NULL;
        return ESP_OK;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT && frame.type != HTTPD_WS_TYPE_BINARY) {
        return ESP_OK;
    }
    if (frame.len == 0) {
        return ESP_OK;
    }
    if (frame.len > 4096) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK) {
        if (!ctx->shell) {
            ret = ssh_ws_start_shell(ctx, (const char *)buf);
        } else if (!terminal_control_manual_input_allowed(
                       SI_TERMINAL_CHANNEL_SSH)) {
            terminal_control_publish_state(SI_TERMINAL_CHANNEL_SSH);
            ret = ESP_OK;
        } else {
            ret = si_ssh_shell_write(ctx->shell, buf, frame.len, 100);
            if (ret != ESP_OK) {
                char msg[80];
                snprintf(msg, sizeof(msg), "\r\n[ssh: write failed: %s]\r\n",
                         esp_err_to_name(ret));
                (void)ssh_ws_send_str(ctx, msg);
            }
        }
    }
    ssh_secure_clear(buf, frame.len + 1);
    free(buf);
    return ret;
}

static esp_err_t ssh_ws_bridge_start(void)
{
    if (!s_ssh_ws_lock) s_ssh_ws_lock = xSemaphoreCreateMutex();
    return s_ssh_ws_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t ssh_ws_pre_handshake(httpd_req_t *req)
{
    esp_err_t ret = si_http_require_action(
        req, SI_CAPABILITY_SSH, "ssh_terminal", SI_AUTHZ_RISK_MEDIUM, NULL);
    if (ret != ESP_OK) return ret;
    if (xSemaphoreTake(s_ssh_ws_lock, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    bool occupied = s_ssh_ws_handle != NULL && s_ssh_ws_fd >= 0;
    xSemaphoreGive(s_ssh_ws_lock);
    if (occupied)
        return si_http_send_text_status(req, "409 Conflict",
                                        "SSH WebSocket already connected");
    ssh_ws_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return ESP_ERR_NO_MEM;
    ctx->handle = req->handle;
    ctx->fd = httpd_req_to_sockfd(req);
    req->sess_ctx = ctx;
    req->free_ctx = ssh_ws_ctx_free;
    return ESP_OK;
}

static esp_err_t ssh_ws_post_handshake(httpd_req_t *req)
{
    ssh_ws_ctx_t *ctx = (ssh_ws_ctx_t *)req->sess_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_ssh_ws_lock, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (s_ssh_ws_handle != NULL && s_ssh_ws_fd >= 0) {
        xSemaphoreGive(s_ssh_ws_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ssh_ws_handle = ctx->handle;
    s_ssh_ws_fd = ctx->fd;
    xSemaphoreGive(s_ssh_ws_lock);
    terminal_control_publish_state(SI_TERMINAL_CHANNEL_SSH);
    si_web_log("INFO", "SSH WebSocket connected");
    return ESP_OK;
}
#else
static esp_err_t ssh_ws_bridge_start(void) { return ESP_OK; }
static esp_err_t ssh_ws_send_terminal_json(const char *json, size_t len)
{
    (void)json; (void)len; return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t ssh_ws_send_terminal_output(const void *data, size_t len)
{
    (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED;
}
#endif
