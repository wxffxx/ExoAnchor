// Transitional domain module: Video settings, lease, snapshot and stream HTTP. Kept in the web compilation unit until its service contract is independent.

#define VIDEO_STREAM_FIRST_FRAME_TIMEOUT_MS 5000U
#define VIDEO_STREAM_STALL_TIMEOUT_MS 5000U
#define VIDEO_SNAPSHOT_FIRST_FRAME_TIMEOUT_MS 5000U
#define VIDEO_SNAPSHOT_POLL_MS 50U

static bool video_stream_peer_closed(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return true;
    }
    uint8_t byte = 0;
    int received = recv(fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    if (received == 0) {
        return true;
    }
    return received < 0 && errno != EAGAIN && errno != EWOULDBLOCK;
}

static const char *video_automation_actor(
    const si_auth_session_context_t *session)
{
    return session && session->principal == SI_PRINCIPAL_MCP ? "mcp" :
                                                               "agent";
}

static esp_err_t video_quality_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_video_control_status_t automation_video = {0};
    si_video_control_get_status(&automation_video);
    if (automation_video.agent_active) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "stop Agent video observation before changing video quality");
    }

    int quality = -1;
    char query[64] = {0};
    char qbuf[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "quality", qbuf, sizeof(qbuf)) == ESP_OK) {
        quality = atoi(qbuf);
    } else if (req->content_len > 0) {
        char buf[128] = {0};
        cJSON *root = NULL;
        esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
        if (ret != ESP_OK) {
            return ret;
        }
        cJSON *q = root ? cJSON_GetObjectItemCaseSensitive(root, "quality") : NULL;
        if (cJSON_IsNumber(q)) {
            quality = q->valueint;
        }
        cJSON_Delete(root);
    }

    if (quality < 1 || quality > 100) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "quality must be 1..100");
    }

    ESP_RETURN_ON_ERROR(si_video_set_quality((uint32_t)quality), TAG, "set video quality");
    ESP_RETURN_ON_ERROR(si_video_control_save_quality((uint32_t)quality), TAG, "save video quality");
    si_web_log("INFO", "Video JPEG quality updated");
    return video_status_handler(req);
}

static esp_err_t video_settings_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_video_control_status_t automation_video = {0};
    si_video_control_get_status(&automation_video);
    if (automation_video.agent_active) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "stop Agent video observation before changing video settings");
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

    cJSON *always_on = cJSON_GetObjectItemCaseSensitive(root, "always_online");
    if (!always_on) {
        always_on = cJSON_GetObjectItemCaseSensitive(root, "always_on");
    }
    if (!always_on) {
        always_on = cJSON_GetObjectItemCaseSensitive(root, "keep_video_online");
    }
    cJSON *preview_fps_raw = cJSON_GetObjectItemCaseSensitive(root,
                                                              "preview_fps_x100");
    cJSON *preview_fps = cJSON_GetObjectItemCaseSensitive(root, "preview_fps");
    bool update_always_on = always_on != NULL;
    bool update_preview_fps = preview_fps_raw != NULL || preview_fps != NULL;
    if (!update_always_on && !update_preview_fps) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "video runtime setting required");
    }
    if (update_always_on && !cJSON_IsBool(always_on)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "always_online must be boolean");
    }

    si_video_control_status_t status;
    si_video_control_get_status(&status);
    bool enabled = update_always_on ? cJSON_IsTrue(always_on) : status.always_on;
    uint32_t preview_fps_x100 =
        si_video_control_preview_fps_valid(status.preview_fps_x100)
            ? status.preview_fps_x100
            : SI_VIDEO_CONTROL_PREVIEW_FPS_X100;
    if (cJSON_IsNumber(preview_fps_raw)) {
        preview_fps_x100 = preview_fps_raw->valueint > 0
                               ? (uint32_t)preview_fps_raw->valueint
                               : 0;
    } else if (cJSON_IsNumber(preview_fps)) {
        preview_fps_x100 = fps_value_to_x100(preview_fps->valuedouble);
    } else if (update_preview_fps) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "preview_fps must be numeric");
    }
    cJSON_Delete(root);

    if (!si_video_control_preview_fps_valid(preview_fps_x100)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "preview_fps must be an integer from 1 to 10");
    }

    ret = si_video_control_save_runtime_settings(enabled, preview_fps_x100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "save video runtime settings: %s", esp_err_to_name(ret));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "failed to save video runtime settings");
    }
    ret = si_video_control_set_always_on(enabled);
    ESP_RETURN_ON_ERROR(ret, TAG, "set video always-on setting");
    ret = si_video_control_set_preview_fps(preview_fps_x100);
    ESP_RETURN_ON_ERROR(ret, TAG, "set video preview FPS");
    ret = si_video_control_apply();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "apply video always-on setting");
    }

    si_web_log("INFO", "Video runtime settings updated");
    return video_status_handler(req);
}

