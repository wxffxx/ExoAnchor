// MCP-only Console credential injection. The bridge sends a credential
// reference and stage, never the username or password. The secret is read,
// used, and cleared entirely inside the device while the MCP session owns the
// supervised control lease.
#define CONSOLE_LOGIN_MCP_MAX_BODY 512

typedef struct {
    si_auth_session_context_t session;
    uint32_t lease_epoch;
    si_hid_owner_token_t hid_owner;
} console_login_mcp_context_t;

static bool console_login_mcp_control_alive(
    const console_login_mcp_context_t *context)
{
    if (!context || !context->session.session_id[0] ||
        context->session.generation == 0U || context->lease_epoch == 0U ||
        !si_hid_is_ready()) {
        return false;
    }
    si_auth_session_context_t live = {0};
    if (!si_auth_session_get_live_context(
            context->session.session_id, context->session.generation,
            &live) ||
        !live.authenticated || live.principal != SI_PRINCIPAL_MCP ||
        live.generation != context->session.generation ||
        strcmp(live.session_id, context->session.session_id) != 0 ||
        !si_capability_set_has(live.capabilities, SI_CAPABILITY_HID)) {
        return false;
    }
    si_control_lease_status_t lease = {0};
    si_control_lease_get_status(&lease);
    if (lease.kvm_active || lease.epoch != context->lease_epoch ||
        !lease.agent_active || strcasecmp(lease.agent_owner, "mcp") != 0 ||
        strcmp(lease.session_id, context->session.session_id) != 0 ||
        strcasecmp(lease.mode, "observe") == 0 ||
        !si_control_lease_owner_session_matches(
            "mcp", context->session.session_id)) {
        return false;
    }
    si_hid_owner_token_t current = {0};
    return si_control_lease_get_hid_owner(
               "mcp", context->session.session_id,
               context->session.generation, context->lease_epoch,
               &current) &&
           si_hid_owner_token_equal(&current, &context->hid_owner);
}

static bool console_login_mcp_authority_guard(void *opaque)
{
    return console_login_mcp_control_alive(opaque);
}

static esp_err_t console_login_mcp_wait_keyboard(
    const console_login_mcp_context_t *context)
{
    TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(500);
    for (;;) {
        if (!console_login_mcp_control_alive(context)) {
            return ESP_ERR_INVALID_STATE;
        }
        si_hid_status_t status;
        si_hid_get_status(&status);
        if (status.keyboard_writable) {
            return ESP_OK;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static esp_err_t console_login_mcp_key(
    const console_login_mcp_context_t *context,
    si_hid_command_type_t type, const char *code)
{
    ESP_RETURN_ON_ERROR(
        console_login_mcp_wait_keyboard(context), TAG,
        "wait for MCP Console keyboard endpoint");
    si_hid_command_t command = {
        .type = type,
        .code = code,
        .error = "MCP Console login HID key failed",
    };
    return si_hid_execute_owned(
        &command, &context->hid_owner,
        console_login_mcp_authority_guard, (void *)context);
}

static void console_login_mcp_release_all(
    const console_login_mcp_context_t *context)
{
    si_hid_command_t command = {
        .type = SI_HID_COMMAND_RELEASE_ALL,
        .error = "MCP Console login HID release failed",
    };
    if (context) {
        (void)si_hid_execute_owned(
            &command, &context->hid_owner,
            console_login_mcp_authority_guard, (void *)context);
    }
}

static esp_err_t console_login_mcp_tap(
    const console_login_mcp_context_t *context, const char *code,
    bool shift, uint32_t key_delay_ms)
{
    esp_err_t ret = ESP_OK;
    if (shift) {
        ret = console_login_mcp_key(
            context, SI_HID_COMMAND_KEY_DOWN, "ShiftLeft");
    }
    if (ret == ESP_OK) {
        ret = console_login_mcp_key(
            context, SI_HID_COMMAND_KEY_DOWN, code);
    }
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(8));
        ret = console_login_mcp_key(
            context, SI_HID_COMMAND_KEY_UP, code);
    }
    if (shift) {
        esp_err_t shift_ret = console_login_mcp_key(
            context, SI_HID_COMMAND_KEY_UP, "ShiftLeft");
        if (ret == ESP_OK) {
            ret = shift_ret;
        }
    }
    if (ret != ESP_OK) {
        console_login_mcp_release_all(context);
        return ret;
    }
    if (key_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(key_delay_ms));
    }
    return ESP_OK;
}

