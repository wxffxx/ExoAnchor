#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

/* Opaque, generation-bound claim for the mutually exclusive internal-memory
 * peak shared by the H.264 codec and the embedded Agent executor.  Zero is
 * never a valid claim. */
typedef uint32_t si_h264_resource_token_t;

typedef struct {
    bool service_initialized;
    bool reference_workspace_reserve_attempted;
    bool reference_workspace_reserved;
    bool reference_workspace_internal;
    bool resource_probe_attempted;
    bool available;
    bool session_active;
    bool pipeline_ready;
    bool restore_pending;
    bool teardown_poisoned;
    bool overlap_enabled;
    bool serial_fallback;
    bool metrics_valid;
    uint16_t width;
    uint16_t height;
    uint8_t target_fps;
    uint8_t yuv_surfaces;
    uint8_t au_slots;
    uint32_t primary_au_capacity_bytes;
    uint32_t small_egress_capacity_bytes;
    uint32_t reference_workspace_required_bytes;
    uint32_t reference_workspace_capacity_bytes;
    uint32_t metrics_age_ms;
    uint32_t metrics_window_ms;
    uint32_t source_fps_x100;
    uint32_t output_fps_x100;
    uint32_t source_drops;
    uint32_t h264_bitrate_bps;
    uint32_t decode_avg_us;
    uint32_t encode_avg_us;
    uint32_t send_avg_us;
    uint32_t decode_max_us;
    uint32_t encode_max_us;
    uint32_t send_max_us;
    uint32_t overlap_pairs;
    uint32_t overlap_misses;
    uint32_t overlap_wait_timeouts;
    uint32_t backpressure_drops_total;
    uint32_t decode_failures_total;
    uint32_t encode_failures_total;
    uint32_t send_failures_total;
    uint32_t sessions_started_total;
    uint32_t sessions_failed_total;
    uint32_t pipeline_starts_total;
    uint32_t access_units_total;
    uint32_t last_sequence;
    uint32_t last_timestamp_ms;
    uint32_t last_jpeg_bytes;
    uint32_t max_jpeg_bytes;
    uint32_t last_au_bytes;
    uint32_t max_au_bytes;
    uint32_t primary_egress_access_units_total;
    uint32_t small_egress_access_units_total;
    esp_err_t resource_error;
    esp_err_t reference_workspace_error;
    esp_err_t restore_error;
} si_h264_stream_status_t;

/* Optional WebSocket transport for the hardware H.264 KVM pipeline. */
esp_err_t si_h264_stream_initialize(void);
void si_h264_stream_get_status(si_h264_stream_status_t *status);
esp_err_t si_h264_stream_ws_post_handshake(httpd_req_t *req);
esp_err_t si_h264_stream_ws_handler(httpd_req_t *req);
esp_err_t si_h264_stream_self_test(void);
esp_err_t si_h264_stream_self_test_file(const char *path);

/* The ESP32-P4 H.264 encoder and the embedded Agent HTTPS executor share a
 * bounded internal-memory budget.  An Agent run claims that budget before its
 * internal stack is created.  The claim suspends an idle encoder (while
 * retaining the boot-validated reference cache and PSRAM frame buffers), or
 * fails closed while a codec pipeline is active. */
esp_err_t si_h264_stream_claim_agent_resources(
    si_h264_resource_token_t *token_out);
bool si_h264_stream_release_agent_resources(
    si_h264_resource_token_t token);
