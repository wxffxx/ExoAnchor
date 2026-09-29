// Shared assets, authentication and settings HTTP adapters.
static uint32_t fps_value_to_x100(double fps)
{
    return fps <= 0.0 ? 0 : (uint32_t)(fps * 100.0 + 0.5);
}

static void add_mcp_settings_json(cJSON *root)
{
    cJSON_AddBoolToObject(root, "enabled", si_mcp_is_enabled());
    cJSON_AddBoolToObject(root, "default_enabled", SI_MCP_DEFAULT_ENABLED);
    cJSON_AddStringToObject(root, "control_owner", "mcp");
    cJSON_AddStringToObject(root, "client_header", "X-ExoAnchor-Client: exoanchor-mcp");
    cJSON_AddStringToObject(root, "scope", "external_mcp_controller");
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
    si_http_set_security_headers(req);
    httpd_resp_set_type(req, type);
    size_t len = (size_t)(end - start);
    if (len > 0 && start[len - 1] == '\0') {
        len--;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    char etag[48];
    snprintf(etag, sizeof(etag), "\"ea-%02x%02x%02x%02x-%zx\"",
             app ? app->app_elf_sha256[0] : 0,
             app ? app->app_elf_sha256[1] : 0,
             app ? app->app_elf_sha256[2] : 0,
             app ? app->app_elf_sha256[3] : 0, len);
    httpd_resp_set_hdr(req, "Cache-Control",
                       "private, max-age=0, must-revalidate");
    httpd_resp_set_hdr(req, "ETag", etag);

    size_t incoming_len = httpd_req_get_hdr_value_len(req, "If-None-Match");
    if (incoming_len > 0 && incoming_len < sizeof(etag)) {
        char incoming[sizeof(etag)] = {0};
        if (httpd_req_get_hdr_value_str(req, "If-None-Match", incoming,
                                        sizeof(incoming)) == ESP_OK &&
            strcmp(incoming, etag) == 0) {
            httpd_resp_set_status(req, "304 Not Modified");
            return httpd_resp_send(req, NULL, 0);
        }
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

#if SI_CFG_UART_TERMINAL_ENABLED
static esp_err_t terminal_handler(httpd_req_t *req)
{
    return send_embedded_html(req, www_terminal_html_start, www_terminal_html_end);
}
#endif

static esp_err_t ui_core_css_handler(httpd_req_t *req)
{
    return send_embedded_asset(req, "text/css; charset=utf-8",
                               www_ui_core_css_start, www_ui_core_css_end);
}

#include "build_profile_asset_module.inc"

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

static esp_err_t exoanchor_ui_mark_svg_handler(httpd_req_t *req)
{
    return send_embedded_asset(req, "image/svg+xml",
                               www_exoanchor_ui_mark_svg_start,
                               www_exoanchor_ui_mark_svg_end);
}

static esp_err_t favicon_svg_handler(httpd_req_t *req)
{
    return send_embedded_asset(req, "image/svg+xml",
                               www_favicon_svg_start, www_favicon_svg_end);
}

#if SI_CFG_UART_TERMINAL_ENABLED
static esp_err_t xterm_css_handler(httpd_req_t *req)
{
    return send_embedded_asset(req, "text/css; charset=utf-8",
                               www_xterm_css_start, www_xterm_css_end);
}

static esp_err_t xterm_js_handler(httpd_req_t *req)
{
    return send_embedded_asset(req, "application/javascript; charset=utf-8",
                               www_xterm_js_start, www_xterm_js_end);
}

static esp_err_t xterm_fit_js_handler(httpd_req_t *req)
{
    return send_embedded_asset(req, "application/javascript; charset=utf-8",
                               www_addon_fit_js_start, www_addon_fit_js_end);
}
#endif

#if SI_CFG_EMBEDDED_AGENT_ENABLED
static esp_err_t skills_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/settings#agent");
    return httpd_resp_send(req, NULL, 0);
}
#endif

static esp_err_t settings_handler(httpd_req_t *req)
{
    return send_embedded_html(req, www_settings_html_start, www_settings_html_end);
}

typedef enum {
    AUTH_LOGIN_JOB_IDLE = 0,
    AUTH_LOGIN_JOB_QUEUED,
    AUTH_LOGIN_JOB_RUNNING,
    AUTH_LOGIN_JOB_ACCEPTED,
    AUTH_LOGIN_JOB_REJECTED,
} auth_login_job_state_t;

#define AUTH_LOGIN_JOB_ID_BYTES 16
#define AUTH_LOGIN_JOB_ID_LEN (AUTH_LOGIN_JOB_ID_BYTES * 2)
#define AUTH_LOGIN_REQUEST_ID_MAX_LEN 64
#define AUTH_LOGIN_JOB_RESULT_TTL_MS 30000U
#define AUTH_LOGIN_JOB_TASK_STACK_SIZE 6144

typedef struct {
    auth_login_job_state_t state;
    char job_id[AUTH_LOGIN_JOB_ID_LEN + 1];
    char request_id[AUTH_LOGIN_REQUEST_ID_MAX_LEN + 1];
    char username[SI_AUTH_USERNAME_MAX_LEN + 1];
    char password[SI_AUTH_PASSWORD_MAX_LEN + 1];
    bool mcp_client;
    uint32_t created_ms;
} auth_login_job_t;

static SemaphoreHandle_t s_auth_login_lock;
static auth_login_job_t s_auth_login_job;

static bool auth_request_is_mcp(httpd_req_t *req)
{
    char client[48] = {0};
    return httpd_req_get_hdr_value_str(req, "X-ExoAnchor-Client",
                                       client, sizeof(client)) == ESP_OK &&
           strcasecmp(client, "exoanchor-mcp") == 0;
}

static void auth_login_job_reset_locked(void)
{
    si_secret_store_clear(&s_auth_login_job, sizeof(s_auth_login_job));
    s_auth_login_job.state = AUTH_LOGIN_JOB_IDLE;
}

static bool auth_login_job_expired_locked(uint32_t now_ms)
{
    return (s_auth_login_job.state == AUTH_LOGIN_JOB_ACCEPTED ||
            s_auth_login_job.state == AUTH_LOGIN_JOB_REJECTED) &&
           (uint32_t)(now_ms - s_auth_login_job.created_ms) >
               AUTH_LOGIN_JOB_RESULT_TTL_MS;
}

static void auth_login_job_make_id(char out[AUTH_LOGIN_JOB_ID_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random[AUTH_LOGIN_JOB_ID_BYTES] = {0};
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); i++) {
        out[i * 2] = hex[random[i] >> 4];
        out[i * 2 + 1] = hex[random[i] & 0x0f];
    }
    out[AUTH_LOGIN_JOB_ID_LEN] = '\0';
    si_secret_store_clear(random, sizeof(random));
}

static esp_err_t auth_login_send_pending(httpd_req_t *req,
                                         const char *job_id)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG,
                        "create login pending response");
    cJSON_AddBoolToObject(root, "pending", true);
    cJSON_AddStringToObject(root, "job_id", job_id ? job_id : "");
    cJSON_AddNumberToObject(root, "poll_after_ms", 100);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}
