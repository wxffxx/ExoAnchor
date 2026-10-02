/* A same-origin custom header prevents cross-site form submissions from claiming
 * an unconfigured device. It is not proof of physical ownership: first setup
 * must take place on a trusted local network. Tickets contain no credentials. */
static char s_setup_nonce[AUTH_LOGIN_JOB_ID_LEN + 1];
static uint32_t s_setup_nonce_created_ms;
static esp_err_t auth_setup_handler(httpd_req_t *req)
{
    if (!si_auth_setup_required() || auth_request_is_mcp(req)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "first-account setup closed");
    }
    if (!s_auth_login_lock ||
        xSemaphoreTake(s_auth_login_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint32_t now = si_monotonic_ms();
    bool expired = !s_setup_nonce[0] || (uint32_t)(now - s_setup_nonce_created_ms) >= 300000U;
    if (req->method == HTTP_GET) {
        if (expired) {
            auth_login_job_make_id(s_setup_nonce);
            s_setup_nonce_created_ms = now;
        }
        cJSON *response = cJSON_CreateObject();
        if (!response) {
            xSemaphoreGive(s_auth_login_lock);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(response, "nonce", s_setup_nonce);
        xSemaphoreGive(s_auth_login_lock);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        esp_err_t ret = si_http_send_json(req, response);
        cJSON_Delete(response);
        return ret;
    }
    char nonce[sizeof(s_setup_nonce)] = {0};
    bool valid = !expired &&
        httpd_req_get_hdr_value_str(req, "X-ExoAnchor-Setup", nonce, sizeof(nonce)) == ESP_OK &&
        strcmp(nonce, s_setup_nonce) == 0;
    xSemaphoreGive(s_auth_login_lock);
    if (!valid) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "setup ticket missing or expired");
    }
    return auth_login_handler(req);
}

static esp_err_t auth_certificate_handler(httpd_req_t *req)
{
    const char *certificate = si_tls_server_certificate();
    if (!certificate) {
        return si_http_send_text_status(req, "503 Service Unavailable", "certificate unavailable");
    }
    httpd_resp_set_type(req, "application/x-pem-file");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=exoanchor-device.pem");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, certificate);
}