static esp_err_t video_resolution_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_video_control_status_t automation_video = {0};
    si_video_control_get_status(&automation_video);
    if (automation_video.agent_active) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "stop Agent video observation before changing KVM mode");
    }

    int width = -1;
    int height = -1;
    uint32_t fps_x100 = 0;
    char query[128] = {0};
    char wbuf[12] = {0};
    char hbuf[12] = {0};
    char fbuf[16] = {0};
    char pfbuf[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "width", wbuf, sizeof(wbuf)) == ESP_OK &&
        httpd_query_key_value(query, "height", hbuf, sizeof(hbuf)) == ESP_OK) {
        width = atoi(wbuf);
        height = atoi(hbuf);
        if (httpd_query_key_value(query, "fps", fbuf, sizeof(fbuf)) == ESP_OK) {
            fps_x100 = fps_value_to_x100(atof(fbuf));
        }
        (void)httpd_query_key_value(query, "pixel_format", pfbuf, sizeof(pfbuf));
    } else if (req->content_len > 0) {
        char buf[192] = {0};
        cJSON *root = NULL;
        esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
        if (ret != ESP_OK) {
            return ret;
        }
        cJSON *w = root ? cJSON_GetObjectItemCaseSensitive(root, "width") : NULL;
        cJSON *h = root ? cJSON_GetObjectItemCaseSensitive(root, "height") : NULL;
        cJSON *fps = root ? cJSON_GetObjectItemCaseSensitive(root, "fps") : NULL;
        cJSON *fps_raw = root ? cJSON_GetObjectItemCaseSensitive(root, "fps_x100") : NULL;
        cJSON *pixel_format = root ? cJSON_GetObjectItemCaseSensitive(root, "pixel_format") : NULL;
        if (cJSON_IsNumber(w) && cJSON_IsNumber(h)) {
            width = w->valueint;
            height = h->valueint;
        }
        if (cJSON_IsNumber(fps_raw)) {
            fps_x100 = (uint32_t)fps_raw->valueint;
        } else if (cJSON_IsNumber(fps)) {
            fps_x100 = fps_value_to_x100(fps->valuedouble);
        }
        if (cJSON_IsString(pixel_format) && pixel_format->valuestring) {
            strlcpy(pfbuf, pixel_format->valuestring, sizeof(pfbuf));
        }
        cJSON_Delete(root);
    }

    if (width < (int)SI_VIDEO_CONTROL_WIDTH_MIN || height < (int)SI_VIDEO_CONTROL_HEIGHT_MIN ||
        width > (int)SI_VIDEO_CONTROL_WIDTH_MAX || height > (int)SI_VIDEO_CONTROL_HEIGHT_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "width/height required");
    }
    if (fps_x100 > SI_VIDEO_CONTROL_FPS_X100_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fps is too high");
    }
    if (pfbuf[0] != '\0' && strcasecmp(pfbuf, "MJPEG") != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "only MJPEG video modes are supported");
    }

    bool applied_to_active_kvm = false;
    esp_err_t mode_ret = si_video_control_change_kvm_mode(
        (uint32_t)width, (uint32_t)height, fps_x100,
        &applied_to_active_kvm);
    if (mode_ret == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "video mode is not available");
    }
    ESP_RETURN_ON_ERROR(mode_ret, TAG, "set video mode transaction");

    uint32_t saved_width = 0;
    uint32_t saved_height = 0;
    uint32_t saved_fps_x100 = 0;
    ESP_RETURN_ON_ERROR(si_video_control_get_kvm_mode(&saved_width, &saved_height, &saved_fps_x100),
                        TAG, "get KVM video mode");
    /* Persistence is the final transaction step and is reached only after an
     * exact physical mode was validated and, for a live KVM owner, applied. */
    ESP_RETURN_ON_ERROR(si_video_control_save_kvm_mode(saved_width, saved_height, saved_fps_x100),
                        TAG, "save KVM video mode");

    char msg[96];
    if (fps_x100 > 0) {
        snprintf(msg, sizeof(msg), "KVM video mode saved as %dx%d @ %.2ffps",
                 width, height, fps_x100 / 100.0);
    } else {
        snprintf(msg, sizeof(msg), "KVM video resolution saved as %dx%d", width, height);
    }
    si_web_log("INFO", msg);
    return video_status_handler(req);
}