static esp_err_t auth_login_send_success(httpd_req_t *req, bool mcp_client)
{
    cJSON *resp = cJSON_CreateObject();
    si_auth_record_login();
    si_auth_status_t auth_status;
    si_auth_get_status(&auth_status);
    if (mcp_client && auth_status.using_default) { cJSON_Delete(resp); return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "change the bootstrap credential through the browser or UART0 before MCP login"); }
    char token[SI_AUTH_TOKEN_LEN + 1] = {0};
    char session_cookie[SI_HTTP_SESSION_COOKIE_MAX_LEN] = {0};
    if (auth_status.enabled &&
        si_auth_create_session_for_client(mcp_client ? "exoanchor-mcp" : "web",
                                          token) != ESP_OK) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "session creation failed");
    }
    if (token[0]) {
        esp_err_t cookie_ret =
            si_http_set_session_cookie(req, token, session_cookie,
                                       sizeof(session_cookie));
        if (cookie_ret != ESP_OK) {
            si_auth_revoke_session(token);
            cJSON_Delete(resp);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "session cookie failed");
        }
    }
    si_auth_session_context_t session = {
        .authenticated = true,
        .principal = mcp_client ? SI_PRINCIPAL_MCP :
                                  SI_PRINCIPAL_BROWSER,
        .capabilities = mcp_client ? SI_CAPABILITIES_MCP_DEFAULT :
                                     SI_CAPABILITIES_WEB_DEFAULT,
    };
    strlcpy(session.session_id, "auth-disabled",
            sizeof(session.session_id));
    if (token[0]) {
        (void)si_auth_token_get_context(token, false, &session);
    }
    cJSON_AddStringToObject(resp, "token", token);
    cJSON_AddStringToObject(resp, "type", "bearer");
    cJSON_AddStringToObject(resp, "session_id", session.session_id);
    cJSON_AddStringToObject(resp, "principal",
                            si_principal_kind_name(session.principal));
    cJSON_AddNumberToObject(resp, "expires_in", 86400);
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

