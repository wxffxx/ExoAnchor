#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t si_http_send_json(httpd_req_t *req, cJSON *root);
esp_err_t si_http_send_text_status(httpd_req_t *req, const char *status,
                                   const char *message);

esp_err_t si_http_recv_body(httpd_req_t *req, char *buf, size_t buf_size);
esp_err_t si_http_recv_json(httpd_req_t *req, char *buf, size_t buf_size,
                            cJSON **out_root);

bool si_http_check_auth(httpd_req_t *req);
esp_err_t si_http_require_auth(httpd_req_t *req);
