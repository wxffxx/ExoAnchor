#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

esp_err_t si_hid_init(void);
bool si_hid_is_ready(void);
bool si_hid_is_mounted(void);

esp_err_t si_hid_handle_json(const cJSON *msg);
void si_hid_key_down(const char *code);
void si_hid_key_up(const char *code);
void si_hid_release_all(void);
void si_hid_mouse_move(float dx, float dy);
void si_hid_mouse_down(uint8_t button);
void si_hid_mouse_up(uint8_t button);
void si_hid_mouse_scroll(int8_t delta_y, int8_t delta_x);

