#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "video_stream_metrics.h"

int main(void)
{
    si_video_stream_metrics_t value;
    si_video_stream_metrics_begin(1, 100);
    si_video_stream_metrics_get(&value, 100);
    assert(!value.valid);
    for (uint32_t i = 1; i <= 25; i++) {
        si_video_stream_metrics_record(1, 1000, 100 + i * 40);
    }
    si_video_stream_metrics_get(&value, 1100);
    assert(value.valid && value.stream_id == 1);
    assert(value.fps_x100 == 2500 && value.bitrate_bps == 200000);
    assert(value.sample_window_ms == 1000 && value.updated_ms == 1100);
    si_video_stream_metrics_record(2, 9000, 2100);
    si_video_stream_metrics_get(&value, 3600);
    assert(value.valid && value.fps_x100 == 2500);
    si_video_stream_metrics_get(&value, 3601);
    assert(!value.valid && value.fps_x100 == 0 && value.bitrate_bps == 0);

    si_video_stream_metrics_begin(2, UINT32_MAX - 499U);
    si_video_stream_metrics_record(1, 9000, 500);
    si_video_stream_metrics_record(2, 2000, 500);
    si_video_stream_metrics_get(&value, 500);
    assert(value.valid && value.stream_id == 2);
    assert(value.sample_window_ms == 1000);
    assert(value.fps_x100 == 100 && value.bitrate_bps == 16000);

    si_video_stream_metrics_begin(3, 0);
    si_video_stream_metrics_record(3, UINT32_MAX, 1000);
    si_video_stream_metrics_get(&value, 1000);
    assert(value.valid && value.bitrate_bps == UINT32_MAX);
    si_video_stream_metrics_begin(0, 0);
    si_video_stream_metrics_record(0, 1000, 1000);
    si_video_stream_metrics_get(&value, 1000);
    assert(!value.valid);
    si_video_stream_metrics_get(NULL, 1000);
    puts("video stream metrics tests: PASS");
    return 0;
}
