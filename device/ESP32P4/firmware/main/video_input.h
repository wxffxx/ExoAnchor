#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SI_VIDEO_MAX_MODES 128

typedef struct {
    bool selected;
    uint32_t width;
    uint32_t height;
    uint32_t fps_x100;
    char pixel_format[16];
} si_video_mode_t;

typedef struct {
    bool enabled;
    bool initialized;
    bool streaming;
    bool frame_ready;
    bool capture_enabled;
    uint32_t width;
    uint32_t height;
    uint32_t target_width;
    uint32_t target_height;
    uint32_t target_fps_x100;
    uint32_t capture_stride_ms;
    uint32_t modes_count;
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
    char capture_owner[16];
    char last_error[96];
} si_video_status_t;

esp_err_t si_video_init(void);
void si_video_get_status(si_video_status_t *status);
esp_err_t si_video_get_modes(si_video_mode_t *modes, size_t max_modes, size_t *out_count);
esp_err_t si_video_set_capture(bool enabled, const char *owner, uint32_t width, uint32_t height,
                               uint32_t fps_x100, bool exact_fps, uint32_t stride_ms);
esp_err_t si_video_set_mode(uint32_t width, uint32_t height, uint32_t fps_x100);
esp_err_t si_video_set_resolution(uint32_t width, uint32_t height);
esp_err_t si_video_set_quality(uint32_t quality);
esp_err_t si_video_acquire_jpeg(uint8_t **out_buf, size_t *out_len, uint32_t *out_frame_id);
