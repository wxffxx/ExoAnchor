// Pure KVM HTTP, WebSocket, and web UI service.
#include "web_server.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "auth_service.h"
#include "cJSON.h"
#include "control_lease.h"
#include "device_http.h"
#include "device_observation_service.h"
#include "device_settings.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_json.h"
#include "http_api.h"
#include "net_manager.h"
#include "nvs_flash.h"
#include "power_control.h"
#include "sdkconfig.h"
#include "status_ws.h"
#include "time_utils.h"
#include "video_control.h"
#include "video_input.h"
#include "web_json.h"

static const char *TAG = "si-web";

extern const uint8_t www_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t www_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t www_kvm_html_start[] asm("_binary_kvm_html_start");
extern const uint8_t www_kvm_html_end[] asm("_binary_kvm_html_end");
extern const uint8_t www_ui_core_css_start[] asm("_binary_ui_core_css_start");
extern const uint8_t www_ui_core_css_end[] asm("_binary_ui_core_css_end");
extern const uint8_t www_ui_core_js_start[] asm("_binary_ui_core_js_start");
extern const uint8_t www_ui_core_js_end[] asm("_binary_ui_core_js_end");
extern const uint8_t www_ui_shell_css_start[] asm("_binary_ui_shell_css_start");
extern const uint8_t www_ui_shell_css_end[] asm("_binary_ui_shell_css_end");
extern const uint8_t www_ui_shell_js_start[] asm("_binary_ui_shell_js_start");
extern const uint8_t www_ui_shell_js_end[] asm("_binary_ui_shell_js_end");
extern const uint8_t www_settings_html_start[] asm("_binary_settings_html_start");
extern const uint8_t www_settings_html_end[] asm("_binary_settings_html_end");

#define SCRATCH_BUFSIZE 1024
#define SI_MAIN_HTTP_MAX_OPEN_SOCKETS 12
#define SI_STREAM_HTTP_MAX_OPEN_SOCKETS 4
#define SI_HTTPD_INTERNAL_SOCKETS_PER_SERVER 3
#define SI_HTTPD_SERVER_COUNT 2
#define SI_NETWORK_SOCKET_HEADROOM 8
#define SI_HTTP_SOCKET_BUDGET_REQUIRED                                      \
    (SI_MAIN_HTTP_MAX_OPEN_SOCKETS + SI_STREAM_HTTP_MAX_OPEN_SOCKETS +      \
     SI_HTTPD_INTERNAL_SOCKETS_PER_SERVER * SI_HTTPD_SERVER_COUNT +          \
     SI_NETWORK_SOCKET_HEADROOM)

_Static_assert(CONFIG_LWIP_MAX_SOCKETS >= SI_HTTP_SOCKET_BUDGET_REQUIRED,
               "LWIP socket budget cannot cover the HTTP servers and connection headroom");

static httpd_handle_t s_server;
static httpd_handle_t s_stream_server;

void si_web_log(const char *level, const char *message)
{
    si_device_observation_log_append(level, message);
}

#include "web/base_settings_http_module.inc"
#include "web/power_http_module.inc"
#include "web/video_http_module.inc"

static esp_err_t ws_auth_pre_handshake(httpd_req_t *req)
{
    return si_http_check_auth(req) ? ESP_OK : ESP_FAIL;
}

static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"error\":\"not found\"}");
}

static void register_uri(httpd_handle_t server, const char *uri,
                         httpd_method_t method,
                         esp_err_t (*handler)(httpd_req_t *), bool websocket)
{
    httpd_uri_t cfg = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = NULL,
    };
#ifdef CONFIG_HTTPD_WS_SUPPORT
    cfg.is_websocket = websocket;
    if (websocket) {
        cfg.ws_pre_handshake_cb = ws_auth_pre_handshake;
    }
#else
    (void)websocket;
#endif
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &cfg));
}

static esp_err_t start_stream_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SI_STREAM_HTTP_PORT;
    config.ctrl_port = (uint16_t)(config.ctrl_port + 1);
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 6144;
    config.max_uri_handlers = 4;
    config.max_open_sockets = SI_STREAM_HTTP_MAX_OPEN_SOCKETS;
    config.lru_purge_enable = true;
    config.send_wait_timeout = 10;

    ESP_RETURN_ON_ERROR(httpd_start(&s_stream_server, &config), TAG,
                        "httpd_start stream");
    register_uri(s_stream_server, "/api/stream", HTTP_GET, stream_handler, false);
    register_uri(s_stream_server, "/stream", HTTP_GET, stream_handler, false);
    ESP_LOGI(TAG, "MJPEG stream server started on port %d", SI_STREAM_HTTP_PORT);
    return ESP_OK;
}

