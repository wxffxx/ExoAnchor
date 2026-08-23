// Authenticated target-host UART HTTP and WebSocket bridge.
#define UART_HTTP_MAX_BODY 4096
#define UART_HTTP_MAX_READ 2048
#define UART_AUTH_PROMPT_MAX 96
#define UART_AUTH_CREDENTIAL_REF_MAX 32
#if SI_CFG_UART_TERMINAL_ENABLED && defined(CONFIG_HTTPD_WS_SUPPORT)
#define UART_WS_MAX_FRAME 4096
#define UART_WS_OUTPUT_BUFFER_BYTES 8192
#define UART_WS_OUTPUT_BATCH_BYTES 512
#define UART_WS_OUTPUT_COALESCE_MS 5
#define UART_WS_OUTPUT_TASK_STACK 4096
#define UART_WS_OUTPUT_TASK_PRIORITY 8

typedef struct {
    httpd_handle_t handle;
    int fd;
} uart_ws_ctx_t;

typedef struct {
    httpd_handle_t handle;
    int fd;
    httpd_ws_type_t type;
    size_t len;
    uint8_t payload[];
} uart_ws_send_job_t;
#endif

static SemaphoreHandle_t s_uart_ws_lock;
static SemaphoreHandle_t s_uart_operation_lock;
#if SI_CFG_UART_TERMINAL_ENABLED && defined(CONFIG_HTTPD_WS_SUPPORT)
static httpd_handle_t s_uart_ws_handle;
static int s_uart_ws_fd = -1;
static StreamBufferHandle_t s_uart_ws_output_buffer;
static TaskHandle_t s_uart_ws_output_task_handle;
static portMUX_TYPE s_uart_ws_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_uart_ws_dropped_bytes;
static bool s_uart_ws_replaying;

static void uart_ws_set_replaying(bool replaying)
{
    portENTER_CRITICAL(&s_uart_ws_stats_lock);
    s_uart_ws_replaying = replaying;
    portEXIT_CRITICAL(&s_uart_ws_stats_lock);
}

static bool uart_ws_is_replaying(void)
{
    portENTER_CRITICAL(&s_uart_ws_stats_lock);
    bool replaying = s_uart_ws_replaying;
    portEXIT_CRITICAL(&s_uart_ws_stats_lock);
    return replaying;
}

