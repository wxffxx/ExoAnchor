#pragma once

// USB UVC video input public API.

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
    uint32_t frame_interval_ms;
    uint32_t last_frame_ms;
    uint32_t fps_x100;
    uint32_t uvc_callbacks;
    uint32_t uvc_queue_drops;
    uint32_t h264_pressure_coalesces;
    uint32_t uvc_buffer_underflows;
    uint32_t uvc_buffer_overflows;
    uint32_t uvc_return_pending;
    uint32_t uvc_return_retries;
    uint32_t uvc_return_failures;
    uint32_t ingest_validate_last_us;
    uint32_t ingest_validate_max_us;
    uint32_t ingest_publish_last_us;
    uint32_t ingest_publish_max_us;
    uint32_t ingest_total_last_us;
    uint32_t ingest_total_max_us;
    char source[24];
    char pixel_format[16];
    char capture_owner[16];
    char last_error[96];
} si_video_status_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    uint32_t frame_id;
    void *token;
} si_video_jpeg_view_t;

/* Reserve the persistent PSRAM frame arena and the 64 KiB internal-DMA UVC
 * budget without creating queues, USB drivers, or tasks.  H.264 candidates
 * use this boot barrier before reserving their latency-critical SRAM. */
esp_err_t si_video_reserve_boot_memory(void);
bool si_video_boot_memory_ready(void);
esp_err_t si_video_init(void);
void si_video_get_status(si_video_status_t *status);
esp_err_t si_video_get_modes(si_video_mode_t *modes, size_t max_modes, size_t *out_count);
esp_err_t si_video_set_capture(bool enabled, const char *owner, uint32_t width, uint32_t height,
                               uint32_t fps_x100, bool exact_fps, uint32_t stride_ms);
esp_err_t si_video_set_mode(uint32_t width, uint32_t height, uint32_t fps_x100);
esp_err_t si_video_set_resolution(uint32_t width, uint32_t height);
esp_err_t si_video_validate_mode(uint32_t width, uint32_t height,
                                 uint32_t fps_x100, bool exact_fps);
esp_err_t si_video_set_quality(uint32_t quality);
esp_err_t si_video_request_jpeg_snapshot(void);
esp_err_t si_video_copy_jpeg(uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t *out_frame_id);
esp_err_t si_video_copy_jpeg_if_new(uint32_t last_frame_id, uint8_t *buf, size_t buf_cap,
                                    size_t *out_len, uint32_t *out_frame_id);
esp_err_t si_video_acquire_jpeg(uint8_t **out_buf, size_t *out_len, uint32_t *out_frame_id);
esp_err_t si_video_borrow_jpeg_if_new(uint32_t last_frame_id, si_video_jpeg_view_t *view);
void si_video_release_jpeg(si_video_jpeg_view_t *view);
/* H.264 owns a separate capacity-one UVC lease path.  Unlike the ordinary
 * MJPEG view above, this view points directly at the UVC host frame and must
 * always be paired with si_video_release_h264_jpeg(). */
/* Wait for the direct H.264 input queue to become non-empty without claiming
 * frame ownership.  Readiness is only a hint: a session flush may revoke the
 * observed frame, so callers must still use the borrow API and handle its
 * epoch/stream validation result. */
esp_err_t si_video_wait_h264_jpeg_ready(uint32_t timeout_ms);
esp_err_t si_video_borrow_h264_jpeg_if_new(uint32_t last_frame_id,
                                           si_video_jpeg_view_t *view);
esp_err_t si_video_release_h264_jpeg(si_video_jpeg_view_t *view);
esp_err_t si_video_flush_h264_jpeg(void);
esp_err_t si_video_begin_mjpeg_send(uint32_t timeout_ms);
void si_video_end_mjpeg_send(void);
esp_err_t si_video_set_h264_transport_active(bool active);
size_t si_video_jpeg_capacity(void);
