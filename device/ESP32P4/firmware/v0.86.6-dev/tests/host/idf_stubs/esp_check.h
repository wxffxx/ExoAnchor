#pragma once

#include "esp_err.h"

#define ESP_RETURN_ON_ERROR(expression, tag, format, ...)                     \
    do {                                                                      \
        (void)(tag);                                                          \
        esp_err_t esp_check_result_ = (expression);                           \
        if (esp_check_result_ != ESP_OK) {                                    \
            return esp_check_result_;                                        \
        }                                                                     \
    } while (0)
