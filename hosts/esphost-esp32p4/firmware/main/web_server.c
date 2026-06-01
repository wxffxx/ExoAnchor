#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/param.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_device.h"
#include "net_manager.h"
#include "sdkconfig.h"
#include "video_input.h"

static const char *TAG = "si-web";

extern const uint8_t www_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t www_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t www_kvm_html_start[] asm("_binary_kvm_html_start");
extern const uint8_t www_kvm_html_end[] asm("_binary_kvm_html_end");

typedef struct {
    char time[16];
    char level[12];
    char message[128];
} log_entry_t;

#define MAX_LOGS 80
#define SCRATCH_BUFSIZE 1024

static httpd_handle_t s_server;
static log_entry_t s_logs[MAX_LOGS];
static size_t s_log_head;
static size_t s_log_count;

static uint32_t uptime_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void format_uptime(uint32_t sec, char *out, size_t out_size)
{
    uint32_t days = sec / 86400;
    uint32_t hours = (sec % 86400) / 3600;
    uint32_t minutes = (sec % 3600) / 60;
    snprintf(out, out_size, "%" PRIu32 "d %" PRIu32 "h %" PRIu32 "m", days, hours, minutes);
}

void si_web_log(const char *level, const char *message)
{
    log_entry_t *entry = &s_logs[s_log_head];
    uint32_t sec = uptime_sec();
    snprintf(entry->time, sizeof(entry->time), "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
             sec / 3600, (sec % 3600) / 60, sec % 60);
    strlcpy(entry->level, level ? level : "INFO", sizeof(entry->level));
    strlcpy(entry->message, message ? message : "", sizeof(entry->message));

    s_log_head = (s_log_head + 1) % MAX_LOGS;
    if (s_log_count < MAX_LOGS) {
        s_log_count++;
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    if (!payload) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc failed");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, payload);
    free(payload);
    return ret;
}

static esp_err_t send_text_status(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message ? message : status);
}

static bool check_auth(httpd_req_t *req)
{
    if (!si_auth_enabled()) {
        return true;
    }

    char auth[96] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) == ESP_OK) {
        const char *prefix = "Bearer ";
        if (strncmp(auth, prefix, strlen(prefix)) == 0 &&
            strcmp(auth + strlen(prefix), CONFIG_SI_AUTH_PASSWORD) == 0) {
            return true;
        }
    }

    char query[128] = {0};
    char token[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "auth", token, sizeof(token)) == ESP_OK &&
        strcmp(token, CONFIG_SI_AUTH_PASSWORD) == 0) {
        return true;
    }

    return false;
}

static esp_err_t send_embedded_html(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return send_embedded_html(req, www_index_html_start, www_index_html_end);
}

static esp_err_t kvm_handler(httpd_req_t *req)
{
    return send_embedded_html(req, www_kvm_html_start, www_kvm_html_end);
}

