#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool enabled;
    bool initialized;
    int scl_gpio;
    int sda_gpio;
    int scl_level;
    int sda_level;
    uint8_t pointer;
    uint32_t scl_falling_edges;
    uint32_t sda_falling_edges;
    int h3_gpio2_level;
    int h3_gpio3_level;
    uint32_t h3_gpio2_falling_edges;
    uint32_t h3_gpio3_falling_edges;
    uint32_t receive_count;
    uint32_t request_count;
} si_ms2109_eeprom_emulator_status_t;

/*
 * Start the experimental MS2109 boot EEPROM emulator.
 *
 * The ESP32-P4 I2C peripheral can match one 7-bit slave address, so this
 * prototype presents a 256-byte, 24C02-style device at address 0x50. It must
 * remain disabled when physical EEPROM U5 is populated on PrototypeV2.3b6.
 */
esp_err_t si_ms2109_eeprom_emulator_init(void);

void si_ms2109_eeprom_emulator_get_status(
    si_ms2109_eeprom_emulator_status_t *status);

/*
 * Override one bus line with a safe open-drain low for continuity testing.
 * Reboot the ESP32-P4 after the test to restore the I2C peripheral routing.
 */
esp_err_t si_ms2109_eeprom_emulator_pull_line_low(bool scl);

/*
 * Enable the ESP32-P4's weak pull-downs briefly and sample both bus lines.
 * The board's external 4.7k pull-ups should keep a connected line high.
 */
esp_err_t si_ms2109_eeprom_emulator_test_external_pullups(
    int *scl_level, int *sda_level);
