#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SI_SETTINGS_SCHEMA_CURRENT_VERSION 1U

typedef struct {
    uint32_t version;
    uint32_t last_good_version;
    uint32_t pending_version;
    bool initialized;
    bool migration_resumed;
    bool recovery_required;
} si_settings_schema_status_t;

/*
 * Establish the version contract for all settings namespaces.
 *
 * Existing pre-schema images are adopted as version 1 without rewriting their
 * keys. Future migrations must be added as idempotent steps. A pending marker
 * is committed before any migration and cleared only with the final version
 * commit, so loss of power resumes the same step on the next boot. Firmware
 * older than the stored or pending schema fails closed instead of interpreting
 * newer data. Factory recovery remains an explicit erase action.
 */
esp_err_t si_settings_schema_initialize(
    si_settings_schema_status_t *status_out);
