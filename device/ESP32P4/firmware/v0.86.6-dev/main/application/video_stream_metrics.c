#include "video_stream_metrics.h"

#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#define SI_STREAM_METRICS_WINDOW_MS 1000U
#define SI_STREAM_METRICS_STALE_MS 2500U

static portMUX_TYPE s_metrics_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_stream_id;
static uint32_t s_window_started_ms;
static uint32_t s_window_frames;
static uint64_t s_window_bytes;
static si_video_stream_metrics_t s_published;

void si_video_stream_metrics_begin(uint32_t stream_id, uint32_t now_ms)
{
    portENTER_CRITICAL(&s_metrics_mux);
    s_stream_id = stream_id;
    s_window_started_ms = now_ms;
    s_window_frames = 0;
    s_window_bytes = 0;
    memset(&s_published, 0, sizeof(s_published));
    s_published.stream_id = stream_id;
    portEXIT_CRITICAL(&s_metrics_mux);
}

void si_video_stream_metrics_record(uint32_t stream_id, size_t frame_bytes,
                                    uint32_t now_ms)
{
    portENTER_CRITICAL(&s_metrics_mux);
    if (stream_id == 0 || stream_id != s_stream_id) {
        portEXIT_CRITICAL(&s_metrics_mux);
        return;
    }
    s_window_frames++;
    s_window_bytes += frame_bytes;
    uint32_t elapsed_ms = now_ms - s_window_started_ms;
    if (elapsed_ms >= SI_STREAM_METRICS_WINDOW_MS) {
        uint64_t fps_x100 =
            (uint64_t)s_window_frames * 100000ULL / elapsed_ms;
        uint64_t bitrate_bps =
            s_window_bytes * 8ULL * 1000ULL / elapsed_ms;
        s_published.valid = true;
        s_published.stream_id = stream_id;
        s_published.fps_x100 =
            fps_x100 > UINT32_MAX ? UINT32_MAX : (uint32_t)fps_x100;
        s_published.bitrate_bps =
            bitrate_bps > UINT32_MAX ? UINT32_MAX : (uint32_t)bitrate_bps;
        s_published.sample_window_ms = elapsed_ms;
        s_published.updated_ms = now_ms;
        s_window_started_ms = now_ms;
        s_window_frames = 0;
        s_window_bytes = 0;
    }
    portEXIT_CRITICAL(&s_metrics_mux);
}

void si_video_stream_metrics_get(si_video_stream_metrics_t *out,
                                 uint32_t now_ms)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_metrics_mux);
    *out = s_published;
    if (!out->valid || out->stream_id != s_stream_id ||
        (uint32_t)(now_ms - out->updated_ms) > SI_STREAM_METRICS_STALE_MS) {
        out->valid = false;
        out->fps_x100 = 0;
        out->bitrate_bps = 0;
        out->sample_window_ms = 0;
    }
    portEXIT_CRITICAL(&s_metrics_mux);
}
