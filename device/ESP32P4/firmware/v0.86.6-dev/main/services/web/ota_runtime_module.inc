// Transitional domain module: OTA runtime. Kept in the web compilation unit until its service contract is independent.
static si_resource_operation_token_t s_ota_upload_maintenance;
static bool ota_board_matches(const char *board)
{
    return board && board[0] != '\0' &&
           strcasecmp(board, SI_BOARD_ID) == 0;
}

static bool ota_sha256_is_valid(const char *sha)
{
    if (!sha || strlen(sha) != OTA_SHA256_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < OTA_SHA256_HEX_LEN; i++) {
        if (!isxdigit((unsigned char)sha[i])) {
            return false;
        }
    }
    return true;
}

static esp_err_t ota_parse_manifest(const char *json, const ota_settings_t *settings,
                                    ota_manifest_t *manifest, char *err, size_t err_size)
{
    memset(manifest, 0, sizeof(*manifest));
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        strlcpy(err, "invalid manifest json", err_size);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *firmware = cJSON_GetObjectItemCaseSensitive(root, "firmware");
    if (!cJSON_IsObject(firmware)) {
        firmware = root;
    }

    const char *board = json_string_any(firmware, "board", "target", "device");
    if (!board) {
        board = json_string_any(root, "board", "target", "device");
    }
    if (!ota_board_matches(board)) {
        strlcpy(err, "manifest board mismatch", err_size);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const char *channel = json_string_any(firmware, "channel", "track", "release_channel");
    if (!channel) {
        channel = json_string_any(root, "channel", "track", "release_channel");
    }
    if (channel && settings && settings->channel[0] &&
        strcasecmp(channel, settings->channel) != 0) {
        strlcpy(err, "manifest channel mismatch", err_size);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const char *version = json_string_any4(firmware, "version", "tag", "name", "firmware_version");
    const char *url = json_string_any4(firmware, "url", "bin_url", "firmware_url", "download_url");
    const char *sha = json_string_any4(firmware, "sha256", "sha256sum", "hash", "digest");
    const char *notes = json_string_any(firmware, "notes", "changelog", "description");
    const cJSON *size = json_number_any(firmware, "size", "bytes");

    if (!version || !version[0]) {
        strlcpy(err, "manifest version missing", err_size);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!url || !ota_url_is_valid(url)) {
        strlcpy(err, "manifest firmware url missing", err_size);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!ota_sha256_is_valid(sha)) {
        strlcpy(err, "manifest sha256 must be present and contain 64 hex chars",
                err_size);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(manifest->version, version, sizeof(manifest->version));
    strlcpy(manifest->url, url, sizeof(manifest->url));
    strlcpy(manifest->sha256, sha, sizeof(manifest->sha256));
    if (notes) {
        strlcpy(manifest->notes, notes, sizeof(manifest->notes));
    }
    if (size && size->valuedouble > 0) {
        manifest->size = (uint32_t)size->valuedouble;
    }
    manifest->update_available = si_version_compare(manifest->version, SI_BMC_VERSION) > 0;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t ota_fetch_manifest(const ota_settings_t *settings, ota_manifest_t *manifest,
                                    char *err, size_t err_size)
{
    if (!settings || settings->manifest_url[0] == '\0') {
        strlcpy(err, "OTA manifest URL not configured", err_size);
        return ESP_ERR_INVALID_STATE;
    }

    char *payload = calloc(1, OTA_MANIFEST_MAX_SIZE + 1);
    if (!payload) {
        strlcpy(err, "manifest buffer alloc failed", err_size);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = settings->manifest_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 12000,
        .buffer_size = 1024,
        .user_agent = "ExoAnchor-ESP32P4-OTA/" SI_BMC_VERSION,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(payload);
        strlcpy(err, "http client init failed", err_size);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        snprintf(err, err_size, "manifest open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        free(payload);
        return ret;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        snprintf(err, err_size, "manifest http status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(payload);
        return ESP_FAIL;
    }
    if (content_length > OTA_MANIFEST_MAX_SIZE) {
        strlcpy(err, "manifest too large", err_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(payload);
        return ESP_ERR_INVALID_SIZE;
    }

    int total = 0;
    while (total < OTA_MANIFEST_MAX_SIZE) {
        int read_len = esp_http_client_read(client, payload + total,
                                            OTA_MANIFEST_MAX_SIZE - total);
        if (read_len < 0) {
            strlcpy(err, "manifest read failed", err_size);
            ret = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            ret = ESP_OK;
            break;
        }
        total += read_len;
    }
    if (ret == ESP_OK && total >= OTA_MANIFEST_MAX_SIZE &&
        !esp_http_client_is_complete_data_received(client)) {
        strlcpy(err, "manifest too large", err_size);
        ret = ESP_ERR_INVALID_SIZE;
    }
    payload[MIN(total, OTA_MANIFEST_MAX_SIZE)] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ret == ESP_OK) {
        ret = ota_parse_manifest(payload, settings, manifest, err, err_size);
    }
    free(payload);
    return ret;
}

static void ota_store_last_manifest(const ota_manifest_t *manifest, const char *message)
{
    s_ota_last_checked = true;
    s_ota_last_update_available = manifest->update_available;
    strlcpy(s_ota_last_version, manifest->version, sizeof(s_ota_last_version));
    strlcpy(s_ota_last_url, manifest->url, sizeof(s_ota_last_url));
    strlcpy(s_ota_last_sha256, manifest->sha256, sizeof(s_ota_last_sha256));
    strlcpy(s_ota_last_notes, manifest->notes, sizeof(s_ota_last_notes));
    s_ota_last_size = manifest->size;
    ota_set_last_message(message);
}

static esp_err_t ota_begin_update(uint32_t expected_size, esp_ota_handle_t *handle,
                                  const esp_partition_t **partition)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_size > 0 && expected_size > next->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    /*
     * Full upfront partition erase can block an HTTP request for minutes on
     * this flash. All OTA sources write strictly in order, so let ESP-IDF
     * erase sectors incrementally as each sequential write reaches them.
     */
    esp_err_t ret = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, handle);
    if (ret != ESP_OK) {
        return ret;
    }
    *partition = next;
    return ESP_OK;
}

static esp_err_t ota_finish_update(esp_ota_handle_t handle, const esp_partition_t *partition)
{
    ESP_RETURN_ON_ERROR(esp_ota_end(handle), TAG, "end ota");
    ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(partition), TAG, "set ota boot partition");
    return ESP_OK;
}

static const char *ota_image_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending_verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default:
        return "undefined";
    }
}

static void ota_upload_session_clear(bool abort_update)
{
    if (abort_update && s_ota_upload_session.active) {
        (void)esp_ota_abort(s_ota_upload_session.handle);
    }
    if (s_ota_upload_session.sha_started) {
        mbedtls_sha256_free(&s_ota_upload_session.sha);
    }
    memset(&s_ota_upload_session, 0, sizeof(s_ota_upload_session));
    if (s_ota_upload_maintenance.generation != 0U) {
        (void)resource_operation_finish(&s_ota_upload_maintenance);
        memset(&s_ota_upload_maintenance, 0,
               sizeof(s_ota_upload_maintenance));
    }
}

static bool ota_upload_session_expire_locked(void)
{
    if (!s_ota_upload_session.active) {
        return false;
    }
    int64_t idle_ms =
        (esp_timer_get_time() - s_ota_upload_session.last_progress_us) / 1000LL;
    if (idle_ms < OTA_UPLOAD_SESSION_TIMEOUT_MS) {
        return false;
    }
    ota_upload_session_clear(true);
    s_ota_busy = false;
    (void)si_video_control_set_suspended(false);
    ota_set_last_message("chunked OTA session expired and was aborted");
    si_web_log("WARN", "Chunked OTA session expired");
    return true;
}

static void ota_upload_add_session_json(cJSON *root)
{
    cJSON_AddBoolToObject(root, "active", s_ota_upload_session.active);
    cJSON_AddStringToObject(root, "session_id",
                            s_ota_upload_session.active ?
                            s_ota_upload_session.session_id : "");
    cJSON_AddNumberToObject(root, "offset", s_ota_upload_session.written);
    cJSON_AddNumberToObject(root, "size", s_ota_upload_session.expected_size);
    cJSON_AddStringToObject(root, "sha256",
                            s_ota_upload_session.active ?
                            s_ota_upload_session.expected_sha256 : "");
    cJSON_AddNumberToObject(root, "chunk_size", OTA_UPLOAD_CHUNK_MAX_SIZE);
}

static esp_err_t ota_upload_send_session_json(httpd_req_t *req, bool ok,
                                               const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    ota_upload_add_session_json(root);
    if (message && message[0]) {
        cJSON_AddStringToObject(root, "message", message);
    }
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static bool ota_upload_query(httpd_req_t *req,
                             char session_id[OTA_UPLOAD_SESSION_ID_LEN + 1],
                             uint32_t *offset)
{
    char query[128] = {0};
    char offset_text[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "session", session_id,
                              OTA_UPLOAD_SESSION_ID_LEN + 1) != ESP_OK ||
        httpd_query_key_value(query, "offset", offset_text,
                              sizeof(offset_text)) != ESP_OK) {
        return false;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(offset_text, &end, 10);
    if (!offset_text[0] || !end || *end || parsed > UINT32_MAX) {
        return false;
    }
    *offset = (uint32_t)parsed;
    return true;
}

static esp_err_t ota_upload_start_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    char buf[256] = {0};
    cJSON *root = NULL;
    ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) {
        return ret;
    }
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    const char *sha256 = json_string_any(root, "sha256", "digest", "hash");
    if (!cJSON_IsNumber(size) || size->valuedouble < 1 ||
        size->valuedouble > UINT32_MAX || !ota_sha256_is_valid(sha256)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "size and sha256 are required");
    }
    uint32_t expected_size = (uint32_t)size->valuedouble;
    char expected_sha256[OTA_SHA256_HEX_LEN + 1] = {0};
    strlcpy(expected_sha256, sha256, sizeof(expected_sha256));
    cJSON_Delete(root);

    if (xSemaphoreTake(s_ota_lock, 0) != pdTRUE) {
        return si_http_send_text_status(req, "409 Conflict",
                                        "OTA operation already running");
    }
    (void)ota_upload_session_expire_locked();
    if (s_ota_upload_session.active || s_ota_busy) {
        ret = ota_upload_send_session_json(req, false,
                                           "OTA upload session already active");
        xSemaphoreGive(s_ota_lock);
        return ret;
    }

    esp_ota_handle_t handle = 0;
    const esp_partition_t *partition = NULL;
    s_ota_busy = true;
    (void)si_video_control_set_suspended(true);
    ret = ota_begin_update(expected_size, &handle, &partition);
    if (ret != ESP_OK) {
        s_ota_busy = false;
        (void)si_video_control_set_suspended(false);
        xSemaphoreGive(s_ota_lock);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   esp_err_to_name(ret));
    }

    ota_upload_session_clear(false);
    if (!automation_begin_maintenance(
            "ota-upload", "固件上传维护中", true,
            &s_ota_upload_maintenance)) {
        (void)esp_ota_abort(handle);
        s_ota_busy = false;
        (void)si_video_control_set_suspended(false);
        xSemaphoreGive(s_ota_lock);
        return si_http_send_text_status(
            req, "409 Conflict",
            "could not stop active Agent/MCP operations for OTA");
    }
    s_ota_upload_session.active = true;
    s_ota_upload_session.handle = handle;
    s_ota_upload_session.partition = partition;
    s_ota_upload_session.expected_size = expected_size;
    s_ota_upload_session.last_progress_us = esp_timer_get_time();
    strlcpy(s_ota_upload_session.expected_sha256, expected_sha256,
            sizeof(s_ota_upload_session.expected_sha256));
    snprintf(s_ota_upload_session.session_id,
             sizeof(s_ota_upload_session.session_id), "%08" PRIx32 "%08" PRIx32,
             esp_random(), esp_random());
    mbedtls_sha256_init(&s_ota_upload_session.sha);
    if (mbedtls_sha256_starts(&s_ota_upload_session.sha, 0) != 0) {
        ota_upload_session_clear(true);
        s_ota_busy = false;
        (void)si_video_control_set_suspended(false);
        xSemaphoreGive(s_ota_lock);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "sha256 initialization failed");
    }
    s_ota_upload_session.sha_started = true;
    ota_set_last_message("chunked OTA upload started");
    si_web_log("INFO", "Chunked OTA upload started");
    ret = ota_upload_send_session_json(req, true, "upload session ready");
    xSemaphoreGive(s_ota_lock);
    return ret;
}