static bool uart_ws_has_manual_client(void)
{
    if (!s_uart_ws_lock) {
        return false;
    }
    if (xSemaphoreTake(s_uart_ws_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        // Agent mutations must fail closed when manual ownership cannot be
        // determined without blocking the runtime.
        return true;
    }
    bool connected = s_uart_ws_handle != NULL && s_uart_ws_fd >= 0;
    xSemaphoreGive(s_uart_ws_lock);
    return connected;
}
#else
static bool uart_ws_has_manual_client(void)
{
    return false;
}
#endif

static esp_err_t uart_mutation_begin(void)
{
    if (!s_uart_operation_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_uart_operation_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void uart_mutation_end(void)
{
    if (s_uart_operation_lock) {
        xSemaphoreGive(s_uart_operation_lock);
    }
}

static esp_err_t uart_mutation_denied_response(httpd_req_t *req,
                                               esp_err_t reason)
{
    return httpd_resp_send_err(
        req, HTTPD_500_INTERNAL_SERVER_ERROR,
        reason == ESP_ERR_TIMEOUT ? "UART ownership check timed out" :
                                    "UART ownership unavailable");
}

typedef struct {
    uint64_t cursor_before;
    size_t bytes_written;
    size_t output_bytes;
    si_target_uart_journal_t journal;
    char credential_source[24];
} uart_auth_result_t;

static bool uart_auth_prompt_valid(const char *prompt)
{
    if (!prompt) return false;
    size_t len = strlen(prompt);
    if (len == 0 || len > UART_AUTH_PROMPT_MAX ||
        !strcasestr(prompt, "password") ||
        isspace((unsigned char)prompt[len - 1])) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char value = (unsigned char)prompt[i];
        if (value < 0x20U || value == 0x7fU) return false;
    }
    return true;
}

static bool uart_auth_credential_ref_valid(const char *credential_ref)
{
    return credential_ref &&
           (!strcmp(credential_ref, "auto://sudo") ||
            !strcmp(credential_ref, "ssh-sudo://default") ||
            !strcmp(credential_ref, "console://default"));
}

static esp_err_t uart_auth_load_credential(
    const char *credential_ref, char *secret, size_t secret_size,
    char *source, size_t source_size)
{
    if (!uart_auth_credential_ref_valid(credential_ref) ||
        !secret || secret_size == 0) return ESP_ERR_INVALID_ARG;
    si_secret_store_clear(secret, secret_size);
    if ((!strcmp(credential_ref, "auto://sudo") ||
         !strcmp(credential_ref, "ssh-sudo://default")) &&
        ssh_target_load_sudo_password(secret, secret_size) == ESP_OK &&
        secret[0]) {
        strlcpy(source, "ssh_sudo", source_size);
        return ESP_OK;
    }
    si_secret_store_clear(secret, secret_size);
    if (!strcmp(credential_ref, "ssh-sudo://default"))
        return ESP_ERR_NOT_FOUND;
    if (si_console_credentials_get_password(secret, secret_size) == ESP_OK &&
        secret[0]) {
        strlcpy(source, "console", source_size);
        return ESP_OK;
    }
    si_secret_store_clear(secret, secret_size);
    return ESP_ERR_NOT_FOUND;
}

static bool uart_auth_prompt_matches_tail(const char *expected_prompt)
{
    uint8_t tail[512] = {0};
    si_target_uart_journal_t journal = {0};
    size_t len = si_target_uart_journal_read(tail, sizeof(tail), &journal);
    while (len > 0 && isspace((unsigned char)tail[len - 1])) len--;
    size_t prompt_len = strlen(expected_prompt);
    return len >= prompt_len &&
           memcmp(tail + len - prompt_len, expected_prompt, prompt_len) == 0;
}

static void uart_auth_redact_echo(uint8_t *data, size_t len,
                                  const char *secret)
{
    if (!data || !secret || !secret[0]) return;
    size_t secret_len = strlen(secret);
    if (secret_len == 0 || secret_len > len) return;
    for (size_t i = 0; i + secret_len <= len; i++) {
        if (memcmp(data + i, secret, secret_len) == 0) {
            memset(data + i, '*', secret_len);
            i += secret_len - 1U;
        }
    }
}

static esp_err_t uart_authenticate_local(
    si_terminal_actor_t actor, const char *job_id, const char *operation_id,
    const char *credential_ref, const char *expected_prompt,
    uint32_t wait_ms, uint8_t *output, size_t output_size,
    uart_auth_result_t *result, char *error, size_t error_size)
{
    if (result) memset(result, 0, sizeof(*result));
    if (error && error_size) error[0] = '\0';
    if (!result || !output || output_size == 0 ||
        !uart_auth_credential_ref_valid(credential_ref) ||
        !uart_auth_prompt_valid(expected_prompt)) {
        if (error && error_size)
            strlcpy(error, "invalid credential_ref or expected_prompt",
                    error_size);
        return ESP_ERR_INVALID_ARG;
    }
    si_target_uart_status_t status = {0};
    si_target_uart_get_status(&status);
    if (!status.initialized) {
        if (error && error_size)
            strlcpy(error, "target UART unavailable", error_size);
        return ESP_ERR_INVALID_STATE;
    }
    si_terminal_control_token_t control = {0};
    if (!terminal_control_begin(
            SI_TERMINAL_CHANNEL_UART, actor, operation_id, job_id,
            actor == SI_TERMINAL_ACTOR_MCP ?
            "MCP 正在通过本地凭据认证 UART" :
            "Agent 正在通过本地凭据认证 UART",
            &control)) {
        if (error && error_size)
            strlcpy(error, "UART automation already active", error_size);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = uart_mutation_begin();
    if (ret != ESP_OK) {
        (void)terminal_control_finish(&control);
        if (error && error_size)
            strlcpy(error, "UART operation lock failed", error_size);
        return ret;
    }
    si_target_uart_get_status(&status);
    if (!status.initialized || !uart_auth_prompt_matches_tail(expected_prompt)) {
        uart_mutation_end();
        (void)terminal_control_finish(&control);
        if (error && error_size)
            strlcpy(error,
                    "current UART tail does not match expected_prompt",
                    error_size);
        return ESP_ERR_INVALID_STATE;
    }
    result->cursor_before = status.journal_next_seq;
    char secret[SI_SSH_PASSWORD_MAX_LEN] = {0};
    ret = uart_auth_load_credential(
        credential_ref, secret, sizeof(secret), result->credential_source,
        sizeof(result->credential_source));
    if (ret != ESP_OK) {
        uart_mutation_end();
        (void)terminal_control_finish(&control);
        if (error && error_size)
            strlcpy(error, "referenced local credential is not configured",
                    error_size);
        return ret;
    }
    terminal_control_publish_input(&control, NULL, true, true);
    size_t secret_len = strlen(secret);
    uint8_t payload[SI_SSH_PASSWORD_MAX_LEN + 1U] = {0};
    memcpy(payload, secret, secret_len);
    payload[secret_len] = '\r';
    ret = si_target_uart_write(payload, secret_len + 1U,
                               &result->bytes_written);
    si_secret_store_clear(payload, sizeof(payload));
    uart_mutation_end();
    if (ret == ESP_OK) {
        uint32_t bounded_wait = wait_ms > 5000U ? 5000U : wait_ms;
        int64_t deadline = si_monotonic_ms() + bounded_wait;
        while (bounded_wait > 0 && si_monotonic_ms() < deadline &&
               !terminal_control_cancel_requested(&control)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        result->output_bytes = si_target_uart_journal_read_from(
            result->cursor_before, output, output_size, &result->journal);
        uart_auth_redact_echo(output, result->output_bytes, secret);
    }
    si_secret_store_clear(secret, sizeof(secret));
    (void)terminal_control_finish(&control);
    if (ret != ESP_OK && error && error_size)
        strlcpy(error, esp_err_to_name(ret), error_size);
    return ret;
}

static void uart_add_journal_bytes_json(
    cJSON *item, const uint8_t *data, size_t len,
    const si_target_uart_journal_t *journal)
{
    char *safe = si_utf8_sanitize((const char *)data, len);
    size_t base64_size = 4U * ((len + 2U) / 3U) + 1U;
    unsigned char *base64 = calloc(1, base64_size);
    size_t base64_len = 0;
    if (base64 && len > 0 &&
        mbedtls_base64_encode(base64, base64_size, &base64_len,
                              data, len) != 0) {
        base64[0] = '\0';
    }
    cJSON_AddNumberToObject(item, "bytes", len);
    cJSON_AddStringToObject(item, "text", safe ? safe : "");
    cJSON_AddStringToObject(item, "base64",
                            base64 ? (const char *)base64 : "");
    cJSON_AddNumberToObject(item, "start_seq",
                            journal ? (double)journal->start_seq : 0);
    cJSON_AddNumberToObject(
        item, "next_cursor",
        journal ? (double)(journal->start_seq + len) : 0);
    cJSON_AddNumberToObject(item, "journal_next_seq",
                            journal ? (double)journal->next_seq : 0);
    cJSON_AddBoolToObject(item, "history_lost",
                          journal ? journal->history_lost : false);
    free(safe);
    free(base64);
}

static void uart_add_status_json(cJSON *root)
{
    si_target_uart_status_t status;
    si_target_uart_get_status(&status);
    cJSON_AddBoolToObject(root, "supported", status.supported);
    cJSON_AddBoolToObject(root, "initialized", status.initialized);
    cJSON_AddNumberToObject(root, "port", status.port);
    cJSON_AddNumberToObject(root, "rx_gpio", status.rx_gpio);
    cJSON_AddNumberToObject(root, "tx_gpio", status.tx_gpio);
    cJSON_AddNumberToObject(root, "baud_rate", status.baud_rate);
    cJSON_AddNumberToObject(root, "default_baud_rate",
                            status.default_baud_rate);
    cJSON_AddNumberToObject(root, "fallback_baud_rate",
                            status.fallback_baud_rate);
    cJSON_AddStringToObject(
        root, "baud_mode",
        status.baud_rate == status.default_baud_rate ? "fast" :
        status.baud_rate == status.fallback_baud_rate ? "fallback" :
        "custom");
    cJSON_AddStringToObject(root, "format", "8N1");
    cJSON_AddBoolToObject(root, "flow_control", false);
    cJSON_AddNumberToObject(root, "rx_bytes", (double)status.rx_bytes);
    cJSON_AddNumberToObject(root, "tx_bytes", (double)status.tx_bytes);
    cJSON_AddNumberToObject(root, "buffered_bytes",
                            (double)status.buffered_bytes);
    cJSON_AddNumberToObject(root, "dropped_bytes",
                            (double)status.dropped_bytes);
    char boot_id[24] = {0};
    snprintf(boot_id, sizeof(boot_id), "%016" PRIx64, status.boot_id);
    cJSON_AddStringToObject(root, "boot_id", boot_id);
    cJSON_AddNumberToObject(root, "uart_generation",
                            (double)status.generation);
    cJSON_AddNumberToObject(root, "journal_start_seq",
                            (double)status.journal_start_seq);
    cJSON_AddNumberToObject(root, "journal_next_seq",
                            (double)status.journal_next_seq);
    char journal_start_cursor[24] = {0};
    char journal_next_cursor[24] = {0};
    snprintf(journal_start_cursor, sizeof(journal_start_cursor),
             "%" PRIu64, status.journal_start_seq);
    snprintf(journal_next_cursor, sizeof(journal_next_cursor),
             "%" PRIu64, status.journal_next_seq);
    cJSON_AddStringToObject(root, "journal_start_cursor",
                            journal_start_cursor);
    cJSON_AddStringToObject(root, "journal_next_cursor",
                            journal_next_cursor);
    cJSON_AddNumberToObject(root, "journal_bytes",
                            (double)status.journal_bytes);
    cJSON_AddBoolToObject(root, "journal_history_lost",
                          status.journal_history_lost);
    uint64_t websocket_dropped_bytes = 0;
#if SI_CFG_UART_TERMINAL_ENABLED && defined(CONFIG_HTTPD_WS_SUPPORT)
    portENTER_CRITICAL(&s_uart_ws_stats_lock);
    websocket_dropped_bytes = s_uart_ws_dropped_bytes;
    portEXIT_CRITICAL(&s_uart_ws_stats_lock);
#endif
    cJSON_AddNumberToObject(root, "websocket_dropped_bytes",
                            (double)websocket_dropped_bytes);
    cJSON_AddStringToObject(root, "last_error", status.last_error);
    cJSON_AddBoolToObject(root, "terminal_enabled",
                          SI_CFG_UART_TERMINAL_ENABLED);
    cJSON_AddStringToObject(root, "websocket",
                            SI_CFG_UART_TERMINAL_ENABLED ?
                            "/api/ws/uart" : "");
    cJSON_AddStringToObject(root, "read_endpoint", "/api/uart/read");
    cJSON_AddStringToObject(root, "write_endpoint", "/api/uart/write");
    cJSON_AddStringToObject(root, "baud_endpoint", "/api/uart/baud");
    cJSON_AddStringToObject(root, "authenticate_endpoint",
                            "/api/uart/authenticate");

    cJSON_AddBoolToObject(root, "websocket_connected",
                          uart_ws_has_manual_client());
    terminal_control_add_status_json(root, SI_TERMINAL_CHANNEL_UART);
}

static esp_err_t uart_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    uart_add_status_json(root);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t uart_baud_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret =
        si_http_require_action(req, SI_CAPABILITY_UART, "uart_baud",
                               SI_AUTHZ_RISK_HIGH, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    char body[UART_HTTP_MAX_BODY + 1];
    cJSON *request = NULL;
    esp_err_t ret = si_http_recv_json(req, body, sizeof(body), &request);
    if (ret != ESP_OK) {
        return ret;
    }

    const cJSON *baud_item =
        cJSON_GetObjectItemCaseSensitive(request, "baud_rate");
    if (!cJSON_IsNumber(baud_item)) {
        cJSON_Delete(request);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "baud_rate number required");
    }
    int baud_rate = baud_item->valueint;
    cJSON_Delete(request);
    if (baud_rate != SI_CFG_TARGET_UART_BAUD_RATE &&
        baud_rate != SI_CFG_TARGET_UART_FALLBACK_BAUD_RATE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "unsupported UART baud mode");
    }

    si_terminal_control_token_t control = {0};
    bool automated = session.principal == SI_PRINCIPAL_MCP;
    if (automated && !terminal_control_begin(
            SI_TERMINAL_CHANNEL_UART, SI_TERMINAL_ACTOR_MCP, "uart_baud",
            NULL, "切换 UART 波特率", &control))
        return si_http_send_text_status(req, "409 Conflict",
                                        "UART automation already active");
    ret = uart_mutation_begin();
    if (ret != ESP_OK) {
        if (automated) (void)terminal_control_finish(&control);
        return uart_mutation_denied_response(req, ret);
    }
    ret = si_target_uart_set_baud_rate(baud_rate);
    uart_mutation_end();
    if (automated) (void)terminal_control_finish(&control);
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(ret));
    }

    cJSON *response = cJSON_CreateObject();
    if (!response) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(response, "ok", true);
    uart_add_status_json(response);
    esp_err_t send_ret = si_http_send_json(req, response);
    cJSON_Delete(response);
    return send_ret;
}