static bool video_lease_auth_live(void *opaque)
{
    const si_auth_session_context_t *expected = opaque;
    si_auth_session_context_t live = {0};
    return expected && expected->principal == SI_PRINCIPAL_BROWSER &&
        si_auth_session_get_live_context(
            expected->session_id, expected->generation, &live) &&
        live.authenticated && live.principal == SI_PRINCIPAL_BROWSER &&
        live.generation == expected->generation &&
        strcmp(live.session_id, expected->session_id) == 0 &&
        si_capability_set_has(live.capabilities,
                              SI_CAPABILITY_VIDEO_CONTROL);
}

static esp_err_t video_lease_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_VIDEO_CONTROL, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char buf[192] = {0};
    cJSON *root = NULL;
    esp_err_t json_ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (json_ret != ESP_OK) {
        return json_ret;
    }
    cJSON *owner = root ? cJSON_GetObjectItemCaseSensitive(root, "owner") : NULL;
    if (!cJSON_IsString(owner) || !owner->valuestring) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "owner required");
    }

    esp_err_t ret = ESP_OK;
    bool log_kvm_acquired = false;
    bool log_kvm_released = false;
    if (strcasecmp(owner->valuestring, "preview") == 0) {
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        if (!cJSON_IsBool(enabled)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled required");
        }
        bool on = cJSON_IsTrue(enabled);
        ret = si_video_control_set_preview(on);
        si_web_log("INFO", on ? "Dashboard preview enabled" : "Dashboard preview disabled");
    } else if (strcasecmp(owner->valuestring, "kvm") == 0) {
        if (session.principal != SI_PRINCIPAL_BROWSER) {
            cJSON_Delete(root);
            return httpd_resp_send_err(
                req, HTTPD_403_FORBIDDEN,
                "KVM ownership requires a browser session");
        }
        si_video_control_status_t previous = {0};
        si_video_control_get_status(&previous);
        cJSON *active = cJSON_GetObjectItemCaseSensitive(root, "active");
        cJSON *force_item = cJSON_GetObjectItemCaseSensitive(root, "force");
        cJSON *reclaim_item = cJSON_GetObjectItemCaseSensitive(root, "reclaim");
        cJSON *claim_item = cJSON_GetObjectItemCaseSensitive(root, "claim");
        cJSON *previous_stream_id_item =
            cJSON_GetObjectItemCaseSensitive(root, "previous_stream_id");
        cJSON *stream_id_item =
            cJSON_GetObjectItemCaseSensitive(root, "stream_id");
        uint32_t stream_id = 0;
        const bool stream_id_present = stream_id_item != NULL;
        if (cJSON_IsNumber(stream_id_item) &&
            stream_id_item->valuedouble > 0 &&
            stream_id_item->valuedouble <= UINT32_MAX &&
            (double)(uint32_t)stream_id_item->valuedouble ==
                stream_id_item->valuedouble) {
            stream_id = (uint32_t)stream_id_item->valuedouble;
        } else if (stream_id_present) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "invalid stream_id");
        }
        bool on = !cJSON_IsBool(active) || cJSON_IsTrue(active);
        bool force = (cJSON_IsBool(force_item) && cJSON_IsTrue(force_item)) ||
                     (cJSON_IsBool(reclaim_item) && cJSON_IsTrue(reclaim_item));
        bool claim = cJSON_IsBool(claim_item) && cJSON_IsTrue(claim_item);
        uint32_t previous_stream_id = 0U;
        if (previous_stream_id_item != NULL) {
            if (!cJSON_IsNumber(previous_stream_id_item) ||
                previous_stream_id_item->valuedouble < 0 ||
                previous_stream_id_item->valuedouble > UINT32_MAX ||
                (double)(uint32_t)previous_stream_id_item->valuedouble !=
                    previous_stream_id_item->valuedouble) {
                cJSON_Delete(root);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                           "invalid previous_stream_id");
            }
            previous_stream_id =
                (uint32_t)previous_stream_id_item->valuedouble;
        }
        if (on) {
            if (stream_id == 0U) {
                cJSON_Delete(root);
                return httpd_resp_send_err(
                    req, HTTPD_400_BAD_REQUEST,
                    "stream_id is required for revocable KVM ownership");
            }
            bool accepted = si_video_control_acquire_kvm_stream_for_session(
                stream_id, force, claim, previous_stream_id,
                session.session_id, session.generation,
                video_lease_auth_live, &session);
            if (!accepted) {
                si_video_control_status_t conflict;
                si_video_control_get_status(&conflict);
                const char *reason = conflict.agent_takeover
                                         ? "agent input control is active"
                                         : "stale KVM stream";
                si_web_log("WARN", reason);
                cJSON_Delete(root);
                return si_http_send_text_status(req, "409 Conflict", reason);
            }
            log_kvm_acquired = !previous.kvm_active;
            ret = si_video_control_apply();
        } else {
            if (stream_id == 0U) {
                cJSON_Delete(root);
                return httpd_resp_send_err(
                    req, HTTPD_400_BAD_REQUEST,
                    "stream_id is required for KVM release");
            }
            bool released = si_video_control_release_kvm_stream_for_session(
                stream_id, session.session_id, session.generation,
                video_lease_auth_live, &session);
            if (!released) {
                cJSON_Delete(root);
                return video_status_handler(req);
            }
            log_kvm_released = previous.kvm_active;
            ret = si_video_control_apply();
        }
    } else if (strcasecmp(owner->valuestring, "agent") == 0) {
        cJSON *active = cJSON_GetObjectItemCaseSensitive(root, "active");
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        cJSON *takeover_item = cJSON_GetObjectItemCaseSensitive(root, "takeover");
        bool on = cJSON_IsBool(active) ? cJSON_IsTrue(active) :
                  (!cJSON_IsBool(enabled) || cJSON_IsTrue(enabled));
        const char *automation_actor = video_automation_actor(&session);
        if (on) {
            if (cJSON_IsBool(takeover_item)) {
                const bool takeover = cJSON_IsTrue(takeover_item);
                si_video_control_touch_automation(automation_actor, takeover);
            } else {
                si_video_control_keep_automation_alive(automation_actor);
            }
            ret = si_video_control_apply();
        } else {
            si_video_control_release_agent();
            ret = si_video_control_apply();
        }
    } else {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown owner");
    }
    cJSON_Delete(root);

    if (ret == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "video mode is not available");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "set video lease");
    if (log_kvm_acquired) {
        si_web_log("INFO", "KVM video lease acquired");
    } else if (log_kvm_released) {
        si_web_log("INFO", "KVM video lease released");
    }
    return video_status_handler(req);
}