static void auth_login_worker(void *arg)
{
    (void)arg;
    char username[SI_AUTH_USERNAME_MAX_LEN + 1] = {0};
    char password[SI_AUTH_PASSWORD_MAX_LEN + 1] = {0};
    bool have_job = false;

    if (s_auth_login_lock &&
        xSemaphoreTake(s_auth_login_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (s_auth_login_job.state == AUTH_LOGIN_JOB_QUEUED) {
            strlcpy(username, s_auth_login_job.username, sizeof(username));
            strlcpy(password, s_auth_login_job.password, sizeof(password));
            si_secret_store_clear(s_auth_login_job.username,
                                  sizeof(s_auth_login_job.username));
            si_secret_store_clear(s_auth_login_job.password,
                                  sizeof(s_auth_login_job.password));
            s_auth_login_job.state = AUTH_LOGIN_JOB_RUNNING;
            have_job = true;
        }
        xSemaphoreGive(s_auth_login_lock);
    }

    bool accepted = have_job &&
                    si_auth_credentials_match(username, password);
    si_secret_store_clear(username, sizeof(username));
    si_secret_store_clear(password, sizeof(password));

    if (have_job) {
        if (accepted) {
            si_auth_clear_login_failures();
        } else {
            si_auth_record_login_failure();
        }
        if (xSemaphoreTake(s_auth_login_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s_auth_login_job.state == AUTH_LOGIN_JOB_RUNNING) {
                s_auth_login_job.state = accepted ?
                    AUTH_LOGIN_JOB_ACCEPTED : AUTH_LOGIN_JOB_REJECTED;
                s_auth_login_job.created_ms = si_monotonic_ms();
            }
            xSemaphoreGive(s_auth_login_lock);
        }
    }
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t auth_login_handler(httpd_req_t *req)
{
    bool mcp_client = auth_request_is_mcp(req);
    if (mcp_client && !si_mcp_is_enabled()) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "mcp disabled in settings");
    }
    if (!si_auth_is_enabled()) {
        return auth_login_send_success(req, mcp_client);
    }

    char buf[SCRATCH_BUFSIZE] = {0};
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) {
        return ret;
    }
    const cJSON *username =
        cJSON_GetObjectItemCaseSensitive(root, "username");
    const cJSON *password =
        cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON *request_id =
        cJSON_GetObjectItemCaseSensitive(root, "request_id");
    size_t username_len =
        cJSON_IsString(username) ? strlen(username->valuestring) : 0;
    size_t password_len =
        cJSON_IsString(password) ? strlen(password->valuestring) : 0;
    size_t request_id_len =
        cJSON_IsString(request_id) ? strlen(request_id->valuestring) : 0;
    if (username_len == 0 || username_len > SI_AUTH_USERNAME_MAX_LEN ||
        password_len == 0 || password_len > SI_AUTH_PASSWORD_MAX_LEN ||
        request_id_len > AUTH_LOGIN_REQUEST_ID_MAX_LEN) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid login request");
    }

    if (!s_auth_login_lock ||
        xSemaphoreTake(s_auth_login_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
        cJSON_Delete(root);
        return si_http_send_text_status(req, "503 Service Unavailable",
                                        "login service busy");
    }
    uint32_t now_ms = si_monotonic_ms();
    if (auth_login_job_expired_locked(now_ms)) {
        auth_login_job_reset_locked();
    }

    bool duplicate =
        request_id_len > 0 &&
        s_auth_login_job.state != AUTH_LOGIN_JOB_IDLE &&
        strcmp(s_auth_login_job.request_id, request_id->valuestring) == 0 &&
        s_auth_login_job.mcp_client == mcp_client;
    if (duplicate) {
        char existing_job_id[AUTH_LOGIN_JOB_ID_LEN + 1] = {0};
        strlcpy(existing_job_id, s_auth_login_job.job_id,
                sizeof(existing_job_id));
        xSemaphoreGive(s_auth_login_lock);
        cJSON_Delete(root);
        return auth_login_send_pending(req, existing_job_id);
    }
    if (s_auth_login_job.state != AUTH_LOGIN_JOB_IDLE) {
        xSemaphoreGive(s_auth_login_lock);
        cJSON_Delete(root);
        httpd_resp_set_hdr(req, "Retry-After", "1");
        return si_http_send_text_status(req, "409 Conflict",
                                        "another login is in progress");
    }

    uint32_t retry_after_ms = si_auth_login_retry_after_ms();
    if (retry_after_ms > 0) {
        xSemaphoreGive(s_auth_login_lock);
        cJSON_Delete(root);
        char retry_after[16];
        snprintf(retry_after, sizeof(retry_after), "%u",
                 (unsigned)((retry_after_ms + 999U) / 1000U));
        httpd_resp_set_hdr(req, "Retry-After", retry_after);
        return si_http_send_text_status(req, "429 Too Many Requests",
                                        "login temporarily locked");
    }

    auth_login_job_reset_locked();
    auth_login_job_make_id(s_auth_login_job.job_id);
    if (request_id_len > 0) {
        strlcpy(s_auth_login_job.request_id, request_id->valuestring,
                sizeof(s_auth_login_job.request_id));
    }
    strlcpy(s_auth_login_job.username, username->valuestring,
            sizeof(s_auth_login_job.username));
    strlcpy(s_auth_login_job.password, password->valuestring,
            sizeof(s_auth_login_job.password));
    s_auth_login_job.mcp_client = mcp_client;
    s_auth_login_job.created_ms = now_ms;
    s_auth_login_job.state = AUTH_LOGIN_JOB_QUEUED;
    char job_id[AUTH_LOGIN_JOB_ID_LEN + 1] = {0};
    strlcpy(job_id, s_auth_login_job.job_id, sizeof(job_id));

    BaseType_t task_ok =
        xTaskCreateWithCaps(auth_login_worker, "si_auth_login",
                            AUTH_LOGIN_JOB_TASK_STACK_SIZE, NULL,
                            tskIDLE_PRIORITY + 2, NULL,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
        auth_login_job_reset_locked();
        xSemaphoreGive(s_auth_login_lock);
        cJSON_Delete(root);
        return si_http_send_text_status(req, "503 Service Unavailable",
                                        "login worker unavailable");
    }
    xSemaphoreGive(s_auth_login_lock);
    cJSON_Delete(root);
    return auth_login_send_pending(req, job_id);
}

