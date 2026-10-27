#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool valid;
    uint32_t stream_id;
    uint32_t fps_x100;
    uint32_t bitrate_bps;
    uint32_t sample_window_ms;
    uint32_t updated_ms;
    /* UVC delivery -> HTTP send complete per MJPEG frame (0 = unknown). */
    uint32_t latency_avg_us;
    uint32_t latency_max_us;
} si_video_stream_metrics_t;

void si_video_stream_metrics_begin(uint32_t stream_id, uint32_t now_ms);
void si_video_stream_metrics_record(uint32_t stream_id, size_t frame_bytes,
                                    uint32_t now_ms, uint32_t latency_us);
void si_video_stream_metrics_get(si_video_stream_metrics_t *out,
                                 uint32_t now_ms);