esp_err_t si_web_server_start(void)
{
    (void)si_auth_is_enabled();
    ESP_RETURN_ON_ERROR(si_video_control_start(), TAG, "start video control");
    ESP_RETURN_ON_ERROR(si_control_lease_start(), TAG, "start control lease");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SI_DEFAULT_HTTP_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 12288;
    config.max_uri_handlers = 64;
    config.max_open_sockets = SI_MAIN_HTTP_MAX_OPEN_SOCKETS;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start");

    register_uri(s_server, "/", HTTP_GET, index_handler, false);
    register_uri(s_server, "/kvm", HTTP_GET, kvm_handler, false);
    register_uri(s_server, "/settings", HTTP_GET, settings_handler, false);
    register_uri(s_server, "/assets/ui-core.css", HTTP_GET, ui_core_css_handler, false);
    register_uri(s_server, "/assets/ui-core.js", HTTP_GET, ui_core_js_handler, false);
    register_uri(s_server, "/assets/ui-shell.css", HTTP_GET, ui_shell_css_handler, false);
    register_uri(s_server, "/assets/ui-shell.js", HTTP_GET, ui_shell_js_handler, false);

    register_uri(s_server, "/api/auth/login", HTTP_POST, auth_login_handler, false);
    register_uri(s_server, "/api/auth/status", HTTP_GET, auth_status_handler, false);
    register_uri(s_server, "/api/settings/account", HTTP_POST, settings_password_handler, false);
    register_uri(s_server, "/api/settings/password", HTTP_POST, settings_password_handler, false);
    register_uri(s_server, "/api/settings/device", HTTP_GET, settings_device_handler, false);
    register_uri(s_server, "/api/settings/device", HTTP_POST, settings_device_handler, false);
    register_uri(s_server, "/api/settings/session", HTTP_GET, settings_session_handler, false);
    register_uri(s_server, "/api/settings/session", HTTP_POST, settings_session_handler, false);
    register_uri(s_server, "/api/settings/system", HTTP_GET, settings_system_handler, false);
    register_uri(s_server, "/api/settings/system", HTTP_POST, settings_system_handler, false);
    register_uri(s_server, "/api/settings/gpio-map", HTTP_GET, power_gpio_map_handler, false);
    register_uri(s_server, "/api/settings/gpio-map", HTTP_POST, power_gpio_map_handler, false);

    register_uri(s_server, "/api/capabilities", HTTP_GET, capabilities_handler, false);
    register_uri(s_server, "/api/status", HTTP_GET, overall_status_handler, false);
    register_uri(s_server, "/api/system/info", HTTP_GET, system_info_handler, false);
    register_uri(s_server, "/api/system/logs", HTTP_GET, logs_handler, false);
    register_uri(s_server, "/api/system/logs/download", HTTP_GET, logs_download_handler, false);
    register_uri(s_server, "/api/hid/status", HTTP_GET, hid_status_handler, false);
    register_uri(s_server, "/api/hid/actions", HTTP_POST, hid_actions_handler, false);
    register_uri(s_server, "/api/control/lease", HTTP_GET, control_lease_handler, false);
    register_uri(s_server, "/api/control/lease", HTTP_POST, control_lease_handler, false);
    register_uri(s_server, "/api/power/status", HTTP_GET, power_status_handler, false);
    register_uri(s_server, "/api/power/action", HTTP_POST, power_action_handler, false);

#ifdef CONFIG_HTTPD_WS_SUPPORT
    register_uri(s_server, "/api/ws/hid", HTTP_GET, hid_ws_handler, true);
#endif
    register_uri(s_server, "/api/video/status", HTTP_GET, video_status_handler, false);
    register_uri(s_server, "/api/video/settings", HTTP_POST, video_settings_handler, false);
    register_uri(s_server, "/api/video/quality", HTTP_POST, video_quality_handler, false);
    register_uri(s_server, "/api/video/resolution", HTTP_POST, video_resolution_handler, false);
    register_uri(s_server, "/api/video/lease", HTTP_POST, video_lease_handler, false);
    register_uri(s_server, "/api/stream", HTTP_GET, stream_redirect_handler, false);
    register_uri(s_server, "/api/snapshot", HTTP_GET, snapshot_handler, false);

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler);
    ESP_RETURN_ON_ERROR(start_stream_server(), TAG, "start stream server");
    si_web_log("INFO", "Stable KVM HTTP server started");
    ESP_LOGI(TAG, "HTTP server started on port %d", SI_DEFAULT_HTTP_PORT);
    return ESP_OK;
}