static esp_err_t uart_read_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    uint64_t cursor = 0;
    size_t max_bytes = UART_HTTP_MAX_READ;
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        if (query_len >= 128U) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "UART read query too large");
        }
        char query[128] = {0};
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "invalid UART read query");
        }
        char value[32] = {0};
        if (httpd_query_key_value(query, "cursor", value,
                                  sizeof(value)) == ESP_OK) {
            char *end = NULL;
            errno = 0;
            unsigned long long parsed = strtoull(value, &end, 10);
            if (errno != 0 || !end || *end != '\0') {
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                           "cursor must be uint64 decimal");
            }
            cursor = (uint64_t)parsed;
        }
        if (httpd_query_key_value(query, "max_bytes", value,
                                  sizeof(value)) == ESP_OK) {
            char *end = NULL;
            errno = 0;
            unsigned long parsed = strtoul(value, &end, 10);
            if (errno != 0 || !end || *end != '\0' ||
                parsed == 0 || parsed > UART_HTTP_MAX_READ) {
                return httpd_resp_send_err(
                    req, HTTPD_400_BAD_REQUEST,
                    "max_bytes must be between 1 and 2048");
            }
            max_bytes = (size_t)parsed;
        }
    }

    uint8_t data[UART_HTTP_MAX_READ + 1] = {0};
    si_target_uart_journal_t journal = {0};
    size_t len = si_target_uart_journal_read_from(
        cursor, data, max_bytes, &journal);
    data[len] = '\0';
    char *text = si_utf8_sanitize((const char *)data, len);
    size_t base64_size = 4U * ((len + 2U) / 3U) + 1U;
    unsigned char *base64 = calloc(1, base64_size);
    size_t base64_len = 0;
    if (base64 && len > 0 &&
        mbedtls_base64_encode(base64, base64_size, &base64_len,
                              data, len) != 0) {
        base64[0] = '\0';
        base64_len = 0;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(text);
        free(base64);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "bytes", len);
    cJSON_AddStringToObject(root, "text", text ? text : "");
    cJSON_AddStringToObject(root, "data", text ? text : "");
    cJSON_AddStringToObject(root, "base64",
                            base64 ? (const char *)base64 : "");
    cJSON_AddBoolToObject(root, "empty", len == 0);
    char boot_id[24] = {0};
    char generation[24] = {0};
    char start_cursor[24] = {0};
    char next_cursor[24] = {0};
    char journal_next_cursor[24] = {0};
    snprintf(boot_id, sizeof(boot_id), "%016" PRIx64, journal.boot_id);
    snprintf(generation, sizeof(generation), "%" PRIu64,
             journal.generation);
    snprintf(start_cursor, sizeof(start_cursor), "%" PRIu64,
             journal.start_seq);
    snprintf(next_cursor, sizeof(next_cursor), "%" PRIu64,
             journal.start_seq + len);
    snprintf(journal_next_cursor, sizeof(journal_next_cursor), "%" PRIu64,
             journal.next_seq);
    cJSON_AddStringToObject(root, "boot_id", boot_id);
    cJSON_AddStringToObject(root, "generation", generation);
    cJSON_AddStringToObject(root, "start_cursor", start_cursor);
    cJSON_AddStringToObject(root, "next_cursor", next_cursor);
    cJSON_AddStringToObject(root, "journal_next_cursor",
                            journal_next_cursor);
    cJSON_AddNumberToObject(
        root, "pending_bytes",
        journal.next_seq > journal.start_seq + len ?
        (double)(journal.next_seq - journal.start_seq - len) : 0);
    cJSON_AddBoolToObject(root, "history_lost", journal.history_lost);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    free(text);
    free(base64);
    return ret;
}

