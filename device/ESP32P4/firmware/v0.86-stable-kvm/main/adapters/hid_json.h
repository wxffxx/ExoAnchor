#pragma once

/* JSON adapter for the transport-neutral HID command API. */

#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

esp_err_t si_hid_json_execute(const cJSON *message);
void si_hid_json_add_action_result(cJSON *results, int index, esp_err_t err,
                                   const char *message);
uint32_t si_hid_json_action_wait_ms(const cJSON *action);
