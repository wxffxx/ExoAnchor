#pragma once

#include <stddef.h>

#include "cJSON.h"
#include "device_observation_service.h"
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t system_info_handler(httpd_req_t *req);
esp_err_t logs_handler(httpd_req_t *req);
esp_err_t logs_download_handler(httpd_req_t *req);
esp_err_t video_status_handler(httpd_req_t *req);
esp_err_t hid_status_handler(httpd_req_t *req);
esp_err_t capabilities_handler(httpd_req_t *req);
esp_err_t control_lease_handler(httpd_req_t *req);
esp_err_t hid_actions_handler(httpd_req_t *req);

void si_device_http_add_video_json(cJSON *root,
                                   const si_device_video_observation_t *video,
                                   const si_device_video_control_observation_t *control,
                                   const si_device_video_mode_observation_t *modes,
                                   size_t mode_count);
void si_device_http_add_hid_json(cJSON *root,
                                 const si_device_hid_observation_t *hid);
void si_device_http_add_power_json(cJSON *root,
                                   const si_device_power_observation_t *power);
void si_device_http_add_ms2109_power_json(
    cJSON *root, const si_device_ms2109_power_observation_t *power);
void si_device_http_add_network_json(cJSON *root,
                                     const si_device_network_observation_t *network);
void si_device_http_add_control_lease_json(
    cJSON *root, const si_device_control_lease_observation_t *lease);
void si_device_http_add_performance_json(
    cJSON *root, const si_device_performance_observation_t *performance,
    int active_connections);
