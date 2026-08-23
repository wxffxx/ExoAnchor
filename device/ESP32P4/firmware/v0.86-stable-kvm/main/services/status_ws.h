#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t overall_status_handler(httpd_req_t *req);

#ifdef CONFIG_HTTPD_WS_SUPPORT
esp_err_t hid_ws_handler(httpd_req_t *req);
#endif
