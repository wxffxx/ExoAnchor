#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t si_boot_key_http_handler(httpd_req_t *req);