static esp_err_t auth_login_status_handler(httpd_req_t *req)
{
    char query[96] = {0};
    char job_id[AUTH_LOGIN_JOB_ID_LEN + 1] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "job_id", job_id,
                              sizeof(job_id)) != ESP_OK ||
        strlen(job_id) != AUTH_LOGIN_JOB_ID_LEN) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "missing login job_id");
    }
    bool mcp_client = auth_request_is_mcp(req);
    if (!s_auth_login_lock ||
        xSemaphoreTake(s_auth_login_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
        return si_http_send_text_status(req, "503 Service Unavailable",
                                        "login service busy");
    }
    if (auth_login_job_expired_locked(si_monotonic_ms())) {
        auth_login_job_reset_locked();
    }
    if (s_auth_login_job.state == AUTH_LOGIN_JOB_IDLE ||
        strcmp(s_auth_login_job.job_id, job_id) != 0 ||
        s_auth_login_job.mcp_client != mcp_client) {
        xSemaphoreGive(s_auth_login_lock);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "login job not found");
    }
    if (s_auth_login_job.state == AUTH_LOGIN_JOB_QUEUED ||
        s_auth_login_job.state == AUTH_LOGIN_JOB_RUNNING) {
        xSemaphoreGive(s_auth_login_lock);
        return auth_login_send_pending(req, job_id);
    }
    bool accepted =
        s_auth_login_job.state == AUTH_LOGIN_JOB_ACCEPTED;
    auth_login_job_reset_locked();
    xSemaphoreGive(s_auth_login_lock);

    if (!accepted) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED,
                                   "invalid credentials");
    }
    return auth_login_send_success(req, mcp_client);
}

static esp_err_t auth_logout_handler(httpd_req_t *req)
{
    if (si_auth_is_enabled()) {
        char token[SI_AUTH_TOKEN_LEN + 1] = {0};
        if (si_http_get_auth_token(req, token, sizeof(token))) {
            si_auth_revoke_session(token);
        }
    }
    si_http_clear_session_cookie(req);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
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
    cJSON_AddBoolToObject(root, "legacy_password",
                          auth_status.legacy_password);
    char boot_id[17];
    snprintf(boot_id, sizeof(boot_id), "%016" PRIx64, s_server_boot_id);
    cJSON_AddStringToObject(root, "boot_id", boot_id);
    cJSON_AddNumberToObject(root, "server_uptime_seconds", uptime_sec());
    cJSON_AddNumberToObject(root, "reset_reason", esp_reset_reason());
    cJSON_AddStringToObject(root, "reset_reason_name",
                            reset_reason_name(esp_reset_reason()));
    si_session_settings_t session_settings;
    si_session_settings_get(&session_settings);
    cJSON_AddBoolToObject(root, "auto_logout_enabled",
                          session_settings.auto_logout_enabled);
    cJSON_AddNumberToObject(root, "auto_logout_minutes",
                            session_settings.auto_logout_minutes);
    si_auth_session_context_t session;
    bool token_valid = si_http_get_session_context(req, &session);
    char refreshed_cookie[SI_HTTP_SESSION_COOKIE_MAX_LEN] = {0};
    if (token_valid && session.principal == SI_PRINCIPAL_BROWSER) {
        (void)si_http_sync_browser_session_cookie_from_bearer(
            req, refreshed_cookie, sizeof(refreshed_cookie));
    }
    cJSON_AddBoolToObject(root, "token_valid", token_valid);
    cJSON_AddStringToObject(root, "session_id",
                            token_valid ? session.session_id : "");
    cJSON_AddStringToObject(
        root, "principal",
        token_valid ? si_principal_kind_name(session.principal) :
                      "anonymous");
    cJSON_AddNumberToObject(root, "login_count", auth_status.login_count);
    si_auth_runtime_status_t runtime_status;
    si_auth_get_runtime_status(&runtime_status);
    cJSON *sessions = cJSON_AddObjectToObject(root, "sessions");
    cJSON_AddNumberToObject(sessions, "browser_active",
                            runtime_status.browser_active);
    cJSON_AddNumberToObject(sessions, "mcp_active",
                            runtime_status.mcp_active);
    cJSON_AddNumberToObject(sessions, "restored",
                            runtime_status.restored);
    cJSON_AddNumberToObject(sessions, "evictions",
                            runtime_status.evictions);
    cJSON_AddNumberToObject(sessions, "expirations",
                            runtime_status.expirations);
    cJSON_AddNumberToObject(sessions, "validation_misses",
                            runtime_status.validation_misses);
    cJSON_AddNumberToObject(sessions, "lock_timeouts",
                            runtime_status.lock_timeouts);
    cJSON_AddNumberToObject(root, "username_max_length", SI_AUTH_USERNAME_MAX_LEN);
    cJSON_AddNumberToObject(root, "min_length", SI_AUTH_PASSWORD_MIN_LEN);
    cJSON_AddNumberToObject(root, "max_length", SI_AUTH_PASSWORD_MAX_LEN);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    si_secret_store_clear(refreshed_cookie, sizeof(refreshed_cookie));
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
    char token[SI_AUTH_TOKEN_LEN + 1] = {0};
    char session_cookie[SI_HTTP_SESSION_COOKIE_MAX_LEN] = {0};
    if (si_auth_create_session(token) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "session creation failed");
    }
    ret = si_http_set_session_cookie(req, token, session_cookie,
                                     sizeof(session_cookie));
    if (ret != ESP_OK) {
        si_auth_revoke_session(token);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "session cookie failed");
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "token", token);
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
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    char device_label[SI_DEVICE_LABEL_MAX_LEN + 1];
    char agent_name[SI_AGENT_DISPLAY_NAME_MAX_LEN + 1];
    si_device_label_get(device_label, sizeof(device_label));
    si_agent_display_name_get(agent_name, sizeof(agent_name));

    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "label", device_label);
        cJSON_AddStringToObject(resp, "default_label", SI_DEVICE_LABEL_DEFAULT);
        cJSON_AddNumberToObject(resp, "max_length", SI_DEVICE_LABEL_MAX_LEN);
        cJSON_AddStringToObject(resp, "agent_name", agent_name);
        cJSON_AddStringToObject(resp, "default_agent_name",
                               SI_AGENT_DISPLAY_NAME_DEFAULT);
        cJSON_AddNumberToObject(resp, "agent_name_max_length",
                               SI_AGENT_DISPLAY_NAME_MAX_LEN);
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
    const char *agent_name_update =
        root ? json_string_any(root, "agent_name", "agent_display_name",
                               "assistant_name") : NULL;
    if (!label && !agent_name_update) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "label or agent_name is required");
    }

    esp_err_t ret = si_device_identity_set(label, agent_name_update);
    cJSON_Delete(root);
    if (ret == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "identity field has an invalid length");
    }
    if (ret == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "identity field contains invalid characters");
    }
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
    }

    si_web_log("INFO", "Device identity updated");
    si_device_label_get(device_label, sizeof(device_label));
    si_agent_display_name_get(agent_name, sizeof(agent_name));
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "label", device_label);
    cJSON_AddStringToObject(resp, "default_label", SI_DEVICE_LABEL_DEFAULT);
    cJSON_AddNumberToObject(resp, "max_length", SI_DEVICE_LABEL_MAX_LEN);
    cJSON_AddStringToObject(resp, "agent_name", agent_name);
    cJSON_AddStringToObject(resp, "default_agent_name",
                           SI_AGENT_DISPLAY_NAME_DEFAULT);
    cJSON_AddNumberToObject(resp, "agent_name_max_length",
                           SI_AGENT_DISPLAY_NAME_MAX_LEN);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static void add_string_array(cJSON *root, const char *name,
                             const char *const *values, size_t value_count)
{
    cJSON *array = cJSON_AddArrayToObject(root, name);
    for (size_t i = 0; array && i < value_count; i++) {
        cJSON_AddItemToArray(array, cJSON_CreateString(values[i]));
    }
}

