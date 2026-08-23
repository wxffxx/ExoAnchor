// Prepend the compile-time profile before the common UI runtime executes.
static esp_err_t ui_core_js_handler(httpd_req_t *req)
{
    si_http_set_security_headers(req);
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control",
                       "private, max-age=0, must-revalidate");
    static const char build_profile[] =
        "window.ExoAnchorBuild={profile:\"" SI_BUILD_PROFILE
        "\",version:\"" SI_BMC_VERSION
        "\",embeddedAgent:"
#if SI_CFG_EMBEDDED_AGENT_ENABLED
        "true"
#else
        "false"
#endif
        ",uartTerminal:"
#if SI_CFG_UART_TERMINAL_ENABLED
        "true"
#else
        "false"
#endif
        ",h264Video:"
#if SI_CFG_VIDEO_H264_ENABLED
        "true"
#else
        "false"
#endif
        ",externalMcp:true};\n";
    ESP_RETURN_ON_ERROR(
        httpd_resp_send_chunk(req, build_profile,
                              sizeof(build_profile) - 1U),
        TAG, "send build profile");
    size_t len = (size_t)(www_ui_core_js_end - www_ui_core_js_start);
    if (len > 0 && www_ui_core_js_start[len - 1] == '\0') {
        len--;
    }
    ESP_RETURN_ON_ERROR(
        httpd_resp_send_chunk(req, (const char *)www_ui_core_js_start, len),
        TAG, "send UI core");
    return httpd_resp_send_chunk(req, NULL, 0);
}