static esp_err_t uart_write_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret =
        si_http_require_action(req, SI_CAPABILITY_UART, "uart_write",
                               SI_AUTHZ_RISK_HIGH, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    char body[UART_HTTP_MAX_BODY + 1];
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, body, sizeof(body), &root);
    if (ret != ESP_OK) {
        return ret;
    }

    const cJSON *data_item =
        cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *newline_item =
        cJSON_GetObjectItemCaseSensitive(root, "append_newline");
    if (!cJSON_IsString(data_item) || !data_item->valuestring) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "data string required");
    }

    size_t len = strlen(data_item->valuestring);
    if (len > UART_HTTP_MAX_BODY) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "UART write too large");
    }

    si_terminal_control_token_t control = {0};
    bool automated = session.principal == SI_PRINCIPAL_MCP;
    if (automated && !terminal_control_begin(
            SI_TERMINAL_CHANNEL_UART, SI_TERMINAL_ACTOR_MCP, "uart_write",
            NULL, "向 UART 发送输入", &control)) {
        cJSON_Delete(root);
        return si_http_send_text_status(req, "409 Conflict",
                                        "UART automation already active");
    }
    if (automated) terminal_control_publish_input(
        &control, data_item->valuestring, cJSON_IsTrue(newline_item),
        terminal_control_text_sensitive(data_item->valuestring));
    ret = uart_mutation_begin();
    if (ret != ESP_OK) {
        if (automated) (void)terminal_control_finish(&control);
        cJSON_Delete(root);
        return uart_mutation_denied_response(req, ret);
    }

    size_t written = 0;
    ret = len > 0 ?
        si_target_uart_write((const uint8_t *)data_item->valuestring,
                             len, &written) : ESP_OK;
    if (ret == ESP_OK && cJSON_IsTrue(newline_item)) {
        size_t newline_written = 0;
        ret = si_target_uart_write((const uint8_t *)"\r", 1,
                                   &newline_written);
        written += newline_written;
    }
    uart_mutation_end();
    if (automated) (void)terminal_control_finish(&control);
    cJSON_Delete(root);

    cJSON *response = cJSON_CreateObject();
    if (!response) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(response, "ok", ret == ESP_OK);
    cJSON_AddNumberToObject(response, "bytes_written", written);
    if (ret != ESP_OK) {
        cJSON_AddStringToObject(response, "error", esp_err_to_name(ret));
    }
    esp_err_t send_ret = si_http_send_json(req, response);
    cJSON_Delete(response);
    return send_ret;
}