static esp_err_t console_login_mcp_type(
    const console_login_mcp_context_t *context, const char *text,
    uint32_t key_delay_ms)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor; cursor++) {
        if (!console_login_mcp_control_alive(context)) {
            console_login_mcp_release_all(context);
            return ESP_ERR_INVALID_STATE;
        }
        si_hid_ascii_key_t key;
        if (!si_hid_ascii_map(*cursor, &key)) {
            console_login_mcp_release_all(context);
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t ret = console_login_mcp_tap(
            context, key.code, key.shift, key_delay_ms);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t console_login_mcp_delay(
    const console_login_mcp_context_t *context, uint32_t delay_ms)
{
    while (delay_ms > 0) {
        if (!console_login_mcp_control_alive(context)) {
            return ESP_ERR_INVALID_STATE;
        }
        uint32_t slice_ms = MIN(delay_ms, 50U);
        vTaskDelay(pdMS_TO_TICKS(slice_ms));
        delay_ms -= slice_ms;
    }
    return ESP_OK;
}

static bool console_login_mcp_stage_valid(const char *stage)
{
    return stage &&
           (strcmp(stage, "username") == 0 ||
            strcmp(stage, "password") == 0 ||
            strcmp(stage, "both") == 0);
}

static esp_err_t console_login_mcp_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret = si_http_require_action(
        req, SI_CAPABILITY_HID, "console_login", SI_AUTHZ_RISK_HIGH,
        &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (session.principal != SI_PRINCIPAL_MCP) {
        return httpd_resp_send_err(
            req, HTTPD_403_FORBIDDEN, "MCP controller required");
    }
    if (req->content_len <= 0 ||
        req->content_len > CONSOLE_LOGIN_MCP_MAX_BODY) {
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char *buf = calloc(1, CONSOLE_LOGIN_MCP_MAX_BODY);
    if (!buf) {
        return httpd_resp_send_err(
            req, HTTPD_500_INTERNAL_SERVER_ERROR,
            "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(
        req, buf, CONSOLE_LOGIN_MCP_MAX_BODY, &root);
    si_secret_store_clear(buf, CONSOLE_LOGIN_MCP_MAX_BODY);
    free(buf);
    if (ret != ESP_OK) {
        return ret;
    }

    const char *credential_ref =
        json_string_any(root, "credential_ref", "credential", "ref");
    const char *stage =
        json_string_any(root, "stage", "operation", "mode");
    const char *between =
        json_string_any(root, "between", "username_submit", "next");
    const cJSON *key_delay_item =
        json_number_any(root, "key_delay_ms", "key_delay");
    const cJSON *field_delay_item =
        json_number_any(root, "inter_field_delay_ms", "wait_ms");
    uint32_t key_delay_ms = key_delay_item &&
        key_delay_item->valuedouble >= 0 ?
        (uint32_t)key_delay_item->valueint : 12;
    uint32_t field_delay_ms = field_delay_item &&
        field_delay_item->valuedouble >= 0 ?
        (uint32_t)field_delay_item->valueint : 700;
    if (!between || !between[0]) {
        between = "enter";
    }
    bool valid = credential_ref &&
        strcmp(credential_ref, "console://default") == 0 &&
        console_login_mcp_stage_valid(stage) &&
        (strcmp(between, "enter") == 0 ||
         strcmp(between, "tab") == 0) &&
        key_delay_ms <= 200 && field_delay_ms <= 5000;
    if (!valid) {
        cJSON_Delete(root);
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "invalid Console login arguments");
    }

    bool needs_username =
        strcmp(stage, "username") == 0 || strcmp(stage, "both") == 0;
    bool needs_password =
        strcmp(stage, "password") == 0 || strcmp(stage, "both") == 0;
    char stage_copy[16] = {0};
    strlcpy(stage_copy, stage, sizeof(stage_copy));
    si_console_credentials_status_t credentials = {0};
    ret = si_console_credentials_get_status(&credentials);
    if (ret != ESP_OK || !credentials.configured ||
        (needs_password && !credentials.password_configured)) {
        cJSON_Delete(root);
        return si_http_send_text_status(
            req, "409 Conflict",
            "Console credential is not fully configured");
    }

    si_control_lease_status_t lease = {0};
    si_control_lease_get_status(&lease);
    console_login_mcp_context_t context = {
        .session = session,
        .lease_epoch = lease.epoch,
    };
    if (!si_control_lease_get_hid_owner(
            "mcp", session.session_id, session.generation, lease.epoch,
            &context.hid_owner)) {
        cJSON_Delete(root);
        return si_http_send_text_status(
            req, "409 Conflict", "HID report owner changed");
    }
    char password[SI_CONSOLE_PASSWORD_MAX_LEN] = {0};
    if (needs_password) {
        ret = si_console_credentials_get_password(
            password, sizeof(password));
    }
    console_login_mcp_release_all(&context);
    if (ret == ESP_OK && needs_username) {
        ret = console_login_mcp_type(
            &context, credentials.username, key_delay_ms);
    }
    if (ret == ESP_OK && strcmp(stage, "both") == 0) {
        ret = console_login_mcp_tap(
            &context,
            strcmp(between, "enter") == 0 ? "Enter" : "Tab",
            false, key_delay_ms);
        if (ret == ESP_OK) {
            ret = console_login_mcp_delay(&context, field_delay_ms);
        }
    } else if (ret == ESP_OK && strcmp(stage, "username") == 0) {
        ret = console_login_mcp_tap(
            &context, "Enter", false, key_delay_ms);
    }
    if (ret == ESP_OK && needs_password) {
        ret = console_login_mcp_type(
            &context, password, key_delay_ms);
    }
    si_secret_store_clear(password, sizeof(password));
    if (ret == ESP_OK && needs_password) {
        ret = console_login_mcp_tap(
            &context, "Enter", false, key_delay_ms);
    }
    console_login_mcp_release_all(&context);
    si_secret_store_clear(&credentials, sizeof(credentials));
    cJSON_Delete(root);

    cJSON *response = cJSON_CreateObject();
    if (!response) {
        return ESP_ERR_NO_MEM;
    }
    bool ok = ret == ESP_OK;
    cJSON_AddBoolToObject(response, "ok", ok);
    cJSON_AddStringToObject(response, "credential_ref",
                            "console://default");
    cJSON_AddStringToObject(response, "stage", stage_copy);
    cJSON_AddBoolToObject(response, "secret_redacted", true);
    cJSON_AddBoolToObject(response, "submitted", ok);
    cJSON_AddBoolToObject(response, "verification_required", ok);
    cJSON_AddStringToObject(response, "status",
                            ok ? "injected-locally" : "hid-failed");
    if (!ok) {
        cJSON_AddStringToObject(response, "error", esp_err_to_name(ret));
    }
    esp_err_t send_ret = si_http_send_json(req, response);
    cJSON_Delete(response);
    return send_ret;
}