static void add_target_profile_json(cJSON *root,
                                    const si_target_profile_settings_t *profile)
{
    static const char *const device_types[] = {
        "unknown", "desktop", "workstation", "server", "laptop", "embedded",
        "virtual_machine", "network_appliance", "other",
    };
    static const char *const operating_systems[] = {
        "unknown", "linux", "windows", "macos", "bsd", "appliance", "other",
    };
    static const char *const environments[] = {
        "unknown", "development", "testing", "staging", "production", "lab",
        "home", "other",
    };
    cJSON_AddStringToObject(root, "name", profile->name);
    cJSON_AddStringToObject(root, "device_type", profile->device_type);
    cJSON_AddStringToObject(root, "operating_system",
                           profile->operating_system);
    cJSON_AddStringToObject(root, "environment", profile->environment);
    cJSON_AddStringToObject(root, "location", profile->location);
    cJSON_AddStringToObject(root, "configuration", profile->configuration);
    cJSON_AddStringToObject(root, "purpose", profile->purpose);
    cJSON_AddStringToObject(root, "notes", profile->notes);
    add_string_array(root, "device_types", device_types,
                     sizeof(device_types) / sizeof(device_types[0]));
    add_string_array(root, "operating_systems", operating_systems,
                     sizeof(operating_systems) /
                     sizeof(operating_systems[0]));
    add_string_array(root, "environments", environments,
                     sizeof(environments) / sizeof(environments[0]));
    cJSON_AddBoolToObject(root, "agent_context_enabled", true);
    cJSON_AddStringToObject(
        root, "agent_context_scope",
        "untrusted metadata only; never grants permission or authority");
}

static esp_err_t target_profile_copy_field(char *out, size_t out_size,
                                           const char *value)
{
    if (!out || out_size == 0 || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(value) >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(out, value, out_size);
    return ESP_OK;
}

static esp_err_t settings_target_profile_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_target_profile_settings_t profile;
    esp_err_t ret = si_target_profile_load(&profile);
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(ret));
    }
    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        add_target_profile_json(resp, &profile);
        ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                                   "method not allowed");
    }

    const size_t target_profile_body_size = 4096;
    char *buf = calloc(1, target_profile_body_size);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "allocation failed");
    }
    cJSON *root = NULL;
    esp_err_t json_ret = si_http_recv_json(req, buf,
                                           target_profile_body_size, &root);
    free(buf);
    if (json_ret != ESP_OK) {
        return json_ret;
    }