static esp_err_t ota_upload_chunk_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    if (req->content_len <= 0 ||
        req->content_len > (int)OTA_UPLOAD_CHUNK_MAX_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid OTA chunk size");
    }
    char session_id[OTA_UPLOAD_SESSION_ID_LEN + 1] = {0};
    uint32_t offset = 0;
    if (!ota_upload_query(req, session_id, &offset)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "session and offset are required");
    }
    if (xSemaphoreTake(s_ota_lock, 0) != pdTRUE) {
        return si_http_send_text_status(req, "409 Conflict",
                                        "OTA operation already running");
    }
    (void)ota_upload_session_expire_locked();
    if (!s_ota_upload_session.active ||
        strcmp(session_id, s_ota_upload_session.session_id) != 0 ||
        offset != s_ota_upload_session.written) {
        ret = ota_upload_send_session_json(req, false,
                                           "OTA session or offset mismatch");
        xSemaphoreGive(s_ota_lock);
        return ret;
    }
    if ((uint32_t)req->content_len >
        s_ota_upload_session.expected_size - s_ota_upload_session.written) {
        ret = ota_upload_send_session_json(req, false,
                                           "OTA chunk exceeds expected image size");
        xSemaphoreGive(s_ota_lock);
        return ret;
    }

    uint8_t *buffer = malloc(OTA_IO_BUFFER_SIZE);
    if (!buffer) {
        xSemaphoreGive(s_ota_lock);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "OTA chunk buffer unavailable");
    }
    uint32_t remaining = (uint32_t)req->content_len;
    int64_t last_receive_us = esp_timer_get_time();
    while (ret == ESP_OK && remaining > 0) {
        int read_len = httpd_req_recv(
            req, (char *)buffer,
            MIN((size_t)remaining, (size_t)OTA_IO_BUFFER_SIZE));
        if (read_len == HTTPD_SOCK_ERR_TIMEOUT) {
            int64_t idle_ms =
                (esp_timer_get_time() - last_receive_us) / 1000LL;
            if (idle_ms < OTA_UPLOAD_IDLE_TIMEOUT_MS) {
                vTaskDelay(1);
                continue;
            }
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        if (read_len <= 0) {
            ret = ESP_FAIL;
            break;
        }
        ret = esp_ota_write(s_ota_upload_session.handle, buffer, read_len);
        if (ret == ESP_OK &&
            mbedtls_sha256_update(&s_ota_upload_session.sha, buffer,
                                  read_len) != 0) {
            ret = ESP_FAIL;
        }
        if (ret != ESP_OK) {
            break;
        }
        remaining -= (uint32_t)read_len;
        s_ota_upload_session.written += (uint32_t)read_len;
        s_ota_upload_session.last_progress_us = esp_timer_get_time();
        last_receive_us = s_ota_upload_session.last_progress_us;
    }
    free(buffer);
    if (ret != ESP_OK) {
        ota_set_last_message("chunked OTA upload interrupted; resume from reported offset");
        esp_err_t send_ret = ota_upload_send_session_json(
            req, false, "chunk interrupted; query status and resume");
        xSemaphoreGive(s_ota_lock);
        return send_ret;
    }
    ret = ota_upload_send_session_json(req, true, "chunk accepted");
    xSemaphoreGive(s_ota_lock);
    return ret;
}

