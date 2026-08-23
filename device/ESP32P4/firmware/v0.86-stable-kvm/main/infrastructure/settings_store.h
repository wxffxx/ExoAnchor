#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Thin transaction facade over the default NVS partition.
 *
 * Namespace/key ownership, defaults, and validation stay in the calling
 * application service. A store is owned by one caller and must not be shared
 * concurrently. Missing values are reported as ESP_ERR_NOT_FOUND, writes are
 * not durable until si_settings_store_commit() succeeds, and close never
 * commits implicitly.
 */
typedef struct {
    uintptr_t handle;
    bool open;
    bool writable;
} si_settings_store_t;

#define SI_SETTINGS_STORE_INITIALIZER {0}

esp_err_t si_settings_store_open_read(si_settings_store_t *store,
                                      const char *namespace_name);
esp_err_t si_settings_store_open_write(si_settings_store_t *store,
                                       const char *namespace_name);
void si_settings_store_close(si_settings_store_t *store);

esp_err_t si_settings_store_get_string(si_settings_store_t *store,
                                       const char *key, char *out,
                                       size_t out_size);
esp_err_t si_settings_store_get_string_size(si_settings_store_t *store,
                                            const char *key,
                                            size_t *size_out);
esp_err_t si_settings_store_get_u8(si_settings_store_t *store,
                                   const char *key, uint8_t *out);
esp_err_t si_settings_store_get_u16(si_settings_store_t *store,
                                    const char *key, uint16_t *out);
esp_err_t si_settings_store_get_u32(si_settings_store_t *store,
                                    const char *key, uint32_t *out);
esp_err_t si_settings_store_get_i32(si_settings_store_t *store,
                                    const char *key, int32_t *out);

esp_err_t si_settings_store_set_string(si_settings_store_t *store,
                                       const char *key, const char *value);
esp_err_t si_settings_store_set_u8(si_settings_store_t *store,
                                   const char *key, uint8_t value);
esp_err_t si_settings_store_set_u16(si_settings_store_t *store,
                                    const char *key, uint16_t value);
esp_err_t si_settings_store_set_u32(si_settings_store_t *store,
                                    const char *key, uint32_t value);
esp_err_t si_settings_store_set_i32(si_settings_store_t *store,
                                    const char *key, int32_t value);
esp_err_t si_settings_store_erase_key(si_settings_store_t *store,
                                      const char *key);
esp_err_t si_settings_store_erase_all(si_settings_store_t *store);
esp_err_t si_settings_store_commit(si_settings_store_t *store);
