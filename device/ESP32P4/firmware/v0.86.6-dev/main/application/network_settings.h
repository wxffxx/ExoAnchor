#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "network_config.h"

typedef struct {
    si_network_config_t active;
    si_network_config_t staged;
    uint8_t active_slot;
    uint8_t staged_slot;
    uint8_t pending_slot;
    bool have_staged;
    bool pending;
    bool recovered_pending;
} si_network_settings_status_t;

esp_err_t si_network_settings_initialize(
    const si_network_config_t *factory_defaults,
    si_network_settings_status_t *status_out);
/*
 * Returns the runtime snapshot published by initialize/persistence operations.
 * It never opens NVS, so it is safe for callers whose task stack is in PSRAM.
 */
esp_err_t si_network_settings_get(si_network_settings_status_t *status_out);
/* Persistence operations fail closed with ESP_ERR_INVALID_STATE on PSRAM stacks. */
esp_err_t si_network_settings_stage(const si_network_config_t *config);
esp_err_t si_network_settings_mark_pending(void);
esp_err_t si_network_settings_confirm(void);
esp_err_t si_network_settings_rollback(void);
esp_err_t si_network_settings_reset(
    const si_network_config_t *factory_defaults);
