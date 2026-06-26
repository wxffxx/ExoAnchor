#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

typedef struct {
    bool enabled;
    bool initialized;
    bool mounted;
    bool ready;
    int dm_gpio;
    int dp_gpio;
    uint32_t tx_messages;
    uint32_t failed_messages;
    char mode[32];
    char port[32];
    char transport[24];
    char last_error[96];
} si_hid_status_t;

esp_err_t si_hid_init(void);
bool si_hid_is_ready(void);
bool si_hid_is_mounted(void);
void si_hid_get_status(si_hid_status_t *status);

esp_err_t si_hid_handle_json(const cJSON *msg);
void si_hid_key_down(const char *code);
void si_hid_key_up(const char *code);
void si_hid_release_all(void);
void si_hid_mouse_move(float dx, float dy);
void si_hid_mouse_down(uint8_t button);
void si_hid_mouse_up(uint8_t button);
void si_hid_mouse_scroll(int8_t delta_y, int8_t delta_x);
void si_hid_abs_move(uint16_t x, uint16_t y);
void si_hid_abs_mouse_down(uint8_t button);
void si_hid_abs_mouse_up(uint8_t button);
