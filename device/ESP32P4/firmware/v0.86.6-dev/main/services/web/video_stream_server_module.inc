// Dedicated video listener.  The browser keeps ordinary UI/API traffic on the
// main server, while H.264 WebSocket writes are serialized by this HTTPD task.
// A future TLS terminator may proxy this listener, but firmware does not expose
// a second H.264 route on the UI server because that would reintroduce two
// concurrent HTTPD session domains around one codec pipeline.
static esp_err_t start_stream_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SI_STREAM_HTTP_PORT;
    config.ctrl_port = (uint16_t)(config.ctrl_port + 1);
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 6144;
    config.max_uri_handlers = 8;
    config.max_open_sockets = SI_STREAM_HTTP_MAX_OPEN_SOCKETS;
    config.lru_purge_enable = true;
    config.send_wait_timeout = 1;
    ESP_RETURN_ON_ERROR(httpd_start(&s_stream_server, &config), TAG,
                        "httpd_start stream");
    register_uri(s_stream_server, "/api/stream", HTTP_GET, stream_handler,
                 false);
#if SI_CFG_VIDEO_H264_ENABLED && defined(CONFIG_HTTPD_WS_SUPPORT)
    register_uri(s_stream_server, "/api/ws/video/h264", HTTP_GET,
                 si_h264_stream_ws_handler, true);
#endif
    ESP_LOGI(TAG, "Compatibility video server started on port %d (H.264=%s)",
             SI_STREAM_HTTP_PORT,
             SI_CFG_VIDEO_H264_ENABLED ? "experimental" : "off");
    return ESP_OK;
}
