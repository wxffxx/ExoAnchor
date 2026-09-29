#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef uintptr_t nvs_handle_t;

typedef enum {
    NVS_READONLY,
    NVS_READWRITE,
} nvs_open_mode_t;

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t mode,
                   nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out,
                      size_t *length);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out);
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out);
esp_err_t nvs_get_i32(nvs_handle_t handle, const char *key, int32_t *out);

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
esp_err_t nvs_set_i32(nvs_handle_t handle, const char *key, int32_t value);

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
