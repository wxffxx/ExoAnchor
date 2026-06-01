#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool enabled;
    bool initialized;
    bool streaming;
    bool frame_ready;
    uint32_t width;
    uint32_t height;
    uint32_t data_lanes;
    uint32_t lane_bitrate_mbps;
    uint32_t jpeg_quality;
    uint32_t frames_captured;
    uint32_t frames_encoded;
    uint32_t frames_dropped;
    uint32_t last_jpeg_size;
    uint32_t last_frame_ms;
    uint32_t fps_x100;
    char source[24];
    char pixel_format[16];
    char last_error[96];
} si_video_status_t;

esp_err_t si_video_init(void);
void si_video_get_status(si_video_status_t *status);
esp_err_t si_video_set_quality(uint32_t quality);
esp_err_t si_video_acquire_jpeg(uint8_t **out_buf, size_t *out_len, uint32_t *out_frame_id);

