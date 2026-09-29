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
    bool setup;
    uint32_t credential_generation;
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
static esp_err_t auth_login_send_success(httpd_req_t *req, bool mcp_client, uint32_t generation)
{
    si_auth_status_t status;
    si_auth_get_status(&status);
    char token[SI_AUTH_TOKEN_LEN + 1] = {0};
    char cookie[SI_HTTP_SESSION_COOKIE_MAX_LEN];
    if (si_auth_create_session_for_generation(mcp_client ? "exoanchor-mcp" : "web", generation, token) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "session creation failed");
    if (!mcp_client && si_http_set_session_cookie(req, token, cookie, sizeof(cookie)) != ESP_OK) {
        si_auth_revoke_session(token);
        return ESP_FAIL;
    }
    si_auth_record_login();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "token", token);
    cJSON_AddStringToObject(resp, "username", status.username);
    cJSON_AddStringToObject(resp, "type", "bearer");
    cJSON_AddBoolToObject(resp, "enabled", true);
    cJSON_AddBoolToObject(resp, "using_default", status.using_default);
    cJSON_AddBoolToObject(resp, "must_change_credentials", status.using_default);
    cJSON_AddNumberToObject(resp, "expires_in", 86400);
    esp_err_t ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    si_secret_store_clear(token, sizeof(token));
    return ret;
}

static void auth_login_worker(void *arg)
{
    (void)arg;
    char username[SI_AUTH_USERNAME_MAX_LEN + 1] = {0};
    char password[SI_AUTH_PASSWORD_MAX_LEN + 1] = {0};
    bool have_job = false;
    bool setup = false;
    uint32_t expected_generation = 0;

    if (s_auth_login_lock &&
        xSemaphoreTake(s_auth_login_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (s_auth_login_job.state == AUTH_LOGIN_JOB_QUEUED) {
            strlcpy(username, s_auth_login_job.username, sizeof(username));
            strlcpy(password, s_auth_login_job.password, sizeof(password));
            si_secret_store_clear(s_auth_login_job.username,
                                  sizeof(s_auth_login_job.username));
            si_secret_store_clear(s_auth_login_job.password,
                                  sizeof(s_auth_login_job.password));
            setup = s_auth_login_job.setup;
            expected_generation = s_auth_login_job.credential_generation;
            s_auth_login_job.state = AUTH_LOGIN_JOB_RUNNING;
            have_job = true;
        }
        xSemaphoreGive(s_auth_login_lock);
    }

    bool accepted = have_job && (setup ?
        si_auth_setup_credentials(username, password, expected_generation) == ESP_OK :
        si_auth_credentials_match(username, password));
    uint32_t accepted_generation = setup && accepted ? expected_generation + 1U : expected_generation;
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
                s_auth_login_job.credential_generation = accepted_generation;
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
    bool setup = strncmp(req->uri, "/api/auth/setup", 15) == 0;
    if (!si_auth_is_enabled()) {
        return auth_login_send_success(req, mcp_client, 0);
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
    if (setup && (mcp_client || !si_auth_setup_required() ||
        !cJSON_IsString(username) || !cJSON_IsString(password) ||
        si_auth_validate_username(username->valuestring) != ESP_OK ||
        si_auth_validate_password(password->valuestring) != ESP_OK)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "first-account setup unavailable or invalid");
    }
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
        s_auth_login_job.mcp_client == mcp_client && s_auth_login_job.setup == setup;
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
    s_auth_login_job.setup = setup;
    s_auth_login_job.credential_generation = si_auth_credential_generation();
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
    uint32_t generation = s_auth_login_job.credential_generation;
    bool accepted =
        s_auth_login_job.state == AUTH_LOGIN_JOB_ACCEPTED &&
        generation == si_auth_credential_generation();
    auth_login_job_reset_locked();
    xSemaphoreGive(s_auth_login_lock);

    if (!accepted) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED,
                                   "invalid credentials");
    }
    return auth_login_send_success(req, mcp_client, generation);
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
    cJSON_AddBoolToObject(root, "setup_required", auth_status.setup_required);
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

    const char *username = json_string_any(root, "username", "new_username", "newUsername");
    const char *password = json_string_any(root, "new_password", "newPassword", "password");
    si_auth_status_t auth_status;
    si_auth_get_status(&auth_status);
    if (!username) {
        username = auth_status.username;
    }
    bool authorized = si_http_check_recent_auth(req);
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
    char token[SI_AUTH_TOKEN_LEN + 1] = {0};
    char cookie[SI_HTTP_SESSION_COOKIE_MAX_LEN];
    if (si_auth_create_session(token) != ESP_OK ||
        si_http_set_session_cookie(req, token, cookie, sizeof(cookie)) != ESP_OK) {
        si_auth_revoke_session(token);
        cJSON_Delete(resp);
        return ESP_FAIL;
    }
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

#include "auth_setup_http_module.inc"
