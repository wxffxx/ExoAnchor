#pragma once
#include "esp_http_server.h"
esp_err_t si_tls_server_start(httpd_handle_t *server, const httpd_config_t *config);