static esp_err_t ota_upload_status_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
        return si_http_send_text_status(req, "409 Conflict",
                                        "OTA operation busy");
    }
    (void)ota_upload_session_expire_locked();
    ret = ota_upload_send_session_json(req, true, "upload status");
    xSemaphoreGive(s_ota_lock);
    return ret;
}

static esp_err_t ota_upload_finish_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    char buf[128] = {0};
    cJSON *root = NULL;
    ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) {
        return ret;
    }
    const char *session_id = json_string_any(root, "session_id", "session", "id");
    char requested_session[OTA_UPLOAD_SESSION_ID_LEN + 1] = {0};
    if (session_id) {
        strlcpy(requested_session, session_id, sizeof(requested_session));
    }
    cJSON_Delete(root);
    if (!requested_session[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "session_id is required");
    }
    if (xSemaphoreTake(s_ota_lock, 0) != pdTRUE) {
        return si_http_send_text_status(req, "409 Conflict",
                                        "OTA operation already running");
    }
    if (!s_ota_upload_session.active ||
        strcmp(requested_session, s_ota_upload_session.session_id) != 0 ||
        s_ota_upload_session.written != s_ota_upload_session.expected_size) {
        ret = ota_upload_send_session_json(req, false,
                                           "OTA upload is incomplete");
        xSemaphoreGive(s_ota_lock);
        return ret;
    }

    uint8_t digest[32] = {0};
    if (mbedtls_sha256_finish(&s_ota_upload_session.sha, digest) != 0) {
        ret = ESP_FAIL;
    } else {
        mbedtls_sha256_free(&s_ota_upload_session.sha);
        s_ota_upload_session.sha_started = false;
    }
    char sha_hex[OTA_SHA256_HEX_LEN + 1] = {0};
    bytes_to_hex(digest, sizeof(digest), sha_hex, sizeof(sha_hex));
    if (ret == ESP_OK &&
        strcasecmp(sha_hex, s_ota_upload_session.expected_sha256) != 0) {
        ret = ESP_ERR_INVALID_CRC;
    }
    uint32_t written = s_ota_upload_session.written;
    bool handle_consumed = false;
    if (ret == ESP_OK) {
        /* esp_ota_end consumes the handle even when image validation fails. */
        handle_consumed = true;
        ret = ota_finish_update(s_ota_upload_session.handle,
                                s_ota_upload_session.partition);
    }
    if (ret != ESP_OK) {
        char error_message[96] = {0};
        snprintf(error_message, sizeof(error_message),
                 "chunked OTA verification failed: %s",
                 esp_err_to_name(ret));
        ota_upload_session_clear(!handle_consumed);
        s_ota_busy = false;
        (void)si_video_control_set_suspended(false);
        ota_set_last_message(error_message);
        si_web_log("WARN", error_message);
        xSemaphoreGive(s_ota_lock);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   esp_err_to_name(ret));
    }

    ota_upload_session_clear(false);
    s_ota_busy = false;
    (void)si_video_control_set_suspended(false);
    ota_set_last_message("chunked OTA installed, reboot required");
    si_web_log("INFO", "Chunked OTA upload installed");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "sha256", sha_hex);
    cJSON_AddNumberToObject(resp, "size", written);
    cJSON_AddBoolToObject(resp, "reboot_required", true);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    xSemaphoreGive(s_ota_lock);
    return ret;
}

