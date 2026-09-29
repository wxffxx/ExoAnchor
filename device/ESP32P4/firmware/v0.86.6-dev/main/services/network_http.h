#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t si_network_status_http_handler(httpd_req_t *req);
esp_err_t si_network_config_http_handler(httpd_req_t *req);
