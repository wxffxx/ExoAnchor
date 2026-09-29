#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const uint8_t *data;
    size_t len;
    uint32_t frame_id;
    void *token;
} si_video_frame_store_view_t;

/* Legacy workspace shape retained for source compatibility.  The packed
 * maximum-frame arena cannot expose a second full-capacity scratch region;
 * both scratch functions return ESP_ERR_NOT_SUPPORTED. */
typedef struct {
    uint8_t *data;
    size_t capacity;
    uint32_t token;
} si_video_frame_store_scratch_t;

esp_err_t si_video_frame_store_init(size_t capacity);
void si_video_frame_store_clear(void);
esp_err_t si_video_frame_store_publish(const uint8_t *data, size_t len,
                                       uint32_t *frame_id);
esp_err_t si_video_frame_store_acquire(uint32_t last_frame_id,
                                       si_video_frame_store_view_t *view);
void si_video_frame_store_release(si_video_frame_store_view_t *view);
esp_err_t si_video_frame_store_copy(uint32_t last_frame_id, bool require_new,
                                    uint8_t *buffer, size_t capacity,
                                    size_t *out_len, uint32_t *out_frame_id);
esp_err_t si_video_frame_store_scratch_acquire(
    uint32_t timeout_ms, si_video_frame_store_scratch_t *scratch);
esp_err_t si_video_frame_store_scratch_release(
    si_video_frame_store_scratch_t *scratch);
size_t si_video_frame_store_capacity(void);