static esp_err_t ota_upload_abort_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
        return si_http_send_text_status(req, "409 Conflict",
                                        "OTA operation busy");
    }
    ota_upload_session_clear(true);
    s_ota_busy = false;
    (void)si_video_control_set_suspended(false);
    ota_set_last_message("chunked OTA upload aborted");
    si_web_log("WARN", "Chunked OTA upload aborted");
    ret = ota_upload_send_session_json(req, true, "upload aborted");
    xSemaphoreGive(s_ota_lock);
    return ret;
}

static esp_err_t ota_apply_http_firmware(const ota_manifest_t *manifest,
                                         char sha_hex[OTA_SHA256_HEX_LEN + 1],
                                         uint32_t *written_out)
{
    uint8_t *buffer = malloc(OTA_IO_BUFFER_SIZE);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = manifest->url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 30000,
        .buffer_size = OTA_IO_BUFFER_SIZE,
        .user_agent = "ExoAnchor-ESP32P4-OTA/" SI_BMC_VERSION,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        free(buffer);
        return ret;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(buffer);
        return ESP_FAIL;
    }

    uint32_t expected_size = manifest->size;
    if (content_length > 0) {
        expected_size = (uint32_t)content_length;
    }

    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *partition = NULL;
    ret = ota_begin_update(expected_size, &ota_handle, &partition);
    if (ret != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(buffer);
        return ret;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    ret = mbedtls_sha256_starts(&sha, 0) == 0 ? ESP_OK : ESP_FAIL;

    uint32_t written = 0;
    while (ret == ESP_OK) {
        int read_len = esp_http_client_read(client, (char *)buffer, OTA_IO_BUFFER_SIZE);
        if (read_len < 0) {
            ret = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            break;
        }
        if ((uint32_t)read_len > partition->size - written) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        ret = esp_ota_write(ota_handle, buffer, read_len);
        if (ret != ESP_OK) {
            break;
        }
        if (mbedtls_sha256_update(&sha, buffer, read_len) != 0) {
            ret = ESP_FAIL;
            break;
        }
        written += (uint32_t)read_len;
    }

    if (ret == ESP_OK && content_length > 0 && written != (uint32_t)content_length) {
        ret = ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[32] = {0};
    if (ret == ESP_OK && mbedtls_sha256_finish(&sha, digest) != 0) {
        ret = ESP_FAIL;
    }
    mbedtls_sha256_free(&sha);
    bytes_to_hex(digest, sizeof(digest), sha_hex, OTA_SHA256_HEX_LEN + 1);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buffer);

    if (ret == ESP_OK &&
        strcasecmp(sha_hex, manifest->sha256) != 0) {
        ret = ESP_ERR_INVALID_CRC;
    }

    if (ret == ESP_OK) {
        ret = ota_finish_update(ota_handle, partition);
    } else {
        (void)esp_ota_abort(ota_handle);
    }
    if (written_out) {
        *written_out = written;
    }
    return ret;
}