static esp_err_t auth_login_handler(httpd_req_t *req)
{
    char buf[SCRATCH_BUFSIZE] = {0};
    int len = httpd_req_recv(req, buf, MIN(req->content_len, SCRATCH_BUFSIZE - 1));
    if (len < 0) {
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    bool ok = !si_auth_enabled();
    if (!ok) {
        cJSON *root = cJSON_Parse(buf);
        cJSON *password = root ? cJSON_GetObjectItemCaseSensitive(root, "password") : NULL;
        ok = cJSON_IsString(password) && strcmp(password->valuestring, CONFIG_SI_AUTH_PASSWORD) == 0;
        cJSON_Delete(root);
    }

    if (!ok) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid credentials");
    }

    cJSON_AddStringToObject(resp, "token", CONFIG_SI_AUTH_PASSWORD);
    cJSON_AddStringToObject(resp, "type", "bearer");
    esp_err_t ret = send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t system_info_handler(httpd_req_t *req)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);

    si_net_status_t net;
    si_net_get_status(&net);
    char uptime[32];
    format_uptime(uptime_sec(), uptime, sizeof(uptime));

    cJSON *root = cJSON_CreateObject();
    cJSON *cpu = cJSON_AddObjectToObject(root, "cpu");
    cJSON_AddNumberToObject(cpu, "usage_percent", 0);
    cJSON_AddNumberToObject(cpu, "cores", chip_info.cores);
    cJSON_AddNumberToObject(cpu, "freq_mhz", 0);

    cJSON *memory = cJSON_AddObjectToObject(root, "memory");
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    cJSON_AddNumberToObject(memory, "total_mb", 0);
    cJSON_AddNumberToObject(memory, "used_mb", 0);
    cJSON_AddNumberToObject(memory, "available_mb", free_heap / 1024 / 1024);
    cJSON_AddNumberToObject(memory, "free_bytes", free_heap);
    cJSON_AddNumberToObject(memory, "minimum_free_bytes", min_heap);
    cJSON_AddNumberToObject(memory, "usage_percent", 0);

    cJSON *temp = cJSON_AddObjectToObject(root, "temperature");
    cJSON_AddNumberToObject(temp, "celsius", 0);
    cJSON_AddStringToObject(temp, "source", "unavailable");

    cJSON *disk = cJSON_AddObjectToObject(root, "disk");
    cJSON_AddNumberToObject(disk, "total_gb", flash_size / 1024.0 / 1024.0 / 1024.0);
    cJSON_AddNumberToObject(disk, "used_gb", 0);
    cJSON_AddNumberToObject(disk, "free_gb", 0);
    cJSON_AddNumberToObject(disk, "usage_percent", 0);

    cJSON *network = cJSON_AddObjectToObject(root, "network");
    cJSON *eth = cJSON_AddObjectToObject(network, "ethernet");
    cJSON_AddBoolToObject(eth, "up", net.connected);
    cJSON_AddBoolToObject(eth, "link_up", net.link_up);
    cJSON_AddStringToObject(eth, "ipv4", net.ip[0] ? net.ip : "No IP");
    cJSON_AddStringToObject(eth, "netmask", net.netmask);
    cJSON_AddStringToObject(eth, "gateway", net.gateway);
    cJSON_AddStringToObject(eth, "mac", net.mac);

    cJSON *uptime_obj = cJSON_AddObjectToObject(root, "uptime");
    cJSON_AddNumberToObject(uptime_obj, "seconds", uptime_sec());
    cJSON_AddStringToObject(uptime_obj, "formatted", uptime);

    cJSON *load = cJSON_AddObjectToObject(root, "load");
    cJSON_AddNumberToObject(load, "1min", 0);
    cJSON_AddNumberToObject(load, "5min", 0);
    cJSON_AddNumberToObject(load, "15min", 0);
    cJSON_AddStringToObject(root, "hostname", SI_BMC_HOSTNAME);

    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t logs_handler(httpd_req_t *req)
{
    int count = 50;
    char query[64] = {0};
    char nbuf[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "n", nbuf, sizeof(nbuf)) == ESP_OK) {
        count = atoi(nbuf);
        if (count < 1) {
            count = 1;
        } else if (count > MAX_LOGS) {
            count = MAX_LOGS;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *logs = cJSON_AddArrayToObject(root, "logs");
    size_t total = MIN((size_t)count, s_log_count);
    size_t start = (s_log_head + MAX_LOGS - total) % MAX_LOGS;
    for (size_t i = 0; i < total; i++) {
        size_t idx = (start + i) % MAX_LOGS;
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "time", s_logs[idx].time);
        cJSON_AddStringToObject(entry, "level", s_logs[idx].level);
        cJSON_AddStringToObject(entry, "message", s_logs[idx].message);
        cJSON_AddItemToArray(logs, entry);
    }
    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t hid_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", "native_usb_tinyusb");
    cJSON_AddBoolToObject(root, "connected", si_hid_is_mounted());
    cJSON_AddStringToObject(root, "port", "USB-OTG");
    cJSON *keyboard = cJSON_AddObjectToObject(root, "keyboard");
    cJSON_AddBoolToObject(keyboard, "available", si_hid_is_ready());
    cJSON *mouse = cJSON_AddObjectToObject(root, "mouse");
    cJSON_AddBoolToObject(mouse, "available", si_hid_is_ready());
    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t control_bridge_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "source", "esp32p4_native_usb_hid");
    cJSON_AddBoolToObject(root, "connected", si_hid_is_mounted());
    cJSON_AddStringToObject(root, "port", "USB-OTG");
    cJSON *caps = cJSON_AddObjectToObject(root, "capabilities");
    cJSON_AddBoolToObject(caps, "keyboard", true);
    cJSON_AddBoolToObject(caps, "mouse", true);
    cJSON_AddBoolToObject(caps, "websocket_hid", true);
    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t control_bridge_config_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "source", "esp32p4_native_usb_hid");
    cJSON_AddStringToObject(root, "transport", "websocket_to_tinyusb");
    cJSON *commands = cJSON_AddObjectToObject(root, "commands");
    cJSON_AddStringToObject(commands, "keydown", "USB HID keyboard press");
    cJSON_AddStringToObject(commands, "keyup", "USB HID keyboard release");
    cJSON_AddStringToObject(commands, "mousemove", "USB HID relative mouse move");
    cJSON_AddStringToObject(commands, "wheel", "USB HID mouse wheel");
    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static void add_video_json(cJSON *root)
{
    si_video_status_t st;
    si_video_get_status(&st);

    cJSON_AddBoolToObject(root, "enabled", st.enabled);
    cJSON_AddBoolToObject(root, "connected", st.frame_ready);
    cJSON_AddBoolToObject(root, "initialized", st.initialized);
    cJSON_AddBoolToObject(root, "streaming", st.streaming);
    cJSON_AddStringToObject(root, "source", st.source);
    cJSON_AddStringToObject(root, "pixel_format", st.pixel_format);
    cJSON_AddNumberToObject(root, "width", st.width);
    cJSON_AddNumberToObject(root, "height", st.height);
    char resolution[24];
    snprintf(resolution, sizeof(resolution), "%" PRIu32 "x%" PRIu32, st.width, st.height);
    cJSON_AddStringToObject(root, "resolution", resolution);
    cJSON_AddNumberToObject(root, "data_lanes", st.data_lanes);
    cJSON_AddNumberToObject(root, "lane_bitrate_mbps", st.lane_bitrate_mbps);
    cJSON_AddNumberToObject(root, "quality", st.jpeg_quality);
    cJSON_AddNumberToObject(root, "frames_captured", st.frames_captured);
    cJSON_AddNumberToObject(root, "frames_encoded", st.frames_encoded);
    cJSON_AddNumberToObject(root, "frames_dropped", st.frames_dropped);
    cJSON_AddNumberToObject(root, "last_jpeg_size", st.last_jpeg_size);
    cJSON_AddNumberToObject(root, "last_frame_ms", st.last_frame_ms);
    cJSON_AddNumberToObject(root, "fps", st.fps_x100 / 100.0);
    cJSON_AddStringToObject(root, "last_error", st.last_error);
}

