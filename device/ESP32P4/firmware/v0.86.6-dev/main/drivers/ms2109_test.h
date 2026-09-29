#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SI_MS2109_AT24C16_SIZE 2048U
#define SI_MS2109_AT24C16_PAGE_SIZE 16U

typedef enum {
    /* Enable the switched 3.3 V rail, wait, then enable the 1.2 V buck. */
    SI_MS2109_POWER_SEQUENCE_3V3_FIRST = 0,
    /* Pre-enable the 1.2 V buck while its switched 3.3 V input is off. */
    SI_MS2109_POWER_SEQUENCE_CORE_PRE_ENABLE = 1,
} si_ms2109_power_sequence_t;

typedef struct {
    bool enabled;
    bool initialized;
    bool power_on;
    bool write_protected;
    int switch_gpio;
    int core_enable_gpio;
    int switch_output_level;
    int core_enable_output_level;
    int wp_gpio;
    int scl_gpio;
    int sda_gpio;
    int last_scl_level;
    int last_sda_level;
    uint32_t operation_count;
    si_ms2109_power_sequence_t last_power_sequence;
    uint32_t last_sequence_delay_ms;
    size_t last_programmed_bytes;
    uint32_t last_crc32;
    bool last_verified;
    esp_err_t last_result;
} si_ms2109_test_status_t;

/*
 * Initialize the V2.4-only MS2109 validation controls. The safe boot state is
 * EEPROM write-protected and both MS2109 rails enabled in hardware-safe order.
 */
esp_err_t si_ms2109_test_init(void);

void si_ms2109_test_get_status(si_ms2109_test_status_t *status);

esp_err_t si_ms2109_test_set_power(bool on);
esp_err_t si_ms2109_test_cycle_power(uint32_t off_time_ms);
esp_err_t si_ms2109_test_cycle_power_sequence(
    si_ms2109_power_sequence_t sequence, uint32_t sequence_delay_ms,
    uint32_t off_time_ms);

/* Probe all eight AT24C16 block addresses. Bit 0 represents 0x50. */
esp_err_t si_ms2109_test_probe_eeprom(uint8_t *address_mask);

/*
 * Read a physical AT24C16 range. V2.4a6 keeps MS2109 powered so its switched
 * I2C pull-ups remain valid, waits for the boot-time EEPROM read to finish,
 * then restores the previous power state after the P4 transaction.
 */
esp_err_t si_ms2109_test_read_eeprom(size_t offset, uint8_t *data, size_t size);

/*
 * Program and byte-for-byte verify one complete 2 KiB AT24C16 image. WP is
 * restored and the I2C pins are released on every exit path.
 */
esp_err_t si_ms2109_test_program_eeprom(const uint8_t *image, size_t size,
                                        uint32_t *crc32, bool *verified);
