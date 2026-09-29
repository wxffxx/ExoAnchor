#pragma once

#include <stddef.h>

#include "esp_err.h"

#define SI_TARGET_NAME_MAX_LEN 64
#define SI_TARGET_DEVICE_TYPE_MAX_LEN 24
#define SI_TARGET_OS_MAX_LEN 24
#define SI_TARGET_ENVIRONMENT_MAX_LEN 24
#define SI_TARGET_LOCATION_MAX_LEN 96
#define SI_TARGET_CONFIGURATION_MAX_LEN 256
#define SI_TARGET_PURPOSE_MAX_LEN 256
#define SI_TARGET_NOTES_MAX_LEN 256

typedef struct {
    char name[SI_TARGET_NAME_MAX_LEN + 1];
    char device_type[SI_TARGET_DEVICE_TYPE_MAX_LEN + 1];
    char operating_system[SI_TARGET_OS_MAX_LEN + 1];
    char environment[SI_TARGET_ENVIRONMENT_MAX_LEN + 1];
    char location[SI_TARGET_LOCATION_MAX_LEN + 1];
    char configuration[SI_TARGET_CONFIGURATION_MAX_LEN + 1];
    char purpose[SI_TARGET_PURPOSE_MAX_LEN + 1];
    char notes[SI_TARGET_NOTES_MAX_LEN + 1];
} si_target_profile_settings_t;

void si_target_profile_defaults(si_target_profile_settings_t *settings);
esp_err_t si_target_profile_load(si_target_profile_settings_t *settings);
esp_err_t si_target_profile_save(const si_target_profile_settings_t *settings);
