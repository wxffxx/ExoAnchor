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
    /* A cold start keeps both rails off for tens of seconds so a heat-soaked
     * MS2109 really restarts; the short cycle (<= 5 s) is not enough. */
    bool cold_start_active;
    uint32_t cold_start_off_ms;
    uint32_t cold_start_remaining_ms;
} si_ms2109_power_status_t;

#define SI_MS2109_COLD_START_MIN_MS 30000U
#define SI_MS2109_COLD_START_MAX_MS 60000U
#define SI_MS2109_COLD_START_DEFAULT_MS 45000U

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

/* Power the MS2109 off now and back on after off_time_ms (30..60 s) without
 * blocking the caller.  A later si_ms2109_power_set()/_cycle() cancels the
 * pending power-on and applies the requested state instead. */
esp_err_t si_ms2109_power_cold_start(uint32_t off_time_ms);