static esp_err_t video_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    add_video_json(root);
    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t video_quality_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
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
        int len = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
        if (len < 0) {
            return ESP_FAIL;
        }
        cJSON *root = cJSON_Parse(buf);
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
    si_web_log("INFO", "Video JPEG quality updated");
    return video_status_handler(req);
}

static esp_err_t snapshot_handler(httpd_req_t *req)
{
    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    esp_err_t ret = si_video_acquire_jpeg(&jpeg, &jpeg_len, NULL);
    if (ret != ESP_OK) {
        return send_text_status(req, "503 Service Unavailable", esp_err_to_name(ret));
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send(req, (const char *)jpeg, jpeg_len);
    free(jpeg);
    return ret;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    esp_err_t ret = si_video_acquire_jpeg(&jpeg, &jpeg_len, NULL);
    if (ret != ESP_OK) {
        return send_text_status(req, "503 Service Unavailable", esp_err_to_name(ret));
    }

    char header[128];
    int header_len = snprintf(header, sizeof(header),
                              "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                              jpeg_len);
    if (httpd_resp_send_chunk(req, header, header_len) != ESP_OK ||
        httpd_resp_send_chunk(req, (const char *)jpeg, jpeg_len) != ESP_OK ||
        httpd_resp_send_chunk(req, "\r\n--frame--\r\n", 13) != ESP_OK) {
        free(jpeg);
        return ESP_OK;
    }
    free(jpeg);
    return httpd_resp_send_chunk(req, NULL, 0);
}

int si_web_ws_client_count(void)
{
    if (!s_server) {
        return 0;
    }
    size_t clients = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(s_server, &clients, fds) != ESP_OK) {
        return 0;
    }
    int count = 0;
    for (size_t i = 0; i < clients; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            count++;
        }
    }
    return count;
}