#define COPY_OPTIONAL(json_key, field)                                      \
    do {                                                                    \
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, json_key);      \
        if (item) {                                                         \
            if (!cJSON_IsString(item) || !item->valuestring) {               \
                ret = ESP_ERR_INVALID_ARG;                                  \
            } else if (ret == ESP_OK) {                                     \
                ret = target_profile_copy_field(                            \
                    profile.field, sizeof(profile.field), item->valuestring);\
            }                                                               \
        }                                                                   \
    } while (0)
    ret = ESP_OK;
    COPY_OPTIONAL("name", name);
    COPY_OPTIONAL("device_type", device_type);
    COPY_OPTIONAL("operating_system", operating_system);
    COPY_OPTIONAL("environment", environment);
    COPY_OPTIONAL("location", location);
    COPY_OPTIONAL("configuration", configuration);
    COPY_OPTIONAL("purpose", purpose);
    COPY_OPTIONAL("notes", notes);
#undef COPY_OPTIONAL
    cJSON_Delete(root);

    if (ret == ESP_OK) {
        ret = si_target_profile_save(&profile);
    }
    if (ret == ESP_ERR_INVALID_ARG || ret == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid target profile field");
    }
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(ret));
    }

    si_web_log("INFO", "Target host information updated");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    add_target_profile_json(resp, &profile);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t settings_session_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
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

static esp_err_t settings_mcp_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        esp_err_t auth_ret =
            si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
        if (auth_ret != ESP_OK) {
            return auth_ret;
        }
        cJSON *resp = cJSON_CreateObject();
        add_mcp_settings_json(resp);
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }

    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    char buf[192] = {0};
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) {
        return ret;
    }

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (!enabled) {
        enabled = cJSON_GetObjectItemCaseSensitive(root, "mcp_enabled");
    }
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled must be boolean");
    }
    bool next_enabled = cJSON_IsTrue(enabled);
    cJSON_Delete(root);

    if (si_mcp_is_enabled() && !next_enabled) {
        (void)automation_request_stop_actor(
            SI_RESOURCE_OPERATION_ACTOR_MCP,
            "MCP controller disabled in Settings");
    }
    ret = si_mcp_set_enabled(next_enabled);
    ESP_RETURN_ON_ERROR(ret, TAG, "save mcp settings");
    si_web_log("INFO", next_enabled ? "MCP controller enabled" : "MCP controller disabled");

    cJSON *resp = cJSON_CreateObject();
    add_mcp_settings_json(resp);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static bool agent_tools_mode_valid(const char *mode)
{
    return mode && (strcasecmp(mode, "observe") == 0 ||
                    strcasecmp(mode, "supervised") == 0 ||
                    strcasecmp(mode, "autonomous") == 0);
}

