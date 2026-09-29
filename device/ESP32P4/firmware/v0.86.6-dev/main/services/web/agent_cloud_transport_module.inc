/* Provider transport, bounded async retry, and heap diagnostics. */
#define AGENT_CLOUD_IO_SLICE_MS 250

typedef struct {
    char *payload;
    size_t payload_len;
    bool too_large;
} agent_cloud_response_t;

static esp_err_t agent_cloud_http_event(esp_http_client_event_t *event)
{
    agent_cloud_response_t *response =
        event ? (agent_cloud_response_t *)event->user_data : NULL;
    if (!response || event->event_id != HTTP_EVENT_ON_DATA ||
        !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    size_t data_len = (size_t)event->data_len;
    if (data_len > AGENT_CLOUD_RESPONSE_MAX - response->payload_len) {
        response->too_large = true;
        return ESP_OK;
    }
    memcpy(response->payload + response->payload_len, event->data, data_len);
    response->payload_len += data_len;
    response->payload[response->payload_len] = '\0';
    return ESP_OK;
}

static void agent_cloud_release_context_request_lock(bool *lock_held)
{
    if (lock_held && *lock_held) {
        if (s_agent_run_lock) {
            xSemaphoreGive(s_agent_run_lock);
        }
        *lock_held = false;
    }
}

static esp_err_t agent_cloud_post_json(const char *job_id, const char *url,
                                       const char *api_key, const char *body,
                                       bool context_request_lock_held,
                                       char **out_payload, int *out_status,
                                       char *err, size_t err_size)
{
    if (!url || !api_key || !body || !out_payload || !out_status ||
        agent_api_validate_endpoint(url, false) != ESP_OK) {
        agent_cloud_release_context_request_lock(
            &context_request_lock_held);
        if (err && err_size) {
            strlcpy(err, "cloud endpoint must be non-empty HTTPS", err_size);
        }
        return ESP_ERR_INVALID_ARG;
    }
    *out_payload = NULL;
    *out_status = 0;

    const size_t internal_free_before = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest_before = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free_before = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest_before = heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t dma_internal_free_before = heap_caps_get_free_size(
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t dma_internal_largest_before =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t dma_internal_minimum_before = heap_caps_get_minimum_free_size(
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t dma_free_before = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t dma_largest_before =
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const size_t dma_minimum_before =
        heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);
    ESP_LOGI(TAG,
             "Agent cloud transport start: internal_free=%u "
             "internal_largest=%u psram_free=%u psram_largest=%u "
             "dma_free=%u dma_largest=%u dma_minimum=%u "
             "dma_internal_free=%u dma_internal_largest=%u "
             "dma_internal_minimum=%u",
             (unsigned)internal_free_before,
             (unsigned)internal_largest_before,
             (unsigned)psram_free_before,
             (unsigned)psram_largest_before,
             (unsigned)dma_free_before,
             (unsigned)dma_largest_before,
             (unsigned)dma_minimum_before,
             (unsigned)dma_internal_free_before,
             (unsigned)dma_internal_largest_before,
             (unsigned)dma_internal_minimum_before);

    char *payload = agent_alloc(AGENT_CLOUD_RESPONSE_MAX + 1);
    if (!payload) {
        agent_cloud_release_context_request_lock(
            &context_request_lock_held);
        strlcpy(err, "cloud response alloc failed", err_size);
        return ESP_ERR_NO_MEM;
    }

    agent_cloud_response_t response = {
        .payload = payload,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = AGENT_CLOUD_IO_SLICE_MS,
        .buffer_size = OTA_IO_BUFFER_SIZE,
        .user_agent = "ExoAnchor-ESP32P4-Agent/" SI_BMC_VERSION,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .event_handler = agent_cloud_http_event,
        .user_data = &response,
        .is_async = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        agent_cloud_release_context_request_lock(
            &context_request_lock_held);
        free(payload);
        strlcpy(err, "cloud http client init failed", err_size);
        return ESP_ERR_NO_MEM;
    }

    char auth[AGENT_API_SECRET_MAX_LEN + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, strlen(body));
    memset(auth, 0, sizeof(auth));

    int64_t started_ms = esp_timer_get_time() / 1000;
    esp_err_t ret = ESP_ERR_HTTP_EAGAIN;
    int socket_errno = 0;
    int tls_code = 0;
    int tls_flags = 0;
    esp_err_t tls_error = ESP_OK;
    bool transport_diagnostic_captured = false;
    /*
     * Async esp_http_client uses the short timeout as a cancellation slice.
     * Each normal EAGAIN slice is logged by the component as a warning, which
     * can flood UART diagnostics with dozens of false failures during one
     * healthy model response. Keep real HTTP errors visible while this single
     * Agent request is active, then restore the previous component level.
     */
    esp_log_level_t previous_http_log_level =
        esp_log_level_get("HTTP_CLIENT");
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_ERROR);
    do {
        /* A context-bearing first slice already owns the Run lock after a
         * generation-aware source check. Once that lock is released, and for
         * every context-free request, refresh the exact source binding before
         * each bounded provider slice. Logout/password rotation therefore
         * fail-stops an in-flight provider call instead of waiting for the
         * overall cloud timeout. */
        if (!context_request_lock_held &&
            !agent_run_ensure_live_source(job_id)) {
            strlcpy(err, "cloud request source authorization revoked",
                    err_size);
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        if ((esp_timer_get_time() / 1000) - started_ms >=
            AGENT_CLOUD_TIMEOUT_MS) {
            strlcpy(err, "cloud request timeout", err_size);
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        ret = esp_http_client_perform(client);
        /* The first perform hands the request to the HTTP state machine. Only
         * now may Settings complete a page-context revocation transaction. */
        agent_cloud_release_context_request_lock(
            &context_request_lock_held);
        if (ret == ESP_ERR_HTTP_EAGAIN) {
            /* ESP-IDF 5.5.5 classifies a failed async POST write solely from
             * the process-wide errno value. A hard mbedTLS/AES failure does
             * not update errno, so a stale EAGAIN can otherwise leave the
             * request in REQ_COMPLETE_HEADER and replay the same failed write
             * until the overall timeout. Transport TLS errors are never a
             * readiness condition: capture them on the same slice and stop.
             */
            int slice_tls_code = 0;
            int slice_tls_flags = 0;
            int slice_errno = esp_http_client_get_errno(client);
            esp_err_t slice_tls_error =
                esp_http_client_get_and_clear_last_tls_error(
                    client, &slice_tls_code, &slice_tls_flags);
            if (si_agent_cloud_async_error_is_fatal(
                    slice_errno, slice_tls_error, slice_tls_code)) {
                socket_errno = slice_errno;
                tls_error = slice_tls_error;
                tls_code = slice_tls_code;
                tls_flags = slice_tls_flags;
                transport_diagnostic_captured = true;
                ret = slice_tls_error != ESP_OK
                          ? slice_tls_error
                          : ESP_ERR_HTTP_WRITE_DATA;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    } while (ret == ESP_ERR_HTTP_EAGAIN);
    agent_cloud_release_context_request_lock(&context_request_lock_held);
    esp_log_level_set("HTTP_CLIENT", previous_http_log_level);

    *out_status = esp_http_client_get_status_code(client);
    if (ret != ESP_OK) {
        if (!transport_diagnostic_captured) {
            socket_errno = esp_http_client_get_errno(client);
            tls_error = esp_http_client_get_and_clear_last_tls_error(
                client, &tls_code, &tls_flags);
        }
    }
    esp_http_client_cleanup(client);
    if (response.too_large) {
        strlcpy(err, "cloud response too large", err_size);
        ret = ESP_ERR_INVALID_SIZE;
    }
    if (ret != ESP_OK) {
        const size_t dma_internal_free_after = heap_caps_get_free_size(
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t dma_internal_largest_after =
            heap_caps_get_largest_free_block(
                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t dma_free_after =
            heap_caps_get_free_size(MALLOC_CAP_DMA);
        const size_t dma_largest_after =
            heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        if (!err[0]) {
#if defined(CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC)
            if (ret == ESP_ERR_HTTP_CONNECT &&
                internal_largest_before < CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN) {
                snprintf(
                    err, err_size,
                    "cloud TLS memory unavailable: internal largest=%u, "
                    "TLS RX=%u TX=%u",
                    (unsigned)internal_largest_before,
                    (unsigned)CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN,
                    (unsigned)CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN);
            } else
#endif
            {
            snprintf(err, err_size, "cloud request failed: %s",
                     esp_err_to_name(ret));
            }
        }
        ESP_LOGE(TAG,
                 "Agent cloud transport failed: ret=%s errno=%d "
                 "tls_error=0x%x tls_code=%d tls_flags=0x%x "
                 "internal_free=%u internal_largest=%u "
                 "dma_free=%u dma_largest=%u "
                 "dma_internal_free=%u dma_internal_largest=%u",
                 esp_err_to_name(ret), socket_errno, (unsigned)tls_error,
                 tls_code,
                 (unsigned)tls_flags, (unsigned)internal_free_before,
                 (unsigned)internal_largest_before,
                 (unsigned)dma_free_after,
                 (unsigned)dma_largest_after,
                 (unsigned)dma_internal_free_after,
                 (unsigned)dma_internal_largest_after);
        free(payload);
        return ret;
    }
    *out_payload = payload;
    return ESP_OK;
}