static esp_err_t overall_status_handler(httpd_req_t *req)
{
    si_net_status_t net;
    si_net_get_status(&net);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "server_uptime", uptime_sec());
    cJSON *video = cJSON_AddObjectToObject(root, "video");
    add_video_json(video);

    cJSON *hid = cJSON_AddObjectToObject(root, "hid");
    cJSON_AddStringToObject(hid, "mode", "native_usb_tinyusb");
    cJSON_AddStringToObject(hid, "port", "USB-OTG");
    cJSON_AddBoolToObject(hid, "connected", si_hid_is_mounted());
    cJSON *keyboard = cJSON_AddObjectToObject(hid, "keyboard");
    cJSON_AddBoolToObject(keyboard, "available", si_hid_is_ready());
    cJSON *mouse = cJSON_AddObjectToObject(hid, "mouse");
    cJSON_AddBoolToObject(mouse, "available", si_hid_is_ready());

    cJSON *bridge = cJSON_AddObjectToObject(root, "control_bridge");
    cJSON_AddStringToObject(bridge, "source", "esp32p4_native_usb_hid");
    cJSON_AddBoolToObject(bridge, "connected", si_hid_is_mounted());
    cJSON_AddStringToObject(bridge, "port", "USB-OTG");
    cJSON *caps = cJSON_AddObjectToObject(bridge, "capabilities");
    cJSON_AddBoolToObject(caps, "keyboard", true);
    cJSON_AddBoolToObject(caps, "mouse", true);
    cJSON_AddBoolToObject(caps, "websocket_hid", true);

    cJSON *network = cJSON_AddObjectToObject(root, "network");
    cJSON_AddBoolToObject(network, "configured", net.configured);
    cJSON_AddBoolToObject(network, "connected", net.connected);
    cJSON_AddBoolToObject(network, "link_up", net.link_up);
    cJSON_AddStringToObject(network, "interface", net.interface);
    cJSON_AddStringToObject(network, "driver", net.driver);
    cJSON_AddStringToObject(network, "ip", net.ip);
    cJSON_AddStringToObject(network, "netmask", net.netmask);
    cJSON_AddStringToObject(network, "gateway", net.gateway);
    cJSON_AddStringToObject(network, "mac", net.mac);

    cJSON_AddNumberToObject(root, "active_connections", si_web_ws_client_count());
    cJSON_AddBoolToObject(root, "authEnabled", si_auth_enabled());

    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t hid_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        si_web_log("INFO", "KVM HID WebSocket connected");
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    if (frame.len == 0 || frame.len > 512) {
        return ESP_OK;
    }
    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK && frame.type == HTTPD_WS_TYPE_TEXT) {
        cJSON *root = cJSON_Parse((const char *)buf);
        if (root) {
            si_hid_handle_json(root);
            cJSON_Delete(root);
        }
    }
    free(buf);
    return ret;
}

static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"error\":\"not found\"}");
}

static void register_uri(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *), bool websocket)
{
    httpd_uri_t cfg = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = NULL,
        .is_websocket = websocket,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &cfg));
}

esp_err_t si_web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SI_DEFAULT_HTTP_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 24;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start");

    register_uri(s_server, "/", HTTP_GET, index_handler, false);
    register_uri(s_server, "/kvm", HTTP_GET, kvm_handler, false);
    register_uri(s_server, "/api/auth/login", HTTP_POST, auth_login_handler, false);
    register_uri(s_server, "/api/status", HTTP_GET, overall_status_handler, false);
    register_uri(s_server, "/api/system/info", HTTP_GET, system_info_handler, false);
    register_uri(s_server, "/api/system/logs", HTTP_GET, logs_handler, false);
    register_uri(s_server, "/api/hid/status", HTTP_GET, hid_status_handler, false);
    register_uri(s_server, "/api/ws/hid", HTTP_GET, hid_ws_handler, true);
    register_uri(s_server, "/api/control-bridge/status", HTTP_GET, control_bridge_status_handler, false);
    register_uri(s_server, "/api/control-bridge/config", HTTP_GET, control_bridge_config_handler, false);
    register_uri(s_server, "/api/video/status", HTTP_GET, video_status_handler, false);
    register_uri(s_server, "/api/video/quality", HTTP_POST, video_quality_handler, false);
    register_uri(s_server, "/api/stream", HTTP_GET, stream_handler, false);
    register_uri(s_server, "/api/snapshot", HTTP_GET, snapshot_handler, false);

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler);
    si_web_log("INFO", "HTTP/WebSocket server started");
    ESP_LOGI(TAG, "HTTP server started on port %d", SI_DEFAULT_HTTP_PORT);
    return ESP_OK;
}
