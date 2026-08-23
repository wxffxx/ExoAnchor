#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef void (*si_target_uart_rx_callback_t)(const uint8_t *data, size_t len,
                                             void *user_ctx);

typedef struct {
    bool supported;
    bool initialized;
    int port;
    int rx_gpio;
    int tx_gpio;
    int baud_rate;
    int default_baud_rate;
    int fallback_baud_rate;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t buffered_bytes;
    uint64_t dropped_bytes;
    uint64_t boot_id;
    uint64_t generation;
    uint64_t journal_start_seq;
    uint64_t journal_next_seq;
    uint64_t journal_bytes;
    bool journal_history_lost;
    char last_error[96];
} si_target_uart_status_t;

typedef struct {
    uint64_t boot_id;
    uint64_t generation;
    uint64_t start_seq;
    uint64_t next_seq;
    size_t bytes;
    bool history_lost;
} si_target_uart_journal_t;

esp_err_t si_target_uart_init(void);
esp_err_t si_target_uart_set_baud_rate(int baud_rate);
void si_target_uart_get_status(si_target_uart_status_t *status);
size_t si_target_uart_journal_read(uint8_t *data, size_t len,
                                   si_target_uart_journal_t *journal);
size_t si_target_uart_journal_read_from(uint64_t cursor, uint8_t *data,
                                        size_t len,
                                        si_target_uart_journal_t *journal);
void si_target_uart_set_rx_callback(si_target_uart_rx_callback_t callback,
                                    void *user_ctx);
size_t si_target_uart_read(uint8_t *data, size_t len, TickType_t wait_ticks);
size_t si_target_uart_read_cli(uint8_t *data, size_t len,
                               TickType_t wait_ticks);
esp_err_t si_target_uart_write(const uint8_t *data, size_t len,
                               size_t *written);
