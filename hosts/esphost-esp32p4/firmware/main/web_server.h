#pragma once

#include "esp_err.h"

esp_err_t si_web_server_start(void);
void si_web_log(const char *level, const char *message);
int si_web_ws_client_count(void);

