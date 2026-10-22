/* Browser-only WebAuthn management and passwordless login. No dynamic RP ID
 * from Host, forwarded headers, or untrusted login input. */
static esp_err_t passkey_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    uint32_t generation = si_auth_credential_generation();
    char token[SI_AUTH_TOKEN_LEN + 1] = {0}, binding[44] = {0};
    bool browser = si_http_get_auth_token(req, token, sizeof(token)) && si_auth_token_is_browser(token);
    bool recent = browser && si_auth_token_is_recent(token);
    if (browser) browser = si_passkey_session_binding(token, binding);
    si_secret_store_clear(token, sizeof(token));
    if (req->method == HTTP_GET) {
        if (!browser) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "browser login required");
        cJSON *state = si_passkey_status(true);
        if (!state) return ESP_ERR_NO_MEM;
        esp_err_t ret = si_http_send_json(req, state); cJSON_Delete(state); return ret;
    }
    char origin[SI_PASSKEY_ORIGIN_MAX + 1], host[SI_PASSKEY_ORIGIN_MAX + 1];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        !si_passkey_origin_valid(origin) || strcmp(origin + 8, host))
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "use the trusted HTTPS hostname for passkeys");
    si_auth_status_t auth_status; si_auth_get_status(&auth_status);
    if (!auth_status.enabled || auth_status.using_default || auth_status.setup_required)
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "complete account setup first");
    if (si_auth_login_retry_after_ms())
        return si_http_send_text_status(req, "429 Too Many Requests", "authentication temporarily locked");
    /* Flat requests only. Bound allocation before receiving/parsing. */
    if (req->content_len == 0 || req->content_len >= 6144)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid passkey request size");
    char *buf = calloc(1, req->content_len + 1); cJSON *body = NULL, *result = NULL;
    if (!buf) return ESP_ERR_NO_MEM;
    esp_err_t ret = si_http_recv_body(req, buf, req->content_len + 1);
    if (ret != ESP_OK) goto cleanup;
    if (memchr(buf, 0, req->content_len) || strstr(buf, "\\u0000")) goto invalid;
    body = si_passkey_parse_json(buf);
    if (!cJSON_IsObject(body)) goto invalid;
    unsigned fields = 0;
    for (const cJSON *a = body->child; a; a = a->next) {
        if (++fields > 12 || !a->string || !cJSON_IsString(a)) goto invalid;
        for (const cJSON *b = a->next; b; b = b->next)
            if (b->string && !strcmp(a->string, b->string)) goto invalid;
    }
    const char *action = json_string_any(body, "action", NULL, NULL);
    if (!action) goto invalid;
    bool login_begin = !strcmp(action, "login_options"), login_finish = !strcmp(action, "login_finish");
    bool enroll = !strcmp(action, "register_options"), finish = !strcmp(action, "register_finish");
    bool configure = !strcmp(action, "configure"), remove = !strcmp(action, "remove");
    if (!login_begin && !login_finish && !enroll && !finish && !configure && !remove) goto invalid;
    if (!login_begin && !login_finish && !browser) {
        ret = httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "browser login required"); goto cleanup;
    }
    if (enroll || configure || remove) {
        const char *password = json_string_any(body, "current_password", NULL, NULL);
        const char *otp = json_string_any(body, "otp", NULL, NULL);
        if (!recent) {
            ret = httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "sign in again before managing passkeys"); goto cleanup;
        }
        if (!password || !password[0] || strlen(password) > SI_AUTH_PASSWORD_MAX_LEN || (otp && strlen(otp) > SI_TWO_STEP_CODE_MAX)) goto invalid;
        if (!si_auth_credentials_match(auth_status.username, password) || !si_two_step_verify(otp, auth_client_time(body), generation)) {
            si_auth_record_login_failure();
            ret = httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "current password and second factor required"); goto cleanup;
        }
    }
    if (login_begin || enroll) {
        ret = si_passkey_begin(enroll, origin, generation, binding,
                              json_string_any(body, "label", NULL, NULL), &result);
    } else if (login_finish || finish) {
        ret = si_passkey_finish(finish, origin, binding, body, &generation);
        if (ret == ESP_OK && login_finish) {
            si_auth_clear_login_failures();
            ret = auth_login_send_success(req, false, generation); goto cleanup;
        }
        if (ret != ESP_OK) si_auth_record_login_failure();
    } else if (configure) {
        ret = si_passkey_configure(origin, generation);
    } else {
        ret = si_passkey_remove(json_string_any(body, "id", NULL, NULL), generation);
    }
    if (ret != ESP_OK) {
        ret = httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "passkey request rejected; check hostname, storage, expiry, and credential limit");
    } else {
        if (!result) {
            si_http_clear_session_cookie(req);
            result = cJSON_CreateObject();
            if (!result) { ret = ESP_ERR_NO_MEM; goto cleanup; }
            cJSON_AddBoolToObject(result, "ok", true);
            cJSON_AddBoolToObject(result, "login_required", true);
        }
        ret = si_http_send_json(req, result);
    }
    goto cleanup;
invalid:
    ret = httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid passkey request");
cleanup:
    if (body) for (cJSON *item = body->child; item; item = item->next)
        if (cJSON_IsString(item)) si_secret_store_clear(item->valuestring, strlen(item->valuestring));
    cJSON_Delete(body); cJSON_Delete(result);
    si_secret_store_clear(buf, req->content_len + 1); free(buf); return ret;
}