static esp_err_t ota_apply_uploaded_firmware(httpd_req_t *req,
                                             char sha_hex[OTA_SHA256_HEX_LEN + 1],
                                             uint32_t *written_out)
{
    if (req->content_len <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buffer = malloc(OTA_IO_BUFFER_SIZE);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *partition = NULL;
    esp_err_t ret = ota_begin_update((uint32_t)req->content_len, &ota_handle, &partition);
    if (ret != ESP_OK) {
        free(buffer);
        return ret;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    ret = mbedtls_sha256_starts(&sha, 0) == 0 ? ESP_OK : ESP_FAIL;

    uint32_t remaining = (uint32_t)req->content_len;
    uint32_t written = 0;
    int64_t last_progress_us = esp_timer_get_time();
    while (ret == ESP_OK && remaining > 0) {
        size_t to_read = MIN((size_t)remaining, (size_t)OTA_IO_BUFFER_SIZE);
        int read_len = httpd_req_recv(req, (char *)buffer, to_read);
        if (read_len == HTTPD_SOCK_ERR_TIMEOUT) {
            int64_t idle_ms =
                (esp_timer_get_time() - last_progress_us) / 1000LL;
            if (idle_ms < OTA_UPLOAD_IDLE_TIMEOUT_MS) {
                vTaskDelay(1);
                continue;
            }
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        if (read_len <= 0) {
            ret = ESP_FAIL;
            break;
        }
        ret = esp_ota_write(ota_handle, buffer, read_len);
        if (ret != ESP_OK) {
            break;
        }
        if (mbedtls_sha256_update(&sha, buffer, read_len) != 0) {
            ret = ESP_FAIL;
            break;
        }
        remaining -= (uint32_t)read_len;
        written += (uint32_t)read_len;
        last_progress_us = esp_timer_get_time();
    }

    uint8_t digest[32] = {0};
    if (ret == ESP_OK && mbedtls_sha256_finish(&sha, digest) != 0) {
        ret = ESP_FAIL;
    }
    mbedtls_sha256_free(&sha);
    bytes_to_hex(digest, sizeof(digest), sha_hex, OTA_SHA256_HEX_LEN + 1);
    free(buffer);

    if (ret == ESP_OK) {
        ret = ota_finish_update(ota_handle, partition);
    } else {
        (void)esp_ota_abort(ota_handle);
    }
    if (written_out) {
        *written_out = written;
    }
    return ret;
}

static void add_ota_json(cJSON *root)
{
    ota_settings_t settings;
    ota_settings_snapshot(&settings);

    cJSON_AddStringToObject(root, "current_version", SI_BMC_VERSION);
    cJSON_AddBoolToObject(root, "busy", s_ota_busy);
    cJSON_AddBoolToObject(root, "auto_check_supported", false);

    cJSON *settings_json = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddStringToObject(settings_json, "manifest_url", settings.manifest_url);
    cJSON_AddStringToObject(settings_json, "default_manifest_url", OTA_MANIFEST_URL_DEFAULT);
    cJSON_AddStringToObject(settings_json, "channel", settings.channel);
    cJSON_AddBoolToObject(settings_json, "auto_check", settings.auto_check);

    cJSON *last = cJSON_AddObjectToObject(root, "last_check");
    cJSON_AddBoolToObject(last, "checked", s_ota_last_checked);
    cJSON_AddBoolToObject(last, "update_available", s_ota_last_update_available);
    cJSON_AddStringToObject(last, "version", s_ota_last_version);
    cJSON_AddStringToObject(last, "url", s_ota_last_url);
    cJSON_AddStringToObject(last, "sha256", s_ota_last_sha256);
    cJSON_AddStringToObject(last, "notes", s_ota_last_notes);
    cJSON_AddNumberToObject(last, "size", s_ota_last_size);
    cJSON_AddNumberToObject(last, "checked_uptime_seconds", s_ota_last_check_sec);
    cJSON_AddStringToObject(last, "message", s_ota_last_message);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    cJSON *partition = cJSON_AddObjectToObject(root, "partition");
    cJSON_AddStringToObject(partition, "running", running ? running->label : "--");
    cJSON_AddStringToObject(partition, "boot", boot ? boot->label : "--");
    cJSON_AddNumberToObject(partition, "running_address", running ? running->address : 0);
    cJSON_AddNumberToObject(partition, "boot_address", boot ? boot->address : 0);
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    cJSON_AddBoolToObject(partition, "rollback_enabled", true);
#else
    cJSON_AddBoolToObject(partition, "rollback_enabled", false);
#endif
    esp_ota_img_states_t running_state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t running_state_ret = running ?
        esp_ota_get_state_partition(running, &running_state) : ESP_ERR_NOT_FOUND;
    cJSON_AddStringToObject(
        partition, "running_state",
        running_state_ret == ESP_OK ? ota_image_state_name(running_state) :
        (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY ?
         "factory" : "unavailable"));

    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "idf_app_version", desc ? desc->version : "");
}

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    cJSON *root = cJSON_CreateObject();
    add_ota_json(root);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t ota_settings_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }

    char buf[SCRATCH_BUFSIZE] = {0};
    cJSON *root = NULL;
    ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) {
        return ret;
    }

    ota_settings_t current;
    ota_settings_snapshot(&current);
    const char *url = json_string_any(root, "manifest_url", "url", "manifest");
    const char *channel = json_string_any(root, "channel", "track", "release_channel");
    cJSON *auto_check = cJSON_GetObjectItemCaseSensitive(root, "auto_check");
    ret = ota_settings_save(url ? url : current.manifest_url,
                            channel ? channel : current.channel,
                            cJSON_IsBool(auto_check) ? cJSON_IsTrue(auto_check) : current.auto_check);
    cJSON_Delete(root);
    if (ret == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid OTA settings");
    }
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
    }

    si_web_log("INFO", "OTA settings updated");
    return ota_status_handler(req);
}