static esp_err_t agent_tools_validate_json_text(const char *text, size_t max_len,
                                                bool expect_array)
{
    if (!text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(text) > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON *root = cJSON_Parse(text);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    bool ok = expect_array ? cJSON_IsArray(root) : cJSON_IsObject(root);
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t agent_tools_validate_policy_json(const char *text)
{
    if (!text || strlen(text) > AGENT_TOOLS_POLICY_MAX_LEN) {
        return text ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_Parse(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *tools =
        cJSON_GetObjectItemCaseSensitive(root, "tools");
    if (tools && !cJSON_IsObject(tools)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *tool = NULL;
    cJSON_ArrayForEach(tool, tools) {
        if (!cJSON_IsObject(tool)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        const char *bool_fields[] = {
            "enabled", "agent_enabled", "mcp_enabled",
        };
        for (size_t i = 0;
             i < sizeof(bool_fields) / sizeof(bool_fields[0]); i++) {
            const cJSON *value =
                cJSON_GetObjectItemCaseSensitive(tool, bool_fields[i]);
            if (value && !cJSON_IsBool(value)) {
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }
        }
        const cJSON *permission =
            cJSON_GetObjectItemCaseSensitive(tool, "permission");
        if (permission &&
            (!cJSON_IsString(permission) ||
             !agent_tools_mode_valid(permission->valuestring))) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        const cJSON *risk =
            cJSON_GetObjectItemCaseSensitive(tool, "risk");
        if (risk &&
            (!cJSON_IsString(risk) ||
             (strcasecmp(risk->valuestring, "low") != 0 &&
              strcasecmp(risk->valuestring, "medium") != 0 &&
              strcasecmp(risk->valuestring, "high") != 0))) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static bool agent_web_search_strategy_valid(const char *strategy)
{
    return strategy &&
           (strcasecmp(strategy, "agent") == 0 ||
            strcasecmp(strategy, "agent_max") == 0 ||
            strcasecmp(strategy, "turbo") == 0 ||
            strcasecmp(strategy, "max") == 0);
}

void si_web_add_agent_tools_settings_json(cJSON *root)
{
    char mode[16] = "observe";
    char *policy = heap_caps_calloc(1, AGENT_TOOLS_POLICY_MAX_LEN + 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!policy) {
        policy = calloc(1, AGENT_TOOLS_POLICY_MAX_LEN + 1);
    }
    char *skills = heap_caps_calloc(1, AGENT_TOOLS_SKILLS_MAX_LEN + 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!skills) {
        skills = calloc(1, AGENT_TOOLS_SKILLS_MAX_LEN + 1);
    }

    (void)agent_tools_load_string(AGENT_TOOLS_MODE_KEY, "observe",
                                  mode, sizeof(mode));
    if (!agent_tools_mode_valid(mode)) {
        strlcpy(mode, "observe", sizeof(mode));
    }
    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddNumberToObject(root, "policy_schema_version", 2);
    cJSON_AddStringToObject(root, "settings_endpoint", "/api/settings/agent-tools");
    cJSON_AddStringToObject(root, "default_mode", mode);
    cJSON_AddNumberToObject(root, "policy_max_length", AGENT_TOOLS_POLICY_MAX_LEN);
    cJSON_AddNumberToObject(root, "skills_max_length", AGENT_TOOLS_SKILLS_MAX_LEN);

    cJSON *modes = cJSON_AddArrayToObject(root, "modes");
    cJSON_AddItemToArray(modes, cJSON_CreateString("observe"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("supervised"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("autonomous"));

    if (policy) {
        (void)agent_tools_load_string(AGENT_TOOLS_POLICY_KEY, AGENT_TOOLS_POLICY_DEFAULT,
                                      policy, AGENT_TOOLS_POLICY_MAX_LEN + 1);
        cJSON_AddStringToObject(root, "policy_json", policy);
        cJSON *parsed = cJSON_Parse(policy);
        if (parsed) {
            cJSON_AddItemToObject(root, "policy", parsed);
        }
    }
    if (skills) {
        (void)agent_tools_load_string(AGENT_TOOLS_SKILLS_KEY, AGENT_TOOLS_SKILLS_DEFAULT,
                                      skills, AGENT_TOOLS_SKILLS_MAX_LEN + 1);
#if SI_CFG_EMBEDDED_AGENT_ENABLED
        cJSON *parsed = agent_skills_parse_with_defaults(skills, AGENT_TOOLS_SKILLS_MAX_LEN + 1);
#else
        cJSON *parsed = cJSON_Parse(skills);
        if (!cJSON_IsArray(parsed)) { cJSON_Delete(parsed); strlcpy(skills, "[]", AGENT_TOOLS_SKILLS_MAX_LEN + 1); parsed = cJSON_CreateArray(); }
#endif
        cJSON_AddStringToObject(root, "skills_json", skills);
        if (parsed) {
            cJSON_AddItemToObject(root, "skills", parsed);
        }
    }
    agent_web_search_settings_t web_search;
    agent_web_search_defaults(&web_search);
    (void)agent_web_search_load(&web_search);
    cJSON *web = cJSON_AddObjectToObject(root, "web_search");
    if (web) {
        cJSON_AddBoolToObject(web, "enabled", web_search.enabled);
        cJSON_AddStringToObject(web, "provider", "qwen_chat_search");
        cJSON_AddStringToObject(web, "profile", web_search.profile);
        cJSON_AddStringToObject(web, "endpoint", web_search.endpoint);
        cJSON_AddStringToObject(web, "model", web_search.model);
        cJSON_AddStringToObject(web, "strategy", web_search.strategy);
        cJSON_AddBoolToObject(web, "api_key_configured",
                              web_search.api_key_configured);
        cJSON_AddBoolToObject(web, "available",
                              agent_web_search_available(&web_search));
        cJSON_AddStringToObject(web, "api_key_source",
                                web_search.api_key_configured ?
                                "web_search" : "agent_profile");
        cJSON_AddStringToObject(web, "default_endpoint_source",
                                "empty endpoint uses selected API profile base");
    }
    free(policy);
    free(skills);
}

static esp_err_t settings_agent_tools_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        si_web_add_agent_tools_settings_json(resp);
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }

    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }
    if (req->content_len <= 0 || req->content_len > AGENT_TOOLS_MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char *buf = calloc(1, AGENT_TOOLS_MAX_BODY);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, AGENT_TOOLS_MAX_BODY, &root);
    free(buf);
    if (ret != ESP_OK) {
        return ret;
    }
    cJSON *reset = cJSON_GetObjectItemCaseSensitive(root, "reset_default");
    if (!reset) {
        reset = cJSON_GetObjectItemCaseSensitive(root, "reset");
    }
    if (cJSON_IsTrue(reset)) {
        cJSON_Delete(root);
        (void)automation_request_stop_all(
            "Agent/MCP tool policy reset in Settings");
        ret = agent_tools_reset_settings();
        if (ret != ESP_OK) {
            return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
        }
        cJSON *resp = cJSON_CreateObject();
        si_web_add_agent_tools_settings_json(resp);
        esp_err_t send_ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return send_ret;
    }

    const char *mode = json_string_any(root, "default_mode", "mode", NULL);
    const char *policy = json_string_any(root, "policy_json", "policy", NULL);
    const char *skills = json_string_any(root, "skills_json", "skills", NULL);
    const cJSON *web_search = cJSON_GetObjectItemCaseSensitive(root, "web_search");
    bool web_search_present = cJSON_IsObject(web_search);
    const cJSON *web_enabled = web_search_present ?
                                cJSON_GetObjectItemCaseSensitive(web_search, "enabled") : NULL;
    const cJSON *web_clear_key = web_search_present ?
                                 cJSON_GetObjectItemCaseSensitive(web_search, "clear_api_key") : NULL;
    const char *web_profile = web_search_present ?
                              json_string_any(web_search, "profile", "api_profile", "profile_id") : NULL;
    const char *web_endpoint = web_search_present ?
                               json_string_any(web_search, "endpoint", "api_base", "base_url") : NULL;
    const char *web_model = web_search_present ?
                            json_string_any(web_search, "model", "model_name", NULL) : NULL;
    const char *web_strategy = web_search_present ?
                               json_string_any(web_search, "strategy", "search_strategy", NULL) : NULL;
    const char *web_api_key = web_search_present ?
                              json_string_any(web_search, "api_key", "key", "secret") : NULL;
    if (mode && !agent_tools_mode_valid(mode)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "default_mode must be observe, supervised, or autonomous");
    }
    if (policy) {
        ret = agent_tools_validate_policy_json(policy);
        if (ret == ESP_ERR_INVALID_SIZE) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "policy_json too large");
        }
        if (ret != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(
                req, HTTPD_400_BAD_REQUEST,
                "policy_json must contain boolean Agent/MCP switches and valid permission/risk values");
        }
    }
    if (skills) {
        ret = agent_tools_validate_json_text(skills, AGENT_TOOLS_SKILLS_MAX_LEN, true);
        if (ret == ESP_ERR_INVALID_SIZE) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "skills_json too large");
        }
        if (ret != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "skills_json must be a JSON array");
        }
    }
    if (web_search_present) {
        if (web_enabled && !cJSON_IsBool(web_enabled)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "web_search.enabled must be boolean");
        }
        if (web_profile && agent_api_profile_slot(web_profile) < 0) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "web_search.profile must be a known API profile");
        }
        if (web_endpoint &&
            agent_api_validate_endpoint(web_endpoint, true) != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "web_search.endpoint must use https");
        }
        if (web_model &&
            agent_api_validate_text(web_model, AGENT_API_MODEL_MAX_LEN, false) != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "web_search.model invalid or too large");
        }
        if (web_strategy && !agent_web_search_strategy_valid(web_strategy)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "web_search.strategy must be agent, agent_max, turbo, or max");
        }
        if (web_api_key && web_api_key[0] &&
            agent_api_validate_text(web_api_key, AGENT_API_SECRET_MAX_LEN, false) != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "web_search.api_key invalid or too large");
        }
    }

    si_agent_tools_update_t update = {
        .mode = mode,
        .policy = policy,
        .skills = skills,
        .web_present = web_search_present,
        .web_enabled_present = cJSON_IsBool(web_enabled),
        .web_enabled = cJSON_IsTrue(web_enabled),
        .web_profile = web_profile,
        .web_endpoint = web_endpoint,
        .web_model = web_model,
        .web_strategy = web_strategy,
        .web_api_key = web_api_key,
        .clear_web_api_key = cJSON_IsTrue(web_clear_key),
    };
    (void)automation_request_stop_all(
        "Agent/MCP tool policy changed in Settings");
    ret = si_agent_tools_update(&update);
    if (web_api_key && web_api_key[0]) {
        memset((char *)web_api_key, 0, strlen(web_api_key));
    }
    cJSON_Delete(root);

    if (ret != ESP_OK) {
        return si_http_send_text_status(req, "500 Internal Server Error", esp_err_to_name(ret));
    }

    cJSON *resp = cJSON_CreateObject();
    si_web_add_agent_tools_settings_json(resp);
    esp_err_t send_ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return send_ret;
}

