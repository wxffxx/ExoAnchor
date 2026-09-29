#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SI_DEVICE_LABEL_DEFAULT "ESP32-P4"
#define SI_DEVICE_LABEL_MAX_LEN 48
#define SI_SESSION_LOGOUT_DEFAULT_ENABLED true
#define SI_SESSION_LOGOUT_DEFAULT_MINUTES 15U
#define SI_SESSION_LOGOUT_MIN_MINUTES 1U
#define SI_SESSION_LOGOUT_MAX_MINUTES 1440U

typedef struct {
    bool auto_logout_enabled;
    uint32_t auto_logout_minutes;
} si_session_settings_t;

void si_device_label_get(char *label, size_t label_size);
esp_err_t si_device_label_set(const char *label);

void si_session_settings_get(si_session_settings_t *settings);
esp_err_t si_session_settings_set(const si_session_settings_t *settings);