static esp_err_t snapshot_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_video_control_status_t control = {0};
    si_video_control_get_status(&control);
    bool cold_start = !control.active_enabled;
    const char *snapshot_actor = video_automation_actor(&session);
    if (cold_start) {
        si_video_control_keep_automation_alive(snapshot_actor);
        esp_err_t start_ret = si_video_control_apply();
        if (start_ret != ESP_OK) {
            si_web_log("WARN", "Snapshot could not start demand-driven video capture");
            return si_http_send_text_status(req, "503 Service Unavailable",
                                            esp_err_to_name(start_ret));
        }
    }

    ESP_RETURN_ON_ERROR(si_video_request_jpeg_snapshot(), TAG,
                        "request fresh video snapshot");

    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    TickType_t deadline =
        xTaskGetTickCount() +
        pdMS_TO_TICKS(VIDEO_SNAPSHOT_FIRST_FRAME_TIMEOUT_MS);
    do {
        ret = si_video_acquire_jpeg(&jpeg, &jpeg_len, NULL);
        if (ret == ESP_OK) {
            break;
        }
        if (ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_NOT_FINISHED &&
            ret != ESP_ERR_TIMEOUT) {
            break;
        }
        if (cold_start) {
            si_video_control_keep_automation_alive(snapshot_actor);
        }
        vTaskDelay(pdMS_TO_TICKS(VIDEO_SNAPSHOT_POLL_MS));
    } while ((int32_t)(xTaskGetTickCount() - deadline) < 0);

    if (ret != ESP_OK) {
        si_web_log("WARN", "Snapshot first frame timeout");
        return si_http_send_text_status(req, "503 Service Unavailable",
                                        esp_err_to_name(ret));
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send(req, (const char *)jpeg, jpeg_len);
    free(jpeg);
    return ret;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    bool agent_stream = false;
    uint32_t stream_id = 0;
    bool stream_id_present = false;
    bool stream_id_valid = true;
    uint32_t agent_stream_epoch = 0U;
    const char *stream_actor = video_automation_actor(&session);
    char query[96] = {0};
    char owner[16] = {0};
    char stream_id_text[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "owner", owner,
                                  sizeof(owner)) == ESP_OK &&
            strcasecmp(owner, "agent") == 0) {
            agent_stream = true;
        }
        if (httpd_query_key_value(query, "stream_id", stream_id_text,
                                  sizeof(stream_id_text)) == ESP_OK) {
            stream_id_present = true;
            char *end = NULL;
            unsigned long parsed = strtoul(stream_id_text, &end, 10);
            if (end && *end == '\0' && parsed > 0 && parsed <= UINT32_MAX) {
                stream_id = (uint32_t)parsed;
            } else {
                stream_id_valid = false;
            }
        }
    }
    if (stream_id_present && !stream_id_valid) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid stream_id");
    }

    if (agent_stream) {
        /* Observation is shared: an Agent viewer consumes the same immutable
         * JPEG frame store while a human KVM keeps the capture mode and input
         * lease.  Only Agent input takeover is exclusive; read-only video must
         * not evict or block the human viewer. */
        si_video_control_keep_automation_alive(stream_actor);
        (void)si_video_control_apply();
        agent_stream_epoch = si_video_control_agent_stream_epoch();
    } else if (stream_id != 0U) {
        /* A browser transport consumes the lease established by POST. It must
         * never acquire or renew that lease itself: an old async GET can begin
         * after the matching release and would otherwise resurrect KVM. */
        if (!si_video_control_kvm_stream_is_current(stream_id)) {
            return si_http_send_text_status(req, "409 Conflict",
                                            "stale KVM video stream");
        }
    } else {
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "stream_id is required for revocable KVM video");
    }

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    uint32_t last_frame_id = 0;
    TickType_t first_frame_deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(VIDEO_STREAM_FIRST_FRAME_TIMEOUT_MS);
    TickType_t last_frame_tick = 0;
    TickType_t next_send_tick = 0;
    const TickType_t frame_interval = pdMS_TO_TICKS(SI_CFG_VIDEO_STREAM_DELAY_MS);
    uint32_t last_video_touch_ms = 0;
    uint32_t last_peer_check_ms = 0;
    if (!agent_stream) {
        si_video_stream_metrics_begin(stream_id, si_monotonic_ms());
    }

    while (true) {
        uint32_t now_ms = si_monotonic_ms();
        if (agent_stream &&
            agent_stream_epoch !=
                si_video_control_agent_stream_epoch()) {
            return ESP_OK;
        }
        if (!agent_stream && stream_id != 0) {
            if (!si_video_control_kvm_stream_is_current(stream_id)) {
                return ESP_OK;
            }
        }
        if (last_peer_check_ms == 0 ||
            (uint32_t)(now_ms - last_peer_check_ms) >= 250U) {
            if (video_stream_peer_closed(req)) {
                return ESP_OK;
            }
            last_peer_check_ms = now_ms;
        }
        if (last_video_touch_ms == 0 ||
            (uint32_t)(now_ms - last_video_touch_ms) >= 1000U) {
            if (agent_stream) {
                si_video_control_keep_automation_alive(stream_actor);
            }
            last_video_touch_ms = now_ms;
        }

        /*
         * Pace before consuming the next frame.  Marking a frame as consumed
         * and then waiting would discard it whenever capture and stream timing
         * differ by even one scheduler tick (30 fps input commonly collapsed
         * to roughly 16 fps here).
         */
        TickType_t now = xTaskGetTickCount();
        if (frame_interval > 0 && next_send_tick != 0 &&
            (int32_t)(now - next_send_tick) < 0) {
            vTaskDelay(next_send_tick - now);
            continue;
        }

        /* Serialize MJPEG senders while an immutable input-frame view is
         * borrowed.  The HTTP consumer owns only the borrow token; codec
         * buffers and output-mode resources remain private to their
         * respective pipeline. */
        esp_err_t gate_ret = si_video_begin_mjpeg_send(1200);
        if (gate_ret == ESP_ERR_INVALID_STATE) {
            /* Output changed to H.264. End the superseded multipart response
             * instead of spinning until the browser happens to abort it. */
            return ESP_OK;
        }
        if (gate_ret != ESP_OK) {
            continue;
        }
        si_video_jpeg_view_t jpeg_view = {0};
        esp_err_t ret =
            si_video_borrow_jpeg_if_new(last_frame_id, &jpeg_view);
        if (ret == ESP_ERR_NOT_FINISHED) {
            si_video_end_mjpeg_send();
            TickType_t now = xTaskGetTickCount();
            if (last_frame_id == 0 &&
                (int32_t)(now - first_frame_deadline) >= 0) {
                si_web_log("WARN",
                           agent_stream ?
                               "Agent MJPEG stream first frame timeout" :
                               "KVM MJPEG stream first frame timeout");
                return si_http_send_text_status(
                    req, "503 Service Unavailable",
                    "video stream first frame timeout");
            }
            if (last_frame_tick != 0 &&
                (int32_t)(now - last_frame_tick) >=
                    (int32_t)pdMS_TO_TICKS(VIDEO_STREAM_STALL_TIMEOUT_MS)) {
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (ret != ESP_OK) {
            si_video_end_mjpeg_send();
            TickType_t now = xTaskGetTickCount();
            if (last_frame_id == 0 &&
                (int32_t)(now - first_frame_deadline) >= 0) {
                return si_http_send_text_status(req, "503 Service Unavailable", esp_err_to_name(ret));
            }
            if (last_frame_tick != 0 &&
                (int32_t)(now - last_frame_tick) >=
                    (int32_t)pdMS_TO_TICKS(VIDEO_STREAM_STALL_TIMEOUT_MS)) {
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const size_t jpeg_len = jpeg_view.len;
        const uint32_t frame_id = jpeg_view.frame_id;
        /* The packed frame store reference, not the output-mode gate, owns
         * these immutable bytes. Release the gate before network I/O so one
         * slow MJPEG client cannot block a mode transition or another reader;
         * clear/generation changes defer slot reuse until this view releases. */
        si_video_end_mjpeg_send();
        TickType_t send_started_tick = xTaskGetTickCount();

        char header[128];
        int header_len = snprintf(header, sizeof(header),
                                  "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                                  jpeg_len);
        esp_err_t send_ret = httpd_resp_send_chunk(req, header, header_len);
        if (send_ret == ESP_OK) {
            send_ret = httpd_resp_send_chunk(
                req, (const char *)jpeg_view.data, jpeg_len);
        }
        si_video_release_jpeg(&jpeg_view);
        if (send_ret == ESP_OK) {
            send_ret = httpd_resp_send_chunk(req, "\r\n", 2);
        }
        if (send_ret != ESP_OK) {
            return ESP_OK;
        }
        last_frame_id = frame_id;
        last_frame_tick = xTaskGetTickCount();
        if (!agent_stream) {
            si_video_stream_metrics_record(
                stream_id, (size_t)header_len + jpeg_len + 2U,
                si_monotonic_ms());
        }
        if (frame_interval > 0) {
            next_send_tick = send_started_tick + frame_interval;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static int httpd_client_count(httpd_handle_t server)
{
    if (!server) {
        return 0;
    }
    size_t clients = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(server, &clients, fds) != ESP_OK) {
        return 0;
    }
    return (int)clients;
}

int si_web_client_count(void)
{
    return httpd_client_count(s_server) + httpd_client_count(s_stream_server);
}