static esp_err_t uart_authenticate_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t ret = si_http_require_action(
        req, SI_CAPABILITY_UART, "uart_auth",
        SI_AUTHZ_RISK_HIGH, &session);
    if (ret != ESP_OK) return ret;
    if (session.principal != SI_PRINCIPAL_MCP)
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "MCP principal required");
    char body[512] = {0};
    cJSON *request = NULL;
    ret = si_http_recv_json(req, body, sizeof(body), &request);
    if (ret != ESP_OK) return ret;
    const char *credential_ref = json_string_any(
        request, "credential_ref", "credential", NULL);
    const char *expected_prompt = json_string_any(
        request, "expected_prompt", "prompt", NULL);
    const cJSON *wait_item = json_number_any(
        request, "wait_ms", "timeout_ms");
    const cJSON *max_item = json_number_any(
        request, "read_max_bytes", "max_bytes");
    uint32_t wait_ms = wait_item && wait_item->valueint >= 0 ?
        (uint32_t)wait_item->valueint : 500U;
    size_t max_bytes = max_item && max_item->valueint > 0 ?
        (size_t)max_item->valueint : 1024U;
    if (max_bytes > UART_HTTP_MAX_READ) max_bytes = UART_HTTP_MAX_READ;
    if (!credential_ref) credential_ref = "auto://sudo";
    uint8_t *output = calloc(1, max_bytes + 1U);
    if (!output) {
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }
    uart_auth_result_t auth = {0};
    char error[128] = {0};
    ret = uart_authenticate_local(
        SI_TERMINAL_ACTOR_MCP, NULL, "uart_auth", credential_ref,
        expected_prompt, wait_ms, output, max_bytes, &auth,
        error, sizeof(error));
    cJSON_Delete(request);
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        free(output);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(response, "ok", ret == ESP_OK);
    cJSON_AddBoolToObject(response, "credential_redacted", true);
    cJSON_AddStringToObject(response, "credential_source",
                            auth.credential_source);
    cJSON_AddNumberToObject(response, "cursor_before",
                            (double)auth.cursor_before);
    cJSON_AddBoolToObject(response, "submitted", ret == ESP_OK);
    if (ret == ESP_OK) {
        cJSON *response_output =
            cJSON_AddObjectToObject(response, "output");
        uart_add_journal_bytes_json(response_output, output,
                                    auth.output_bytes, &auth.journal);
    } else {
        cJSON_AddStringToObject(response, "error",
                                error[0] ? error : esp_err_to_name(ret));
    }
    free(output);
    esp_err_t send_ret = si_http_send_json(req, response);
    cJSON_Delete(response);
    return send_ret;
}