static esp_err_t ota_check_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    if (xSemaphoreTake(s_ota_lock, 0) != pdTRUE) {
        return si_http_send_text_status(req, "409 Conflict", "OTA operation already running");
    }

    ota_settings_t settings;
    ota_settings_snapshot(&settings);
    ota_manifest_t manifest;
    char err[128] = {0};
    ret = ota_fetch_manifest(&settings, &manifest, err, sizeof(err));
    if (ret == ESP_OK) {
        ota_store_last_manifest(&manifest, manifest.update_available ? "update available" : "already latest");
        si_web_log("INFO", manifest.update_available ? "OTA update available" : "OTA already latest");
    } else {
        ota_set_last_message(err);
        si_web_log("WARN", "OTA manifest check failed");
    }
    xSemaphoreGive(s_ota_lock);

    if (ret != ESP_OK) {
        return si_http_send_text_status(req, "502 Bad Gateway", err[0] ? err : esp_err_to_name(ret));
    }
    return ota_status_handler(req);
}

static esp_err_t ota_install_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    si_resource_operation_token_t maintenance = {0};
    if (!automation_begin_maintenance(
            "ota-install", "OTA 固件安装中", false, &maintenance)) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "could not stop active Agent/MCP operations for OTA");
    }
    bool force = false;
    if (req->content_len > 0 && req->content_len < 128) {
        char buf[128] = {0};
        if (si_http_recv_body(req, buf, sizeof(buf)) == ESP_OK) {
            cJSON *root = cJSON_Parse(buf);
            cJSON *force_json = root ? cJSON_GetObjectItemCaseSensitive(root, "force") : NULL;
            force = cJSON_IsTrue(force_json);
            cJSON_Delete(root);
        }
    }

    if (xSemaphoreTake(s_ota_lock, 0) != pdTRUE) {
        (void)resource_operation_finish(&maintenance);
        return si_http_send_text_status(req, "409 Conflict", "OTA operation already running");
    }
    s_ota_busy = true;
    (void)si_video_control_set_suspended(true);

    ota_settings_t settings;
    ota_settings_snapshot(&settings);
    ota_manifest_t manifest;
    char err[128] = {0};
    ret = ota_fetch_manifest(&settings, &manifest, err, sizeof(err));
    if (ret == ESP_OK) {
        ota_store_last_manifest(&manifest, manifest.update_available ? "update available" : "already latest");
    }
    if (ret == ESP_OK && !manifest.update_available && !force) {
        ret = ESP_ERR_INVALID_STATE;
        strlcpy(err, "already latest", sizeof(err));
    }

    char sha_hex[OTA_SHA256_HEX_LEN + 1] = {0};
    uint32_t written = 0;
    if (ret == ESP_OK) {
        si_web_log("INFO", "OTA firmware download started");
        ret = ota_apply_http_firmware(&manifest, sha_hex, &written);
        if (ret == ESP_OK) {
            ota_set_last_message("firmware installed, reboot required");
            si_web_log("INFO", "OTA firmware installed");
        } else {
            snprintf(err, sizeof(err), "firmware install failed: %s", esp_err_to_name(ret));
            ota_set_last_message(err);
            si_web_log("WARN", "OTA firmware install failed");
        }
    }

    s_ota_busy = false;
    (void)si_video_control_set_suspended(false);
    xSemaphoreGive(s_ota_lock);
    (void)resource_operation_finish(&maintenance);

    if (ret != ESP_OK) {
        return si_http_send_text_status(req, ret == ESP_ERR_INVALID_STATE ? "409 Conflict" : "500 Internal Server Error",
                                err[0] ? err : esp_err_to_name(ret));
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "version", manifest.version);
    cJSON_AddStringToObject(resp, "sha256", sha_hex);
    cJSON_AddNumberToObject(resp, "size", written);
    cJSON_AddBoolToObject(resp, "reboot_required", true);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    si_resource_operation_token_t maintenance = {0};
    if (!automation_begin_maintenance(
            "ota-upload-legacy", "固件上传维护中", false,
            &maintenance)) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "could not stop active Agent/MCP operations for OTA");
    }
    if (xSemaphoreTake(s_ota_lock, 0) != pdTRUE) {
        (void)resource_operation_finish(&maintenance);
        return si_http_send_text_status(req, "409 Conflict", "OTA operation already running");
    }
    s_ota_busy = true;
    (void)si_video_control_set_suspended(true);

    char sha_hex[OTA_SHA256_HEX_LEN + 1] = {0};
    uint32_t written = 0;
    si_web_log("INFO", "OTA upload started");
    ret = ota_apply_uploaded_firmware(req, sha_hex, &written);
    if (ret == ESP_OK) {
        ota_set_last_message("uploaded firmware installed, reboot required");
        si_web_log("INFO", "OTA upload installed");
    } else {
        char err[96];
        snprintf(err, sizeof(err), "OTA upload failed: %s", esp_err_to_name(ret));
        ota_set_last_message(err);
        si_web_log("WARN", "OTA upload failed");
    }

    s_ota_busy = false;
    (void)si_video_control_set_suspended(false);
    xSemaphoreGive(s_ota_lock);
    (void)resource_operation_finish(&maintenance);

    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "sha256", sha_hex);
    cJSON_AddNumberToObject(resp, "size", written);
    cJSON_AddBoolToObject(resp, "reboot_required", true);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static void ota_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(650));
    esp_restart();
}

static esp_err_t ota_reboot_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    si_resource_operation_token_t maintenance = {0};
    if (!automation_begin_maintenance(
            "device-reboot", "设备即将重启", false, &maintenance)) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "could not stop active Agent/MCP operations before reboot");
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "rebooting");
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    (void)xTaskCreate(ota_restart_task, "si_ota_reboot", 2048, NULL, tskIDLE_PRIORITY + 3, NULL);
    return ret;
}

static const char *storage_file_type(const struct stat *st)
{
    if (!st) {
        return "unknown";
    }
    if (S_ISDIR(st->st_mode)) {
        return "directory";
    }
    if (S_ISREG(st->st_mode)) {
        return "file";
    }
    return "other";
}

static const char *storage_basename(const char *path)
{
    if (!path || !path[0]) {
        return "download.bin";
    }
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static const char *storage_mime_type(const char *path)
{
    const char *name = storage_basename(path);
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".ttf") == 0) {
        return "font/ttf";
    }
    if (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".log") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (strcasecmp(dot, ".json") == 0) {
        return "application/json";
    }
    return "application/octet-stream";
}
