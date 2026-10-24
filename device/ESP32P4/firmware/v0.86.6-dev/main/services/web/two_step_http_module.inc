/* Browser account security. Seeds and recovery codes are returned only during
 * enrollment, never in public status or logs. Mutations also require the
 * current password; enrollment confirmation carries a random capability. */
static esp_err_t two_step_handler(httpd_req_t *req)
{
    char token[SI_AUTH_TOKEN_LEN + 1] = {0};
    bool authenticated = si_http_get_auth_token(req, token, sizeof(token)) &&
        si_auth_token_is_browser(token);
    bool recent = authenticated && si_auth_token_is_recent(token);
    si_secret_store_clear(token, sizeof(token));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!authenticated)
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "browser login required");
    if (req->method == HTTP_GET) {
        si_two_step_status_t status;
        si_two_step_get_status(&status);
        cJSON *response = cJSON_CreateObject();
        if (!response) return ESP_ERR_NO_MEM;
        cJSON_AddBoolToObject(response, "enabled", status.enabled);
        cJSON_AddBoolToObject(response, "storage_ok", status.storage_ok);
        cJSON_AddBoolToObject(response, "time_ready", status.time_ready);
        cJSON_AddNumberToObject(response, "recovery_remaining", status.recovery_remaining);
        esp_err_t ret = si_http_send_json(req, response);
        cJSON_Delete(response);
        return ret;
    }
    char buf[SCRATCH_BUFSIZE] = {0};
    cJSON *body = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &body);
    if (ret != ESP_OK) return ret;
    const char *action = json_string_any(body, "action", NULL, NULL);
    const char *password = json_string_any(body, "current_password", NULL, NULL);
    const char *otp = json_string_any(body, "otp", NULL, NULL);
    const char *id = json_string_any(body, "enrollment_id", NULL, NULL);
    bool begin = action && strcmp(action, "begin") == 0;
    bool confirm = action && strcmp(action, "confirm") == 0;
    bool disable = action && strcmp(action, "disable") == 0;
    if ((!begin && !confirm && !disable) || !password ||
        strlen(password) > SI_AUTH_PASSWORD_MAX_LEN ||
        (otp && strlen(otp) > SI_TWO_STEP_CODE_MAX) || (id && strlen(id) != 32)) {
        ret = httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid two-step request");
        goto cleanup;
    }
    /* Confirmation carries a random, five-minute enrollment capability. */
    if (!confirm && !recent) {
        ret = httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "sign in again before changing two-step verification");
        goto cleanup;
    }
    if (si_auth_login_retry_after_ms()) {
        ret = si_http_send_text_status(req, "429 Too Many Requests", "authentication temporarily locked");
        goto cleanup;
    }
    uint32_t generation = si_auth_credential_generation();
    si_auth_status_t auth_status;
    si_auth_get_status(&auth_status);
    if (auth_status.using_default || !si_auth_credentials_match(auth_status.username, password)) {
        si_auth_record_login_failure();
        ret = httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "current password required");
        goto cleanup;
    }
    if (begin) {
        si_two_step_enrollment_t enrollment = {0};
        ret = si_two_step_begin(generation, &enrollment);
        if (ret == ESP_OK) {
            cJSON *response = cJSON_CreateObject();
            if (!response) ret = ESP_ERR_NO_MEM;
            else {
                char uri[192];
                snprintf(uri, sizeof(uri), "otpauth://totp/ExoAnchor:%.8s?secret=%s&issuer=ExoAnchor&algorithm=SHA1&digits=6&period=30", enrollment.id, enrollment.secret);
                cJSON_AddStringToObject(response, "enrollment_id", enrollment.id);
                cJSON_AddStringToObject(response, "secret", enrollment.secret);
                cJSON_AddStringToObject(response, "otpauth_uri", uri);
                cJSON *codes = cJSON_AddArrayToObject(response, "recovery_codes");
                for (unsigned i = 0; i < SI_TWO_STEP_RECOVERY_COUNT; ++i)
                    cJSON_AddItemToArray(codes, cJSON_CreateString(enrollment.recovery[i]));
                ret = si_http_send_json(req, response);
                for (cJSON *item = response->child; item; item = item->next) {
                    if (cJSON_IsString(item)) si_secret_store_clear(item->valuestring, strlen(item->valuestring));
                }
                cJSON *code = NULL;
                cJSON_ArrayForEach(code, codes) if (cJSON_IsString(code)) si_secret_store_clear(code->valuestring, strlen(code->valuestring));
                cJSON_Delete(response);
                si_secret_store_clear(uri, sizeof(uri));
            }
        } else ret = si_http_send_text_status(req, "409 Conflict", "cannot enroll: check storage and current two-step status");
        si_secret_store_clear(&enrollment, sizeof(enrollment));
    } else {
        cJSON *response = cJSON_CreateObject();
        if (!response) { ret = ESP_ERR_NO_MEM; goto cleanup; }
        cJSON_AddBoolToObject(response, "ok", true);
        cJSON_AddBoolToObject(response, "login_required", true);
        int64_t client_time = auth_client_time(body);
        ret = confirm ? si_two_step_confirm(id, otp, client_time, generation) :
            si_two_step_disable(otp, client_time, generation);
        if (ret == ESP_OK) {
            si_http_clear_session_cookie(req);
            ret = si_http_send_json(req, response);
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ret = httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "invalid, expired, or used code; check clock and storage");
        } else {
            /* The commit fence already revoked sessions; state was reloaded. */
            si_http_clear_session_cookie(req);
            ret = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not save two-step settings; sign in again and check status");
        }
        cJSON_Delete(response);
    }
cleanup:
    if (password) si_secret_store_clear((void *)password, strlen(password));
    if (otp) si_secret_store_clear((void *)otp, strlen(otp));
    cJSON_Delete(body);
    si_secret_store_clear(buf, sizeof(buf));
    return ret;
}

static esp_err_t auth_http_initialize(void)
{
    (void)si_auth_is_enabled();
    (void)si_two_step_initialize(); /* Storage errors fail closed at login. */
    (void)si_passkey_initialize();
    si_two_step_clock_start();
    if (!s_auth_login_lock) s_auth_login_lock = xSemaphoreCreateMutex();
    if (!s_auth_login_lock) return ESP_ERR_NO_MEM;
    return auth_login_worker_start();
}