#if SI_CFG_UART_TERMINAL_ENABLED && defined(CONFIG_HTTPD_WS_SUPPORT)
static void uart_ws_send_work(void *arg)
{
    uart_ws_send_job_t *job = (uart_ws_send_job_t *)arg;
    if (!job) {
        return;
    }
    httpd_ws_frame_t frame = {
        .type = job->type,
        .payload = job->payload,
        .len = job->len,
    };
    (void)httpd_ws_send_frame_async(job->handle, job->fd, &frame);
    free(job);
}

static esp_err_t uart_ws_send_async(httpd_handle_t handle, int fd,
                                    httpd_ws_type_t type,
                                    const void *data, size_t len)
{
    if (!handle || fd < 0 || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uart_ws_send_job_t *job = malloc(sizeof(*job) + len);
    if (!job) {
        return ESP_ERR_NO_MEM;
    }
    job->handle = handle;
    job->fd = fd;
    job->type = type;
    job->len = len;
    memcpy(job->payload, data, len);
    esp_err_t ret = httpd_queue_work(handle, uart_ws_send_work, job);
    if (ret != ESP_OK) {
        free(job);
    }
    return ret;
}

static esp_err_t uart_ws_send_terminal_json(const char *json, size_t len)
{
    httpd_handle_t handle = NULL;
    int fd = -1;
    if (!json || len == 0 || !s_uart_ws_lock ||
        xSemaphoreTake(s_uart_ws_lock, pdMS_TO_TICKS(20)) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    handle = s_uart_ws_handle;
    fd = s_uart_ws_fd;
    xSemaphoreGive(s_uart_ws_lock);
    return handle && fd >= 0 ?
        uart_ws_send_async(handle, fd, HTTPD_WS_TYPE_TEXT, json, len) :
        ESP_ERR_NOT_FOUND;
}

static void uart_ws_output_task(void *arg)
{
    (void)arg;
    uint8_t data[UART_WS_OUTPUT_BATCH_BYTES];

    while (true) {
        while (uart_ws_is_replaying()) {
            vTaskDelay(pdMS_TO_TICKS(UART_WS_OUTPUT_COALESCE_MS));
        }
        size_t len = xStreamBufferReceive(s_uart_ws_output_buffer,
                                          data, sizeof(data),
                                          portMAX_DELAY);
        if (len == 0) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(UART_WS_OUTPUT_COALESCE_MS));
        while (len < sizeof(data)) {
            size_t more = xStreamBufferReceive(s_uart_ws_output_buffer,
                                               data + len,
                                               sizeof(data) - len, 0);
            if (more == 0) {
                break;
            }
            len += more;
        }

        while (uart_ws_is_replaying()) {
            vTaskDelay(pdMS_TO_TICKS(UART_WS_OUTPUT_COALESCE_MS));
        }
        httpd_handle_t handle = NULL;
        int fd = -1;
        if (s_uart_ws_lock &&
            xSemaphoreTake(s_uart_ws_lock,
                           pdMS_TO_TICKS(20)) == pdTRUE) {
            handle = s_uart_ws_handle;
            fd = s_uart_ws_fd;
            xSemaphoreGive(s_uart_ws_lock);
        }
        if (handle && fd >= 0) {
            (void)uart_ws_send_async(handle, fd,
                                     HTTPD_WS_TYPE_BINARY,
                                     data, len);
        }
    }
}

static void uart_ws_output_callback(const uint8_t *data, size_t len,
                                    void *user_ctx)
{
    (void)user_ctx;
    bool connected = false;
    if (!s_uart_ws_lock ||
        xSemaphoreTake(s_uart_ws_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    connected = s_uart_ws_handle != NULL && s_uart_ws_fd >= 0;
    xSemaphoreGive(s_uart_ws_lock);
    if (connected && s_uart_ws_output_buffer) {
        size_t buffered =
            xStreamBufferSend(s_uart_ws_output_buffer, data, len, 0);
        portENTER_CRITICAL(&s_uart_ws_stats_lock);
        s_uart_ws_dropped_bytes += len - buffered;
        portEXIT_CRITICAL(&s_uart_ws_stats_lock);
    }
}

static void uart_ws_ctx_free(void *ptr)
{
    uart_ws_ctx_t *ctx = (uart_ws_ctx_t *)ptr;
    if (!ctx) {
        return;
    }
    if (s_uart_ws_lock &&
        xSemaphoreTake(s_uart_ws_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_uart_ws_handle == ctx->handle && s_uart_ws_fd == ctx->fd) {
            s_uart_ws_handle = NULL;
            s_uart_ws_fd = -1;
            uart_ws_set_replaying(false);
        }
        xSemaphoreGive(s_uart_ws_lock);
    }
    free(ctx);
}

static esp_err_t uart_ws_bridge_start(void)
{
    if (!s_uart_operation_lock) {
        s_uart_operation_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_uart_operation_lock, ESP_ERR_NO_MEM, TAG,
                            "create UART operation mutex");
    }
    if (!s_uart_ws_lock) {
        s_uart_ws_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_uart_ws_lock, ESP_ERR_NO_MEM, TAG,
                            "create UART WebSocket mutex");
    }
    if (!s_uart_ws_output_buffer) {
        s_uart_ws_output_buffer =
            xStreamBufferCreate(UART_WS_OUTPUT_BUFFER_BYTES, 1);
        ESP_RETURN_ON_FALSE(s_uart_ws_output_buffer, ESP_ERR_NO_MEM, TAG,
                            "create UART WebSocket output buffer");
    }
    if (!s_uart_ws_output_task_handle) {
        BaseType_t task_ret =
            xTaskCreate(uart_ws_output_task, "uart-ws-output",
                        UART_WS_OUTPUT_TASK_STACK, NULL,
                        UART_WS_OUTPUT_TASK_PRIORITY,
                        &s_uart_ws_output_task_handle);
        if (task_ret != pdPASS) {
            vStreamBufferDelete(s_uart_ws_output_buffer);
            s_uart_ws_output_buffer = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    si_target_uart_set_rx_callback(uart_ws_output_callback, NULL);
    return ESP_OK;
}

static esp_err_t uart_ws_pre_handshake(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_action(req, SI_CAPABILITY_UART, "uart_terminal",
                               SI_AUTHZ_RISK_MEDIUM, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    si_target_uart_status_t status;
    si_target_uart_get_status(&status);
    if (!status.initialized) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "target UART unavailable");
    }
    if (xSemaphoreTake(s_uart_ws_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    bool occupied = s_uart_ws_handle != NULL && s_uart_ws_fd >= 0;
    xSemaphoreGive(s_uart_ws_lock);
    if (occupied) {
        return si_http_send_text_status(req, "409 Conflict",
                                        "UART WebSocket already connected");
    }

    uart_ws_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    ctx->handle = req->handle;
    ctx->fd = httpd_req_to_sockfd(req);
    req->sess_ctx = ctx;
    req->free_ctx = uart_ws_ctx_free;
    return ESP_OK;
}

static esp_err_t uart_ws_post_handshake(httpd_req_t *req)
{
    /* ESP-IDF 5.5 completes the upgrade without calling the normal URI
     * handler.  Publish the manual owner and send an application-level replay
     * acknowledgement here, after the 101 response is on the wire. */
    uart_ws_ctx_t *ctx = (uart_ws_ctx_t *)req->sess_ctx;
    if (!ctx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_uart_ws_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_uart_ws_handle != NULL && s_uart_ws_fd >= 0) {
        xSemaphoreGive(s_uart_ws_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t stale[UART_WS_OUTPUT_BATCH_BYTES];
    while (xStreamBufferReceive(s_uart_ws_output_buffer, stale,
                                sizeof(stale), 0) > 0) {
    }
    s_uart_ws_handle = ctx->handle;
    s_uart_ws_fd = ctx->fd;
    uart_ws_set_replaying(true);
    xSemaphoreGive(s_uart_ws_lock);

    uint8_t *replay = heap_caps_malloc(
        UART_WS_OUTPUT_BUFFER_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!replay) {
        replay = malloc(UART_WS_OUTPUT_BUFFER_BYTES);
    }
    si_target_uart_journal_t journal = {0};
    size_t replay_len = 0;
    if (replay) {
        replay_len = si_target_uart_journal_read(
            replay, UART_WS_OUTPUT_BUFFER_BYTES, &journal);
    } else {
        (void)si_target_uart_journal_read(NULL, 0, &journal);
    }
    char replay_meta[256];
    snprintf(replay_meta, sizeof(replay_meta),
             "{\"type\":\"uart.replay\",\"boot_id\":\"%016" PRIx64
             "\",\"generation\":%" PRIu64
             ",\"start_seq\":%" PRIu64
             ",\"next_seq\":%" PRIu64
             ",\"bytes\":%u,\"history_lost\":%s}",
             journal.boot_id, journal.generation,
             journal.start_seq, journal.next_seq,
             (unsigned)replay_len,
             journal.history_lost ? "true" : "false");

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)replay_meta,
        .len = strlen(replay_meta),
    };
    esp_err_t ret = httpd_ws_send_frame(req, &frame);
    if (ret == ESP_OK && replay_len > 0) {
        frame.type = HTTPD_WS_TYPE_BINARY;
        frame.payload = replay;
        frame.len = replay_len;
        ret = httpd_ws_send_frame(req, &frame);
    }
    uart_ws_set_replaying(false);
    free(replay);
    if (ret != ESP_OK) {
        si_web_log("WARN", "Target UART WebSocket replay failed");
        uart_ws_ctx_free(ctx);
        req->sess_ctx = NULL;
        req->free_ctx = NULL;
        return ret;
    }
    terminal_control_publish_state(SI_TERMINAL_CHANNEL_UART);
    si_web_log("INFO", "Target UART WebSocket connected");
    return ESP_OK;
}

static esp_err_t uart_ws_handler(httpd_req_t *req)
{

    uart_ws_ctx_t *ctx = (uart_ws_ctx_t *)req->sess_ctx;
    if (!ctx) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_BINARY;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        uart_ws_ctx_free(ctx);
        req->sess_ctx = NULL;
        req->free_ctx = NULL;
        return ESP_OK;
    }
    if ((frame.type != HTTPD_WS_TYPE_TEXT &&
         frame.type != HTTPD_WS_TYPE_BINARY) || frame.len == 0) {
        return ESP_OK;
    }
    if (frame.len > UART_WS_MAX_FRAME) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *data = malloc(frame.len);
    if (!data) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = data;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK) {
        if (!terminal_control_manual_input_allowed(
                SI_TERMINAL_CHANNEL_UART)) {
            terminal_control_publish_state(SI_TERMINAL_CHANNEL_UART);
            free(data);
            return ESP_OK;
        }
        ret = uart_mutation_begin();
        if (ret == ESP_OK && !terminal_control_manual_input_allowed(
                                 SI_TERMINAL_CHANNEL_UART)) {
            uart_mutation_end();
            terminal_control_publish_state(SI_TERMINAL_CHANNEL_UART);
            free(data);
            return ESP_OK;
        }
        size_t written = 0;
        if (ret == ESP_OK) {
            ret = si_target_uart_write(data, frame.len, &written);
            uart_mutation_end();
        }
        if (ret != ESP_OK) {
            char message[80];
            snprintf(message, sizeof(message),
                     "\r\n[uart: write failed: %s]\r\n",
                     esp_err_to_name(ret));
            (void)uart_ws_send_async(ctx->handle, ctx->fd,
                                     HTTPD_WS_TYPE_TEXT,
                                     message, strlen(message));
        }
    }
    free(data);
    return ret;
}
#else
static esp_err_t uart_ws_bridge_start(void)
{
    if (!s_uart_operation_lock) {
        s_uart_operation_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_uart_operation_lock, ESP_ERR_NO_MEM, TAG,
                            "create UART operation mutex");
    }
    if (!s_uart_ws_lock) {
        s_uart_ws_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_uart_ws_lock, ESP_ERR_NO_MEM, TAG,
                            "create UART operation mutex");
    }
    return ESP_OK;
}
#endif
