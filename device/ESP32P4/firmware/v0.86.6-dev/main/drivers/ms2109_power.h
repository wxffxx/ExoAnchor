#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool enabled;
    bool initialized;
    bool power_on;
    int switch_gpio;
    int core_enable_gpio;
    int switch_output_level;
    int core_enable_output_level;
    uint32_t operation_count;
    esp_err_t last_result;
} si_ms2109_power_status_t;

/* Enable the production MS2109 rails in the board-defined safe order. */
esp_err_t si_ms2109_power_init(void);

/* Read the commanded rail state and GPIO output levels. */
esp_err_t si_ms2109_power_get_status(si_ms2109_power_status_t *status);

/*
 * Control both rails as one power domain. On is always 3.3 V then 1.2 V;
 * off is always 1.2 V then 3.3 V.
 */
esp_err_t si_ms2109_power_set(bool on);
esp_err_t si_ms2109_power_cycle(uint32_t off_time_ms);
