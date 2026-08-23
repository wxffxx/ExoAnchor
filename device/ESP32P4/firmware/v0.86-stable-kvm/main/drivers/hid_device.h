#pragma once

// USB HID device public API.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SI_HID_ABS_MAX 32767
#define SI_HID_COMBO_MODIFIER_MAX 8
#define SI_HID_COMBO_KEY_MAX 6

typedef enum {
    SI_HID_COMMAND_INVALID = 0,
    SI_HID_COMMAND_UNSUPPORTED,
    SI_HID_COMMAND_KEY_DOWN,
    SI_HID_COMMAND_KEY_UP,
    SI_HID_COMMAND_MOUSE_MOVE,
    SI_HID_COMMAND_MOUSE_MOVE_COUNTS,
    SI_HID_COMMAND_ABS_MOVE,
    SI_HID_COMMAND_ABS_MOUSE_DOWN,
    SI_HID_COMMAND_ABS_MOUSE_UP,
    SI_HID_COMMAND_ABS_CLICK,
    SI_HID_COMMAND_MOUSE_DOWN,
    SI_HID_COMMAND_MOUSE_UP,
    SI_HID_COMMAND_CLICK,
    SI_HID_COMMAND_WHEEL,
    SI_HID_COMMAND_COMBO,
    SI_HID_COMMAND_RELEASE_ALL,
} si_hid_command_type_t;

typedef struct {
    si_hid_command_type_t type;
    const char *code;
    const char *error;
    float dx;
    float dy;
    int32_t x;
    int32_t y;
    bool has_x;
    bool has_y;
    uint8_t button;
    int8_t wheel_y;
    int8_t wheel_x;
    const char *modifiers[SI_HID_COMBO_MODIFIER_MAX];
    size_t modifier_count;
    const char *keys[SI_HID_COMBO_KEY_MAX];
    size_t key_count;
} si_hid_command_t;

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

esp_err_t si_hid_execute(const si_hid_command_t *command);
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
