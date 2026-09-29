// Transitional domain module: Video settings, lease, snapshot and stream HTTP. Kept in the web compilation unit until its service contract is independent.
static esp_err_t video_quality_handler(httpd_req_t *req)
{
    if (!si_http_check_auth(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
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
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
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
    if (!si_http_check_auth(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
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

    bool kvm_active = si_video_control_set_kvm_mode((uint32_t)width, (uint32_t)height, fps_x100);
    if (kvm_active) {
        esp_err_t ret = si_video_control_apply();
        if (ret == ESP_ERR_NOT_FOUND) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "video mode is not available");
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "set video mode");
    }

    uint32_t saved_width = 0;
    uint32_t saved_height = 0;
    uint32_t saved_fps_x100 = 0;
    ESP_RETURN_ON_ERROR(si_video_control_get_kvm_mode(&saved_width, &saved_height, &saved_fps_x100),
                        TAG, "get KVM video mode");
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

static esp_err_t video_lease_handler(httpd_req_t *req)
{
    if (!si_http_check_auth(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
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
        cJSON *active = cJSON_GetObjectItemCaseSensitive(root, "active");
        cJSON *force_item = cJSON_GetObjectItemCaseSensitive(root, "force");
        cJSON *reclaim_item = cJSON_GetObjectItemCaseSensitive(root, "reclaim");
        bool on = !cJSON_IsBool(active) || cJSON_IsTrue(active);
        bool force = (cJSON_IsBool(force_item) && cJSON_IsTrue(force_item)) ||
                     (cJSON_IsBool(reclaim_item) && cJSON_IsTrue(reclaim_item));
        if (on) {
            if (force) {
                si_video_control_release_agent();
            }
            bool accepted = si_video_control_touch_kvm();
            if (!accepted && !force) {
                cJSON_Delete(root);
                return si_http_send_text_status(req, "409 Conflict", "agent video takeover is active");
            }
            ret = si_video_control_apply();
        } else {
            si_video_control_release_kvm();
            ret = si_video_control_apply();
        }
    } else if (strcasecmp(owner->valuestring, "agent") == 0) {
        cJSON *active = cJSON_GetObjectItemCaseSensitive(root, "active");
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        cJSON *takeover_item = cJSON_GetObjectItemCaseSensitive(root, "takeover");
        bool on = cJSON_IsBool(active) ? cJSON_IsTrue(active) :
                  (!cJSON_IsBool(enabled) || cJSON_IsTrue(enabled));
        if (on) {
            if (cJSON_IsBool(takeover_item)) {
                si_video_control_touch_agent(cJSON_IsTrue(takeover_item));
            } else {
                si_video_control_keep_agent_alive();
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
    return video_status_handler(req);
}

static esp_err_t snapshot_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    esp_err_t ret = si_video_acquire_jpeg(&jpeg, &jpeg_len, NULL);
    if (ret != ESP_OK) {
        return si_http_send_text_status(req, "503 Service Unavailable", esp_err_to_name(ret));
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send(req, (const char *)jpeg, jpeg_len);
    free(jpeg);
    return ret;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    uint8_t *jpeg = heap_caps_malloc(SI_CFG_VIDEO_JPEG_BUFFER_SIZE,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg) {
        jpeg = malloc(SI_CFG_VIDEO_JPEG_BUFFER_SIZE);
    }
    if (!jpeg) {
        return si_http_send_text_status(req, "503 Service Unavailable", "stream buffer allocation failed");
    }

    bool agent_stream = false;
    char query[64] = {0};
    char owner[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "owner", owner, sizeof(owner)) == ESP_OK &&
        strcasecmp(owner, "agent") == 0) {
        agent_stream = true;
    }

    if (agent_stream) {
        si_video_control_keep_agent_alive();
    } else {
        si_video_control_touch_kvm();
    }
    (void)si_video_control_apply();

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");

    uint32_t last_frame_id = 0;
    TickType_t first_frame_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    TickType_t next_send_tick = 0;
    const TickType_t frame_interval = pdMS_TO_TICKS(SI_CFG_VIDEO_STREAM_DELAY_MS);
    uint32_t last_video_touch_ms = 0;

    while (true) {
        uint32_t now_ms = si_monotonic_ms();
        if (last_video_touch_ms == 0 ||
            (uint32_t)(now_ms - last_video_touch_ms) >= 1000U) {
            if (agent_stream) {
                si_video_control_keep_agent_alive();
            } else {
                si_video_control_touch_kvm();
            }
            last_video_touch_ms = now_ms;
        }

        size_t jpeg_len = 0;
        uint32_t frame_id = 0;
        esp_err_t ret = si_video_copy_jpeg(jpeg, SI_CFG_VIDEO_JPEG_BUFFER_SIZE,
                                           &jpeg_len, &frame_id);
        if (ret != ESP_OK) {
            if (last_frame_id == 0 &&
                (int32_t)(xTaskGetTickCount() - first_frame_deadline) >= 0) {
                free(jpeg);
                return si_http_send_text_status(req, "503 Service Unavailable", esp_err_to_name(ret));
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (frame_id == last_frame_id) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        last_frame_id = frame_id;

        TickType_t now = xTaskGetTickCount();
        if (frame_interval > 0 && next_send_tick != 0 &&
            (int32_t)(now - next_send_tick) < 0) {
            vTaskDelay(next_send_tick - now);
            continue;
        }

        char header[128];
        int header_len = snprintf(header, sizeof(header),
                                  "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                                  jpeg_len);
        if (httpd_resp_send_chunk(req, header, header_len) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)jpeg, jpeg_len) != ESP_OK ||
            httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
            free(jpeg);
            return ESP_OK;
        }
        if (frame_interval > 0) {
            next_send_tick = now + frame_interval;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static esp_err_t stream_redirect_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    char host[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || host[0] == '\0') {
        si_net_status_t net;
        si_net_get_status(&net);
        strlcpy(host, net.ip[0] ? net.ip : "127.0.0.1", sizeof(host));
    }
    char *port = strchr(host, ':');
    if (port) {
        *port = '\0';
    }

    char query[192] = {0};
    char token[96] = {0};
    bool has_token = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                     httpd_query_key_value(query, "auth", token, sizeof(token)) == ESP_OK;
    char location[192];
    if (has_token) {
        snprintf(location, sizeof(location), "http://%s:%u/api/stream?auth=%s",
                 host, (unsigned)SI_STREAM_HTTP_PORT, token);
    } else {
        snprintf(location, sizeof(location), "http://%s:%u/api/stream",
                 host, (unsigned)SI_STREAM_HTTP_PORT);
    }
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, location);
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
