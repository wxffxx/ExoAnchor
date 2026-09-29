#pragma once

// TF storage public API.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "storage_layout.h"

typedef struct {
    bool supported;
    bool mounted;
    bool degraded_mode;
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    double usage_percent;
    uint32_t sector_size;
    uint32_t cluster_size;
    uint32_t bus_frequency_khz;
    uint8_t bus_width;
    char mount_point[16];
    char card_name[16];
    char card_type[16];
    char bus_mode[24];
    char fallback_reason[96];
    char last_error[96];
} si_storage_tf_status_t;

esp_err_t si_storage_init(void);
void si_storage_get_tf_status(si_storage_tf_status_t *out);
esp_err_t si_storage_ensure_dir(const char *path);
esp_err_t si_storage_ensure_layout(void);
