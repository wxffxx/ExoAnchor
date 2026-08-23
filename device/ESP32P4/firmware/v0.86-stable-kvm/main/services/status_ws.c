// Overall status HTTP and HID WebSocket adapters.
#include "status_ws.h"

#include <stdint.h>
#include <stdlib.h>

#include "auth_service.h"
#include "cJSON.h"
#include "device_http.h"
#include "device_observation_service.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "hid_json.h"
#include "http_api.h"
#include "web_server.h"

#define HID_WS_MAX_FRAME 1024

static const char *TAG = "si-status-ws";

esp_err_t overall_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_device_network_observation_t network_status;
    si_device_video_observation_t video_status;
    si_device_video_control_observation_t video_control;
    si_device_video_mode_observation_t *video_modes =
        calloc(SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES, sizeof(*video_modes));
    size_t video_mode_count = 0;
    si_device_hid_observation_t hid_status;
    si_device_control_lease_observation_t control_lease_status;
    si_device_power_observation_t power_status;
    si_device_performance_observation_t performance_status;
    si_device_observation_get_network(&network_status);
    si_device_observation_get_video(&video_status, &video_control, video_modes,
                                    SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES,
                                    &video_mode_count);
    si_device_observation_get_hid(&hid_status);
    si_device_observation_get_control_lease(&control_lease_status);
    si_device_observation_get_power(&power_status);
    si_device_observation_get_performance(&performance_status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "server_uptime",
                            si_device_observation_uptime_seconds());
    cJSON *video = cJSON_AddObjectToObject(root, "video");
    si_device_http_add_video_json(video, &video_status, &video_control,
                                  video_modes, video_mode_count);
    free(video_modes);
    cJSON *hid = cJSON_AddObjectToObject(root, "hid");
    si_device_http_add_hid_json(hid, &hid_status);
    cJSON *control_lease = cJSON_AddObjectToObject(root, "control_lease");
    si_device_http_add_control_lease_json(control_lease, &control_lease_status);
    cJSON *power = cJSON_AddObjectToObject(root, "power");
    si_device_http_add_power_json(power, &power_status);

    cJSON *network = cJSON_AddObjectToObject(root, "network");
    si_device_http_add_network_json(network, &network_status);

    cJSON_AddNumberToObject(root, "active_connections", si_web_client_count());
    cJSON_AddBoolToObject(root, "authEnabled", si_auth_is_enabled());
    cJSON *performance = cJSON_AddObjectToObject(root, "performance");
    si_device_http_add_performance_json(performance, &performance_status,
                                        si_web_client_count());

    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

#ifdef CONFIG_HTTPD_WS_SUPPORT
esp_err_t hid_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (!si_http_check_auth(req)) {
            return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
        }
        si_web_log("INFO", "KVM HID WebSocket connected");
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }
    if (frame.len == 0) {
        return ESP_OK;
    }
    if (frame.len > HID_WS_MAX_FRAME) {
        ESP_LOGW(TAG, "HID WebSocket frame too large: %u", (unsigned)frame.len);
        return ESP_ERR_INVALID_SIZE;
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
            esp_err_t hid_ret = si_hid_json_execute(root);
            if (hid_ret != ESP_OK && hid_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "HID command failed: %s", esp_err_to_name(hid_ret));
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "Invalid HID WebSocket JSON");
        }
    }
    free(buf);
    return ret;
}
#endif