#if SI_CFG_EMBEDDED_AGENT_ENABLED
static void add_agent_prompt_json(cJSON *root)
{
    si_agent_prompt_settings_t settings;
    esp_err_t ret = si_agent_prompt_get(&settings);
    if (ret != ESP_OK || settings.system_prompt[0] == '\0') {
        strlcpy(settings.system_prompt, si_agent_prompt_default(),
                sizeof(settings.system_prompt));
        settings.using_default = true;
    }
    cJSON_AddStringToObject(root, "system_prompt", settings.system_prompt);
    cJSON_AddStringToObject(root, "default_system_prompt", si_agent_prompt_default());
    cJSON_AddBoolToObject(root, "using_default", settings.using_default);
    cJSON_AddBoolToObject(root, "stored_in_nvs", !settings.using_default);
    cJSON_AddNumberToObject(root, "max_length", SI_AGENT_PROMPT_MAX_LEN);
}

static esp_err_t settings_agent_prompt_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        add_agent_prompt_json(resp);
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }

    char *buf = calloc(1, AGENT_PROMPT_MAX_BODY);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, AGENT_PROMPT_MAX_BODY, &root);
    free(buf);
    if (ret != ESP_OK) {
        return ret;
    }

    cJSON *reset = cJSON_GetObjectItemCaseSensitive(root, "reset_default");
    (void)automation_request_stop_actor(
        SI_RESOURCE_OPERATION_ACTOR_AGENT,
        "Agent prompt changed in Settings");
    if (cJSON_IsTrue(reset)) {
        cJSON_Delete(root);
        ret = si_agent_prompt_reset();
    } else {
        const char *prompt = json_string_any(root, "system_prompt", "prompt", "system");
        if (!prompt) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "system_prompt required");
        }
        ret = si_agent_prompt_set(prompt);
        cJSON_Delete(root);
    }
    if (ret == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "system prompt must be 1..2048 bytes");
    }
    if (ret == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "system prompt contains invalid control characters");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "save agent prompt");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    add_agent_prompt_json(resp);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

#include "agent_api_settings_http_module.inc"
#endif
