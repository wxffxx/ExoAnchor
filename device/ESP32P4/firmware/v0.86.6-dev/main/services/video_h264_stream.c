#include "video_h264_stream.h"

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "app_config.h"
#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_api.h"
#include "soc/hp_sys_clkrst_struct.h"
#include "soc/soc.h"
#include "soc/spi1_mem_s_reg.h"
#include "soc/spi_mem_s_reg.h"
#include "time_utils.h"
#include "video_control.h"
#include "video_input.h"
#include "video_stream_metrics.h"

#define SI_H264_TASK_STACK 8192
#define SI_H264_TASK_PRIORITY 8
#define SI_H264_DECODE_TASK_STACK 4096
#define SI_H264_DECODE_TASK_PRIORITY 9
#define SI_H264_HEADER_SIZE 24U
#define SI_H264_PAYLOAD_OFFSET 128U
#define SI_H264_AU_SLOT_COUNT 2U
#define SI_H264_PRIMARY_AU_SLOT 0U
#define SI_H264_SMALL_EGRESS_SLOT 1U
#define SI_H264_SMALL_EGRESS_PAYLOAD_SIZE (1024U * 1024U)
#define SI_H264_JPEG_TIMEOUT_MS 120
#define SI_H264_QP_MIN 25
#define SI_H264_QP_MAX 51
#define SI_H264_SELF_TEST_FRAMES 120U
#define SI_H264_CHAIN_LOG_INTERVAL_US 2000000LL
#define SI_H264_METRICS_STALE_MS 5000U
#define SI_H264_PIPELINE_IDLE_GRACE_MS 2000U
#define SI_H264_OVERLAP_WAIT_PERIODS 2U
#define SI_H264_OVERLAP_WAIT_SLACK_MS 10U
#define SI_H264_STREAM_LOCK_TIMEOUT_MS 250U
#define SI_H264_WORKER_IO_TIMEOUT_MS 250U
#define SI_H264_WORKER_STOP_TIMEOUT_MS (SI_H264_JPEG_TIMEOUT_MS + 500U)
#define SI_H264_SEND_EAGAIN_RETRY_MS 5U
#define SI_H264_SEND_TIMEOUT_MS 1000U
/* HTTPD sends the WebSocket header and payload in two override calls. The
 * in-flight work must finish both budgets before teardown frees AU storage;
 * later queued work observes send_stopping and returns without sending. */
#define SI_H264_EGRESS_STOP_TIMEOUT_MS (2U * SI_H264_SEND_TIMEOUT_MS + 500U)
#define SI_H264_RESTORE_RETRY_INITIAL_MS 100U
#define SI_H264_RESTORE_RETRY_MAX_MS 1000U
#define SI_H264_RESOURCE_RESERVE_WAIT_MS 10000U
#define SI_H264_RESOURCE_CLAIM_WAIT_MS 250U
#define SI_H264_ENCODER_INTERNAL_MIN_FREE (24U * 1024U)
#define SI_H264_ENCODER_INTERNAL_MIN_LARGEST (18U * 1024U)
#define SI_AGENT_EXECUTOR_INTERNAL_MIN_FREE (48U * 1024U)
#define SI_AGENT_EXECUTOR_INTERNAL_MIN_LARGEST (26U * 1024U)
#define SI_H264_MAX_JPEG_INPUT_SIZE                                      \
    ((size_t)SI_CFG_VIDEO_WIDTH * (size_t)SI_CFG_VIDEO_HEIGHT * 2U)

_Static_assert(SI_H264_HEADER_SIZE <= SI_H264_PAYLOAD_OFFSET,
               "H.264 wire header must fit inside reserved AU headroom");
_Static_assert(SI_H264_AU_SLOT_COUNT > 0U && SI_H264_AU_SLOT_COUNT < 32U,
               "H.264 AU ownership mask must fit in uint32_t");
_Static_assert(SI_H264_AU_SLOT_COUNT == 2U,
               "H.264 egress requires one primary and one small slot");

static const char *TAG = "si-h264-stream";

static void log_h264_memory_and_clock_policy(void)
{
#ifdef CONFIG_SPIRAM_SPEED
    const unsigned psram_speed_mhz = (unsigned)CONFIG_SPIRAM_SPEED;
#else
    const unsigned psram_speed_mhz = 0U;
#endif
#ifdef CONFIG_SPIRAM_MODE_HEX
    const char *psram_mode = "hex";
#else
    const char *psram_mode = "not-hex";
#endif
    /* Record the fields after esp_h264_enc_open(), rather than inferring the
     * active clock solely from sdkconfig.  ESP32-P4 TRM defines src_sel=1 as
     * PLL_F240M_CLK.  Keep div_num raw here: the evidence remains valid even
     * if a future IDF changes the divider interpretation. */
    ESP_LOGI(TAG,
             "H.264 clock register: enabled=%u src_sel=%u div_num=%u; PSRAM build=%uMHz mode=%s",
             (unsigned)HP_SYS_CLKRST.peri_clk_ctrl26.reg_h264_clk_en,
             (unsigned)HP_SYS_CLKRST.peri_clk_ctrl26.reg_h264_clk_src_sel,
             (unsigned)HP_SYS_CLKRST.peri_clk_ctrl26.reg_h264_clk_div_num,
             psram_speed_mhz, psram_mode);
    ESP_LOGI(TAG,
             "PSRAM clock registers: pll_en=%u core_en=%u src_sel=%u core_div_num=%u mspi2_bus=0x%08x mspi3_bus=0x%08x",
             (unsigned)HP_SYS_CLKRST.peri_clk_ctrl00.reg_psram_pll_clk_en,
             (unsigned)HP_SYS_CLKRST.peri_clk_ctrl00.reg_psram_core_clk_en,
             (unsigned)HP_SYS_CLKRST.peri_clk_ctrl00.reg_psram_clk_src_sel,
             (unsigned)HP_SYS_CLKRST.peri_clk_ctrl00
                 .reg_psram_core_clk_div_num,
             (unsigned)REG_READ(SPI_MEM_S_SRAM_CLK_REG),
             (unsigned)REG_READ(SPI1_MEM_S_CLOCK_REG));
}

static esp_err_t h264_jpeg_decoder_process(
    jpeg_decoder_handle_t decoder, const jpeg_decode_cfg_t *config,
    const uint8_t *input, uint32_t input_size, uint8_t *output,
    uint32_t output_size, uint32_t *decoded_size)
{
    /* The new ingest boundary publishes only structurally valid, immutable
     * JPEGs.  Use the official IDF decoder unchanged; transport corruption is
     * rejected before it reaches the codec rather than hidden by interrupt
     * masks or guessed entropy repairs. */
    return jpeg_decoder_process(decoder, config, input, input_size, output,
                                output_size, decoded_size);
}

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    httpd_handle_t server;
    int fd;
    uint32_t session;
    uint32_t stream_id;
    si_h264_resource_token_t resource_token;
    bool teardown;
} si_h264_stream_state_t;

typedef struct {
    const uint8_t *jpeg;
    si_video_jpeg_view_t jpeg_view;
    uint32_t jpeg_len;
    uint8_t *yuv420;
    uint32_t yuv420_capacity;
    uint32_t yuv_len;
    esp_err_t result;
    int64_t decode_started_us;
    int64_t decode_finished_us;
    uint32_t jpeg_frame;
    uint32_t previous_jpeg_frame;
    uint32_t timestamp_ms;
    uint32_t jpeg_bytes;
    uint32_t copy_us;
    uint32_t info_us;
    bool h264_uvc_lease;
    bool stop;
} si_h264_decode_job_t;

static void decode_job_release_source(si_h264_decode_job_t *job)
{
    if (!job || !job->jpeg_view.token) {
        return;
    }
    if (job->h264_uvc_lease) {
        esp_err_t err = si_video_release_h264_jpeg(&job->jpeg_view);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "release direct H.264 UVC frame failed: %s",
                     esp_err_to_name(err));
        }
    } else {
        si_video_release_jpeg(&job->jpeg_view);
    }
}

static size_t h264_encoder_output_capacity(size_t packet_capacity)
{
    if (packet_capacity <= SI_H264_PAYLOAD_OFFSET) {
        return 0;
    }
    /* Rate control does not impose a hard per-access-unit ceiling.  A first
     * IDR generated from a near-maximum-entropy MS2109 JPEG can exceed 1 MiB
     * even when the configured long-term bitrate is 8 Mbit/s.  Advertise the
     * complete preallocated AU buffer to the encoder; otherwise valid input
     * deterministically fails with ESP_H264_ERR_BUFF_NOT_ENOUGH. */
    return packet_capacity - SI_H264_PAYLOAD_OFFSET;
}

typedef struct {
    TaskHandle_t task;
    QueueHandle_t requests;
    QueueHandle_t results;
    SemaphoreHandle_t stopped;
    jpeg_decoder_handle_t decoder;
    jpeg_decoder_handle_t *owner_decoder;
    bool stop_requested;
    bool stopped_confirmed;
    bool poisoned;
} si_h264_decode_worker_t;

typedef struct si_h264_pipeline si_h264_pipeline_t;

typedef struct {
    httpd_handle_t server;
    int fd;
    uint32_t session;
    uint32_t stream_id;
    uint8_t slot_id;
    uint8_t *payload;
    size_t frame_len;
} si_h264_send_job_t;

typedef struct {
    si_h264_pipeline_t *pipeline;
    si_h264_send_job_t job;
} si_h264_send_work_t;

typedef struct {
    uint8_t *storage;
    uint32_t storage_capacity;
    si_h264_send_work_t send_work;
} si_h264_au_slot_t;

struct si_h264_pipeline {
    jpeg_decoder_handle_t jpeg_decoder;
    si_h264_decode_worker_t decode_worker;
    esp_h264_enc_handle_t h264_encoder;
    bool h264_open;
    bool owns_codec_gate;
    bool uses_preallocated_encoder;
    bool uses_preallocated_buffers;
    uint8_t *yuv420;
    size_t yuv420_capacity;
    uint8_t *yuv420_alt;
    size_t yuv420_alt_capacity;
    si_h264_au_slot_t au_slots[SI_H264_AU_SLOT_COUNT];
    uint8_t au_slot_count;
    bool egress_initialized;
    volatile uint32_t free_au_mask;
    volatile bool send_stopping;
    volatile esp_err_t send_result;
    volatile uint32_t send_result_session;
    volatile uint32_t last_send_us;
    volatile bool encoder_reset_required;
    bool teardown_poisoned;
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    bool pts_origin_valid;
    uint32_t pts_origin_ms;
};

typedef struct {
    int64_t started_us;
    uint32_t frames;
    uint32_t source_frames;
    uint32_t source_drops;
    uint64_t jpeg_bytes;
    uint64_t h264_bytes;
    uint64_t copy_us;
    uint64_t info_us;
    uint64_t decode_us;
    uint64_t encode_us;
    uint64_t move_us;
    uint64_t send_wait_us;
    uint64_t send_us;
    uint32_t max_decode_us;
    uint32_t max_encode_us;
    uint32_t max_send_wait_us;
    uint32_t max_send_us;
    uint32_t overlap_pairs;
    uint32_t overlap_misses;
    uint32_t overlap_wait_events;
    uint32_t overlap_wait_timeouts;
    uint64_t overlap_us;
    uint64_t pair_span_us;
    uint64_t overlap_wait_us;
    uint32_t max_pair_span_us;
    uint32_t max_overlap_wait_us;
} si_h264_chain_metrics_t;

typedef struct {
    bool pending;
    uint32_t decode_frame;
    int64_t encode_started_us;
    int64_t encode_finished_us;
} si_h264_overlap_probe_t;

static si_h264_stream_state_t s_stream = {
    .fd = -1,
};

static volatile bool s_service_initialized;
static volatile bool s_reference_workspace_reserve_claimed;
static volatile bool s_reference_workspace_reserve_attempted;
static volatile bool s_reference_workspace_reserved;
static volatile bool s_reference_workspace_internal;
static volatile uint32_t s_reference_workspace_required_bytes;
static volatile uint32_t s_reference_workspace_capacity_bytes;
static volatile esp_err_t s_reference_workspace_error = ESP_ERR_NOT_FINISHED;
static volatile bool s_runtime_reserve_claimed;
static volatile bool s_runtime_reserve_terminal_failure;
static volatile uint32_t s_resource_owner_token;
static volatile uint32_t s_resource_owner_epoch;
static volatile bool s_resource_probe_attempted;
static volatile bool s_runtime_available;
static volatile bool s_session_active;
static volatile bool s_pipeline_ready;
static volatile bool s_restore_pending;
static volatile bool s_teardown_poisoned;
static volatile esp_err_t s_resource_error = ESP_ERR_NOT_FINISHED;
static volatile esp_err_t s_restore_error = ESP_OK;
static volatile uint32_t s_pipeline_width;
static volatile uint32_t s_pipeline_height;
static volatile uint32_t s_pipeline_target_fps;
static volatile uint32_t s_pipeline_yuv_surfaces;
static volatile uint32_t s_pipeline_au_slots;
static volatile uint32_t s_pipeline_primary_au_capacity;
static volatile uint32_t s_pipeline_small_egress_capacity;
static volatile bool s_pipeline_overlap_enabled;
static volatile bool s_pipeline_serial_fallback;
static volatile uint32_t s_metrics_updated_ms;
static volatile uint32_t s_metrics_window_ms;
static volatile uint32_t s_metrics_source_fps_x100;
static volatile uint32_t s_metrics_output_fps_x100;
static volatile uint32_t s_metrics_source_drops;
static volatile uint32_t s_metrics_h264_bitrate_bps;
static volatile uint32_t s_metrics_decode_avg_us;
static volatile uint32_t s_metrics_encode_avg_us;
static volatile uint32_t s_metrics_send_avg_us;
static volatile uint32_t s_metrics_decode_max_us;
static volatile uint32_t s_metrics_encode_max_us;
static volatile uint32_t s_metrics_send_max_us;
static volatile uint32_t s_metrics_overlap_pairs;
static volatile uint32_t s_metrics_overlap_misses;
static volatile uint32_t s_metrics_overlap_wait_timeouts;
static volatile uint32_t s_backpressure_drops_total;
static volatile uint32_t s_decode_failures_total;
static volatile uint32_t s_encode_failures_total;
static volatile uint32_t s_send_failures_total;
static volatile uint32_t s_sessions_started_total;
static volatile uint32_t s_sessions_failed_total;
static volatile uint32_t s_pipeline_starts_total;
static volatile uint32_t s_access_units_total;
static volatile uint32_t s_last_sequence;
static volatile uint32_t s_last_timestamp_ms;
static volatile uint32_t s_last_jpeg_bytes;
static volatile uint32_t s_max_jpeg_bytes;
static volatile uint32_t s_last_au_bytes;
static volatile uint32_t s_max_au_bytes;
static volatile uint32_t s_primary_egress_access_units_total;
static volatile uint32_t s_small_egress_access_units_total;

