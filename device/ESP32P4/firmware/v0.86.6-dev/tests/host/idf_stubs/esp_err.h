#pragma once

#include <stdint.h>

typedef int32_t esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_NO_MEM 0x107
#define ESP_ERR_TIMEOUT 0x108
#define ESP_ERR_NOT_FINISHED 0x109
#define ESP_ERR_NOT_ALLOWED 0x10a
#define ESP_ERR_INVALID_RESPONSE 0x10b

#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c

const char *esp_err_to_name(esp_err_t err);