static void h264_telemetry_update_max(volatile uint32_t *target,
                                      uint32_t value)
{
    uint32_t observed = __atomic_load_n(target, __ATOMIC_RELAXED);
    while (value > observed &&
           !__atomic_compare_exchange_n(target, &observed, value, true,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
}

static void h264_telemetry_pipeline_clear(void)
{
    __atomic_store_n(&s_pipeline_ready, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_width, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_height, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_target_fps, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_yuv_surfaces, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_au_slots, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_primary_au_capacity, 0U,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_small_egress_capacity, 0U,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_overlap_enabled, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_serial_fallback, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_updated_ms, 0U, __ATOMIC_RELEASE);
}

static void h264_telemetry_pipeline_publish(
    const si_h264_pipeline_t *pipeline)
{
    const bool overlap = pipeline && pipeline->decode_worker.task;
    const uint32_t yuv_surfaces =
        pipeline && pipeline->yuv420 ? (pipeline->yuv420_alt ? 2U : 1U) : 0U;
    const uint32_t au_slots = pipeline ? pipeline->au_slot_count : 0U;
    const uint32_t primary_au_capacity =
        pipeline && pipeline->au_slots[SI_H264_PRIMARY_AU_SLOT].storage_capacity >
                        SI_H264_PAYLOAD_OFFSET
            ? pipeline->au_slots[SI_H264_PRIMARY_AU_SLOT].storage_capacity -
                  SI_H264_PAYLOAD_OFFSET
            : 0U;
    const uint32_t small_egress_capacity =
        pipeline && pipeline->au_slots[SI_H264_SMALL_EGRESS_SLOT].storage_capacity >
                        SI_H264_PAYLOAD_OFFSET
            ? pipeline->au_slots[SI_H264_SMALL_EGRESS_SLOT].storage_capacity -
                  SI_H264_PAYLOAD_OFFSET
            : 0U;
    __atomic_store_n(&s_pipeline_width,
                     pipeline ? pipeline->width : 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_height,
                     pipeline ? pipeline->height : 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_target_fps,
                     pipeline ? pipeline->fps : 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_yuv_surfaces, yuv_surfaces,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_au_slots, au_slots, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_primary_au_capacity,
                     primary_au_capacity, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_small_egress_capacity,
                     small_egress_capacity, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_overlap_enabled, overlap,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_serial_fallback,
                     !overlap || yuv_surfaces < 2U || au_slots < 2U,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_updated_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_ready, pipeline != NULL, __ATOMIC_RELEASE);
    if (pipeline) {
        __atomic_fetch_add(&s_pipeline_starts_total, 1U, __ATOMIC_RELAXED);
    }
}

static bool stream_still_current(uint32_t session, int fd);
static void stream_detach(uint32_t session, int fd);
static void stream_fail(uint32_t session, int fd);

/* ESP-IDF 5.5's default HTTPD WebSocket sender calls send() once and treats
 * any non-negative result as success.  POSIX explicitly permits a short
 * write, which is routine for multi-megabyte high-entropy IDR frames.  The
 * advertised WebSocket payload length then exceeds the bytes put on the
 * wire, leaving the browser waiting forever and making subsequent frames
 * part of the truncated payload.  Install this override only on H.264
 * sessions so the HTTPD helper's single call has send-all semantics. */
static int h264_socket_send_all(httpd_handle_t handle, int socket_fd,
                                const char *buffer, size_t length, int flags)
{
    (void)handle;
    if (!buffer || length > INT_MAX) {
        errno = !buffer ? EINVAL : EOVERFLOW;
        return -1;
    }
    size_t sent = 0;
    const int64_t deadline_us =
        esp_timer_get_time() +
        (int64_t)SI_H264_SEND_TIMEOUT_MS * 1000LL;
    while (sent < length) {
        /* One deadline covers positive short writes and EINTR as well as
         * backpressure. Per-call nonblocking I/O prevents SO_SNDTIMEO from
         * consuming a fresh timeout on every attempt without changing the
         * socket's receive behavior for HTTPD control frames. */
        if (esp_timer_get_time() >= deadline_us) {
            errno = ETIMEDOUT;
            return -1;
        }
        ssize_t result = send(socket_fd, buffer + sent, length - sent,
                              flags | MSG_DONTWAIT);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            TickType_t retry_ticks =
                pdMS_TO_TICKS(SI_H264_SEND_EAGAIN_RETRY_MS);
            vTaskDelay(retry_ticks > 0 ? retry_ticks : 1);
            continue;
        }
        if (result <= 0) {
            if (result == 0) {
                errno = EPIPE;
            }
            /* HTTPD treats any nonnegative override result as success. */
            return -1;
        }
        sent += (size_t)result;
    }
    return (int)sent;
}

static esp_h264_enc_handle_t s_preallocated_encoder;
static bool s_preallocated_encoder_claimed;
static uint16_t s_preallocated_width;
static uint16_t s_preallocated_height;
static uint8_t s_preallocated_fps;
static uint8_t *s_preallocated_yuv420;
static size_t s_preallocated_yuv420_capacity;
static uint8_t *s_preallocated_yuv420_alt;
static size_t s_preallocated_yuv420_alt_capacity;
static uint8_t *s_preallocated_au[SI_H264_AU_SLOT_COUNT];
static uint32_t s_preallocated_au_capacity[SI_H264_AU_SLOT_COUNT];
static bool s_preallocated_buffers_claimed;
static bool s_codec_pipeline_claimed;

enum {
    SI_H264_RESOURCE_OWNER_NONE = 0U,
    SI_H264_RESOURCE_OWNER_AGENT = 1U,
    SI_H264_RESOURCE_OWNER_H264 = 2U,
    SI_H264_RESOURCE_OWNER_KIND_MASK = 3U,
};

static esp_err_t h264_runtime_gate_take(uint32_t timeout_ms)
{
    const int64_t deadline_us =
        esp_timer_get_time() +
        (int64_t)timeout_ms * 1000LL;
    do {
        bool expected = false;
        if (__atomic_compare_exchange_n(&s_runtime_reserve_claimed,
                                        &expected, true, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return ESP_OK;
        }
        vTaskDelay(1);
    } while (esp_timer_get_time() < deadline_us);
    return ESP_ERR_TIMEOUT;
}

static void h264_runtime_gate_give(void)
{
    __atomic_store_n(&s_runtime_reserve_claimed, false, __ATOMIC_RELEASE);
}

/* The low two bits encode the owner kind; the upper bits form a generation.
 * This lets a cleanup path compare-and-release one exact claim without a
 * stale task being able to clear a newer owner. The runtime gate is held by
 * every caller of this helper. */
static si_h264_resource_token_t h264_resource_next_token(uint32_t owner_kind)
{
    uint32_t epoch =
        __atomic_load_n(&s_resource_owner_epoch, __ATOMIC_RELAXED) + 1U;
    if (epoch == 0U || epoch > (UINT32_MAX >> 2U)) {
        epoch = 1U;
    }
    __atomic_store_n(&s_resource_owner_epoch, epoch, __ATOMIC_RELAXED);
    return (epoch << 2U) | owner_kind;
}

static bool h264_resource_owner_is(si_h264_resource_token_t token,
                                   uint32_t owner_kind)
{
    return token != 0U &&
           (token & SI_H264_RESOURCE_OWNER_KIND_MASK) == owner_kind &&
           __atomic_load_n(&s_resource_owner_token, __ATOMIC_ACQUIRE) ==
               token;
}

static esp_err_t h264_resource_claim_locked(
    uint32_t owner_kind, si_h264_resource_token_t *token_out)
{
    if (!token_out || owner_kind == SI_H264_RESOURCE_OWNER_NONE ||
        __atomic_load_n(&s_resource_owner_token, __ATOMIC_ACQUIRE) != 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    const si_h264_resource_token_t token =
        h264_resource_next_token(owner_kind);
    __atomic_store_n(&s_resource_owner_token, token, __ATOMIC_RELEASE);
    *token_out = token;
    return ESP_OK;
}

static bool h264_resource_release_exact(si_h264_resource_token_t token,
                                        uint32_t owner_kind)
{
    if (token == 0U ||
        (token & SI_H264_RESOURCE_OWNER_KIND_MASK) != owner_kind) {
        return false;
    }
    uint32_t expected = token;
    return __atomic_compare_exchange_n(
        &s_resource_owner_token, &expected, 0U, false, __ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE);
}

static esp_err_t h264_stream_claim_h264_resources(
    si_h264_resource_token_t *token_out)
{
    if (!token_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *token_out = 0U;
    esp_err_t gate_err =
        h264_runtime_gate_take(SI_H264_RESOURCE_CLAIM_WAIT_MS);
    if (gate_err != ESP_OK) {
        return gate_err;
    }
    esp_err_t claim_err = h264_resource_claim_locked(
        SI_H264_RESOURCE_OWNER_H264, token_out);
    h264_runtime_gate_give();
    return claim_err;
}

static bool h264_stream_release_h264_resources(
    si_h264_resource_token_t token)
{
    if (__atomic_load_n(&s_session_active, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_pipeline_ready, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_codec_pipeline_claimed, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_preallocated_encoder_claimed, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_preallocated_buffers_claimed,
                        __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_teardown_poisoned, __ATOMIC_ACQUIRE)) {
        ESP_LOGE(TAG,
                 "retaining H.264 resource owner after incomplete teardown");
        return false;
    }
    return h264_resource_release_exact(token,
                                       SI_H264_RESOURCE_OWNER_H264);
}

esp_err_t si_h264_stream_claim_agent_resources(
    si_h264_resource_token_t *token_out)
{
    if (!token_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *token_out = 0U;

    esp_err_t gate_err =
        h264_runtime_gate_take(SI_H264_RESOURCE_CLAIM_WAIT_MS);
    if (gate_err != ESP_OK) {
        return gate_err;
    }

    si_h264_resource_token_t token = 0U;
    esp_err_t claim_err = h264_resource_claim_locked(
        SI_H264_RESOURCE_OWNER_AGENT, &token);
    if (claim_err != ESP_OK) {
        h264_runtime_gate_give();
        return claim_err;
    }

    const bool codec_busy =
        __atomic_load_n(&s_session_active, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_pipeline_ready, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_codec_pipeline_claimed, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_preallocated_encoder_claimed, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_preallocated_buffers_claimed, __ATOMIC_ACQUIRE);
    if (codec_busy) {
        (void)h264_resource_release_exact(
            token, SI_H264_RESOURCE_OWNER_AGENT);
        h264_runtime_gate_give();
        return ESP_ERR_INVALID_STATE;
    }

    /* The boot-time internal reference cache is part of the normal Agent
     * baseline.  Only the idle encoder handle owns the additional strict-
     * internal db_tmp/descriptors that made HTTPS/newlib allocations abort.
     * Deleting it returns those blocks while esp_h264's guarded patch keeps
     * the largest reference/deblocking workspaces cached for a later retry. */
    if (s_preallocated_encoder) {
        esp_h264_err_t delete_err =
            esp_h264_enc_del(s_preallocated_encoder);
        if (delete_err != ESP_H264_ERR_OK) {
            (void)h264_resource_release_exact(
                token, SI_H264_RESOURCE_OWNER_AGENT);
            h264_runtime_gate_give();
            ESP_LOGE(TAG,
                     "cannot suspend idle H.264 encoder for Agent: %d",
                     (int)delete_err);
            return ESP_FAIL;
        }
        s_preallocated_encoder = NULL;
        s_preallocated_width = 0;
        s_preallocated_height = 0;
        s_preallocated_fps = 0;
    }

    /* The idle handle is gone (or was already absent), so availability must
     * be republished only by a later H.264 reserve. This remains true even if
     * the Agent preflight below decides that current heap pressure is too
     * high and gives its token back. */
    __atomic_store_n(&s_runtime_available, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_resource_error, ESP_ERR_NOT_FINISHED,
                     __ATOMIC_RELEASE);

    const size_t internal_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal_free < SI_AGENT_EXECUTOR_INTERNAL_MIN_FREE ||
        internal_largest < SI_AGENT_EXECUTOR_INTERNAL_MIN_LARGEST) {
        (void)h264_resource_release_exact(
            token, SI_H264_RESOURCE_OWNER_AGENT);
        h264_runtime_gate_give();
        ESP_LOGW(TAG,
                 "Agent resource claim deferred: internal=%u largest=%u need=%u/%u",
                 (unsigned)internal_free, (unsigned)internal_largest,
                 (unsigned)SI_AGENT_EXECUTOR_INTERNAL_MIN_FREE,
                 (unsigned)SI_AGENT_EXECUTOR_INTERNAL_MIN_LARGEST);
        return ESP_ERR_NO_MEM;
    }
    h264_runtime_gate_give();
    *token_out = token;
    ESP_LOGI(TAG,
             "Agent claimed codec memory budget: internal=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT));
    return ESP_OK;
}

bool si_h264_stream_release_agent_resources(
    si_h264_resource_token_t token)
{
    return h264_resource_release_exact(token,
                                       SI_H264_RESOURCE_OWNER_AGENT);
}

/* Build-time patched into esp_h264_enc_hw_param.c.  This is intentionally a
 * narrow private ABI: the upstream source hash and exact insertion fragments
 * are guarded by the project CMake override. */
extern esp_h264_err_t esp_h264_exoanchor_reserve_ref_workspace(
    uint16_t width, uint32_t *required_bytes, uint32_t *capacity_bytes);
extern void esp_h264_exoanchor_release_cached_workspaces(void);

static void log_pipeline_heap(const char *stage)
{
    ESP_LOGI(TAG,
             "%s heap: internal=%u/%u psram=%u/%u encoder=%d/%d buffers=%d/%d",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM |
                                                        MALLOC_CAP_8BIT),
             s_preallocated_encoder != NULL,
             __atomic_load_n(&s_preallocated_encoder_claimed,
                             __ATOMIC_ACQUIRE),
             s_preallocated_yuv420 != NULL &&
                 s_preallocated_au[0] != NULL,
             __atomic_load_n(&s_preallocated_buffers_claimed,
                             __ATOMIC_ACQUIRE));
}

static esp_err_t jpeg_decode_worker_recreate(
    si_h264_decode_worker_t *worker)
{
    if (!worker) {
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_decoder_handle_t stale = worker->decoder;
    worker->decoder = NULL;
    if (worker->owner_decoder) {
        *worker->owner_decoder = NULL;
    }
    if (stale) {
        esp_err_t err = jpeg_del_decoder_engine(stale);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to reset JPEG decoder after bad frame: %s",
                     esp_err_to_name(err));
            return err;
        }
    }

    jpeg_decode_engine_cfg_t jpeg_cfg = {
        .intr_priority = 0,
        .timeout_ms = SI_H264_JPEG_TIMEOUT_MS,
    };
    esp_err_t err = jpeg_new_decoder_engine(&jpeg_cfg, &worker->decoder);
    if (err == ESP_OK && worker->owner_decoder) {
        *worker->owner_decoder = worker->decoder;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to recreate JPEG decoder after bad frame: %s",
                 esp_err_to_name(err));
    }
    return err;
}

static void jpeg_decode_worker_task(void *arg)
{
    si_h264_decode_worker_t *worker = arg;
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_YUV420,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT709,
    };
    si_h264_decode_job_t job = {0};
    while (true) {
        if (xQueueReceive(worker->requests, &job,
                          pdMS_TO_TICKS(SI_H264_WORKER_IO_TIMEOUT_MS)) !=
            pdTRUE) {
            continue;
        }
        if (job.stop) {
            break;
        }
        job.decode_started_us = esp_timer_get_time();
        job.result = h264_jpeg_decoder_process(
            worker->decoder, &cfg, job.jpeg, job.jpeg_len, job.yuv420,
            job.yuv420_capacity, &job.yuv_len);
        if (job.result != ESP_OK) {
            /* A rejected frame is never modified and retried.  Recreate the
             * engine only so a codec error cannot poison the next validated
             * frame. */
            esp_err_t reset_err = jpeg_decode_worker_recreate(worker);
            if (reset_err != ESP_OK) {
                job.result = reset_err;
            }
        }
        job.decode_finished_us = esp_timer_get_time();
        decode_job_release_source(&job);
        if (xQueueSend(worker->results, &job,
                       pdMS_TO_TICKS(SI_H264_WORKER_IO_TIMEOUT_MS)) !=
            pdPASS) {
            worker->poisoned = true;
            ESP_LOGE(TAG, "JPEG decode result queue timed out");
        }
    }
    if (worker->stopped) {
        xSemaphoreGive(worker->stopped);
    }
    vTaskSuspend(NULL);
}

static esp_err_t pipeline_start_decode_worker(si_h264_pipeline_t *pipeline)
{
    if (pipeline->decode_worker.task) {
        return ESP_OK;
    }
    pipeline->decode_worker.decoder = pipeline->jpeg_decoder;
    pipeline->decode_worker.owner_decoder = &pipeline->jpeg_decoder;
    pipeline->decode_worker.stop_requested = false;
    pipeline->decode_worker.stopped_confirmed = false;
    pipeline->decode_worker.poisoned = false;
    pipeline->decode_worker.requests =
        xQueueCreate(1, sizeof(si_h264_decode_job_t));
    pipeline->decode_worker.results =
        xQueueCreate(1, sizeof(si_h264_decode_job_t));
    pipeline->decode_worker.stopped = xSemaphoreCreateBinary();
    if (!pipeline->decode_worker.requests ||
        !pipeline->decode_worker.results ||
        !pipeline->decode_worker.stopped) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t created = xTaskCreatePinnedToCore(
        jpeg_decode_worker_task, "si_h264_jpeg", SI_H264_DECODE_TASK_STACK,
        &pipeline->decode_worker, SI_H264_DECODE_TASK_PRIORITY,
        &pipeline->decode_worker.task, 1);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t pipeline_stop_decode_worker(si_h264_pipeline_t *pipeline)
{
    if (!pipeline) {
        return ESP_ERR_INVALID_ARG;
    }
    si_h264_decode_worker_t *worker = &pipeline->decode_worker;
    TaskHandle_t task = worker->task;
    if (task && worker->requests && worker->results && worker->stopped) {
        si_h264_decode_job_t abandoned = {0};
        while (xQueueReceive(worker->results, &abandoned, 0) == pdPASS) {
            decode_job_release_source(&abandoned);
        }
        if (!worker->stop_requested) {
            si_h264_decode_job_t stop = {.stop = true};
            /* At most one decode is active.  Once it publishes its result it
             * consumes this sentinel, signals stopped, and suspends itself.
             * A failed bounded enqueue leaves every queue/task allocation in
             * place so the owner can retry teardown without use-after-free. */
            if (xQueueSend(worker->requests, &stop,
                           pdMS_TO_TICKS(SI_H264_WORKER_IO_TIMEOUT_MS)) !=
                pdPASS) {
                worker->poisoned = true;
                return ESP_ERR_TIMEOUT;
            }
            worker->stop_requested = true;
        }
        if (!worker->stopped_confirmed) {
            if (xSemaphoreTake(
                    worker->stopped,
                    pdMS_TO_TICKS(SI_H264_WORKER_STOP_TIMEOUT_MS)) != pdTRUE) {
                worker->poisoned = true;
                return ESP_ERR_TIMEOUT;
            }
            worker->stopped_confirmed = true;
        }
        while (xQueueReceive(worker->results, &abandoned, 0) == pdPASS) {
            decode_job_release_source(&abandoned);
        }
        vTaskDelete(task);
        worker->task = NULL;
    } else if (task) {
        worker->poisoned = true;
        return ESP_ERR_INVALID_STATE;
    }
    if (worker->requests) {
        vQueueDelete(worker->requests);
        worker->requests = NULL;
    }
    if (worker->results) {
        vQueueDelete(worker->results);
        worker->results = NULL;
    }
    if (worker->stopped) {
        vSemaphoreDelete(worker->stopped);
        worker->stopped = NULL;
    }
    worker->stop_requested = false;
    worker->stopped_confirmed = false;
    worker->poisoned = false;
    return ESP_OK;
}

static void chain_metrics_reset(si_h264_chain_metrics_t *metrics,
                                int64_t now_us)
{
    memset(metrics, 0, sizeof(*metrics));
    metrics->started_us = now_us;
}

static uint32_t h264_telemetry_rate(uint64_t count, uint64_t scale,
                                    uint64_t elapsed_us)
{
    if (elapsed_us == 0U || count > UINT64_MAX / scale) {
        return 0U;
    }
    uint64_t value = (count * scale + elapsed_us / 2U) / elapsed_us;
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static void h264_telemetry_metrics_publish(
    const si_h264_chain_metrics_t *metrics, int64_t elapsed_us)
{
    if (!metrics || elapsed_us <= 0 || metrics->frames == 0U) {
        return;
    }
    const uint64_t elapsed = (uint64_t)elapsed_us;
    const uint32_t frames = metrics->frames;
    __atomic_store_n(&s_metrics_window_ms,
                     h264_telemetry_rate(elapsed, 1U, 1000U),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_source_fps_x100,
                     h264_telemetry_rate(metrics->source_frames,
                                         100000000ULL, elapsed),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_output_fps_x100,
                     h264_telemetry_rate(frames, 100000000ULL, elapsed),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_source_drops, metrics->source_drops,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_h264_bitrate_bps,
                     h264_telemetry_rate(metrics->h264_bytes, 8000000ULL,
                                         elapsed),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_decode_avg_us,
                     (uint32_t)(metrics->decode_us / frames),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_encode_avg_us,
                     (uint32_t)(metrics->encode_us / frames),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_send_avg_us,
                     (uint32_t)(metrics->send_us / frames),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_decode_max_us, metrics->max_decode_us,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_encode_max_us, metrics->max_encode_us,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_send_max_us, metrics->max_send_us,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_overlap_pairs, metrics->overlap_pairs,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_overlap_misses, metrics->overlap_misses,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_overlap_wait_timeouts,
                     metrics->overlap_wait_timeouts, __ATOMIC_RELEASE);
    __atomic_store_n(&s_metrics_updated_ms, si_monotonic_ms(),
                     __ATOMIC_RELEASE);
}

static int64_t overlap_wait_timeout_us(const si_h264_pipeline_t *pipeline)
{
    uint32_t fps = pipeline && pipeline->fps ? pipeline->fps :
                   SI_CFG_VIDEO_FPS;
    if (fps == 0U) {
        fps = 1U;
    }
    const uint32_t period_ms = (1000U + fps - 1U) / fps;
    return (int64_t)(period_ms * SI_H264_OVERLAP_WAIT_PERIODS +
                     SI_H264_OVERLAP_WAIT_SLACK_MS) * 1000LL;
}

static uint32_t h264_output_fps_cap(uint32_t width, uint32_t height)
{
    return width <= 1280U && height <= 720U
               ? SI_CFG_VIDEO_H264_720P_MAX_FPS
               : SI_CFG_VIDEO_H264_HIGH_RES_MAX_FPS;
}

static bool h264_output_mode_supported(uint32_t width, uint32_t height,
                                       uint32_t requested_fps_x100)
{
    return width >= 80U && width <= 1920U &&
           height >= 80U && height <= 2032U && requested_fps_x100 >= 100U &&
           requested_fps_x100 <= h264_output_fps_cap(width, height) * 100U;
}

static uint8_t h264_output_fps_for_mode(uint32_t width, uint32_t height,
                                        uint32_t requested_fps_x100)
{
    uint32_t fps = (requested_fps_x100 + 50U) / 100U;
    const uint32_t cap = h264_output_fps_cap(width, height);
    if (fps == 0U || fps > cap) {
        fps = cap;
    }
    return (uint8_t)(fps > UINT8_MAX ? UINT8_MAX : fps);
}

static void overlap_probe_reset(si_h264_overlap_probe_t *probe)
{
    if (probe) {
        memset(probe, 0, sizeof(*probe));
    }
}

static void chain_metrics_record_overlap(
    si_h264_chain_metrics_t *metrics, si_h264_overlap_probe_t *probe,
    const si_h264_decode_job_t *decoded)
{
    if (!metrics || !probe || !probe->pending || !decoded) {
        return;
    }
    if (probe->decode_frame != decoded->jpeg_frame ||
        probe->encode_finished_us <= probe->encode_started_us ||
        decoded->decode_finished_us <= decoded->decode_started_us) {
        metrics->overlap_misses++;
        overlap_probe_reset(probe);
        return;
    }

    const int64_t overlap_started_us =
        decoded->decode_started_us > probe->encode_started_us ?
            decoded->decode_started_us : probe->encode_started_us;
    const int64_t overlap_finished_us =
        decoded->decode_finished_us < probe->encode_finished_us ?
            decoded->decode_finished_us : probe->encode_finished_us;
    const int64_t pair_started_us =
        decoded->decode_started_us < probe->encode_started_us ?
            decoded->decode_started_us : probe->encode_started_us;
    const int64_t pair_finished_us =
        decoded->decode_finished_us > probe->encode_finished_us ?
            decoded->decode_finished_us : probe->encode_finished_us;
    const uint32_t overlap_us = overlap_finished_us > overlap_started_us ?
        (uint32_t)(overlap_finished_us - overlap_started_us) : 0U;
    const uint32_t pair_span_us =
        (uint32_t)(pair_finished_us - pair_started_us);

    metrics->overlap_pairs++;
    metrics->overlap_us += overlap_us;
    metrics->pair_span_us += pair_span_us;
    if (overlap_us == 0U) {
        metrics->overlap_misses++;
    }
    if (pair_span_us > metrics->max_pair_span_us) {
        metrics->max_pair_span_us = pair_span_us;
    }
    overlap_probe_reset(probe);
}

static void chain_metrics_report(si_h264_chain_metrics_t *metrics,
                                 int64_t now_us)
{
    int64_t elapsed_us = now_us - metrics->started_us;
    if (elapsed_us < SI_H264_CHAIN_LOG_INTERVAL_US || metrics->frames == 0) {
        return;
    }
    uint32_t frames = metrics->frames;
    ESP_LOGI(TAG,
             "chain %.2fs: source=%.2ffps output=%.2ffps source_drop=%u jpeg=%.2fMbps h264=%.2fMbps avg_us copy=%u info=%u decode=%u encode=%u move=%u send_wait=%u send=%u max_us decode=%u encode=%u send_wait=%u send=%u overlap pairs=%u miss=%u avg=%u span=%u/%u wait=%u/%u timeout=%u",
             elapsed_us / 1000000.0,
             metrics->source_frames * 1000000.0 / elapsed_us,
             frames * 1000000.0 / elapsed_us,
             (unsigned)metrics->source_drops,
             metrics->jpeg_bytes * 8.0 / elapsed_us,
             metrics->h264_bytes * 8.0 / elapsed_us,
             (unsigned)(metrics->copy_us / frames),
             (unsigned)(metrics->info_us / frames),
             (unsigned)(metrics->decode_us / frames),
             (unsigned)(metrics->encode_us / frames),
             (unsigned)(metrics->move_us / frames),
             (unsigned)(metrics->send_wait_us / frames),
             (unsigned)(metrics->send_us / frames),
             (unsigned)metrics->max_decode_us,
             (unsigned)metrics->max_encode_us,
             (unsigned)metrics->max_send_wait_us,
             (unsigned)metrics->max_send_us,
             (unsigned)metrics->overlap_pairs,
             (unsigned)metrics->overlap_misses,
             metrics->overlap_pairs ?
                 (unsigned)(metrics->overlap_us / metrics->overlap_pairs) : 0U,
             metrics->overlap_pairs ?
                 (unsigned)(metrics->pair_span_us / metrics->overlap_pairs) : 0U,
             (unsigned)metrics->max_pair_span_us,
             metrics->overlap_wait_events ?
                 (unsigned)(metrics->overlap_wait_us /
                            metrics->overlap_wait_events) : 0U,
             (unsigned)metrics->max_overlap_wait_us,
             (unsigned)metrics->overlap_wait_timeouts);
    h264_telemetry_metrics_publish(metrics, elapsed_us);
    chain_metrics_reset(metrics, now_us);
}

static void write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static bool pipeline_claim_au_slot(si_h264_pipeline_t *pipeline,
                                   uint8_t slot_id)
{
    if (!pipeline || slot_id >= pipeline->au_slot_count ||
        pipeline->au_slot_count > SI_H264_AU_SLOT_COUNT) {
        return false;
    }
    const uint32_t bit = 1U << slot_id;
    uint32_t available = __atomic_load_n(&pipeline->free_au_mask,
                                         __ATOMIC_ACQUIRE);
    while ((available & bit) != 0U) {
        uint32_t desired = available & ~bit;
        if (__atomic_compare_exchange_n(&pipeline->free_au_mask, &available,
                                        desired, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
    return false;
}

static void pipeline_return_au_slot(si_h264_pipeline_t *pipeline,
                                    uint8_t slot_id)
{
    if (!pipeline || slot_id >= pipeline->au_slot_count) {
        ESP_LOGE(TAG, "invalid H.264 AU slot return: %u/%u",
                 (unsigned)slot_id,
                 pipeline ? (unsigned)pipeline->au_slot_count : 0U);
        return;
    }
    const uint32_t bit = 1U << slot_id;
    uint32_t previous = __atomic_fetch_or(&pipeline->free_au_mask, bit,
                                          __ATOMIC_ACQ_REL);
    if ((previous & bit) != 0U) {
        ESP_LOGE(TAG, "duplicate H.264 AU slot return: %u mask=0x%02x",
                 (unsigned)slot_id, (unsigned)previous);
    }
}

static uint32_t pipeline_all_free_au_mask(
    const si_h264_pipeline_t *pipeline)
{
    if (!pipeline || pipeline->au_slot_count == 0U ||
        pipeline->au_slot_count > SI_H264_AU_SLOT_COUNT) {
        return 0U;
    }
    return (1U << pipeline->au_slot_count) - 1U;
}

/* Runs inside the selected HTTPD task through httpd_queue_work().  This is
 * deliberately not a generic background task: WebSocket header/payload,
 * PONG/CLOSE control frames, handshakes and session destruction must share
 * one server execution context or their socket writes can interleave. */
static void h264_httpd_send_work(void *arg)
{
    si_h264_send_work_t *work = (si_h264_send_work_t *)arg;
    if (!work || !work->pipeline) {
        return;
    }
    si_h264_pipeline_t *pipeline = work->pipeline;
    const si_h264_send_job_t job = work->job;
    esp_err_t err = ESP_OK;
    bool current = false;
    bool send_attempted = false;
    bool active_send_failure = false;

    if (!__atomic_load_n(&pipeline->send_stopping, __ATOMIC_ACQUIRE) &&
        s_stream.lock &&
        xSemaphoreTake(s_stream.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        current = s_stream.server == job.server &&
                  s_stream.session == job.session &&
                  s_stream.fd == job.fd &&
                  s_stream.stream_id == job.stream_id;
        xSemaphoreGive(s_stream.lock);
    }
    if (current &&
        (job.stream_id == 0U ||
         si_video_control_kvm_stream_is_current(job.stream_id))) {
        /* ESP-IDF marks a peer CLOSE as non-WebSocket before its asynchronous
         * session destruction runs.  An AU already queued behind that CLOSE is
         * therefore canceled work, not a network send failure.  Only an error
         * returned by a send that began against a live WebSocket is counted;
         * every path still returns the AU slot below. */
        httpd_ws_client_info_t client_before =
            httpd_ws_get_fd_info(job.server, job.fd);
        if (client_before == HTTPD_WS_CLIENT_WEBSOCKET) {
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_BINARY,
                .payload = job.payload,
                .len = job.frame_len,
            };
            send_attempted = true;
            int64_t started_us = esp_timer_get_time();
            err = httpd_ws_send_frame_async(job.server, job.fd, &frame);
            int64_t finished_us = esp_timer_get_time();
            __atomic_store_n(&pipeline->last_send_us,
                             (uint32_t)(finished_us - started_us),
                             __ATOMIC_RELEASE);
            if (err == ESP_OK) {
                (void)httpd_sess_update_lru_counter(job.server, job.fd);
                si_video_stream_metrics_record(job.stream_id, frame.len,
                                               si_monotonic_ms());
            }
        }
    }

    if (send_attempted && err != ESP_OK) {
        /* Revalidate after the failed write as well.  A peer CLOSE can race the
         * queued callback between its preflight and result classification; in
         * that case the failed call belongs to teardown, not to a live network
         * session.  A transport failure while the same WS is still current is
         * retained as a real failure. */
        httpd_ws_client_info_t client_after =
            httpd_ws_get_fd_info(job.server, job.fd);
        active_send_failure =
            client_after == HTTPD_WS_CLIENT_WEBSOCKET &&
            stream_still_current(job.session, job.fd);
    }
    if (send_attempted && (err == ESP_OK || active_send_failure)) {
        __atomic_store_n(&pipeline->send_result_session, job.session,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&pipeline->send_result, err, __ATOMIC_RELEASE);
    }
    if (active_send_failure) {
        __atomic_fetch_add(&s_send_failures_total, 1U, __ATOMIC_RELAXED);
        __atomic_store_n(&pipeline->encoder_reset_required, true,
                         __ATOMIC_RELEASE);
        ESP_LOGI(TAG, "H.264 WebSocket HTTPD send stopped: %s",
                 esp_err_to_name(err));
        stream_fail(job.session, job.fd);
    }
    pipeline_return_au_slot(pipeline, job.slot_id);
}

static esp_err_t pipeline_start_egress(si_h264_pipeline_t *pipeline)
{
    uint32_t all_free_mask = pipeline_all_free_au_mask(pipeline);
    if (!pipeline || all_free_mask == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    for (uint8_t slot_id = 0; slot_id < pipeline->au_slot_count; slot_id++) {
        pipeline->au_slots[slot_id].send_work.pipeline = pipeline;
    }
    __atomic_store_n(&pipeline->free_au_mask, all_free_mask,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&pipeline->send_stopping, false, __ATOMIC_RELEASE);
    __atomic_store_n(&pipeline->send_result, ESP_OK, __ATOMIC_RELEASE);
    __atomic_store_n(&pipeline->send_result_session, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&pipeline->last_send_us, 0, __ATOMIC_RELEASE);
    pipeline->egress_initialized = true;
    return ESP_OK;
}

static esp_err_t pipeline_stop_egress(si_h264_pipeline_t *pipeline)
{
    if (!pipeline || !pipeline->egress_initialized) {
        return ESP_OK;
    }
    __atomic_store_n(&pipeline->send_stopping, true, __ATOMIC_RELEASE);
    /* Every cleared bit owns a persistent send_work embedded in this
     * pipeline.  Wait until the dedicated HTTPD task has either sent or
     * rejected each work item before freeing its payload or the context. */
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout =
        pdMS_TO_TICKS(SI_H264_EGRESS_STOP_TIMEOUT_MS);
    const uint32_t all_free_mask = pipeline_all_free_au_mask(pipeline);
    while ((__atomic_load_n(&pipeline->free_au_mask, __ATOMIC_ACQUIRE) &
            all_free_mask) != all_free_mask) {
        if ((TickType_t)(xTaskGetTickCount() - started) >= timeout) {
            pipeline->teardown_poisoned = true;
            ESP_LOGE(TAG,
                     "H.264 egress teardown timed out; retaining codec memory for deferred recovery");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

static esp_err_t pipeline_release_internal(si_h264_pipeline_t *pipeline,
                                           bool release_codec_gate)
{
    if (!pipeline) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t worker_err = pipeline_stop_decode_worker(pipeline);
    esp_err_t egress_err = pipeline_stop_egress(pipeline);
    if (worker_err != ESP_OK || egress_err != ESP_OK) {
        pipeline->teardown_poisoned = true;
        return worker_err != ESP_OK ? worker_err : egress_err;
    }
    if (pipeline->h264_encoder) {
        if (pipeline->h264_open) {
            (void)esp_h264_enc_close(pipeline->h264_encoder);
        }
        if (pipeline->uses_preallocated_encoder) {
            __atomic_store_n(&s_preallocated_encoder_claimed, false,
                             __ATOMIC_RELEASE);
        } else {
            (void)esp_h264_enc_del(pipeline->h264_encoder);
        }
    }
    if (pipeline->jpeg_decoder) {
        (void)jpeg_del_decoder_engine(pipeline->jpeg_decoder);
    }
    if (pipeline->uses_preallocated_buffers) {
        __atomic_store_n(&s_preallocated_buffers_claimed, false,
                         __ATOMIC_RELEASE);
    } else {
        free(pipeline->yuv420);
        free(pipeline->yuv420_alt);
        for (uint8_t slot_id = 0; slot_id < SI_H264_AU_SLOT_COUNT;
             slot_id++) {
            if (pipeline->au_slots[slot_id].storage) {
                esp_h264_free(pipeline->au_slots[slot_id].storage);
            }
        }
    }
    const bool retain_codec_gate =
        pipeline->owns_codec_gate && !release_codec_gate;
    if (pipeline->owns_codec_gate && release_codec_gate) {
        __atomic_store_n(&s_codec_pipeline_claimed, false,
                         __ATOMIC_RELEASE);
    }
    h264_telemetry_pipeline_clear();
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->owns_codec_gate = retain_codec_gate;
    return ESP_OK;
}

static esp_err_t pipeline_release(si_h264_pipeline_t *pipeline)
{
    return pipeline_release_internal(pipeline, true);
}

static esp_err_t pipeline_recreate_jpeg_decoder(
    si_h264_pipeline_t *pipeline)
{
    if (!pipeline) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pipeline->jpeg_decoder) {
        esp_err_t err = jpeg_del_decoder_engine(pipeline->jpeg_decoder);
        pipeline->jpeg_decoder = NULL;
        pipeline->decode_worker.decoder = NULL;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JPEG decoder teardown failed: %s",
                     esp_err_to_name(err));
            return err;
        }
    }

    jpeg_decode_engine_cfg_t jpeg_cfg = {
        .intr_priority = 0,
        .timeout_ms = SI_H264_JPEG_TIMEOUT_MS,
    };
    esp_err_t err = jpeg_new_decoder_engine(&jpeg_cfg,
                                             &pipeline->jpeg_decoder);
    if (err == ESP_OK) {
        pipeline->decode_worker.decoder = pipeline->jpeg_decoder;
    }
    return err;
}

static esp_err_t h264_vendor_error_to_esp(esp_h264_err_t err)
{
    switch (err) {
    case ESP_H264_ERR_OK:
        return ESP_OK;
    case ESP_H264_ERR_ARG:
        return ESP_ERR_INVALID_ARG;
    case ESP_H264_ERR_MEM:
        return ESP_ERR_NO_MEM;
    case ESP_H264_ERR_UNSUPPORTED:
        return ESP_ERR_NOT_SUPPORTED;
    case ESP_H264_ERR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case ESP_H264_ERR_OVERFLOW:
        return ESP_ERR_INVALID_SIZE;
    case ESP_H264_ERR_FAIL:
    default:
        return ESP_FAIL;
    }
}

static esp_err_t pipeline_configure(si_h264_pipeline_t *pipeline,
                                    uint16_t width, uint16_t height,
                                    uint8_t fps,
                                    bool preclaimed_buffers,
                                    si_h264_resource_token_t resource_token)
{
    if (!h264_resource_owner_is(resource_token,
                                SI_H264_RESOURCE_OWNER_H264)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t padded_width = ((uint32_t)width + 15U) & ~15U;
    uint32_t padded_height = ((uint32_t)height + 15U) & ~15U;
    uint64_t raw_size_64 = (uint64_t)padded_width * padded_height * 3U / 2U;
    uint64_t primary_au_payload_size_64 = raw_size_64;
    if (primary_au_payload_size_64 < SI_CFG_VIDEO_H264_MAX_ACCESS_UNIT) {
        primary_au_payload_size_64 = SI_CFG_VIDEO_H264_MAX_ACCESS_UNIT;
    }
    uint64_t primary_au_storage_size_64 =
        primary_au_payload_size_64 + SI_H264_PAYLOAD_OFFSET;
    const uint64_t small_egress_storage_size_64 =
        SI_H264_SMALL_EGRESS_PAYLOAD_SIZE + SI_H264_PAYLOAD_OFFSET;
    if (raw_size_64 > UINT32_MAX ||
        primary_au_storage_size_64 > UINT32_MAX ||
        small_egress_storage_size_64 > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t primary_au_storage_size =
        (uint32_t)primary_au_storage_size_64;
    const uint32_t small_egress_storage_size =
        (uint32_t)small_egress_storage_size_64;

    if (!pipeline->owns_codec_gate) {
        esp_err_t gate_err =
            h264_runtime_gate_take(SI_H264_RESOURCE_CLAIM_WAIT_MS);
        if (gate_err != ESP_OK) {
            return gate_err;
        }
        if (!h264_resource_owner_is(resource_token,
                                    SI_H264_RESOURCE_OWNER_H264)) {
            h264_runtime_gate_give();
            ESP_LOGW(TAG,
                     "H.264 codec start deferred while Agent owns the internal-memory budget");
            return ESP_ERR_INVALID_STATE;
        }
        bool expected_pipeline = false;
        if (!__atomic_compare_exchange_n(&s_codec_pipeline_claimed,
                                         &expected_pipeline, true, false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            h264_runtime_gate_give();
            ESP_LOGW(TAG, "H.264 codec pipeline is already owned");
            return ESP_ERR_INVALID_STATE;
        }
        pipeline->owns_codec_gate = true;
        h264_runtime_gate_give();
    }

    /* A live pipeline already owns the only ESP32-P4 H.264 register bank;
     * a new pipeline acquires it above.  Preserve that ownership across the
     * teardown/rebuild so a concurrent diagnostic cannot claim the global
     * encoder between two resolutions. */
    esp_err_t release_err = pipeline_release_internal(pipeline, false);
    if (release_err != ESP_OK) {
        ESP_LOGE(TAG, "deferred H.264 reconfiguration teardown: %s",
                 esp_err_to_name(release_err));
        return release_err;
    }

    bool expected_buffers = false;
    bool use_preallocated_buffers = false;
    if (s_preallocated_yuv420 && s_preallocated_au[0] &&
        raw_size_64 <= s_preallocated_yuv420_capacity &&
        primary_au_storage_size <=
            s_preallocated_au_capacity[SI_H264_PRIMARY_AU_SLOT] &&
        s_preallocated_au[SI_H264_SMALL_EGRESS_SLOT] &&
        small_egress_storage_size <=
            s_preallocated_au_capacity[SI_H264_SMALL_EGRESS_SLOT]) {
        if (preclaimed_buffers) {
            use_preallocated_buffers = __atomic_load_n(
                &s_preallocated_buffers_claimed, __ATOMIC_ACQUIRE);
        } else {
            use_preallocated_buffers = __atomic_compare_exchange_n(
                &s_preallocated_buffers_claimed, &expected_buffers, true,
                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
    }
    if (preclaimed_buffers && !use_preallocated_buffers) {
        pipeline_release(pipeline);
        return ESP_ERR_INVALID_STATE;
    }
    if (use_preallocated_buffers) {
        pipeline->yuv420 = s_preallocated_yuv420;
        pipeline->yuv420_capacity = s_preallocated_yuv420_capacity;
        pipeline->yuv420_alt = s_preallocated_yuv420_alt;
        pipeline->yuv420_alt_capacity =
            s_preallocated_yuv420_alt_capacity;
        for (uint8_t slot_id = 0; slot_id < SI_H264_AU_SLOT_COUNT;
             slot_id++) {
            pipeline->au_slots[slot_id].storage =
                s_preallocated_au[slot_id];
            pipeline->au_slots[slot_id].storage_capacity =
                s_preallocated_au_capacity[slot_id];
        }
        pipeline->au_slot_count = SI_H264_AU_SLOT_COUNT;
        pipeline->uses_preallocated_buffers = true;
        ESP_LOGI(TAG,
                 "using startup-preallocated codec and bounded dual egress");
    }

    if (!pipeline->uses_preallocated_buffers) {
        const uint32_t storage_sizes[SI_H264_AU_SLOT_COUNT] = {
            [SI_H264_PRIMARY_AU_SLOT] = primary_au_storage_size,
            [SI_H264_SMALL_EGRESS_SLOT] = small_egress_storage_size,
        };
        for (uint8_t slot_id = 0; slot_id < SI_H264_AU_SLOT_COUNT;
             slot_id++) {
            uint32_t allocated = 0;
            pipeline->au_slots[slot_id].storage = esp_h264_aligned_calloc(
                128, 1, storage_sizes[slot_id], &allocated,
                ESP_H264_MEM_SPIRAM);
            pipeline->au_slots[slot_id].storage_capacity = allocated;
            if (!pipeline->au_slots[slot_id].storage ||
                allocated < storage_sizes[slot_id]) {
                if (pipeline->au_slots[slot_id].storage) {
                    esp_h264_free(pipeline->au_slots[slot_id].storage);
                    pipeline->au_slots[slot_id].storage = NULL;
                    pipeline->au_slots[slot_id].storage_capacity = 0;
                }
                log_pipeline_heap("access-unit allocation failed");
                ESP_LOGE(TAG,
                         "cannot allocate mandatory H.264 egress slot %u: need=%u got=%u",
                         (unsigned)slot_id,
                         (unsigned)storage_sizes[slot_id],
                         (unsigned)allocated);
                pipeline_release(pipeline);
                return ESP_ERR_NO_MEM;
            }
            pipeline->au_slot_count = slot_id + 1U;
        }

        jpeg_decode_memory_alloc_cfg_t raw_mem = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        pipeline->yuv420 = jpeg_alloc_decoder_mem(
            (size_t)raw_size_64, &raw_mem, &pipeline->yuv420_capacity);
        if (!pipeline->yuv420) {
            log_pipeline_heap("YUV allocation failed");
            ESP_LOGE(TAG, "cannot allocate %u-byte JPEG output in PSRAM",
                     (unsigned)raw_size_64);
            pipeline_release(pipeline);
            return ESP_ERR_NO_MEM;
        }
        pipeline->yuv420_alt = jpeg_alloc_decoder_mem(
            (size_t)raw_size_64, &raw_mem,
            &pipeline->yuv420_alt_capacity);
        if (!pipeline->yuv420_alt) {
            ESP_LOGW(TAG,
                     "second YUV surface unavailable; H.264 will use the serial fallback");
        }
    }

    /* Keep the empirically stable ESP32-P4 rev 3.2 lifecycle: create the JPEG
     * engine before opening H.264.  JPEG uses the generic DMA2D pool while
     * H.264 has its own DMA engine; their relevant shared resource is the
     * external-memory/cache path, not one common DMA2D channel. */
    esp_err_t err = pipeline_recreate_jpeg_decoder(pipeline);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decoder engine creation failed: %s",
                 esp_err_to_name(err));
        pipeline_release(pipeline);
        return err;
    }

    esp_h264_enc_cfg_hw_t h264_cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = (uint8_t)SI_CFG_VIDEO_H264_GOP,
        .fps = fps,
        .res = {
            .width = width,
            .height = height,
        },
        .rc = {
            .bitrate = SI_CFG_VIDEO_H264_BITRATE,
            .qp_min = SI_H264_QP_MIN,
            .qp_max = SI_H264_QP_MAX,
        },
    };

    /* ESP32-P4 exposes one hardware encoder register bank.  Keeping the
     * startup 1080p handle alive while creating a second 720p handle retains
     * the first handle's internal reference-frame allocation and also leaves
     * two handles describing the same global hardware.  Besides wasting the
     * scarce internal SRAM, returning to the old handle can reuse stale macro
     * block geometry.  Reconfigure the single reserved handle transactionally
     * whenever the capture dimensions change.  The codec gate above proves no
     * other pipeline can own or process the handle while it is replaced. */
    if (s_preallocated_encoder &&
        (width != s_preallocated_width || height != s_preallocated_height)) {
        if (__atomic_load_n(&s_preallocated_encoder_claimed,
                            __ATOMIC_ACQUIRE)) {
            ESP_LOGE(TAG, "cannot resize a claimed H.264 encoder");
            pipeline_release(pipeline);
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "rebuilding H.264 encoder for %ux%u -> %ux%u",
                 (unsigned)s_preallocated_width,
                 (unsigned)s_preallocated_height, (unsigned)width,
                 (unsigned)height);
        esp_h264_err_t delete_err =
            esp_h264_enc_del(s_preallocated_encoder);
        if (delete_err != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "H.264 encoder replacement delete failed: %d",
                     (int)delete_err);
            pipeline_release(pipeline);
            return ESP_FAIL;
        }
        s_preallocated_encoder = NULL;
        s_preallocated_width = 0;
        s_preallocated_height = 0;
        s_preallocated_fps = 0;
    }

    if (!s_preallocated_encoder) {
        esp_h264_err_t create_err =
            esp_h264_enc_hw_new(&h264_cfg, &s_preallocated_encoder);
        if (create_err != ESP_H264_ERR_OK) {
            s_preallocated_encoder = NULL;
            ESP_LOGE(TAG, "H.264 encoder creation failed: %d",
                     (int)create_err);
            log_pipeline_heap("encoder creation failed");
            pipeline_release(pipeline);
            return h264_vendor_error_to_esp(create_err);
        }
        s_preallocated_width = width;
        s_preallocated_height = height;
        s_preallocated_fps = fps;
    }

    bool expected_encoder = false;
    if (!__atomic_compare_exchange_n(&s_preallocated_encoder_claimed,
                                     &expected_encoder, true, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        ESP_LOGE(TAG, "reserved H.264 encoder is already claimed");
        pipeline_release(pipeline);
        return ESP_ERR_INVALID_STATE;
    }
    pipeline->h264_encoder = s_preallocated_encoder;
    pipeline->uses_preallocated_encoder = true;

    esp_h264_enc_param_hw_handle_t param = NULL;
    esp_h264_err_t param_err = esp_h264_enc_hw_get_param_hd(
        pipeline->h264_encoder, &param);
    if (param_err != ESP_H264_ERR_OK || !param ||
        esp_h264_enc_set_fps(&param->base, fps) != ESP_H264_ERR_OK ||
        esp_h264_enc_set_bitrate(&param->base,
                                 SI_CFG_VIDEO_H264_BITRATE) !=
            ESP_H264_ERR_OK) {
        pipeline_release(pipeline);
        return ESP_FAIL;
    }
    s_preallocated_fps = fps;
    ESP_LOGI(TAG, "using single reserved H.264 encoder for %ux%u@%u",
             (unsigned)width, (unsigned)height, (unsigned)fps);

    esp_h264_err_t h264_err = ESP_H264_ERR_OK;
    h264_err = esp_h264_enc_open(pipeline->h264_encoder);
    if (h264_err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "H.264 encoder open failed: %d", (int)h264_err);
        pipeline_release(pipeline);
        return ESP_FAIL;
    }
    pipeline->h264_open = true;
    log_h264_memory_and_clock_policy();
    pipeline->width = width;
    pipeline->height = height;
    pipeline->fps = fps;
    pipeline->pts_origin_valid = false;
    esp_err_t send_err = pipeline_start_egress(pipeline);
    if (send_err != ESP_OK) {
        ESP_LOGE(TAG, "H.264 egress unavailable: %s",
                 esp_err_to_name(send_err));
        pipeline_release(pipeline);
        return send_err;
    }
    if (pipeline->yuv420_alt && SI_CFG_VIDEO_H264_PIPELINE_OVERLAP) {
        esp_err_t worker_err = pipeline_start_decode_worker(pipeline);
        if (worker_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "JPEG pipeline worker unavailable; using serial fallback: %s",
                     esp_err_to_name(worker_err));
        }
    }
    ESP_LOGI(TAG,
             "pipeline ready: %ux%u@%u, bitrate=%u, zero-copy-jpeg, raw=%u x%u, primary-au=%u small-egress=%u slots=%u overlap=%d",
             (unsigned)width, (unsigned)height, (unsigned)fps,
             (unsigned)SI_CFG_VIDEO_H264_BITRATE,
             (unsigned)pipeline->yuv420_capacity,
             pipeline->yuv420_alt ? 2U : 1U,
             (unsigned)(pipeline->au_slots[SI_H264_PRIMARY_AU_SLOT]
                            .storage_capacity -
                        SI_H264_PAYLOAD_OFFSET),
             (unsigned)(pipeline->au_slots[SI_H264_SMALL_EGRESS_SLOT]
                            .storage_capacity -
                        SI_H264_PAYLOAD_OFFSET),
             (unsigned)pipeline->au_slot_count,
             pipeline->decode_worker.task != NULL);
    h264_telemetry_pipeline_publish(pipeline);
    return ESP_OK;
}

static esp_err_t h264_runtime_reserve_internal(
    si_h264_resource_token_t resource_token)
{
    if (!h264_resource_owner_is(resource_token,
                                SI_H264_RESOURCE_OWNER_H264)) {
        ESP_LOGW(TAG,
                 "H.264 resource reserve deferred while Agent owns the internal-memory budget");
        return ESP_ERR_INVALID_STATE;
    }

    s_preallocated_width = SI_CFG_VIDEO_WIDTH;
    s_preallocated_height = SI_CFG_VIDEO_HEIGHT;
    s_preallocated_fps = SI_CFG_VIDEO_FPS;
    ESP_LOGI(TAG, "startup heap before H.264 reserve: internal=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT));
    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = (uint8_t)SI_CFG_VIDEO_H264_GOP,
        .fps = s_preallocated_fps,
        .res = {
            .width = s_preallocated_width,
            .height = s_preallocated_height,
        },
        .rc = {
            .bitrate = SI_CFG_VIDEO_H264_BITRATE,
            .qp_min = SI_H264_QP_MIN,
            .qp_max = SI_H264_QP_MAX,
        },
    };
    if (!s_preallocated_encoder) {
        const size_t internal_free = heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t internal_largest = heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        /* An Agent worker releases its logical claim immediately before
         * vTaskDeleteWithCaps(), while IDF frees that internal stack from a
         * cleanup task.  Treat the short interval (and any other fragmented
         * low-memory state) as retryable instead of entering esp_h264, whose
         * strict-internal db_tmp allocation would otherwise fail. */
        if (internal_free < SI_H264_ENCODER_INTERNAL_MIN_FREE ||
            internal_largest < SI_H264_ENCODER_INTERNAL_MIN_LARGEST) {
            ESP_LOGW(TAG,
                     "H.264 resource reserve deferred: internal=%u largest=%u need=%u/%u",
                     (unsigned)internal_free, (unsigned)internal_largest,
                     (unsigned)SI_H264_ENCODER_INTERNAL_MIN_FREE,
                     (unsigned)SI_H264_ENCODER_INTERNAL_MIN_LARGEST);
            return ESP_ERR_INVALID_STATE;
        }
        esp_h264_err_t err = esp_h264_enc_hw_new(&cfg,
                                                  &s_preallocated_encoder);
        if (err != ESP_H264_ERR_OK) {
            s_preallocated_encoder = NULL;
            ESP_LOGE(TAG, "startup H.264 encoder reserve failed: %d", (int)err);
            return h264_vendor_error_to_esp(err);
        }
    }

    uint32_t padded_width = ((uint32_t)s_preallocated_width + 15U) & ~15U;
    uint32_t padded_height = ((uint32_t)s_preallocated_height + 15U) & ~15U;
    size_t raw_size = (size_t)(padded_width * padded_height * 3U / 2U);
    jpeg_decode_memory_alloc_cfg_t raw_mem = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t primary_au_payload_size = raw_size;
    if (primary_au_payload_size < SI_CFG_VIDEO_H264_MAX_ACCESS_UNIT) {
        primary_au_payload_size = SI_CFG_VIDEO_H264_MAX_ACCESS_UNIT;
    }
    if (primary_au_payload_size > SIZE_MAX - SI_H264_PAYLOAD_OFFSET) {
        (void)esp_h264_enc_del(s_preallocated_encoder);
        s_preallocated_encoder = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t storage_sizes[SI_H264_AU_SLOT_COUNT] = {
        [SI_H264_PRIMARY_AU_SLOT] =
            primary_au_payload_size + SI_H264_PAYLOAD_OFFSET,
        [SI_H264_SMALL_EGRESS_SLOT] =
            SI_H264_SMALL_EGRESS_PAYLOAD_SIZE + SI_H264_PAYLOAD_OFFSET,
    };
    const bool buffers_already_reserved =
        s_preallocated_yuv420 &&
        s_preallocated_yuv420_capacity >= raw_size &&
        s_preallocated_au[SI_H264_PRIMARY_AU_SLOT] &&
        s_preallocated_au_capacity[SI_H264_PRIMARY_AU_SLOT] >=
            storage_sizes[SI_H264_PRIMARY_AU_SLOT] &&
        s_preallocated_au[SI_H264_SMALL_EGRESS_SLOT] &&
        s_preallocated_au_capacity[SI_H264_SMALL_EGRESS_SLOT] >=
            storage_sizes[SI_H264_SMALL_EGRESS_SLOT];
    if (!buffers_already_reserved) {
        free(s_preallocated_yuv420);
        s_preallocated_yuv420 = NULL;
        s_preallocated_yuv420_capacity = 0;
        free(s_preallocated_yuv420_alt);
        s_preallocated_yuv420_alt = NULL;
        s_preallocated_yuv420_alt_capacity = 0;
        for (uint8_t slot_id = 0; slot_id < SI_H264_AU_SLOT_COUNT;
             slot_id++) {
            if (s_preallocated_au[slot_id]) {
                esp_h264_free(s_preallocated_au[slot_id]);
                s_preallocated_au[slot_id] = NULL;
            }
            s_preallocated_au_capacity[slot_id] = 0;
        }
        s_preallocated_yuv420 = jpeg_alloc_decoder_mem(
            raw_size, &raw_mem, &s_preallocated_yuv420_capacity);
        s_preallocated_yuv420_alt = jpeg_alloc_decoder_mem(
            raw_size, &raw_mem, &s_preallocated_yuv420_alt_capacity);
        for (uint8_t slot_id = 0; slot_id < SI_H264_AU_SLOT_COUNT;
             slot_id++) {
            uint32_t allocated = 0;
            s_preallocated_au[slot_id] = esp_h264_aligned_calloc(
                128, 1, storage_sizes[slot_id], &allocated,
                ESP_H264_MEM_SPIRAM);
            s_preallocated_au_capacity[slot_id] = allocated;
        }
    }
    const bool au_reserve_complete =
        s_preallocated_au[SI_H264_PRIMARY_AU_SLOT] &&
        s_preallocated_au_capacity[SI_H264_PRIMARY_AU_SLOT] >=
            storage_sizes[SI_H264_PRIMARY_AU_SLOT] &&
        s_preallocated_au[SI_H264_SMALL_EGRESS_SLOT] &&
        s_preallocated_au_capacity[SI_H264_SMALL_EGRESS_SLOT] >=
            storage_sizes[SI_H264_SMALL_EGRESS_SLOT];
    if (!s_preallocated_yuv420 || !au_reserve_complete) {
        free(s_preallocated_yuv420);
        s_preallocated_yuv420 = NULL;
        s_preallocated_yuv420_capacity = 0;
        free(s_preallocated_yuv420_alt);
        s_preallocated_yuv420_alt = NULL;
        s_preallocated_yuv420_alt_capacity = 0;
        for (uint8_t slot_id = 0; slot_id < SI_H264_AU_SLOT_COUNT;
             slot_id++) {
            if (s_preallocated_au[slot_id]) {
                esp_h264_free(s_preallocated_au[slot_id]);
                s_preallocated_au[slot_id] = NULL;
            }
            s_preallocated_au_capacity[slot_id] = 0;
        }
        (void)esp_h264_enc_del(s_preallocated_encoder);
        s_preallocated_encoder = NULL;
        ESP_LOGE(TAG, "startup codec buffer reserve failed");
        log_pipeline_heap("startup buffer reserve failed");
        return ESP_ERR_NO_MEM;
    }
    if (!s_preallocated_yuv420_alt) {
        ESP_LOGW(TAG,
                 "startup second YUV reserve failed; serial H.264 remains available");
    }
    ESP_LOGI(TAG,
             "startup H.264 resources reserved: %ux%u@%u zero-copy-jpeg yuv=%u x%u primary-au=%u small-egress=%u slots=%u internal=%u/%u psram=%u/%u",
             (unsigned)s_preallocated_width,
             (unsigned)s_preallocated_height,
             (unsigned)s_preallocated_fps,
             (unsigned)s_preallocated_yuv420_capacity,
             s_preallocated_yuv420_alt ? 2U : 1U,
             (unsigned)(s_preallocated_au_capacity[
                            SI_H264_PRIMARY_AU_SLOT] -
                        SI_H264_PAYLOAD_OFFSET),
             (unsigned)(s_preallocated_au_capacity[
                            SI_H264_SMALL_EGRESS_SLOT] -
                        SI_H264_PAYLOAD_OFFSET),
             (unsigned)SI_H264_AU_SLOT_COUNT,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM |
                                                        MALLOC_CAP_8BIT));
    return ESP_OK;
}

static esp_err_t h264_runtime_reserve(
    si_h264_resource_token_t resource_token)
{
    if (!__atomic_load_n(&s_reference_workspace_reserved,
                         __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&s_reference_workspace_internal,
                         __ATOMIC_ACQUIRE)) {
        esp_err_t reference_error = __atomic_load_n(
            &s_reference_workspace_error, __ATOMIC_ACQUIRE);
        return reference_error == ESP_OK ? ESP_ERR_INVALID_STATE
                                         : reference_error;
    }
    if (!h264_resource_owner_is(resource_token,
                                SI_H264_RESOURCE_OWNER_H264)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t gate_err =
        h264_runtime_gate_take(SI_H264_RESOURCE_RESERVE_WAIT_MS);
    if (gate_err != ESP_OK) {
        return gate_err;
    }
    if (!h264_resource_owner_is(resource_token,
                                SI_H264_RESOURCE_OWNER_H264)) {
        h264_runtime_gate_give();
        return ESP_ERR_INVALID_STATE;
    }
    if (__atomic_load_n(&s_runtime_available, __ATOMIC_ACQUIRE)) {
        h264_runtime_gate_give();
        return ESP_OK;
    }
    if (__atomic_load_n(&s_runtime_reserve_terminal_failure,
                        __ATOMIC_ACQUIRE)) {
        esp_err_t terminal_error =
            __atomic_load_n(&s_resource_error, __ATOMIC_ACQUIRE);
        h264_runtime_gate_give();
        return terminal_error;
    }
    __atomic_store_n(&s_resource_probe_attempted, true, __ATOMIC_RELEASE);
    esp_err_t err = h264_runtime_reserve_internal(resource_token);
    __atomic_store_n(&s_resource_error, err, __ATOMIC_RELEASE);
    __atomic_store_n(&s_runtime_available, err == ESP_OK,
                     __ATOMIC_RELEASE);
    const bool invariant_failure =
        err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE ||
        err == ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK && invariant_failure) {
        /* Only a deterministic configuration/ABI invariant is terminal for
         * this boot. Allocation pressure, owner contention, timeouts, and
         * opaque hardware failures remain retryable and must never discard
         * the boot-validated strict-internal reference workspace. */
        __atomic_store_n(&s_runtime_reserve_terminal_failure, true,
                         __ATOMIC_RELEASE);
    }
    h264_runtime_gate_give();
    return err;
}

esp_err_t si_h264_stream_initialize(void)
{
    /* The UVC frame arena and internal-DMA budget are reserved first, without
     * starting USB/UVC tasks.  Reserve only the small latency-critical H.264
     * reference workspace at this barrier; all larger codec resources remain
     * lazy so a failed experiment cannot take down MJPEG capture. */
    if (__atomic_load_n(&s_service_initialized, __ATOMIC_ACQUIRE)) {
        return ESP_OK;
    }
    bool expected_claim = false;
    if (!__atomic_compare_exchange_n(
            &s_reference_workspace_reserve_claimed, &expected_claim, true,
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        const int64_t wait_deadline_us =
            esp_timer_get_time() +
            (int64_t)SI_H264_RESOURCE_RESERVE_WAIT_MS * 1000LL;
        while (!__atomic_load_n(&s_reference_workspace_reserve_attempted,
                                __ATOMIC_ACQUIRE) &&
               esp_timer_get_time() < wait_deadline_us) {
            vTaskDelay(1);
        }
        if (!__atomic_load_n(&s_reference_workspace_reserve_attempted,
                             __ATOMIC_ACQUIRE)) {
            return ESP_ERR_TIMEOUT;
        }
        return __atomic_load_n(&s_reference_workspace_reserved,
                               __ATOMIC_ACQUIRE)
                   ? ESP_OK
                   : __atomic_load_n(&s_reference_workspace_error,
                                     __ATOMIC_ACQUIRE);
    }
    if (!si_video_boot_memory_ready()) {
        __atomic_store_n(&s_reference_workspace_reserved, false,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s_reference_workspace_internal, false,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s_reference_workspace_required_bytes, 0U,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s_reference_workspace_capacity_bytes, 0U,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s_reference_workspace_error,
                         ESP_ERR_INVALID_STATE, __ATOMIC_RELEASE);
        __atomic_store_n(&s_resource_error, ESP_ERR_INVALID_STATE,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s_reference_workspace_reserve_attempted, true,
                         __ATOMIC_RELEASE);
        ESP_LOGE(TAG,
                 "H.264 disabled: UVC boot-memory barrier was not established");
        return ESP_ERR_INVALID_STATE;
    }
    __atomic_store_n(&s_resource_probe_attempted, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_runtime_reserve_claimed, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_resource_owner_token, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_resource_owner_epoch, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_runtime_reserve_terminal_failure, false,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_runtime_available, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_session_active, false, __ATOMIC_RELEASE);
    h264_telemetry_pipeline_clear();
    __atomic_store_n(&s_backpressure_drops_total, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_decode_failures_total, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_encode_failures_total, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_send_failures_total, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sessions_started_total, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sessions_failed_total, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pipeline_starts_total, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_access_units_total, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_sequence, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_timestamp_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_jpeg_bytes, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_max_jpeg_bytes, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_au_bytes, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_max_au_bytes, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_primary_egress_access_units_total, 0U,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_small_egress_access_units_total, 0U,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_resource_error, ESP_ERR_NOT_FINISHED,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_restore_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_teardown_poisoned, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_restore_error, ESP_OK, __ATOMIC_RELEASE);

    uint32_t required_bytes = 0;
    uint32_t capacity_bytes = 0;
    ESP_LOGI(TAG,
             "early H.264 reference reserve heap: internal=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT));
    esp_h264_err_t reserve_result =
        esp_h264_exoanchor_reserve_ref_workspace(
            SI_CFG_VIDEO_WIDTH, &required_bytes, &capacity_bytes);
    esp_err_t reserve_error = reserve_result == ESP_H264_ERR_OK
                                  ? ESP_OK
                                  : reserve_result == ESP_H264_ERR_MEM
                                        ? ESP_ERR_NO_MEM
                                        : ESP_FAIL;
    __atomic_store_n(&s_reference_workspace_required_bytes, required_bytes,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_reference_workspace_capacity_bytes, capacity_bytes,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_reference_workspace_reserved,
                     reserve_error == ESP_OK, __ATOMIC_RELEASE);
    __atomic_store_n(&s_reference_workspace_internal,
                     reserve_error == ESP_OK, __ATOMIC_RELEASE);
    __atomic_store_n(&s_reference_workspace_error, reserve_error,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_reference_workspace_reserve_attempted, true,
                     __ATOMIC_RELEASE);
    if (reserve_error != ESP_OK) {
        esp_h264_exoanchor_release_cached_workspaces();
        __atomic_store_n(&s_resource_error, reserve_error,
                         __ATOMIC_RELEASE);
        ESP_LOGE(TAG,
                 "H.264 disabled: %u-byte internal reference workspace reserve failed",
                 (unsigned)required_bytes);
        return reserve_error;
    }

    __atomic_store_n(&s_service_initialized, true, __ATOMIC_RELEASE);
    ESP_LOGI(TAG,
             "H.264 service initialized; internal reference=%u/%u bytes, remaining codec resources deferred",
             (unsigned)required_bytes, (unsigned)capacity_bytes);
    return ESP_OK;
}

void si_h264_stream_get_status(si_h264_stream_status_t *status)
{
    if (!status) {
        return;
    }
    const bool active = __atomic_load_n(&s_session_active,
                                        __ATOMIC_ACQUIRE);
    const uint32_t updated_ms = __atomic_load_n(&s_metrics_updated_ms,
                                                __ATOMIC_ACQUIRE);
    const uint32_t metrics_age_ms = updated_ms == 0U ? 0U :
                                    si_monotonic_ms() - updated_ms;
    const bool metrics_valid = active && updated_ms != 0U &&
                               metrics_age_ms <= SI_H264_METRICS_STALE_MS;
    *status = (si_h264_stream_status_t) {
        .service_initialized = __atomic_load_n(
            &s_service_initialized, __ATOMIC_ACQUIRE),
        .reference_workspace_reserve_attempted = __atomic_load_n(
            &s_reference_workspace_reserve_attempted, __ATOMIC_ACQUIRE),
        .reference_workspace_reserved = __atomic_load_n(
            &s_reference_workspace_reserved, __ATOMIC_ACQUIRE),
        .reference_workspace_internal = __atomic_load_n(
            &s_reference_workspace_internal, __ATOMIC_ACQUIRE),
        .resource_probe_attempted = __atomic_load_n(
            &s_resource_probe_attempted, __ATOMIC_ACQUIRE),
        .available = __atomic_load_n(&s_runtime_available,
                                     __ATOMIC_ACQUIRE),
        .session_active = active,
        .pipeline_ready = __atomic_load_n(&s_pipeline_ready,
                                          __ATOMIC_ACQUIRE),
        .restore_pending = __atomic_load_n(&s_restore_pending,
                                           __ATOMIC_ACQUIRE),
        .teardown_poisoned = __atomic_load_n(&s_teardown_poisoned,
                                             __ATOMIC_ACQUIRE),
        .overlap_enabled = __atomic_load_n(&s_pipeline_overlap_enabled,
                                           __ATOMIC_ACQUIRE),
        .serial_fallback = __atomic_load_n(&s_pipeline_serial_fallback,
                                           __ATOMIC_ACQUIRE),
        .metrics_valid = metrics_valid,
        .width = (uint16_t)__atomic_load_n(&s_pipeline_width,
                                           __ATOMIC_ACQUIRE),
        .height = (uint16_t)__atomic_load_n(&s_pipeline_height,
                                            __ATOMIC_ACQUIRE),
        .target_fps = (uint8_t)__atomic_load_n(&s_pipeline_target_fps,
                                               __ATOMIC_ACQUIRE),
        .yuv_surfaces = (uint8_t)__atomic_load_n(&s_pipeline_yuv_surfaces,
                                                 __ATOMIC_ACQUIRE),
        .au_slots = (uint8_t)__atomic_load_n(&s_pipeline_au_slots,
                                             __ATOMIC_ACQUIRE),
        .primary_au_capacity_bytes = __atomic_load_n(
            &s_pipeline_primary_au_capacity, __ATOMIC_ACQUIRE),
        .small_egress_capacity_bytes = __atomic_load_n(
            &s_pipeline_small_egress_capacity, __ATOMIC_ACQUIRE),
        .reference_workspace_required_bytes = __atomic_load_n(
            &s_reference_workspace_required_bytes, __ATOMIC_ACQUIRE),
        .reference_workspace_capacity_bytes = __atomic_load_n(
            &s_reference_workspace_capacity_bytes, __ATOMIC_ACQUIRE),
        .metrics_age_ms = metrics_valid ? metrics_age_ms : 0U,
        .metrics_window_ms = __atomic_load_n(&s_metrics_window_ms,
                                             __ATOMIC_ACQUIRE),
        .source_fps_x100 = __atomic_load_n(&s_metrics_source_fps_x100,
                                           __ATOMIC_ACQUIRE),
        .output_fps_x100 = __atomic_load_n(&s_metrics_output_fps_x100,
                                           __ATOMIC_ACQUIRE),
        .source_drops = __atomic_load_n(&s_metrics_source_drops,
                                        __ATOMIC_ACQUIRE),
        .h264_bitrate_bps = __atomic_load_n(&s_metrics_h264_bitrate_bps,
                                            __ATOMIC_ACQUIRE),
        .decode_avg_us = __atomic_load_n(&s_metrics_decode_avg_us,
                                         __ATOMIC_ACQUIRE),
        .encode_avg_us = __atomic_load_n(&s_metrics_encode_avg_us,
                                         __ATOMIC_ACQUIRE),
        .send_avg_us = __atomic_load_n(&s_metrics_send_avg_us,
                                       __ATOMIC_ACQUIRE),
        .decode_max_us = __atomic_load_n(&s_metrics_decode_max_us,
                                         __ATOMIC_ACQUIRE),
        .encode_max_us = __atomic_load_n(&s_metrics_encode_max_us,
                                         __ATOMIC_ACQUIRE),
        .send_max_us = __atomic_load_n(&s_metrics_send_max_us,
                                       __ATOMIC_ACQUIRE),
        .overlap_pairs = __atomic_load_n(&s_metrics_overlap_pairs,
                                         __ATOMIC_ACQUIRE),
        .overlap_misses = __atomic_load_n(&s_metrics_overlap_misses,
                                          __ATOMIC_ACQUIRE),
        .overlap_wait_timeouts = __atomic_load_n(
            &s_metrics_overlap_wait_timeouts, __ATOMIC_ACQUIRE),
        .backpressure_drops_total = __atomic_load_n(
            &s_backpressure_drops_total, __ATOMIC_ACQUIRE),
        .decode_failures_total = __atomic_load_n(
            &s_decode_failures_total, __ATOMIC_ACQUIRE),
        .encode_failures_total = __atomic_load_n(
            &s_encode_failures_total, __ATOMIC_ACQUIRE),
        .send_failures_total = __atomic_load_n(
            &s_send_failures_total, __ATOMIC_ACQUIRE),
        .sessions_started_total = __atomic_load_n(
            &s_sessions_started_total, __ATOMIC_ACQUIRE),
        .sessions_failed_total = __atomic_load_n(
            &s_sessions_failed_total, __ATOMIC_ACQUIRE),
        .pipeline_starts_total = __atomic_load_n(
            &s_pipeline_starts_total, __ATOMIC_ACQUIRE),
        .access_units_total = __atomic_load_n(
            &s_access_units_total, __ATOMIC_ACQUIRE),
        .last_sequence = __atomic_load_n(&s_last_sequence,
                                         __ATOMIC_ACQUIRE),
        .last_timestamp_ms = __atomic_load_n(&s_last_timestamp_ms,
                                             __ATOMIC_ACQUIRE),
        .last_jpeg_bytes = __atomic_load_n(&s_last_jpeg_bytes,
                                           __ATOMIC_ACQUIRE),
        .max_jpeg_bytes = __atomic_load_n(&s_max_jpeg_bytes,
                                          __ATOMIC_ACQUIRE),
        .last_au_bytes = __atomic_load_n(&s_last_au_bytes,
                                         __ATOMIC_ACQUIRE),
        .max_au_bytes = __atomic_load_n(&s_max_au_bytes,
                                        __ATOMIC_ACQUIRE),
        .primary_egress_access_units_total = __atomic_load_n(
            &s_primary_egress_access_units_total, __ATOMIC_ACQUIRE),
        .small_egress_access_units_total = __atomic_load_n(
            &s_small_egress_access_units_total, __ATOMIC_ACQUIRE),
        .resource_error = __atomic_load_n(&s_resource_error,
                                          __ATOMIC_ACQUIRE),
        .reference_workspace_error = __atomic_load_n(
            &s_reference_workspace_error, __ATOMIC_ACQUIRE),
        .restore_error = __atomic_load_n(&s_restore_error,
                                         __ATOMIC_ACQUIRE),
    };
}

static void inspect_annex_b(const uint8_t *data, size_t len,
                            bool *has_sps, bool *has_pps, bool *has_idr)
{
    *has_sps = false;
    *has_pps = false;
    *has_idr = false;
    for (size_t i = 0; i + 4 < len; i++) {
        size_t nal = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            nal = i + 3;
        } else if (i + 5 < len && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            nal = i + 4;
        }
        if (nal == 0 || nal >= len) {
            continue;
        }
        uint8_t type = data[nal] & 0x1fU;
        *has_sps |= type == 7U;
        *has_pps |= type == 8U;
        *has_idr |= type == 5U;
    }
}

static void h264_self_test_source_release(
    const uint8_t *jpeg, bool jpeg_owned,
    si_video_jpeg_view_t *borrowed_view, bool buffers_preclaimed)
{
    if (borrowed_view && borrowed_view->token) {
        si_video_release_jpeg(borrowed_view);
    } else if (jpeg_owned) {
        free((void *)jpeg);
    }
    if (buffers_preclaimed) {
        __atomic_store_n(&s_preallocated_buffers_claimed, false,
                         __ATOMIC_RELEASE);
    }
}

static esp_err_t h264_stream_self_test_jpeg(
    const uint8_t *jpeg, size_t jpeg_len, uint32_t frame_id,
    bool jpeg_owned, si_video_jpeg_view_t *borrowed_view,
    bool buffers_preclaimed, uint32_t requested_fps_x100,
    si_h264_resource_token_t resource_token)
{
    esp_err_t err = ESP_OK;
    jpeg_decode_picture_info_t info = {0};
    err = jpeg_decoder_get_info(jpeg, jpeg_len, &info);
    if (err != ESP_OK) {
        h264_self_test_source_release(jpeg, jpeg_owned, borrowed_view,
                                      buffers_preclaimed);
        ESP_LOGE(TAG, "self-test JPEG header failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    if (requested_fps_x100 != 0U &&
        !h264_output_mode_supported(info.width, info.height,
                                    requested_fps_x100)) {
        h264_self_test_source_release(jpeg, jpeg_owned, borrowed_view,
                                      buffers_preclaimed);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (requested_fps_x100 == 0U) {
        requested_fps_x100 = h264_output_fps_cap(info.width, info.height) *
                             100U;
    }
    uint8_t fps = h264_output_fps_for_mode(
        info.width, info.height, requested_fps_x100);
    si_h264_pipeline_t pipeline = {0};
    ESP_LOGI(TAG, "self-test heap before codec: internal=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT));
    err = pipeline_configure(&pipeline, (uint16_t)info.width,
                             (uint16_t)info.height, fps,
                             buffers_preclaimed, resource_token);
    if (err != ESP_OK) {
        pipeline_release(&pipeline);
        h264_self_test_source_release(jpeg, jpeg_owned, borrowed_view,
                                      buffers_preclaimed);
        ESP_LOGE(TAG, "self-test pipeline failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_YUV420,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT709,
    };
    uint32_t yuv_len = 0;
    err = h264_jpeg_decoder_process(pipeline.jpeg_decoder, &decode_cfg,
                               jpeg, jpeg_len, pipeline.yuv420,
                               (uint32_t)pipeline.yuv420_capacity, &yuv_len);
    if (err != ESP_OK) {
        pipeline_release(&pipeline);
        h264_self_test_source_release(jpeg, jpeg_owned, borrowed_view,
                                      buffers_preclaimed);
        ESP_LOGE(TAG, "self-test JPEG decode failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Isolate JPEG-engine reuse from the following H.264 transaction.  A
     * second decode here happens before H.264 has touched the shared YUV
     * buffer, so a failure proves that the decoder lifecycle itself is the
     * boundary we must repair. */
    uint32_t repeated_yuv_len = 0;
    err = h264_jpeg_decoder_process(pipeline.jpeg_decoder, &decode_cfg,
                               jpeg, jpeg_len, pipeline.yuv420,
                               (uint32_t)pipeline.yuv420_capacity,
                               &repeated_yuv_len);
    ESP_LOGI(TAG, "self-test repeated JPEG decode: %s yuv=%u/%u",
             esp_err_to_name(err), (unsigned)repeated_yuv_len,
             (unsigned)yuv_len);
    if (err != ESP_OK || repeated_yuv_len != yuv_len) {
        pipeline_release(&pipeline);
        h264_self_test_source_release(jpeg, jpeg_owned, borrowed_view,
                                      buffers_preclaimed);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    /* The IDF JPEG pool owns DMA2D interrupts for every channel.  It must
     * not remain installed while esp-h264 drives those channels directly. */
    err = jpeg_del_decoder_engine(pipeline.jpeg_decoder);
    pipeline.jpeg_decoder = NULL;
    pipeline.decode_worker.decoder = NULL;
    if (err != ESP_OK) {
        pipeline_release(&pipeline);
        h264_self_test_source_release(jpeg, jpeg_owned, borrowed_view,
                                      buffers_preclaimed);
        return err;
    }

    const uint8_t au_slot_id = SI_H264_PRIMARY_AU_SLOT;
    if (!pipeline_claim_au_slot(&pipeline, au_slot_id)) {
        pipeline_release(&pipeline);
        h264_self_test_source_release(jpeg, jpeg_owned, borrowed_view,
                                      buffers_preclaimed);
        return ESP_ERR_INVALID_STATE;
    }
    si_h264_au_slot_t *au_slot = &pipeline.au_slots[au_slot_id];
    esp_h264_enc_in_frame_t input = {
        .raw_data = {.buffer = pipeline.yuv420, .len = yuv_len},
    };
    esp_h264_enc_out_frame_t output = {
        .raw_data = {
            .buffer = au_slot->storage + SI_H264_PAYLOAD_OFFSET,
            .len = h264_encoder_output_capacity(
                au_slot->storage_capacity),
        },
    };
    esp_h264_err_t h264_err = ESP_H264_ERR_OK;
    bool has_sps = false;
    bool has_pps = false;
    bool has_idr = false;
    uint32_t encoded_frames = 0;
    uint32_t idr_frames = 0;
    uint32_t total_bytes = 0;
    uint64_t encode_us_total = 0;
    uint32_t min_bytes = UINT32_MAX;
    uint32_t max_bytes = 0;
    uint32_t started_ms = si_monotonic_ms();
    for (uint32_t index = 0; index < SI_H264_SELF_TEST_FRAMES; index++) {
        input.pts = (uint32_t)(((uint64_t)index * 90000U) / fps);
        output.length = 0;
        int64_t encode_started_us = esp_timer_get_time();
        h264_err = esp_h264_enc_process(pipeline.h264_encoder, &input,
                                        &output);
        encode_us_total += esp_timer_get_time() - encode_started_us;
        if (h264_err != ESP_H264_ERR_OK || output.length == 0 ||
            output.length > output.raw_data.len) {
            break;
        }
        /* Exercise the same immutable input boundary as the live path. */
        err = pipeline_recreate_jpeg_decoder(&pipeline);
        if (err != ESP_OK) {
            h264_err = ESP_H264_ERR_FAIL;
            break;
        }
        uint32_t alternating_yuv_len = 0;
        err = h264_jpeg_decoder_process(pipeline.jpeg_decoder, &decode_cfg,
                                   jpeg, jpeg_len,
                                   pipeline.yuv420,
                                   (uint32_t)pipeline.yuv420_capacity,
                                   &alternating_yuv_len);
        esp_err_t jpeg_release_err =
            jpeg_del_decoder_engine(pipeline.jpeg_decoder);
        pipeline.jpeg_decoder = NULL;
        pipeline.decode_worker.decoder = NULL;
        if (err == ESP_OK && jpeg_release_err != ESP_OK) {
            err = jpeg_release_err;
        }
        if (err != ESP_OK || alternating_yuv_len != yuv_len) {
            ESP_LOGE(TAG,
                     "self-test alternating JPEG decode failed after H.264 frame %u: %s yuv=%u/%u",
                     (unsigned)(index + 1U), esp_err_to_name(err),
                     (unsigned)alternating_yuv_len, (unsigned)yuv_len);
            h264_err = ESP_H264_ERR_FAIL;
            break;
        }
        bool frame_sps = false;
        bool frame_pps = false;
        bool frame_idr = false;
        inspect_annex_b(output.raw_data.buffer, output.length,
                        &frame_sps, &frame_pps, &frame_idr);
        has_sps |= frame_sps;
        has_pps |= frame_pps;
        has_idr |= frame_idr;
        idr_frames += frame_idr ? 1U : 0U;
        encoded_frames++;
        total_bytes += output.length;
        if (output.length < min_bytes) {
            min_bytes = output.length;
        }
        if (output.length > max_bytes) {
            max_bytes = output.length;
        }
    }
    uint32_t elapsed_ms = si_monotonic_ms() - started_ms;
    ESP_LOGI(TAG,
             "self-test result: jpeg=%uB frame=%u yuv=%uB encode=%d frames=%u/%u elapsed=%ums throughput=%.2ffps encode_avg=%uus au=%u..%uB avg=%uB idr_frames=%u sps=%d pps=%d idr=%d",
             (unsigned)jpeg_len, (unsigned)frame_id, (unsigned)yuv_len,
             (int)h264_err, (unsigned)encoded_frames,
             (unsigned)SI_H264_SELF_TEST_FRAMES, (unsigned)elapsed_ms,
             elapsed_ms ? (encoded_frames * 1000.0) / elapsed_ms : 0.0,
             encoded_frames
                 ? (unsigned)(encode_us_total / encoded_frames)
                 : 0U,
             encoded_frames ? (unsigned)min_bytes : 0,
             (unsigned)max_bytes,
             encoded_frames ? (unsigned)(total_bytes / encoded_frames) : 0,
             (unsigned)idr_frames, has_sps, has_pps, has_idr);
    pipeline_return_au_slot(&pipeline, au_slot_id);
    pipeline_release(&pipeline);
    h264_self_test_source_release(jpeg, jpeg_owned, borrowed_view,
                                  buffers_preclaimed);
    log_pipeline_heap("self-test complete");
    return h264_err == ESP_H264_ERR_OK &&
                   encoded_frames == SI_H264_SELF_TEST_FRAMES && has_sps &&
                   has_pps && has_idr
               ? ESP_OK
               : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t si_h264_stream_self_test(void)
{
    si_video_status_t initial_video = {0};
    si_video_get_status(&initial_video);
    const uint32_t target_width = initial_video.target_width != 0U
                                      ? initial_video.target_width
                                      : initial_video.width;
    const uint32_t target_height = initial_video.target_height != 0U
                                       ? initial_video.target_height
                                       : initial_video.height;
    if (!h264_output_mode_supported(target_width, target_height,
                                    initial_video.target_fps_x100)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Reject unsupported capture contracts before the diagnostic can commit
     * the boot-lifetime YUV/AU/encoder reservation or latch a terminal reserve
     * failure.  The product WebSocket path applies the same ordering. */
    si_h264_resource_token_t resource_token = 0U;
    esp_err_t err =
        h264_stream_claim_h264_resources(&resource_token);
    if (err != ESP_OK) {
        return err;
    }
    err = h264_runtime_reserve(resource_token);
    if (err != ESP_OK) {
        (void)h264_stream_release_h264_resources(resource_token);
        ESP_LOGE(TAG, "reserve H.264 self-test resources: %s",
                 esp_err_to_name(err));
        return err;
    }
    if (!initial_video.capture_enabled || !initial_video.streaming) {
        /* Diagnostics may request observation capture, but must never mint or
         * renew the manual KVM HID authority without a bound browser session. */
        si_video_control_keep_agent_alive();
        err = si_video_control_apply();
        if (err != ESP_OK) {
            (void)h264_stream_release_h264_resources(resource_token);
            ESP_LOGE(TAG, "self-test start capture: %s",
                     esp_err_to_name(err));
            return err;
        }
        /* The retained UVC stream may already be producing before this
         * diagnostic acquires KVM.  Do not impose a fixed settle delay: the
         * bounded MJPEG validator below is the authoritative frame gate. */
        (void)si_video_flush_h264_jpeg();
    }

    si_video_jpeg_view_t jpeg_view = {0};
    err = ESP_ERR_NOT_FOUND;
    for (int attempt = 0; attempt < 60; attempt++) {
        err = si_video_borrow_jpeg_if_new(UINT32_MAX, &jpeg_view);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "self-test JPEG unavailable: %s",
                 esp_err_to_name(err));
        (void)h264_stream_release_h264_resources(resource_token);
        return err;
    }
    err = h264_stream_self_test_jpeg(
        jpeg_view.data, jpeg_view.len, jpeg_view.frame_id, false,
        &jpeg_view, false, initial_video.target_fps_x100, resource_token);
    if (!h264_stream_release_h264_resources(resource_token) &&
        err == ESP_OK) {
        err = ESP_ERR_INVALID_STATE;
    }
    return err;
}

esp_err_t si_h264_stream_self_test_file(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat file_info = {0};
    if (stat(path, &file_info) != 0 || !S_ISREG(file_info.st_mode) ||
        file_info.st_size <= 0 ||
        (uint64_t)file_info.st_size > SI_H264_MAX_JPEG_INPUT_SIZE) {
        ESP_LOGE(TAG, "stress JPEG is missing or outside 1..%u bytes: %s",
                 (unsigned)SI_H264_MAX_JPEG_INPUT_SIZE, path);
        return ESP_ERR_INVALID_SIZE;
    }
    /* Invalid diagnostic inputs must remain side-effect free: do not commit
     * boot-lifetime codec buffers or a terminal reserve failure until the
     * bounded regular-file contract has passed. */
    si_h264_resource_token_t resource_token = 0U;
    esp_err_t err =
        h264_stream_claim_h264_resources(&resource_token);
    if (err != ESP_OK) {
        return err;
    }
    err = h264_runtime_reserve(resource_token);
    if (err != ESP_OK) {
        (void)h264_stream_release_h264_resources(resource_token);
        ESP_LOGE(TAG, "reserve H.264 file self-test resources: %s",
                 esp_err_to_name(err));
        return err;
    }
    size_t jpeg_len = (size_t)file_info.st_size;
    uint8_t *jpeg = heap_caps_aligned_alloc(
        128U, jpeg_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg) {
        (void)h264_stream_release_h264_resources(resource_token);
        return ESP_ERR_NO_MEM;
    }
    FILE *file = fopen(path, "rb");
    if (!file || fread(jpeg, 1, jpeg_len, file) != jpeg_len) {
        if (file) {
            fclose(file);
        }
        free(jpeg);
        (void)h264_stream_release_h264_resources(resource_token);
        return ESP_FAIL;
    }
    fclose(file);
    ESP_LOGI(TAG, "starting file-backed H.264 stress test: %s (%u bytes)",
             path, (unsigned)jpeg_len);
    err = h264_stream_self_test_jpeg(jpeg, jpeg_len, 0, true, NULL, false,
                                     0U, resource_token);
    if (!h264_stream_release_h264_resources(resource_token) &&
        err == ESP_OK) {
        err = ESP_ERR_INVALID_STATE;
    }
    return err;
}

static bool stream_snapshot(httpd_handle_t *server, int *fd,
                            uint32_t *session, uint32_t *stream_id,
                            si_h264_resource_token_t *resource_token)
{
    bool active = false;
    if (s_stream.lock &&
        xSemaphoreTake(s_stream.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        active = s_stream.server && s_stream.fd >= 0 && s_stream.session != 0;
        if (active) {
            *server = s_stream.server;
            *fd = s_stream.fd;
            *session = s_stream.session;
            *stream_id = s_stream.stream_id;
            *resource_token = s_stream.resource_token;
        }
        xSemaphoreGive(s_stream.lock);
    }
    return active;
}

static bool stream_still_current(uint32_t session, int fd)
{
    bool current = false;
    if (s_stream.lock &&
        xSemaphoreTake(s_stream.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        current = s_stream.session == session && s_stream.fd == fd;
        xSemaphoreGive(s_stream.lock);
    }
    return current;
}

static void stream_detach_internal(uint32_t session, int fd,
                                   bool close_session)
{
    if (!s_stream.lock ||
        xSemaphoreTake(s_stream.lock,
                       pdMS_TO_TICKS(SI_H264_STREAM_LOCK_TIMEOUT_MS)) !=
            pdTRUE) {
        ESP_LOGE(TAG, "H.264 stream detach lock timed out");
        return;
    }
    httpd_handle_t server = NULL;
    bool matched = false;
    if (s_stream.session == session && s_stream.fd == fd) {
        server = s_stream.server;
        matched = true;
        s_stream.server = NULL;
        s_stream.fd = -1;
        s_stream.stream_id = 0;
        __atomic_store_n(&s_session_active, false, __ATOMIC_RELEASE);
        __atomic_store_n(&s_restore_pending, true, __ATOMIC_RELEASE);
        if (close_session) {
            __atomic_fetch_add(&s_sessions_failed_total, 1U,
                               __ATOMIC_RELAXED);
        }
    }
    xSemaphoreGive(s_stream.lock);
    if (close_session && matched && server) {
        esp_err_t close_err = httpd_sess_trigger_close(server, fd);
        if (close_err != ESP_OK) {
            ESP_LOGW(TAG, "H.264 failed session close request: %s",
                     esp_err_to_name(close_err));
        }
    }
}

static void stream_detach(uint32_t session, int fd)
{
    stream_detach_internal(session, fd, false);
}

static void stream_fail(uint32_t session, int fd)
{
    stream_detach_internal(session, fd, true);
}

static void stream_finish_fd(int fd, bool error)
{
    uint32_t session = 0;
    if (s_stream.lock &&
        xSemaphoreTake(s_stream.lock,
                       pdMS_TO_TICKS(SI_H264_STREAM_LOCK_TIMEOUT_MS)) ==
            pdTRUE) {
        if (s_stream.fd == fd) {
            session = s_stream.session;
        }
        xSemaphoreGive(s_stream.lock);
    }
    if (session) {
        if (error) {
            stream_fail(session, fd);
        } else {
            stream_detach(session, fd);
        }
    }
}

static esp_err_t pipeline_restart_encoder_for_session(
    si_h264_pipeline_t *pipeline)
{
    if (!pipeline || !pipeline->h264_encoder) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pipeline->h264_open) {
        esp_h264_err_t close_err = esp_h264_enc_close(
            pipeline->h264_encoder);
        if (close_err != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "H.264 session reset close failed: %d",
                     (int)close_err);
            __atomic_store_n(&pipeline->encoder_reset_required, true,
                             __ATOMIC_RELEASE);
            return ESP_FAIL;
        }
        pipeline->h264_open = false;
    }
    esp_h264_err_t open_err = esp_h264_enc_open(pipeline->h264_encoder);
    if (open_err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "H.264 session reset open failed: %d", (int)open_err);
        __atomic_store_n(&pipeline->encoder_reset_required, true,
                         __ATOMIC_RELEASE);
        return ESP_FAIL;
    }
    pipeline->h264_open = true;
    pipeline->pts_origin_valid = false;
    pipeline->pts_origin_ms = 0;
    __atomic_store_n(&pipeline->encoder_reset_required, false,
                     __ATOMIC_RELEASE);
    return ESP_OK;
}

static esp_err_t encode_and_send_frame(
    si_h264_pipeline_t *pipeline, const si_h264_decode_job_t *decoded,
    httpd_handle_t server, int fd, uint32_t session, uint32_t stream_id,
    uint32_t *sequence, bool *logged_first_access_unit,
    si_h264_chain_metrics_t *chain,
    si_h264_overlap_probe_t *overlap_probe,
    uint32_t paired_decode_frame)
{
    if (!pipeline->h264_open) {
        esp_h264_err_t open_err =
            esp_h264_enc_open(pipeline->h264_encoder);
        if (open_err != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG, "hardware H.264 reopen failed: %d",
                     (int)open_err);
            return ESP_FAIL;
        }
        pipeline->h264_open = true;
    }
    if (!pipeline->pts_origin_valid) {
        pipeline->pts_origin_valid = true;
        pipeline->pts_origin_ms = decoded->timestamp_ms;
    }
    uint32_t relative_ms = decoded->timestamp_ms - pipeline->pts_origin_ms;
    esp_h264_enc_in_frame_t input = {
        .raw_data = {
            .buffer = decoded->yuv420,
            .len = decoded->yuv_len,
        },
        .pts = (uint32_t)((uint64_t)relative_ms * 90U),
    };

    if (__atomic_load_n(&pipeline->send_stopping, __ATOMIC_ACQUIRE)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t primary_slot_id = SI_H264_PRIMARY_AU_SLOT;
    int64_t send_wait_started_us = esp_timer_get_time();
    if (!pipeline_claim_au_slot(pipeline, primary_slot_id)) {
        /* The primary slot is the only hardware encoder destination.  It may
         * currently be the second bounded network owner while the small slot
         * sends the previous AU.  Drop this decoded raw frame before touching
         * encoder state; never queue a third frame or encode into the bounded
         * small-egress slot. */
        __atomic_fetch_add(&s_backpressure_drops_total, 1U,
                           __ATOMIC_RELAXED);
        return ESP_ERR_NOT_FINISHED;
    }
    uint32_t send_wait_us =
        (uint32_t)(esp_timer_get_time() - send_wait_started_us);

    esp_err_t prior_send = __atomic_load_n(&pipeline->send_result,
                                           __ATOMIC_ACQUIRE);
    uint32_t prior_send_session = __atomic_load_n(
        &pipeline->send_result_session, __ATOMIC_RELAXED);
    if (prior_send != ESP_OK && prior_send_session == session) {
        pipeline_return_au_slot(pipeline, primary_slot_id);
        __atomic_store_n(&pipeline->encoder_reset_required, true,
                         __ATOMIC_RELEASE);
        ESP_LOGI(TAG, "H.264 prior WebSocket send failed: %s",
                 esp_err_to_name(prior_send));
        return prior_send;
    }

    si_h264_au_slot_t *primary_slot =
        &pipeline->au_slots[primary_slot_id];
    if (!primary_slot->storage ||
        primary_slot->storage_capacity <= SI_H264_PAYLOAD_OFFSET) {
        pipeline_return_au_slot(pipeline, primary_slot_id);
        __atomic_store_n(&pipeline->encoder_reset_required, true,
                         __ATOMIC_RELEASE);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_h264_enc_out_frame_t output = {
        .raw_data = {
            .buffer = primary_slot->storage + SI_H264_PAYLOAD_OFFSET,
            /* Every frame is encoded into the full primary surface.  The
             * smaller slot is selected only after output.length is known, so
             * a high-entropy IDR/P frame can never overflow it or advance the
             * hardware encoder into an unrecoverable partial frame. */
            .len = h264_encoder_output_capacity(
                primary_slot->storage_capacity),
        },
    };
    int64_t encode_started_us = esp_timer_get_time();
    esp_h264_err_t h264_err =
        esp_h264_enc_process(pipeline->h264_encoder, &input, &output);
    int64_t encode_finished_us = esp_timer_get_time();
    if (h264_err != ESP_H264_ERR_OK || output.length == 0 ||
        output.length > output.raw_data.len) {
        ESP_LOGW(TAG, "hardware H.264 encode failed: %d, output=%u/%u",
                 (int)h264_err, (unsigned)output.length,
                 (unsigned)output.raw_data.len);
        pipeline_return_au_slot(pipeline, primary_slot_id);
        __atomic_fetch_add(&s_encode_failures_total, 1U,
                           __ATOMIC_RELAXED);
        /* The encoder frame counter may already have advanced.  Never retry
         * into another slot: terminate this session and close/open before the
         * next one so its first AU is a fresh SPS/PPS/IDR. */
        __atomic_store_n(&pipeline->encoder_reset_required, true,
                         __ATOMIC_RELEASE);
        return ESP_FAIL;
    }
    if (!*logged_first_access_unit) {
        const uint8_t *prefix = output.raw_data.buffer;
        ESP_LOGI(TAG,
                 "first H.264 AU: type=%d bytes=%u prefix=%02x %02x %02x %02x %02x %02x %02x %02x",
                 (int)output.frame_type, (unsigned)output.length,
                 output.length > 0 ? prefix[0] : 0,
                 output.length > 1 ? prefix[1] : 0,
                 output.length > 2 ? prefix[2] : 0,
                 output.length > 3 ? prefix[3] : 0,
                 output.length > 4 ? prefix[4] : 0,
                 output.length > 5 ? prefix[5] : 0,
                 output.length > 6 ? prefix[6] : 0,
                 output.length > 7 ? prefix[7] : 0);
        *logged_first_access_unit = true;
    }

    int64_t move_started_us = esp_timer_get_time();
    uint8_t send_slot_id = primary_slot_id;
    si_h264_au_slot_t *send_slot = primary_slot;
    bool small_egress = false;
    si_h264_au_slot_t *small_slot =
        &pipeline->au_slots[SI_H264_SMALL_EGRESS_SLOT];
    const size_t small_payload_capacity =
        h264_encoder_output_capacity(small_slot->storage_capacity);
    if (output.length <= small_payload_capacity &&
        pipeline_claim_au_slot(pipeline, SI_H264_SMALL_EGRESS_SLOT)) {
        memcpy(small_slot->storage + SI_H264_PAYLOAD_OFFSET,
               output.raw_data.buffer, output.length);
        send_slot_id = SI_H264_SMALL_EGRESS_SLOT;
        send_slot = small_slot;
        small_egress = true;
        /* The HTTPD work now owns the copied small slot.  Release the primary
         * immediately so the next frame can encode while this AU is sent. */
        pipeline_return_au_slot(pipeline, primary_slot_id);
    }
    /* Put the 24-byte wire header in the selected immutable send slot.  The
     * HTTPD callback returns exactly that slot after send/rejection. */
    uint8_t *wire_packet =
        send_slot->storage + SI_H264_PAYLOAD_OFFSET - SI_H264_HEADER_SIZE;
    memcpy(wire_packet, "EAH1", 4);
    wire_packet[4] = 1;
    wire_packet[5] =
        output.frame_type == ESP_H264_FRAME_TYPE_IDR ? 1U : 0U;
    write_le16(wire_packet + 6, SI_H264_HEADER_SIZE);
    write_le16(wire_packet + 8, pipeline->width);
    write_le16(wire_packet + 10, pipeline->height);
    write_le32(wire_packet + 12, *sequence);
    write_le32(wire_packet + 16, decoded->timestamp_ms);
    write_le32(wire_packet + 20, output.length);
    int64_t move_finished_us = esp_timer_get_time();

    if (!stream_still_current(session, fd) ||
        (stream_id != 0U &&
         !si_video_control_kvm_stream_is_current(stream_id))) {
        pipeline_return_au_slot(pipeline, send_slot_id);
        __atomic_store_n(&pipeline->encoder_reset_required, true,
                         __ATOMIC_RELEASE);
        return ESP_ERR_INVALID_STATE;
    }
    size_t frame_len = SI_H264_HEADER_SIZE + output.length;
    uint32_t send_us = __atomic_load_n(&pipeline->last_send_us,
                                       __ATOMIC_ACQUIRE);
    int64_t send_finished_us = esp_timer_get_time();
    si_h264_send_work_t *work = &send_slot->send_work;
    work->pipeline = pipeline;
    work->job = (si_h264_send_job_t){
        .server = server,
        .fd = fd,
        .session = session,
        .stream_id = stream_id,
        .slot_id = send_slot_id,
        .payload = wire_packet,
        .frame_len = frame_len,
    };
    if (httpd_queue_work(server, h264_httpd_send_work, work) != ESP_OK) {
        pipeline_return_au_slot(pipeline, send_slot_id);
        __atomic_store_n(&pipeline->encoder_reset_required, true,
                         __ATOMIC_RELEASE);
        ESP_LOGE(TAG, "H.264 HTTPD work queue rejected AU slot %u",
                 (unsigned)send_slot_id);
        __atomic_fetch_add(&s_send_failures_total, 1U, __ATOMIC_RELAXED);
        return ESP_ERR_TIMEOUT;
    }
    if (small_egress) {
        __atomic_fetch_add(&s_small_egress_access_units_total, 1U,
                           __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&s_primary_egress_access_units_total, 1U,
                           __ATOMIC_RELAXED);
    }
    if (overlap_probe && paired_decode_frame != 0U) {
        *overlap_probe = (si_h264_overlap_probe_t) {
            .pending = true,
            .decode_frame = paired_decode_frame,
            .encode_started_us = encode_started_us,
            .encode_finished_us = encode_finished_us,
        };
    }
    uint32_t source_advance =
        decoded->previous_jpeg_frame == 0 ||
                decoded->jpeg_frame <= decoded->previous_jpeg_frame
            ? 1U
            : decoded->jpeg_frame - decoded->previous_jpeg_frame;
    uint32_t decode_us =
        (uint32_t)(decoded->decode_finished_us - decoded->decode_started_us);
    uint32_t encode_us = (uint32_t)(encode_finished_us - encode_started_us);
    chain->frames++;
    chain->source_frames += source_advance;
    chain->source_drops += source_advance > 1U ? source_advance - 1U : 0U;
    chain->jpeg_bytes += decoded->jpeg_bytes;
    chain->h264_bytes += output.length;
    chain->copy_us += decoded->copy_us;
    chain->info_us += decoded->info_us;
    chain->decode_us += decode_us;
    chain->encode_us += encode_us;
    chain->move_us += move_finished_us - move_started_us;
    chain->send_wait_us += send_wait_us;
    chain->send_us += send_us;
    __atomic_fetch_add(&s_access_units_total, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_last_sequence, *sequence, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_timestamp_ms, decoded->timestamp_ms,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_jpeg_bytes, decoded->jpeg_bytes,
                     __ATOMIC_RELEASE);
    h264_telemetry_update_max(&s_max_jpeg_bytes, decoded->jpeg_bytes);
    __atomic_store_n(&s_last_au_bytes, output.length, __ATOMIC_RELEASE);
    h264_telemetry_update_max(&s_max_au_bytes, output.length);
    if (decode_us > chain->max_decode_us) chain->max_decode_us = decode_us;
    if (encode_us > chain->max_encode_us) chain->max_encode_us = encode_us;
    if (send_wait_us > chain->max_send_wait_us) {
        chain->max_send_wait_us = send_wait_us;
    }
    if (send_us > chain->max_send_us) chain->max_send_us = send_us;
    chain_metrics_report(chain, send_finished_us);
    (*sequence)++;
    return ESP_OK;
}

static bool flush_decoded_frame(
    si_h264_pipeline_t *pipeline, si_h264_decode_job_t *decoded,
    bool *decoded_ready, httpd_handle_t server, int fd, uint32_t session,
    uint32_t stream_id, uint32_t *sequence,
    bool *logged_first_access_unit, si_h264_chain_metrics_t *chain,
    si_h264_overlap_probe_t *overlap_probe,
    uint32_t paired_decode_frame)
{
    if (!*decoded_ready) {
        return true;
    }
    esp_err_t err = encode_and_send_frame(
        pipeline, decoded, server, fd, session, stream_id, sequence,
        logged_first_access_unit, chain, overlap_probe,
        paired_decode_frame);
    *decoded_ready = false;
    if (err == ESP_ERR_NOT_FINISHED) {
        return true;
    }
    if (err != ESP_OK) {
        stream_fail(session, fd);
        return false;
    }
    return true;
}

static void h264_stream_task(void *arg)
{
    (void)arg;
    si_h264_pipeline_t pipeline = {0};
    uint32_t local_session = 0;
    uint32_t last_jpeg_frame = 0;
    uint32_t sequence = 0;
    bool logged_first_jpeg = false;
    bool logged_first_access_unit = false;
    uint32_t jpeg_only_probe_remaining = 0;
    bool decoded_ready = false;
    bool decode_inflight = false;
    int64_t inactive_since_us = 0;
    int64_t restore_retry_not_before_us = 0;
    uint32_t restore_retry_ms = SI_H264_RESTORE_RETRY_INITIAL_MS;
    int64_t decoded_wait_started_us = 0;
    si_h264_decode_job_t decoded = {0};
    si_h264_chain_metrics_t chain = {0};
    si_h264_overlap_probe_t overlap_probe = {0};
    chain_metrics_reset(&chain, esp_timer_get_time());

    while (true) {
        httpd_handle_t server = NULL;
        int fd = -1;
        uint32_t session = 0;
        uint32_t stream_id = 0;
        si_h264_resource_token_t resource_token = 0U;
        if (!stream_snapshot(&server, &fd, &session, &stream_id,
                             &resource_token)) {
            if (__atomic_load_n(&s_restore_pending, __ATOMIC_ACQUIRE)) {
                int64_t now_us = esp_timer_get_time();
                if (inactive_since_us == 0) {
                    inactive_since_us = now_us;
                }
                if (now_us - inactive_since_us >=
                        (int64_t)SI_H264_PIPELINE_IDLE_GRACE_MS * 1000LL &&
                    now_us >= restore_retry_not_before_us) {
                    /* Serialize the final inactivity check with the WebSocket
                     * handshake.  Otherwise a new client can attach after the
                     * snapshot above but before cleanup, and have its freshly
                     * selected H.264 transport disabled underneath it. */
                    bool still_inactive = false;
                    bool cleanup_claimed = false;
                    if (s_stream.lock &&
                        xSemaphoreTake(s_stream.lock,
                                       pdMS_TO_TICKS(250)) == pdTRUE) {
                        still_inactive = (!s_stream.server ||
                                          s_stream.fd < 0) &&
                            __atomic_load_n(&s_restore_pending,
                                            __ATOMIC_ACQUIRE);
                        if (still_inactive) {
                            s_stream.teardown = true;
                            cleanup_claimed = true;
                        }
                        xSemaphoreGive(s_stream.lock);
                    }
                    if (cleanup_claimed) {
                        /* A sender may need s_stream.lock to reject and return
                         * its final old-session AU.  The teardown flag keeps a
                         * new handshake from publishing while cleanup runs,
                         * without holding that lock across the worker join. */
                        esp_err_t cleanup_err = pipeline_release(&pipeline);
                        if (cleanup_err == ESP_OK) {
                            local_session = 0;
                            last_jpeg_frame = 0;
                            sequence = 0;
                            logged_first_jpeg = false;
                            logged_first_access_unit = false;
                            jpeg_only_probe_remaining = 0;
                            decoded_ready = false;
                            decode_inflight = false;
                            decoded_wait_started_us = 0;
                            overlap_probe_reset(&overlap_probe);
                            memset(&decoded, 0, sizeof(decoded));
                            chain_metrics_reset(&chain, now_us);
                            cleanup_err =
                                si_video_control_set_h264_transport(false);
                        }

                        bool restore_complete = false;
                        if (cleanup_err == ESP_OK && s_stream.lock &&
                            xSemaphoreTake(
                                s_stream.lock,
                                pdMS_TO_TICKS(
                                    SI_H264_STREAM_LOCK_TIMEOUT_MS)) ==
                                pdTRUE) {
                            if (!s_stream.server || s_stream.fd < 0) {
                                __atomic_store_n(&s_teardown_poisoned, false,
                                                 __ATOMIC_RELEASE);
                                const si_h264_resource_token_t
                                    resource_token =
                                        s_stream.resource_token;
                                if (resource_token == 0U ||
                                    h264_stream_release_h264_resources(
                                        resource_token)) {
                                    s_stream.resource_token = 0U;
                                    s_stream.teardown = false;
                                    __atomic_store_n(&s_restore_pending, false,
                                                     __ATOMIC_RELEASE);
                                    __atomic_store_n(&s_restore_error, ESP_OK,
                                                     __ATOMIC_RELEASE);
                                    restore_complete = true;
                                } else {
                                    cleanup_err = ESP_ERR_INVALID_STATE;
                                }
                            } else {
                                cleanup_err = ESP_ERR_INVALID_STATE;
                            }
                            xSemaphoreGive(s_stream.lock);
                        } else if (cleanup_err == ESP_OK) {
                            cleanup_err = ESP_ERR_TIMEOUT;
                        }

                        if (restore_complete) {
                            inactive_since_us = 0;
                            restore_retry_not_before_us = 0;
                            restore_retry_ms =
                                SI_H264_RESTORE_RETRY_INITIAL_MS;
                            ESP_LOGI(TAG,
                                     "H.264 client released; codec closed and MJPEG capture profile restored");
                        } else {
                            __atomic_store_n(&s_restore_pending, true,
                                             __ATOMIC_RELEASE);
                            __atomic_store_n(&s_restore_error, cleanup_err,
                                             __ATOMIC_RELEASE);
                            __atomic_store_n(
                                &s_teardown_poisoned,
                                pipeline.teardown_poisoned,
                                __ATOMIC_RELEASE);
                            const uint32_t scheduled_retry_ms =
                                restore_retry_ms;
                            restore_retry_not_before_us =
                                now_us +
                                (int64_t)scheduled_retry_ms * 1000LL;
                            if (restore_retry_ms <
                                SI_H264_RESTORE_RETRY_MAX_MS) {
                                restore_retry_ms *= 2U;
                                if (restore_retry_ms >
                                    SI_H264_RESTORE_RETRY_MAX_MS) {
                                    restore_retry_ms =
                                        SI_H264_RESTORE_RETRY_MAX_MS;
                                }
                            }
                            ESP_LOGW(TAG,
                                     "MJPEG restore deferred after H.264 teardown: %s; retry in %u ms",
                                     esp_err_to_name(cleanup_err),
                                     (unsigned)scheduled_retry_ms);
                        }
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        inactive_since_us = 0;
        restore_retry_not_before_us = 0;
        restore_retry_ms = SI_H264_RESTORE_RETRY_INITIAL_MS;

        if (session != local_session) {
            bool pipeline_ready = pipeline.h264_encoder != NULL;
            local_session = session;
            last_jpeg_frame = 0;
            sequence = 0;
            logged_first_jpeg = false;
            logged_first_access_unit = false;
            jpeg_only_probe_remaining = 0;
            decoded_ready = false;
            decode_inflight = false;
            decoded_wait_started_us = 0;
            overlap_probe_reset(&overlap_probe);
            memset(&decoded, 0, sizeof(decoded));
            chain_metrics_reset(&chain, esp_timer_get_time());
            if (pipeline_ready) {
                /* Keep the expensive encoder workspace, but never carry its
                 * P-frame reference chain across browser sessions.  Drain the
                 * old decode job, close/open the encoder, and reset sequence
                 * and PTS so the new client's first AU is SPS/PPS/IDR. */
                esp_err_t restart_err =
                    pipeline_stop_decode_worker(&pipeline);
                if (restart_err == ESP_OK && !pipeline.jpeg_decoder) {
                    restart_err = pipeline_recreate_jpeg_decoder(&pipeline);
                }
                if (restart_err == ESP_OK) {
                    restart_err =
                        pipeline_restart_encoder_for_session(&pipeline);
                }
                if (restart_err == ESP_OK && pipeline.yuv420_alt &&
                    SI_CFG_VIDEO_H264_PIPELINE_OVERLAP) {
                    restart_err = pipeline_start_decode_worker(&pipeline);
                }
                if (restart_err != ESP_OK) {
                    __atomic_store_n(&pipeline.encoder_reset_required, true,
                                     __ATOMIC_RELEASE);
                    stream_fail(session, fd);
                    continue;
                }
                ESP_LOGI(TAG,
                         "H.264 client replaced; reusing open codec pipeline after reset to a fresh IDR");
            }
            /* Revoke pending direct input on every browser session boundary.
             * Cold start drops any frame queued before the WebSocket session;
             * a warm reconnect cannot inherit input queued for the prior
             * client.  The ingest validator, rather than an arbitrary delay,
             * rejects malformed transition data. */
            (void)si_video_flush_h264_jpeg();
            ESP_LOGI(TAG,
                     "discarded pending frame at H.264 session boundary");
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        httpd_ws_client_info_t client = httpd_ws_get_fd_info(server, fd);
        if (client == HTTPD_WS_CLIENT_HTTP) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (client != HTTPD_WS_CLIENT_WEBSOCKET) {
            ESP_LOGW(TAG, "H.264 client is not an active WebSocket: fd=%d state=%d",
                     fd, (int)client);
            stream_detach(session, fd);
            continue;
        }

        esp_err_t err = ESP_OK;

        /* Espressif's P4 M2M pipeline overlaps JPEG decode and H.264 encode.
         * Consume the prior decode result, then submit the next frame before
         * encoding it.  Two YUV surfaces keep the DMA engines from sharing a
         * producer/consumer buffer.  If the second surface or worker was not
         * available, retain the serial path as a safe fallback. */
        bool overlap = pipeline.decode_worker.task &&
                       pipeline.decode_worker.results &&
                       pipeline.decode_worker.requests;
        if (overlap && decode_inflight) {
            si_h264_decode_job_t completed = {0};
            if (xQueueReceive(pipeline.decode_worker.results, &completed,
                              pdMS_TO_TICKS(SI_H264_JPEG_TIMEOUT_MS + 50U)) !=
                pdPASS) {
                ESP_LOGE(TAG, "JPEG pipeline result timed out");
                if (overlap_probe.pending) {
                    chain.overlap_misses++;
                    overlap_probe_reset(&overlap_probe);
                }
                stream_fail(session, fd);
                decode_inflight = false;
                continue;
            }
            decode_inflight = false;
            chain_metrics_record_overlap(&chain, &overlap_probe, &completed);
            if (completed.result == ESP_OK) {
                if (jpeg_only_probe_remaining > 0) {
                    ESP_LOGI(TAG,
                             "JPEG-only live probe passed: frame=%u remaining=%u bytes=%u yuv=%u",
                             (unsigned)completed.jpeg_frame,
                             (unsigned)(jpeg_only_probe_remaining - 1U),
                             (unsigned)completed.jpeg_len,
                             (unsigned)completed.yuv_len);
                    jpeg_only_probe_remaining--;
                    decoded_ready = false;
                    decoded_wait_started_us = 0;
                } else {
                    decoded = completed;
                    decoded_ready = true;
                    decoded_wait_started_us = completed.decode_finished_us;
                }
            } else {
                __atomic_fetch_add(&s_decode_failures_total, 1U,
                                   __ATOMIC_RELAXED);
                ESP_LOGW(TAG, "hardware JPEG decode failed: %s",
                         esp_err_to_name(completed.result));
                decoded_ready = false;
                decoded_wait_started_us = 0;
            }
        } else if (!overlap && decoded_ready) {
            const bool flushed = flush_decoded_frame(
                &pipeline, &decoded, &decoded_ready, server, fd,
                session, stream_id, &sequence,
                &logged_first_access_unit, &chain, &overlap_probe, 0U);
            decoded_wait_started_us = 0;
            if (!flushed) {
                continue;
            }
        }

        si_video_jpeg_view_t jpeg_view = {0};
        uint32_t previous_jpeg_frame = last_jpeg_frame;
        int64_t copy_started_us = esp_timer_get_time();
        err = si_video_borrow_h264_jpeg_if_new(last_jpeg_frame, &jpeg_view);
        int64_t copy_finished_us = esp_timer_get_time();
        if (err == ESP_ERR_NOT_FINISHED || err == ESP_ERR_NOT_FOUND) {
            if (overlap && decoded_ready) {
                if (decoded_wait_started_us == 0) {
                    decoded_wait_started_us = copy_finished_us;
                }
                if (copy_finished_us - decoded_wait_started_us <
                    overlap_wait_timeout_us(&pipeline)) {
                    /* Preserve decoded A until B exists.  Encoding A now
                     * would finish before B can enter the decoder and turn
                     * the nominal D(B)/E(A) pipeline back into D+E serial
                     * work.  The bounded timeout below still emits A when
                     * the UVC source stalls. */
                    const int64_t remaining_us =
                        overlap_wait_timeout_us(&pipeline) -
                        (copy_finished_us - decoded_wait_started_us);
                    uint32_t wait_ms =
                        (uint32_t)((remaining_us + 999LL) / 1000LL);
                    if (wait_ms > 5U) {
                        wait_ms = 5U;
                    }
                    (void)si_video_wait_h264_jpeg_ready(wait_ms);
                    continue;
                }
                chain.overlap_wait_timeouts++;
                if (!flush_decoded_frame(
                        &pipeline, &decoded, &decoded_ready, server, fd,
                        session, stream_id, &sequence,
                        &logged_first_access_unit, &chain, &overlap_probe,
                        0U)) {
                    decoded_wait_started_us = 0;
                    continue;
                }
                decoded_wait_started_us = 0;
            }
            (void)si_video_wait_h264_jpeg_ready(5U);
            continue;
        }
        if (err != ESP_OK) {
            if (overlap && !flush_decoded_frame(
                               &pipeline, &decoded, &decoded_ready, server,
                               fd, session, stream_id, &sequence,
                               &logged_first_access_unit, &chain,
                               &overlap_probe, 0U)) {
                decoded_wait_started_us = 0;
                continue;
            }
            decoded_wait_started_us = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t jpeg_len = jpeg_view.len;
        uint32_t jpeg_frame = jpeg_view.frame_id;
        last_jpeg_frame = jpeg_frame;

        /* The UVC assembler and this staging copy are both CPU accesses.
         * Coherency between the cores is provided by the shared cache and the
         * frame lease; writing the whole leased image back to PSRAM here is
         * unnecessary.  On maximum-entropy 1080p MJPEG it also occupies the
         * external-memory path long enough to halve isochronous completion
         * cadence.  Only the codec-owned destination is published below,
         * immediately before DMA2D reads it. */

        si_video_status_t video = {0};
        si_video_get_status(&video);
        jpeg_decode_picture_info_t info = {
            .width = pipeline.width,
            .height = pipeline.height,
            .sample_method = JPEG_DOWN_SAMPLING_YUV420,
        };
        int64_t info_started_us = esp_timer_get_time();
        bool need_info = !pipeline.h264_encoder ||
                         pipeline.width != video.width ||
                         pipeline.height != video.height;
        if (need_info) {
            err = jpeg_decoder_get_info(jpeg_view.data, jpeg_len, &info);
            if (err != ESP_OK || info.width < 80 || info.width > 1920 ||
                info.height < 80 || info.height > 2032) {
                ESP_LOGW(TAG, "unsupported JPEG for H.264: %ux%u (%s)",
                         (unsigned)info.width, (unsigned)info.height,
                         esp_err_to_name(err));
                (void)si_video_release_h264_jpeg(&jpeg_view);
                continue;
            }
        }
        int64_t info_finished_us = esp_timer_get_time();
        if (!logged_first_jpeg) {
            ESP_LOGI(TAG, "first JPEG: frame=%u bytes=%u %ux%u sampling=%d",
                     (unsigned)jpeg_frame, (unsigned)jpeg_len,
                     (unsigned)info.width, (unsigned)info.height,
                     (int)info.sample_method);
            logged_first_jpeg = true;
        }
        if (!h264_output_mode_supported(info.width, info.height,
                                        video.target_fps_x100)) {
            ESP_LOGW(TAG,
                     "active capture mode left H.264 contract: %ux%u@%u.%02u",
                     (unsigned)info.width, (unsigned)info.height,
                     (unsigned)(video.target_fps_x100 / 100U),
                     (unsigned)(video.target_fps_x100 % 100U));
            (void)si_video_release_h264_jpeg(&jpeg_view);
            stream_fail(session, fd);
            continue;
        }
        /* Keep the encoder's SPS/VUI and rate-control clock equal to the
         * validated output contract for the active resolution.  1080p is
         * capped at 25 FPS while the lower-cost 720p path runs at 30 FPS. */
        uint8_t fps = h264_output_fps_for_mode(
            info.width, info.height, video.target_fps_x100);

        if (!pipeline.h264_encoder || pipeline.width != info.width ||
            pipeline.height != info.height || pipeline.fps != fps) {
            decoded_ready = false;
            decode_inflight = false;
            decoded_wait_started_us = 0;
            overlap_probe_reset(&overlap_probe);
            memset(&decoded, 0, sizeof(decoded));
            err = pipeline_configure(&pipeline, (uint16_t)info.width,
                                     (uint16_t)info.height, fps, false,
                                     resource_token);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "pipeline configuration failed: %s",
                         esp_err_to_name(err));
                (void)si_video_release_h264_jpeg(&jpeg_view);
                stream_fail(session, fd);
                continue;
            }
            jpeg_only_probe_remaining = 0;
        }
        overlap = pipeline.decode_worker.task &&
                  pipeline.decode_worker.results &&
                  pipeline.decode_worker.requests;

        /* Keep the leased UVC frame on the normal zero-copy path.  A full
         * copy and entropy repair is attempted only after hardware rejects
         * the source.  The failed decoder is recreated before any retry so
         * one malformed MS2109 frame cannot poison subsequent frames. */
        if (jpeg_len > SI_H264_MAX_JPEG_INPUT_SIZE) {
            ESP_LOGE(TAG, "JPEG input exceeds supported maximum: %u/%u",
                     (unsigned)jpeg_len,
                     (unsigned)SI_H264_MAX_JPEG_INPUT_SIZE);
            (void)si_video_release_h264_jpeg(&jpeg_view);
            continue;
        }
        copy_started_us = esp_timer_get_time();
        copy_finished_us = copy_started_us;
        uint8_t *frame_input = (uint8_t *)jpeg_view.data;
        /* jpeg_decoder_process() is the cache-ownership boundary.  IDF
         * writes the compressed entropy range back to PSRAM immediately
         * before starting DMA2D, so doing another full-frame C2M sync here
         * doubles external-memory traffic on the largest MS2109 frames and
         * can starve the isochronous producer. */

        si_h264_decode_job_t completed = {
            .jpeg = frame_input,
            .jpeg_view = jpeg_view,
            .jpeg_len = (uint32_t)jpeg_len,
            .yuv420 = decoded_ready && decoded.yuv420 == pipeline.yuv420
                          ? pipeline.yuv420_alt
                          : pipeline.yuv420,
            .yuv420_capacity =
                (uint32_t)(decoded_ready &&
                                   decoded.yuv420 == pipeline.yuv420
                               ? pipeline.yuv420_alt_capacity
                               : pipeline.yuv420_capacity),
            .jpeg_frame = jpeg_frame,
            .previous_jpeg_frame = previous_jpeg_frame,
            .timestamp_ms = (uint32_t)(copy_finished_us / 1000),
            .jpeg_bytes = (uint32_t)jpeg_len,
            .copy_us = (uint32_t)(copy_finished_us - copy_started_us),
            .info_us = (uint32_t)(info_finished_us - info_started_us),
            .h264_uvc_lease = true,
        };
        jpeg_decode_cfg_t decode_cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_YUV420,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT709,
        };
        if (!pipeline.jpeg_decoder) {
            err = pipeline_recreate_jpeg_decoder(&pipeline);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "JPEG decoder acquire failed: %s",
                         esp_err_to_name(err));
                (void)si_video_release_h264_jpeg(&jpeg_view);
                decoded_ready = false;
                decoded_wait_started_us = 0;
                continue;
            }
        }
        if (overlap) {
            const uint32_t paired_decode_frame =
                decoded_ready ? jpeg_frame : 0U;
            if (xQueueSend(pipeline.decode_worker.requests, &completed,
                           pdMS_TO_TICKS(20)) != pdPASS) {
                ESP_LOGE(TAG, "JPEG pipeline request queue is blocked");
                (void)si_video_release_h264_jpeg(&jpeg_view);
                stream_fail(session, fd);
                continue;
            }
            decode_inflight = true;
            if (paired_decode_frame != 0U && decoded_wait_started_us != 0) {
                const uint32_t wait_us =
                    (uint32_t)(esp_timer_get_time() -
                               decoded_wait_started_us);
                chain.overlap_wait_events++;
                chain.overlap_wait_us += wait_us;
                if (wait_us > chain.max_overlap_wait_us) {
                    chain.max_overlap_wait_us = wait_us;
                }
            }
            if (!flush_decoded_frame(
                    &pipeline, &decoded, &decoded_ready, server, fd,
                    session, stream_id, &sequence,
                    &logged_first_access_unit, &chain, &overlap_probe,
                    paired_decode_frame)) {
                decoded_wait_started_us = 0;
                continue;
            }
            decoded_wait_started_us = 0;
            continue;
        }
        completed.decode_started_us = esp_timer_get_time();
        completed.result = h264_jpeg_decoder_process(
            pipeline.jpeg_decoder, &decode_cfg, completed.jpeg,
            completed.jpeg_len, completed.yuv420,
            completed.yuv420_capacity, &completed.yuv_len);
        if (completed.result != ESP_OK) {
            /* Drop the frame exactly as received.  Reset the decoder for the
             * next validated frame; never synthesize entropy bytes. */
            esp_err_t reset_err = pipeline_recreate_jpeg_decoder(&pipeline);
            if (reset_err != ESP_OK) {
                completed.result = reset_err;
            }
        }
        completed.decode_finished_us = esp_timer_get_time();
        decode_job_release_source(&completed);
        if (completed.result == ESP_OK) {
            if (jpeg_only_probe_remaining > 0) {
                ESP_LOGI(TAG,
                         "JPEG-only live probe passed: frame=%u remaining=%u bytes=%u yuv=%u",
                         (unsigned)jpeg_frame,
                         (unsigned)(jpeg_only_probe_remaining - 1U),
                         (unsigned)jpeg_len, (unsigned)completed.yuv_len);
                jpeg_only_probe_remaining--;
                decoded_ready = false;
            } else {
                decoded = completed;
                decoded_ready = true;
            }
        } else {
            __atomic_fetch_add(&s_decode_failures_total, 1U,
                               __ATOMIC_RELAXED);
            ESP_LOGW(TAG, "hardware JPEG decode failed: %s",
                     esp_err_to_name(completed.result));
            decoded_ready = false;
        }
    }
}

static esp_err_t ensure_stream_task(void)
{
    if (s_stream.task) {
        return ESP_OK;
    }
    if (!s_stream.lock) {
        s_stream.lock = xSemaphoreCreateMutex();
        if (!s_stream.lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        h264_stream_task, "si_h264_stream", SI_H264_TASK_STACK, NULL,
        SI_H264_TASK_PRIORITY, &s_stream.task, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t parse_stream_id(httpd_req_t *req, uint32_t *stream_id,
                                 bool *present)
{
    if (!req || !stream_id || !present) {
        return ESP_ERR_INVALID_ARG;
    }
    *stream_id = 0U;
    *present = false;
    char query[96] = {0};
    char value[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "stream_id", value,
                              sizeof(value)) != ESP_OK) {
        return ESP_OK;
    }
    *present = true;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    *stream_id = (uint32_t)parsed;
    return ESP_OK;
}

esp_err_t si_h264_stream_ws_post_handshake(httpd_req_t *req)
{
    /* ESP-IDF 5.5 completes a WebSocket upgrade without calling the normal
     * URI handler.  Attach the producer here so a receive frame is not
     * required before the server can begin sending video. */
    ESP_RETURN_ON_ERROR(si_h264_stream_initialize(), TAG,
                        "initialize H.264 service");

    si_video_status_t video = {0};
    si_video_get_status(&video);
    const uint32_t mode_width = video.target_width != 0U
                                    ? video.target_width : video.width;
    const uint32_t mode_height = video.target_height != 0U
                                     ? video.target_height : video.height;
    if (!h264_output_mode_supported(mode_width, mode_height,
                                    video.target_fps_x100)) {
        ESP_LOGW(TAG,
                 "reject unsupported H.264 capture mode: %ux%u@%u.%02u",
                 (unsigned)mode_width, (unsigned)mode_height,
                 (unsigned)(video.target_fps_x100 / 100U),
                 (unsigned)(video.target_fps_x100 % 100U));
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_ERROR(ensure_stream_task(), TAG,
                        "start H.264 stream task");

    httpd_handle_t old_server = NULL;
    int old_fd = -1;
    uint32_t stream_id = 0U;
    bool stream_id_present = false;
    ESP_RETURN_ON_ERROR(parse_stream_id(req, &stream_id,
                                        &stream_id_present),
                        TAG, "invalid H.264 stream id");
    if (stream_id_present &&
        !si_video_control_kvm_stream_is_current(stream_id)) {
        return ESP_ERR_INVALID_STATE;
    }
    int new_fd = httpd_req_to_sockfd(req);
    /* The idle grace period is intentionally replaceable: a new browser may
     * take over the still-live H.264 pipeline and cancel the pending MJPEG
     * restore.  Once teardown has actually been claimed by the worker, reject
     * the request so the client can retry without racing pipeline release. */
    if (xSemaphoreTake(s_stream.lock,
                       pdMS_TO_TICKS(SI_H264_STREAM_LOCK_TIMEOUT_MS)) !=
        pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_stream.teardown) {
        xSemaphoreGive(s_stream.lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* The first validation occurred before waiting for this mutex.  Recheck
     * now so a delayed old handshake cannot overwrite a newer session. */
    if (stream_id_present &&
        !si_video_control_kvm_stream_is_current(stream_id)) {
        xSemaphoreGive(s_stream.lock);
        return ESP_ERR_INVALID_STATE;
    }
    old_server = s_stream.server;
    old_fd = s_stream.fd;
    si_h264_resource_token_t resource_token = s_stream.resource_token;
    bool resource_claimed_here = false;
    if (resource_token == 0U) {
        esp_err_t claim_err =
            h264_stream_claim_h264_resources(&resource_token);
        if (claim_err != ESP_OK) {
            xSemaphoreGive(s_stream.lock);
            ESP_LOGW(TAG,
                     "H.264 session deferred by peak-resource owner: %s",
                     esp_err_to_name(claim_err));
            return claim_err;
        }
        resource_claimed_here = true;
    } else if (!h264_resource_owner_is(
                   resource_token, SI_H264_RESOURCE_OWNER_H264)) {
        xSemaphoreGive(s_stream.lock);
        ESP_LOGE(TAG, "H.264 stream retained a stale resource token");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t reserve_err = h264_runtime_reserve(resource_token);
    if (reserve_err != ESP_OK) {
        if (resource_claimed_here) {
            (void)h264_stream_release_h264_resources(resource_token);
        }
        xSemaphoreGive(s_stream.lock);
        ESP_LOGE(TAG, "H.264 runtime resource probe failed: %s",
                 esp_err_to_name(reserve_err));
        return reserve_err;
    }
    esp_err_t send_override_err = httpd_sess_set_send_override(
        req->handle, new_fd, h264_socket_send_all);
    if (send_override_err != ESP_OK) {
        if (resource_claimed_here) {
            (void)h264_stream_release_h264_resources(resource_token);
        }
        xSemaphoreGive(s_stream.lock);
        ESP_LOGE(TAG, "cannot install H.264 send-all transport: %s",
                 esp_err_to_name(send_override_err));
        return send_override_err;
    }

    /* Prepare every fallible transport resource before publishing the new
     * session.  A failure must leave the old producer tuple intact. */
    esp_err_t transport_err = si_video_control_set_h264_transport(true);
    if (transport_err != ESP_OK) {
        if (resource_claimed_here) {
            (void)h264_stream_release_h264_resources(resource_token);
        }
        xSemaphoreGive(s_stream.lock);
        ESP_LOGE(TAG, "cannot select H.264 UVC input profile: %s",
                 esp_err_to_name(transport_err));
        return transport_err;
    }
    if (stream_id_present &&
        !si_video_control_kvm_stream_is_current(stream_id)) {
        const bool had_old_session = old_server && old_fd >= 0;
        bool retain_claim_for_restore = false;
        if (!had_old_session) {
            esp_err_t restore_err =
                si_video_control_set_h264_transport(false);
            if (restore_err != ESP_OK) {
                s_stream.resource_token = resource_token;
                s_stream.teardown = true;
                retain_claim_for_restore = true;
                __atomic_store_n(&s_restore_pending, true,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&s_restore_error, restore_err,
                                 __ATOMIC_RELEASE);
            }
        }
        if (resource_claimed_here && !retain_claim_for_restore) {
            (void)h264_stream_release_h264_resources(resource_token);
        }
        xSemaphoreGive(s_stream.lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_stream.session++;
    if (s_stream.session == 0) {
        s_stream.session = 1;
    }
    s_stream.server = req->handle;
    s_stream.fd = new_fd;
    s_stream.stream_id = stream_id;
    s_stream.resource_token = resource_token;
    __atomic_store_n(&s_session_active, true, __ATOMIC_RELEASE);
    __atomic_fetch_add(&s_sessions_started_total, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_metrics_updated_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_restore_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_restore_error, ESP_OK, __ATOMIC_RELEASE);
    __atomic_store_n(&s_teardown_poisoned, false, __ATOMIC_RELEASE);
    xSemaphoreGive(s_stream.lock);

    if (old_server && old_fd >= 0 &&
        (old_server != req->handle || old_fd != new_fd)) {
        (void)httpd_sess_trigger_close(old_server, old_fd);
    }
    si_video_stream_metrics_begin(stream_id, si_monotonic_ms());
    ESP_LOGI(TAG, "H.264 WebSocket connected: fd=%d stream=%u",
             new_fd, (unsigned)stream_id);
    return ESP_OK;
}

esp_err_t si_h264_stream_ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t frame = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK || frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        stream_finish_fd(fd, ret != ESP_OK);
        return ret == ESP_OK ? ESP_OK : ret;
    }

    if (frame.len > 0) {
        uint8_t scratch[32];
        if (frame.len > sizeof(scratch)) {
            stream_finish_fd(httpd_req_to_sockfd(req), true);
            return ESP_ERR_INVALID_SIZE;
        }
        frame.payload = scratch;
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        if (ret != ESP_OK) {
            stream_finish_fd(httpd_req_to_sockfd(req), true);
        }
    }
    return ret;
}
