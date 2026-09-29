// USB UVC video source, ingest queue, and immutable application frame store.
#include "video_input.h"

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_private/uvc_esp_video.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "video_frame_store.h"
#include "video_mjpeg.h"

static const char *TAG = "si-video";

/* Capture work is deliberately isolated on core 0.  Codec and network tasks
 * run independently and only consume immutable application-owned frames. */
#define SI_UVC_DRIVER_PRIORITY 16
#define SI_USB_HOST_PRIORITY 15
#define SI_UVC_INGEST_PRIORITY 14
#define SI_UVC_RETURN_PRIORITY 14
#define SI_UVC_SUPERVISOR_PRIORITY 9
#define SI_UVC_OPEN_TIMEOUT_MS 5000U
#define SI_UVC_NO_FRAME_TIMEOUT_MS 5000U
#define SI_UVC_CLOSE_DRAIN_TIMEOUT_MS 1500U
#define SI_UVC_MISSING_DEVICE_RECOVERY_DELAY_MS 5000U
#define SI_UVC_MISSING_DEVICE_RECOVERY_INTERVAL_MS 15000U
#define SI_UVC_INVALID_FRAME_FAULT_LIMIT 8U
#define SI_UVC_HEALTHY_RESET_MS 2000U
#define SI_UVC_HEALTHY_RESET_CALLBACKS 8U
#define SI_UVC_DMA_RESERVE_BYTES (64U * 1024U)
#define SI_UVC_BOOT_MEMORY_WAIT_MS 10000U
#define SI_UVC_RETURN_RETRY_DELAY_MS 5U
#define SI_UVC_RETURN_LOG_INTERVAL 64U
#define SI_UVC_RETENTION_CAP_BIT UINT32_C(0x80000000)
#define SI_UVC_RETENTION_COUNT_MASK UINT32_C(0x7fffffff)

#if SI_CFG_VIDEO_H264_ENABLED && SI_CFG_VIDEO_FRAME_BUFFERS < 3
#error "The direct H.264 prefetch path requires at least three UVC buffers"
#elif SI_CFG_VIDEO_FRAME_BUFFERS < 2
#error "The decoupled UVC ingest pipeline requires at least two UVC buffers"
#endif

typedef struct {
    bool valid;
    uint8_t dev_addr;
    uint8_t stream_index;
    uint32_t logical_fps_x100;
    uvc_host_stream_format_t format;
} si_uvc_selection_t;

typedef struct {
    si_video_mode_t public_mode;
    uint8_t dev_addr;
    uint8_t stream_index;
    uvc_host_stream_format_t format;
} si_uvc_mode_t;

typedef struct {
    const uvc_host_frame_t *frame;
    uvc_host_stream_hdl_t stream;
    uint32_t epoch;
    uint32_t h264_epoch;
    uint32_t frame_id;
} si_uvc_frame_item_t;

typedef struct {
    si_uvc_frame_item_t item;
    uint32_t attempts;
} si_uvc_return_item_t;

/* The public token points at this stable, private record rather than at an
 * ingest-task stack object.  The record carries every ownership discriminator
 * needed to reject a stale or duplicate release.  With three UVC buffers the
 * singleton lease and the capacity-one pending queue may coexist: the third
 * buffer remains available to the USB producer. */
typedef struct {
    bool active;
    si_uvc_frame_item_t item;
} si_h264_uvc_lease_t;

typedef enum {
    SI_UVC_SESSION_CLOSED = 0,
    SI_UVC_SESSION_OPEN_STOPPED,
    SI_UVC_SESSION_STARTED,
    SI_UVC_SESSION_DRAINING,
    SI_UVC_SESSION_FAULTED,
} si_uvc_session_state_t;

static si_video_status_t s_status;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_transport_gate;
static QueueHandle_t s_uvc_frame_queue;
static QueueHandle_t s_uvc_return_queue;
static QueueHandle_t s_h264_frame_queue;
static SemaphoreHandle_t s_h264_frame_lock;
static TaskHandle_t s_usb_host_task;
static TaskHandle_t s_uvc_supervisor_task;
static TaskHandle_t s_uvc_ingest_task;
static TaskHandle_t s_uvc_return_task;

/* Only the supervisor writes the stream handle.  The callback takes an atomic
 * snapshot and includes it in every retained-frame queue item. */
static uvc_host_stream_hdl_t s_uvc_stream;
/* The UVC stream object owns the scarce internal-DMA URB ring.  Keep that
 * object alive across ordinary consumer/owner changes and only destroy it on
 * a real transport failure or device replacement.  `started` describes the
 * alternate-setting/URB state; `open_selection` describes the format already
 * negotiated into the retained stream object. */
static si_uvc_session_state_t s_uvc_session_state = SI_UVC_SESSION_CLOSED;
static si_uvc_selection_t s_uvc_open_selection;
static uint32_t s_uvc_open_quality;
static uint32_t s_uvc_requested_mjpeg_quality;
static uint32_t s_uvc_epoch = 1U;
/* Revoked before stop/drain begins and enabled only after stream_start()
 * succeeds.  Epoch alone cannot reject a callback produced in the small
 * interval between generation invalidation and the driver's stop call. */
static bool s_uvc_accept_frames;
/* Packed callback-admission state.  The high bit is the H.264 transition cap;
 * the low bits count frames retained from usb_host_uvc.  Sharing one CAS
 * linearization point prevents a callback from reserving the final physical
 * driver buffer in the gap between enabling the cap and publishing H.264. */
static uint32_t s_uvc_retention_state;
static uint32_t s_uvc_callbacks;
static uint32_t s_uvc_queue_drops;
static uint32_t s_h264_pressure_coalesces;
static uint32_t s_uvc_buffer_underflows;
static uint32_t s_uvc_buffer_overflows;
static uint32_t s_uvc_return_pending;
static uint32_t s_uvc_return_retries;
static uint32_t s_uvc_return_failures;
static uint32_t s_uvc_invalid_streak;
static bool s_uvc_transport_fault;
/* The UVC driver pauses a stream before reporting DEVICE_DISCONNECTED.  The
 * callback may run while the application state mutex is busy, so it only
 * publishes this atomic edge.  The supervisor consumes it, invalidates the
 * selected device under s_lock, and owns the close transition. */
static bool s_uvc_device_gone_pending;
/* Supervisor-owned level state.  Keep it asserted until the old stream handle
 * is safely closed; close retries must not fall back to SET_INTERFACE on a
 * device which is already absent. */
static bool s_uvc_device_gone_active;
static bool s_uvc_root_forced_off;
static bool s_uvc_close_pending;
static uint32_t s_uvc_stop_failures;
static uint32_t s_no_frame_reopen_count;

static uint8_t *s_uvc_frame_arena;
static uint8_t *s_uvc_frame_buffers[SI_CFG_VIDEO_FRAME_BUFFERS];
static size_t s_uvc_frame_capacity;
static void *s_uvc_dma_reserve;
static bool s_boot_memory_reserve_claimed;
static bool s_boot_memory_reserved;

static si_uvc_selection_t s_uvc_selection;
static EXT_RAM_BSS_ATTR si_uvc_mode_t s_modes[SI_VIDEO_MAX_MODES];
static size_t s_mode_count;
static uint32_t s_mode_generation;
static uint32_t s_target_width;
static uint32_t s_target_height;
static uint32_t s_target_fps_x100;
static bool s_target_exact_fps;
static bool s_capture_enabled;
static uint32_t s_capture_stride_ms;
static char s_capture_owner[16];
static uint32_t s_last_accepted_frame_ms;
static uint32_t s_fps_last_ms;
static uint32_t s_fps_last_frames;
static bool s_h264_transport_active;
static uint32_t s_mjpeg_snapshot_request_generation;
static uint32_t s_mjpeg_snapshot_completed_generation;
static uint32_t s_h264_frame_epoch = 1U;
static uint32_t s_h264_next_frame_id;
static si_h264_uvc_lease_t s_h264_borrowed;
static uint32_t s_ingest_validate_last_us;
static uint32_t s_ingest_validate_max_us;
static uint32_t s_ingest_publish_last_us;
static uint32_t s_ingest_publish_max_us;
static uint32_t s_ingest_total_last_us;
static uint32_t s_ingest_total_max_us;

static void atomic_update_max_u32(uint32_t *target, uint32_t value)
{
    uint32_t current = __atomic_load_n(target, __ATOMIC_RELAXED);
    while (value > current &&
           !__atomic_compare_exchange_n(target, &current, value, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
}

static uint32_t diff_u32(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t uvc_frame_heap_caps(void)
{
#ifdef CONFIG_SPIRAM
    return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    return MALLOC_CAP_8BIT;
#endif
}

static const char *uvc_format_name(enum uvc_host_stream_format format)
{
    switch (format) {
    case UVC_VS_FORMAT_MJPEG:
        return "MJPEG";
    case UVC_VS_FORMAT_YUY2:
        return "YUY2";
    case UVC_VS_FORMAT_H264:
        return "H264";
    case UVC_VS_FORMAT_H265:
        return "H265";
    case UVC_VS_FORMAT_NV12:
        return "NV12";
    case UVC_VS_FORMAT_DEFAULT:
    default:
        return "DEFAULT";
    }
}

static uint32_t uvc_interval_to_fps_x100(uint32_t interval)
{
    return interval == 0U ? 0U :
           (uint32_t)((1000000000ULL + interval / 2U) / interval);
}

static float uvc_interval_to_fps(uint32_t interval)
{
    return interval == 0U ? 0.0f : 10000000.0f / (float)interval;
}

static uint32_t fps_float_to_x100(float fps)
{
    return fps <= 0.0f ? 0U : (uint32_t)(fps * 100.0f + 0.5f);
}

static void set_error_locked(const char *message, esp_err_t err)
{
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s: %s",
             message ? message : "video", esp_err_to_name(err));
}

static void update_fps_locked(uint32_t timestamp_ms)
{
    if (s_fps_last_ms == 0U) {
        s_fps_last_ms = timestamp_ms;
        s_fps_last_frames = s_status.frames_encoded;
        return;
    }
    const uint32_t elapsed = timestamp_ms - s_fps_last_ms;
    if (elapsed >= 1000U) {
        const uint32_t frames = s_status.frames_encoded - s_fps_last_frames;
        s_status.fps_x100 = (frames * 100000U) / elapsed;
        s_fps_last_ms = timestamp_ms;
        s_fps_last_frames = s_status.frames_encoded;
    }
}

static void reset_frame_state_locked(const char *message)
{
    s_status.streaming = false;
    s_status.frame_ready = false;
    s_status.last_jpeg_size = 0U;
    s_status.frame_interval_ms = 0U;
    s_status.last_frame_ms = 0U;
    s_status.fps_x100 = 0U;
    s_last_accepted_frame_ms = 0U;
    s_fps_last_ms = 0U;
    s_fps_last_frames = s_status.frames_encoded;
    if (message) {
        strlcpy(s_status.last_error, message, sizeof(s_status.last_error));
    } else {
        s_status.last_error[0] = '\0';
    }
}

static uint32_t mode_area(const si_uvc_mode_t *mode)
{
    return mode->public_mode.width * mode->public_mode.height;
}

static bool mode_is_better(const si_uvc_mode_t *candidate,
                           const si_uvc_mode_t *best,
                           uint32_t width, uint32_t height,
                           uint32_t fps_x100)
{
    if (!best) {
        return true;
    }
    const uint32_t target_area = width * height;
    const uint32_t candidate_area = mode_area(candidate);
    const uint32_t best_area = mode_area(best);
    uint32_t candidate_rank = candidate_area <= target_area ? 0U : 1U;
    uint32_t best_rank = best_area <= target_area ? 0U : 1U;
    if (candidate->public_mode.width == width &&
        candidate->public_mode.height == height) {
        candidate_rank = 0U;
    }
    if (best->public_mode.width == width && best->public_mode.height == height) {
        best_rank = 0U;
    }
    if (candidate_rank != best_rank) {
        return candidate_rank < best_rank;
    }
    const uint32_t candidate_area_diff = diff_u32(candidate_area, target_area);
    const uint32_t best_area_diff = diff_u32(best_area, target_area);
    if (candidate_area_diff != best_area_diff) {
        return candidate_area_diff < best_area_diff;
    }
    if (fps_x100 > 0U) {
        const uint32_t candidate_fps_diff =
            diff_u32(candidate->public_mode.fps_x100, fps_x100);
        const uint32_t best_fps_diff =
            diff_u32(best->public_mode.fps_x100, fps_x100);
        if (candidate_fps_diff != best_fps_diff) {
            return candidate_fps_diff < best_fps_diff;
        }
    }
    return candidate->public_mode.fps_x100 > best->public_mode.fps_x100;
}

static bool select_stored_mode_locked(uint32_t width, uint32_t height,
                                      bool exact_resolution,
                                      uint32_t fps_x100, bool exact_fps,
                                      si_uvc_selection_t *selection)
{
    if (!selection || s_mode_count == 0U) {
        return false;
    }
    const si_uvc_mode_t *best = NULL;
    for (size_t i = 0; i < s_mode_count; i++) {
        const si_uvc_mode_t *mode = &s_modes[i];
        if (exact_resolution &&
            (mode->public_mode.width != width ||
             mode->public_mode.height != height)) {
            continue;
        }
        if (exact_fps && fps_x100 > 0U &&
            diff_u32(mode->public_mode.fps_x100, fps_x100) > 50U) {
            continue;
        }
        if (mode_is_better(mode, best, width, height, fps_x100)) {
            best = mode;
        }
    }
    if (!best) {
        return false;
    }

    *selection = (si_uvc_selection_t) {
        .valid = true,
        .dev_addr = best->dev_addr,
        .stream_index = best->stream_index,
        .logical_fps_x100 = best->public_mode.fps_x100,
        .format = best->format,
    };
    return true;
}

static void add_uvc_mode_locked(const uvc_host_frame_info_t *info,
                                uint8_t dev_addr, uint8_t stream_index,
                                uint32_t fps_x100)
{
    if (!info || fps_x100 == 0U || s_mode_count >= SI_VIDEO_MAX_MODES) {
        return;
    }
    for (size_t i = 0; i < s_mode_count; i++) {
        const si_uvc_mode_t *mode = &s_modes[i];
        if (mode->public_mode.width == info->h_res &&
            mode->public_mode.height == info->v_res &&
            mode->format.format == info->format &&
            diff_u32(mode->public_mode.fps_x100, fps_x100) <= 50U) {
            return;
        }
    }
    si_uvc_mode_t *mode = &s_modes[s_mode_count++];
    mode->public_mode.width = info->h_res;
    mode->public_mode.height = info->v_res;
    mode->public_mode.fps_x100 = fps_x100;
    strlcpy(mode->public_mode.pixel_format, uvc_format_name(info->format),
            sizeof(mode->public_mode.pixel_format));
    mode->dev_addr = dev_addr;
    mode->stream_index = stream_index;
    mode->format = (uvc_host_stream_format_t) {
        .h_res = info->h_res,
        .v_res = info->v_res,
        .fps = fps_x100 / 100.0f,
        .format = info->format,
    };
}

static void store_uvc_modes_locked(const uvc_host_frame_info_t *list,
                                   size_t list_size, uint8_t dev_addr,
                                   uint8_t stream_index)
{
    s_mode_count = 0U;
    memset(s_modes, 0, sizeof(s_modes));
    for (size_t i = 0; i < list_size; i++) {
        const uvc_host_frame_info_t *info = &list[i];
        if (info->format != UVC_VS_FORMAT_MJPEG ||
            info->h_res == 0U || info->v_res == 0U) {
            continue;
        }
        if (info->interval_type == 0U) {
            add_uvc_mode_locked(info, dev_addr, stream_index,
                                uvc_interval_to_fps_x100(
                                    info->default_interval));
        } else {
            uint8_t count = info->interval_type;
            if (count > CONFIG_UVC_INTERVAL_ARRAY_SIZE) {
                count = CONFIG_UVC_INTERVAL_ARRAY_SIZE;
            }
            for (uint8_t j = 0; j < count; j++) {
                add_uvc_mode_locked(info, dev_addr, stream_index,
                                    uvc_interval_to_fps_x100(
                                        info->interval[j]));
            }
            add_uvc_mode_locked(info, dev_addr, stream_index,
                                uvc_interval_to_fps_x100(
                                    info->default_interval));
        }
    }
    s_status.modes_count = (uint32_t)s_mode_count;
    s_mode_generation++;
}

static bool uvc_selection_matches(const si_uvc_selection_t *a,
                                  const si_uvc_selection_t *b)
{
    return a && b && a->valid && b->valid &&
           a->dev_addr == b->dev_addr &&
           a->stream_index == b->stream_index &&
           a->format.h_res == b->format.h_res &&
           a->format.v_res == b->format.v_res &&
           a->format.format == b->format.format &&
           diff_u32(fps_float_to_x100(a->format.fps),
                    fps_float_to_x100(b->format.fps)) <= 50U;
}

static bool uvc_transport_matches(const si_uvc_selection_t *a,
                                  const si_uvc_selection_t *b)
{
    return a && b && a->valid && b->valid &&
           a->dev_addr == b->dev_addr &&
           a->stream_index == b->stream_index &&
           a->format.h_res == b->format.h_res &&
           a->format.v_res == b->format.v_res &&
           a->format.format == b->format.format &&
           diff_u32(fps_float_to_x100(a->format.fps),
                    fps_float_to_x100(b->format.fps)) <= 50U;
}

static esp_err_t get_uvc_selection(si_uvc_selection_t *selection,
                                   bool *selection_valid,
                                   bool *capture_requested,
                                   uint32_t *mjpeg_quality)
{
    if (!selection || !selection_valid) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const bool valid = s_uvc_selection.valid;
    *selection_valid = valid;
    if (valid) {
        *selection = s_uvc_selection;
    }
    if (capture_requested) {
        *capture_requested = s_capture_enabled;
    }
    if (mjpeg_quality) {
        *mjpeg_quality = s_status.jpeg_quality;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static esp_err_t reserve_uvc_dma_budget(void)
{
    if (s_uvc_dma_reserve) {
        return ESP_OK;
    }
    /* usb_host_transfer_alloc() needs four cache-aligned internal-DMA payload
     * buffers (12 KiB each after the MS2109 MPS rounding), plus the endpoint
     * descriptor list. Reserve one aligned block during boot, before optional
     * codecs and HTTP clients can fragment this heap. The guarded UVC component
     * retains that first complete URB ring across later physical closes, so a
     * restart never depends on coalescing this heap back into one large block. */
    s_uvc_dma_reserve = heap_caps_aligned_alloc(
        512U, SI_UVC_DMA_RESERVE_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_uvc_dma_reserve, ESP_ERR_NO_MEM, TAG,
                        "reserve UVC internal-DMA budget");
    ESP_LOGI(TAG, "reserved %u bytes for retained UVC DMA ring",
             (unsigned)SI_UVC_DMA_RESERVE_BYTES);
    return ESP_OK;
}

static void release_uvc_dma_budget_for_open(void)
{
    if (!s_uvc_dma_reserve) {
        return;
    }
    heap_caps_free(s_uvc_dma_reserve);
    s_uvc_dma_reserve = NULL;
    ESP_LOGI(TAG, "released UVC DMA reserve for stream open");
}

static esp_err_t reserve_uvc_frame_arena(void)
{
    if (s_uvc_frame_arena) {
        return ESP_OK;
    }
    const size_t raw_max =
        (size_t)SI_CFG_VIDEO_WIDTH * (size_t)SI_CFG_VIDEO_HEIGHT * 2U;
    s_uvc_frame_capacity = raw_max > SI_CFG_VIDEO_JPEG_BUFFER_SIZE ?
                           raw_max : SI_CFG_VIDEO_JPEG_BUFFER_SIZE;
    if (s_uvc_frame_capacity == 0U ||
        (size_t)SI_CFG_VIDEO_FRAME_BUFFERS >
            SIZE_MAX / s_uvc_frame_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t total =
        s_uvc_frame_capacity * (size_t)SI_CFG_VIDEO_FRAME_BUFFERS;
    s_uvc_frame_arena = heap_caps_aligned_alloc(
        128U, total, uvc_frame_heap_caps());
    ESP_RETURN_ON_FALSE(s_uvc_frame_arena, ESP_ERR_NO_MEM, TAG,
                        "allocate persistent UVC frame arena");
    for (size_t i = 0; i < SI_CFG_VIDEO_FRAME_BUFFERS; i++) {
        s_uvc_frame_buffers[i] =
            s_uvc_frame_arena + i * s_uvc_frame_capacity;
    }
    ESP_LOGI(TAG, "UVC arena ready: buffers=%u stride=%u total=%u",
             (unsigned)SI_CFG_VIDEO_FRAME_BUFFERS,
             (unsigned)s_uvc_frame_capacity, (unsigned)total);
    return ESP_OK;
}

static void note_drop_locked(const char *message)
{
    s_status.frames_dropped++;
    if (message) {
        strlcpy(s_status.last_error, message, sizeof(s_status.last_error));
    }
}

static uint32_t uvc_retained_frame_count(void)
{
    return __atomic_load_n(&s_uvc_retention_state, __ATOMIC_ACQUIRE) &
           SI_UVC_RETENTION_COUNT_MASK;
}

static void release_uvc_frame_retention(void)
{
    uint32_t state = __atomic_load_n(&s_uvc_retention_state,
                                     __ATOMIC_ACQUIRE);
    while (true) {
        const uint32_t count = state & SI_UVC_RETENTION_COUNT_MASK;
        /* Every callback which returns false creates exactly one lease.  A
         * release at zero is a double-return/ownership corruption, not
         * recoverable backpressure; keep the packed CAP bit intact even in
         * non-assert builds. */
        assert(count > 0U);
        if (count == 0U) {
            __atomic_store_n(&s_uvc_transport_fault, true,
                             __ATOMIC_RELEASE);
            return;
        }
        const uint32_t next =
            (state & SI_UVC_RETENTION_CAP_BIT) | (count - 1U);
        if (__atomic_compare_exchange_n(&s_uvc_retention_state, &state, next,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return;
        }
    }
}

static void return_retained_uvc_frame(const si_uvc_frame_item_t *item)
{
    if (!item || !item->stream || !item->frame) {
        ESP_LOGE(TAG, "invalid retained UVC frame release");
        return;
    }
    /* usb_host_uvc's return operation only resets the frame descriptor and
     * performs a zero-time send to its empty-buffer queue.  Return the common
     * path synchronously so a dropped/coalesced H.264 input becomes available
     * to the next UVC FID immediately.  Deferring every return to another task
     * leaves all three physical buffers transiently retained while the first
     * H.264 pipeline is configured, which produces a real driver underflow at
     * the next frame boundary.
     *
     * A correctly owned frame cannot fail this queue send: it was removed from
     * that same queue before being handed to us.  Keep the bounded recovery
     * worker for defensive fault containment, but only after an actual driver
     * return failure.  End-to-end retention is decremented only after the
     * driver has accepted the frame. */
    esp_err_t ret = uvc_host_frame_return(
        item->stream, (uvc_host_frame_t *)item->frame);
    if (ret == ESP_OK) {
        release_uvc_frame_retention();
        return;
    }

    const si_uvc_return_item_t pending = {
        .item = *item,
        .attempts = 1U,
    };
    __atomic_add_fetch(&s_uvc_return_failures, 1U, __ATOMIC_RELAXED);
    ESP_LOGW(TAG, "direct UVC frame return failed; scheduling recovery: %s",
             esp_err_to_name(ret));
    __atomic_add_fetch(&s_uvc_return_pending, 1U, __ATOMIC_ACQ_REL);
    if (!s_uvc_return_queue ||
        xQueueSend(s_uvc_return_queue, &pending, 0) != pdPASS) {
        __atomic_sub_fetch(&s_uvc_return_pending, 1U, __ATOMIC_ACQ_REL);
        __atomic_add_fetch(&s_uvc_return_failures, 1U, __ATOMIC_RELAXED);
        __atomic_store_n(&s_uvc_transport_fault, true, __ATOMIC_RELEASE);
        ESP_LOGE(TAG,
                 "UVC return recovery queue invariant failed; retaining stream ownership");
        if (s_uvc_supervisor_task) {
            xTaskNotifyGive(s_uvc_supervisor_task);
        }
    }
}

static void uvc_return_task(void *arg)
{
    (void)arg;
    si_uvc_return_item_t pending = {0};
    while (true) {
        if (xQueueReceive(s_uvc_return_queue, &pending,
                          portMAX_DELAY) != pdPASS) {
            continue;
        }
        esp_err_t ret = uvc_host_frame_return(
            pending.item.stream, (uvc_host_frame_t *)pending.item.frame);
        if (ret == ESP_OK) {
            __atomic_sub_fetch(&s_uvc_return_pending, 1U,
                               __ATOMIC_ACQ_REL);
            release_uvc_frame_retention();
            if (pending.attempts > 0U && s_uvc_supervisor_task) {
                xTaskNotifyGive(s_uvc_supervisor_task);
            }
            memset(&pending, 0, sizeof(pending));
            continue;
        }

        pending.attempts++;
        __atomic_add_fetch(&s_uvc_return_retries, 1U, __ATOMIC_RELAXED);
        if (pending.attempts == 1U) {
            __atomic_add_fetch(&s_uvc_return_failures, 1U,
                               __ATOMIC_RELAXED);
        }
        if (pending.attempts == 1U ||
            pending.attempts % SI_UVC_RETURN_LOG_INTERVAL == 0U) {
            ESP_LOGW(TAG,
                     "return retained UVC frame retry=%u pending=%u: %s",
                     (unsigned)pending.attempts,
                     (unsigned)__atomic_load_n(&s_uvc_return_pending,
                                                __ATOMIC_RELAXED),
                     esp_err_to_name(ret));
        }

        /* A failed zero-time queue return is normally transient: the UVC
         * producer must first take another empty buffer. Rotate this lease to
         * the tail so other independently returnable frames are not starved.
         * With queue capacity > physical frame count, requeue cannot block. */
        if (xQueueSend(s_uvc_return_queue, &pending, 0) != pdPASS) {
            ESP_LOGE(TAG,
                     "UVC return retry queue invariant failed; retrying locally");
            do {
                vTaskDelay(pdMS_TO_TICKS(SI_UVC_RETURN_RETRY_DELAY_MS));
                ret = uvc_host_frame_return(
                    pending.item.stream,
                    (uvc_host_frame_t *)pending.item.frame);
                __atomic_add_fetch(&s_uvc_return_retries, 1U,
                                   __ATOMIC_RELAXED);
            } while (ret != ESP_OK);
            __atomic_sub_fetch(&s_uvc_return_pending, 1U,
                               __ATOMIC_ACQ_REL);
            release_uvc_frame_retention();
            memset(&pending, 0, sizeof(pending));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(SI_UVC_RETURN_RETRY_DELAY_MS));
    }
}

/* Caller holds s_h264_frame_lock. Borrowed frames are deliberately not
 * force-returned: the consumer remains responsible for the matching release.
 * A queued frame is only detached here; after the lease mutex is released the
 * caller returns it synchronously, transferring it to the recovery worker only
 * if the driver's zero-time empty-buffer queue send actually fails. */
static bool take_queued_h264_frame_locked(si_uvc_frame_item_t *item)
{
    if (!s_h264_frame_queue || !item) {
        return false;
    }
    memset(item, 0, sizeof(*item));
    return xQueueReceive(s_h264_frame_queue, item, 0) == pdPASS;
}

static void advance_h264_frame_epoch_locked(void)
{
    if (++s_h264_frame_epoch == 0U) {
        s_h264_frame_epoch = 1U;
    }
}

static void invalidate_h264_frame_queue(void)
{
    si_uvc_frame_item_t queued = {0};
    bool have_queued = false;
    if (!s_h264_frame_lock ||
        xSemaphoreTake(s_h264_frame_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    advance_h264_frame_epoch_locked();
    have_queued = take_queued_h264_frame_locked(&queued);
    xSemaphoreGive(s_h264_frame_lock);
    if (have_queued) {
        return_retained_uvc_frame(&queued);
    }
}

static esp_err_t handoff_h264_uvc_frame(const si_uvc_frame_item_t *source,
                                        uint32_t *out_frame_id)
{
    if (!source || !source->frame || !source->stream ||
        !s_h264_frame_queue || !s_h264_frame_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_h264_frame_lock, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint32_t active_epoch =
        __atomic_load_n(&s_uvc_epoch, __ATOMIC_ACQUIRE);
    const uvc_host_stream_hdl_t active_stream =
        __atomic_load_n(&s_uvc_stream, __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&s_h264_transport_active, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&s_uvc_accept_frames, __ATOMIC_ACQUIRE) ||
        source->epoch != active_epoch || source->stream != active_stream) {
        xSemaphoreGive(s_h264_frame_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* Three UVC buffers permit one frame in the decoder, one frame pending
     * for the next decode, and one frame owned by USB.  Do not reject the
     * pending frame merely because the consumer still holds its current
     * lease; the single-slot queue is the backpressure boundary. */
    if (uxQueueMessagesWaiting(s_h264_frame_queue) != 0U) {
        xSemaphoreGive(s_h264_frame_lock);
        return ESP_ERR_NOT_FINISHED;
    }

    si_uvc_frame_item_t queued = *source;
    queued.h264_epoch = s_h264_frame_epoch;
    if (++s_h264_next_frame_id == 0U) {
        s_h264_next_frame_id = 1U;
    }
    queued.frame_id = s_h264_next_frame_id;
    if (xQueueSend(s_h264_frame_queue, &queued, 0) != pdPASS) {
        xSemaphoreGive(s_h264_frame_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    if (out_frame_id) {
        *out_frame_id = queued.frame_id;
    }
    xSemaphoreGive(s_h264_frame_lock);
    return ESP_OK;
}

static bool try_reserve_uvc_frame(void)
{
    /* During H.264 transition/operation, keep one physical UVC frame in the
     * driver's ring.  Normal MJPEG operation deliberately keeps its existing
     * uncapped callback/ingest contract.
     *
     * CAP and count occupy one atomic word: either this reservation linearizes
     * first under ordinary MJPEG semantics, or cap activation linearizes first
     * and this callback observes the N-1 boundary. */
    const uint32_t limit = SI_CFG_VIDEO_FRAME_BUFFERS - 1U;
    uint32_t state = __atomic_load_n(&s_uvc_retention_state,
                                     __ATOMIC_ACQUIRE);
    while (true) {
        const uint32_t retained = state & SI_UVC_RETENTION_COUNT_MASK;
        if ((state & SI_UVC_RETENTION_CAP_BIT) != 0U && retained >= limit) {
            return false;
        }
        assert(retained < SI_UVC_RETENTION_COUNT_MASK);
        if (retained == SI_UVC_RETENTION_COUNT_MASK) {
            return false;
        }
        const uint32_t next =
            (state & SI_UVC_RETENTION_CAP_BIT) | (retained + 1U);
        if (__atomic_compare_exchange_n(&s_uvc_retention_state, &state, next,
                                        false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
}

static bool try_enable_uvc_retention_cap(void)
{
    const uint32_t limit = SI_CFG_VIDEO_FRAME_BUFFERS - 1U;
    uint32_t state = __atomic_load_n(&s_uvc_retention_state,
                                     __ATOMIC_ACQUIRE);
    while (true) {
        const uint32_t retained = state & SI_UVC_RETENTION_COUNT_MASK;
        if (retained > limit) {
            return false;
        }
        if ((state & SI_UVC_RETENTION_CAP_BIT) != 0U) {
            return true;
        }
        const uint32_t next = state | SI_UVC_RETENTION_CAP_BIT;
        if (__atomic_compare_exchange_n(&s_uvc_retention_state, &state, next,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
}

static void disable_uvc_retention_cap(void)
{
    __atomic_fetch_and(&s_uvc_retention_state,
                       SI_UVC_RETENTION_COUNT_MASK, __ATOMIC_ACQ_REL);
}

static bool uvc_frame_cb(const uvc_host_frame_t *frame, void *user_ctx)
{
    (void)user_ctx;
    if (!frame || !frame->data || frame->data_len == 0U ||
        frame->vs_format.format != UVC_VS_FORMAT_MJPEG) {
        return true;
    }
    __atomic_add_fetch(&s_uvc_callbacks, 1U, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&s_uvc_accept_frames, __ATOMIC_ACQUIRE)) {
        return true;
    }
    const uvc_host_stream_hdl_t stream =
        __atomic_load_n(&s_uvc_stream, __ATOMIC_ACQUIRE);
    if (!stream || !s_uvc_frame_queue) {
        return true;
    }
    const si_uvc_frame_item_t item = {
        .frame = frame,
        .stream = stream,
        .epoch = __atomic_load_n(&s_uvc_epoch, __ATOMIC_ACQUIRE),
    };
    if (!try_reserve_uvc_frame()) {
        /* Returning true transfers this completed frame straight back to
         * usb_host_uvc in the current transfer callback.  This is an
         * intentional H.264 transition/operation pressure coalesce, not a
         * queue or UVC error. */
        __atomic_add_fetch(&s_h264_pressure_coalesces, 1U,
                           __ATOMIC_RELAXED);
        return true;
    }
    if (xQueueSend(s_uvc_frame_queue, &item, 0) != pdPASS) {
        release_uvc_frame_retention();
        __atomic_add_fetch(&s_uvc_queue_drops, 1U, __ATOMIC_RELAXED);
        return true;
    }
    /* Ownership is now exclusively with ingest until frame_return(). */
    return false;
}

static void uvc_ingest_task(void *arg)
{
    (void)arg;
    si_uvc_frame_item_t item = {0};
    while (true) {
        if (xQueueReceive(s_uvc_frame_queue, &item, portMAX_DELAY) != pdPASS) {
            continue;
        }
        const int64_t ingest_started_us = esp_timer_get_time();
        uint32_t validate_us = 0U;
        uint32_t publish_us = 0U;
        bool measured_publish = false;
        const uint32_t active_epoch =
            __atomic_load_n(&s_uvc_epoch, __ATOMIC_ACQUIRE);
        const uvc_host_stream_hdl_t active_stream =
            __atomic_load_n(&s_uvc_stream, __ATOMIC_ACQUIRE);
        const bool current =
                             __atomic_load_n(&s_uvc_accept_frames,
                                             __ATOMIC_ACQUIRE) &&
                             item.epoch == active_epoch &&
                             item.stream == active_stream;
        bool should_publish = current;
        uint32_t timestamp_ms = now_ms();
        const bool h264_route = __atomic_load_n(
            &s_h264_transport_active, __ATOMIC_ACQUIRE);

        if (current && s_lock &&
            xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_status.frames_captured++;
            should_publish = s_capture_enabled;
            if (should_publish && s_capture_stride_ms > 0U &&
                s_last_accepted_frame_ms != 0U &&
                timestamp_ms - s_last_accepted_frame_ms <
                    s_capture_stride_ms) {
                should_publish = false;
            }
            xSemaphoreGive(s_lock);
        } else if (current) {
            should_publish = false;
        }

        si_mjpeg_validation_t validation = {0};
        bool valid_mjpeg = true;
        if (should_publish) {
            const int64_t validate_started_us = esp_timer_get_time();
            valid_mjpeg = si_mjpeg_validate(item.frame->data,
                                            item.frame->data_len,
                                            item.frame->vs_format.h_res,
                                            item.frame->vs_format.v_res,
                                            &validation);
            validate_us = (uint32_t)(esp_timer_get_time() -
                                     validate_started_us);
        }
        if (should_publish && !valid_mjpeg) {
            const uint32_t streak = __atomic_add_fetch(
                &s_uvc_invalid_streak, 1U, __ATOMIC_ACQ_REL);
            if (s_lock &&
                xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                note_drop_locked("invalid UVC MJPEG frame");
                xSemaphoreGive(s_lock);
            }
            if (streak >= SI_UVC_INVALID_FRAME_FAULT_LIMIT) {
                __atomic_store_n(&s_uvc_transport_fault, true,
                                 __ATOMIC_RELEASE);
                if (s_uvc_supervisor_task) {
                    xTaskNotifyGive(s_uvc_supervisor_task);
                }
            }
            should_publish = false;
        }
        if (should_publish && h264_route && validation.sof_marker != 0xc0U) {
            /* ESP32-P4's JPEG decoder accepts baseline SOF0 only. Keep other
             * structurally valid JPEG families available to the browser's
             * MJPEG path, but never feed them to the hardware H.264 chain. */
            if (s_lock &&
                xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                note_drop_locked("H.264 input is not baseline JPEG");
                xSemaphoreGive(s_lock);
            }
            should_publish = false;
        }

        bool h264_owns_frame = false;
        if (should_publish) {
            uint32_t frame_id = 0U;
            const int64_t publish_started_us = esp_timer_get_time();
            if (h264_route) {
                const uint32_t snapshot_request = __atomic_load_n(
                    &s_mjpeg_snapshot_request_generation,
                    __ATOMIC_ACQUIRE);
                const uint32_t snapshot_completed = __atomic_load_n(
                    &s_mjpeg_snapshot_completed_generation,
                    __ATOMIC_ACQUIRE);
                if (snapshot_request != snapshot_completed &&
                    si_video_frame_store_publish(item.frame->data,
                                                 item.frame->data_len,
                                                 NULL) == ESP_OK) {
                    /* Copy at most one validated input frame per snapshot
                     * generation. Normal H.264 operation remains zero-copy
                     * from the retained UVC lease. */
                    __atomic_store_n(
                        &s_mjpeg_snapshot_completed_generation,
                        snapshot_request, __ATOMIC_RELEASE);
                }
            }
            esp_err_t publish_ret = h264_route ?
                handoff_h264_uvc_frame(&item, &frame_id) :
                si_video_frame_store_publish(item.frame->data,
                                             item.frame->data_len,
                                             &frame_id);
            publish_us = (uint32_t)(esp_timer_get_time() - publish_started_us);
            measured_publish = true;
            if (publish_ret == ESP_OK) {
                h264_owns_frame = h264_route;
                __atomic_store_n(&s_uvc_invalid_streak, 0U,
                                 __ATOMIC_RELEASE);
                if (s_lock &&
                    xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                    s_status.width = item.frame->vs_format.h_res;
                    s_status.height = item.frame->vs_format.v_res;
                    s_status.last_jpeg_size = item.frame->data_len;
                    s_status.frame_interval_ms =
                        s_last_accepted_frame_ms == 0U ? 0U :
                        timestamp_ms - s_last_accepted_frame_ms;
                    s_status.last_frame_ms = 0U;
                    s_status.frames_encoded++;
                    s_status.frame_ready = true;
                    s_status.streaming = true;
                    strlcpy(s_status.pixel_format, "MJPEG",
                            sizeof(s_status.pixel_format));
                    s_status.last_error[0] = '\0';
                    s_last_accepted_frame_ms = timestamp_ms;
                    update_fps_locked(timestamp_ms);
                    xSemaphoreGive(s_lock);
                }
            } else if (s_lock &&
                       xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (!h264_route &&
                    publish_ret == ESP_ERR_NOT_FINISHED) {
                    /* The one-arena store deliberately coalesces when a
                     * borrowed old frame plus the incoming complex frame do
                     * not fit concurrently.  This is normal latest-frame
                     * backpressure, not UVC corruption or a transport fault. */
                    s_status.frames_dropped++;
                } else if (h264_route) {
                    if (publish_ret == ESP_ERR_NOT_FINISHED) {
                        /* A 30/50 fps UVC producer feeding the product's
                         * 25 fps H.264 policy will periodically coalesce a
                         * pending latest frame. This is bounded
                         * backpressure, not a capture fault, and must not
                         * poison last_error or make the browser tear down
                         * its only consumer. */
                        s_status.frames_dropped++;
                    } else {
                        note_drop_locked("H.264 UVC lease handoff failed");
                    }
                } else {
                    note_drop_locked(publish_ret == ESP_ERR_INVALID_SIZE ?
                                     "UVC MJPEG exceeds frame store" :
                                     "video frame store busy");
                }
                xSemaphoreGive(s_lock);
            }
        }

        if (!h264_owns_frame) {
            return_retained_uvc_frame(&item);
        }
        if (validate_us > 0U || measured_publish) {
            const uint32_t total_us =
                (uint32_t)(esp_timer_get_time() - ingest_started_us);
            __atomic_store_n(&s_ingest_validate_last_us, validate_us,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&s_ingest_publish_last_us, publish_us,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&s_ingest_total_last_us, total_us,
                             __ATOMIC_RELAXED);
            atomic_update_max_u32(&s_ingest_validate_max_us, validate_us);
            atomic_update_max_u32(&s_ingest_publish_max_us, publish_us);
            atomic_update_max_u32(&s_ingest_total_max_us, total_us);
        }
    }
}

static void uvc_stream_event_cb(const uvc_host_stream_event_data_t *event,
                                void *user_ctx)
{
    (void)user_ctx;
    if (!event) {
        return;
    }
    const char *error = NULL;
    bool fatal_transport_fault = false;
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        error = "UVC transfer error";
        fatal_transport_fault = true;
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        error = "UVC camera disconnected";
        fatal_transport_fault = true;
        /* Publish the disconnect independently of the best-effort status lock.
         * Otherwise a busy s_lock leaves the old selection valid and the
         * supervisor can reopen a stale dev_addr forever. */
        __atomic_store_n(&s_uvc_device_gone_pending, true,
                         __ATOMIC_RELEASE);
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        error = "UVC frame buffer overflow";
        __atomic_add_fetch(&s_uvc_buffer_overflows, 1U, __ATOMIC_RELAXED);
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        error = "UVC frame buffer underflow";
        __atomic_add_fetch(&s_uvc_buffer_underflows, 1U, __ATOMIC_RELAXED);
        break;
#ifdef UVC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case UVC_HOST_DEVICE_SUSPENDED:
        error = "UVC camera suspended";
        break;
    case UVC_HOST_DEVICE_RESUMED:
        ESP_LOGI(TAG, "UVC camera resumed");
        return;
#endif
    default:
        return;
    }

    /* Buffer pressure is normal backpressure with a bounded two-buffer UVC
     * ring: the driver drops the affected frame and can continue with the
     * next FID. Tearing down the whole USB stream here turns a recoverable
     * drop into a multi-second outage. A true transfer/disconnect error still
     * goes through the supervisor-owned close/reopen path. */
    if (fatal_transport_fault) {
        ESP_LOGW(TAG, "%s", error);
    } else {
        ESP_LOGD(TAG, "%s", error);
    }
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        note_drop_locked(error);
        xSemaphoreGive(s_lock);
    }
    if (fatal_transport_fault) {
        __atomic_store_n(&s_uvc_transport_fault, true, __ATOMIC_RELEASE);
        if (s_uvc_supervisor_task) {
            xTaskNotifyGive(s_uvc_supervisor_task);
        }
    }
}

static void uvc_driver_event_cb(const uvc_host_driver_event_data_t *event,
                                void *user_ctx)
{
    (void)user_ctx;
    if (!event || event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        return;
    }
    size_t list_size = event->device_connected.frame_info_num;
    if (list_size == 0U) {
        return;
    }
    uvc_host_frame_info_t *list = calloc(list_size, sizeof(*list));
    if (!list) {
        return;
    }
    esp_err_t ret = uvc_host_get_frame_list(
        event->device_connected.dev_addr,
        event->device_connected.uvc_stream_index,
        (uvc_host_frame_info_t (*)[])list, &list_size);
    if (ret != ESP_OK) {
        free(list);
        return;
    }
    for (size_t i = 0; i < list_size; i++) {
        ESP_LOGI(TAG, "UVC mode[%u]: %s %ux%u %.1ffps",
                 (unsigned)i, uvc_format_name(list[i].format),
                 list[i].h_res, list[i].v_res,
                 uvc_interval_to_fps(list[i].default_interval));
    }

    si_uvc_selection_t selected = {0};
    bool has_selection = false;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        store_uvc_modes_locked(list, list_size,
                               event->device_connected.dev_addr,
                               event->device_connected.uvc_stream_index);
        /* Physical camera selection is independent from browser demand.  Pick
         * and publish the configured mode as soon as enumeration completes so
         * the supervisor can pre-open the retained stream while the reserved
         * internal-DMA block is still contiguous.  s_capture_enabled controls
         * start/stop only; gating selection on it defers URB allocation until
         * the first authenticated HTTP request and recreates the fragmentation
         * failure that retained sessions are intended to eliminate. */
        has_selection = select_stored_mode_locked(
            s_target_width, s_target_height, true,
            s_target_fps_x100, s_target_exact_fps, &selected);
        if (has_selection) {
            s_uvc_selection = selected;
            s_status.width = selected.format.h_res;
            s_status.height = selected.format.v_res;
            s_status.target_fps_x100 = selected.logical_fps_x100;
            strlcpy(s_status.pixel_format, "MJPEG",
                    sizeof(s_status.pixel_format));
            reset_frame_state_locked(s_capture_enabled
                                         ? "opening UVC stream"
                                         : "video capture idle");
        } else {
            s_uvc_selection.valid = false;
            reset_frame_state_locked("configured UVC mode unavailable");
        }
        xSemaphoreGive(s_lock);
    }
    free(list);
    if (has_selection && s_uvc_supervisor_task) {
        xTaskNotifyGive(s_uvc_supervisor_task);
    }
}

static void usb_host_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t event_flags = 0U;
        esp_err_t ret = usb_host_lib_handle_events(portMAX_DELAY,
                                                    &event_flags);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "USB host event loop: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            (void)usb_host_device_free_all();
        }
    }
}

static bool wait_for_ingest_drain(uint32_t timeout_ms)
{
    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (uvc_retained_frame_count() > 0U) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

static void invalidate_stream_generation(void)
{
    __atomic_store_n(&s_uvc_accept_frames, false, __ATOMIC_RELEASE);
    uint32_t epoch = __atomic_add_fetch(&s_uvc_epoch, 1U,
                                        __ATOMIC_ACQ_REL);
    if (epoch == 0U) {
        __atomic_store_n(&s_uvc_epoch, 1U, __ATOMIC_RELEASE);
    }
    /* Revoke and return every queued direct UVC lease before the supervisor
     * waits for end-to-end ownership to drain. */
    invalidate_h264_frame_queue();
    si_video_frame_store_clear();
}

static bool power_cycle_uvc_root_port(void)
{
    if (__atomic_load_n(&s_uvc_stream, __ATOMIC_ACQUIRE)) {
        ESP_LOGE(TAG, "refusing USB power cycle with live stream handle");
        return false;
    }
    esp_err_t ret = usb_host_lib_set_root_port_power(false);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return false;
    }
    s_uvc_root_forced_off = true;
    vTaskDelay(pdMS_TO_TICKS(700));
    (void)usb_host_device_free_all();
    vTaskDelay(pdMS_TO_TICKS(300));
    ret = usb_host_lib_set_root_port_power(true);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        s_uvc_root_forced_off = false;
        vTaskDelay(pdMS_TO_TICKS(1000));
        return true;
    }
    return false;
}

static bool ensure_uvc_root_port_powered(void)
{
    if (!s_uvc_root_forced_off) {
        return true;
    }
    esp_err_t ret = usb_host_lib_set_root_port_power(true);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "restore USB root-port power: %s",
                 esp_err_to_name(ret));
        return false;
    }
    s_uvc_root_forced_off = false;
    vTaskDelay(pdMS_TO_TICKS(1000));
    return true;
}

static bool stop_uvc_stream(void)
{
    uvc_host_stream_hdl_t stream =
        __atomic_load_n(&s_uvc_stream, __ATOMIC_ACQUIRE);
    if (!stream) {
        s_uvc_session_state = SI_UVC_SESSION_CLOSED;
        return true;
    }

    invalidate_stream_generation();
    if (s_uvc_session_state == SI_UVC_SESSION_STARTED ||
        s_uvc_session_state == SI_UVC_SESSION_FAULTED) {
        esp_err_t stop_ret = uvc_host_stream_stop(stream);
        if (stop_ret != ESP_OK && stop_ret != ESP_ERR_INVALID_STATE) {
            s_uvc_stop_failures++;
            s_uvc_session_state = SI_UVC_SESSION_FAULTED;
            ESP_LOGW(TAG, "stop retained UVC stream: %s",
                     esp_err_to_name(stop_ret));
            return false;
        }
        s_uvc_session_state = SI_UVC_SESSION_DRAINING;
    } else if (s_uvc_session_state == SI_UVC_SESSION_OPEN_STOPPED) {
        return true;
    }
    if (!wait_for_ingest_drain(SI_UVC_CLOSE_DRAIN_TIMEOUT_MS)) {
        s_uvc_session_state = SI_UVC_SESSION_DRAINING;
        s_uvc_stop_failures++;
        ESP_LOGW(TAG, "UVC ingest did not drain after stop");
        return false;
    }
    s_uvc_session_state = SI_UVC_SESSION_OPEN_STOPPED;
    s_uvc_stop_failures = 0U;
    return true;
}

static bool close_uvc_stream(bool stop_first)
{
    uvc_host_stream_hdl_t stream =
        __atomic_load_n(&s_uvc_stream, __ATOMIC_ACQUIRE);
    if (!stream) {
        s_uvc_session_state = SI_UVC_SESSION_CLOSED;
        memset(&s_uvc_open_selection, 0, sizeof(s_uvc_open_selection));
        s_uvc_open_quality = 0U;
        return true;
    }

    if (stop_first && !stop_uvc_stream()) {
        /* Never power-cycle or free through a stop failure.  usb_host_uvc may
         * still have URB callbacks referencing this object; retrying is safe,
         * freeing it is not. */
        return false;
    } else if (!stop_first) {
        if (s_uvc_session_state == SI_UVC_SESSION_STARTED ||
            s_uvc_session_state == SI_UVC_SESSION_DRAINING ||
            s_uvc_session_state == SI_UVC_SESSION_FAULTED) {
            ESP_LOGW(TAG, "refusing UVC close before stopped/drained state");
            return false;
        }
        invalidate_stream_generation();
    }

    if (!wait_for_ingest_drain(SI_UVC_CLOSE_DRAIN_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "UVC ingest did not drain; keeping handle open");
        return false;
    }
    esp_err_t close_ret = uvc_host_stream_close(stream);
    if (close_ret != ESP_OK) {
        ESP_LOGW(TAG, "close UVC stream: %s", esp_err_to_name(close_ret));
        return false;
    }
    __atomic_store_n(&s_uvc_stream, NULL, __ATOMIC_RELEASE);
    s_uvc_session_state = SI_UVC_SESSION_CLOSED;
    memset(&s_uvc_open_selection, 0, sizeof(s_uvc_open_selection));
    s_uvc_open_quality = 0U;
    s_uvc_stop_failures = 0U;
    s_uvc_close_pending = false;

    return true;
}

static bool close_disconnected_uvc_stream(void)
{
    uvc_host_stream_hdl_t stream =
        __atomic_load_n(&s_uvc_stream, __ATOMIC_ACQUIRE);
    if (!stream) {
        s_uvc_session_state = SI_UVC_SESSION_CLOSED;
        memset(&s_uvc_open_selection, 0, sizeof(s_uvc_open_selection));
        s_uvc_open_quality = 0U;
        return true;
    }

    /* usb_host_uvc reports DEVICE_DISCONNECTED only after it has paused the
     * stream.  Calling uvc_host_stream_stop() here would issue SET_INTERFACE(0)
     * to a device which no longer exists and can therefore fail forever.  The
     * safe replacement is: revoke the application generation, drain every
     * frame retained by our callback, mark the already-paused handle stopped,
     * and then let the guarded upstream close release the interface. */
    invalidate_stream_generation();
    s_uvc_session_state = SI_UVC_SESSION_DRAINING;
    if (!wait_for_ingest_drain(SI_UVC_CLOSE_DRAIN_TIMEOUT_MS)) {
        s_uvc_stop_failures++;
        ESP_LOGW(TAG, "UVC ingest did not drain after device disconnect");
        return false;
    }
    s_uvc_session_state = SI_UVC_SESSION_OPEN_STOPPED;

    /* close_uvc_stream(false) never frees after a failed interface release:
     * the patched upstream close returns the error while retaining ownership
     * of the handle and its URBs, so the supervisor can retry safely. */
    return close_uvc_stream(false);
}

static void consume_uvc_device_gone(void)
{
    s_uvc_device_gone_active = true;

    /* The supervisor, rather than the driver callback, owns public selection
     * mutation.  Use an unbounded take here: proceeding with an uncleared
     * selection is worse than waiting for the short application-state critical
     * section, because it can reopen the disconnected device address. */
    if (s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_uvc_selection.valid = false;
        s_mode_count = 0U;
        memset(s_modes, 0, sizeof(s_modes));
        s_status.modes_count = 0U;
        s_mode_generation++;
        reset_frame_state_locked("UVC camera disconnected");
        xSemaphoreGive(s_lock);
    }
    s_uvc_close_pending = true;
}

static void uvc_supervisor_task(void *arg)
{
    (void)arg;
    TickType_t missing_since = 0;
    TickType_t last_missing_recovery = 0;
    while (true) {
        if (!ensure_uvc_root_port_powered()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (__atomic_exchange_n(&s_uvc_device_gone_pending, false,
                                __ATOMIC_ACQ_REL)) {
            consume_uvc_device_gone();
        }
        if (s_uvc_close_pending) {
            const bool closed = s_uvc_device_gone_active
                                    ? close_disconnected_uvc_stream()
                                    : close_uvc_stream(true);
            if (!closed) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            s_uvc_close_pending = false;
            s_uvc_device_gone_active = false;
        }
        if (__atomic_exchange_n(&s_uvc_transport_fault, false,
                                __ATOMIC_ACQ_REL)) {
            s_uvc_close_pending = true;
            continue;
        }
        si_uvc_selection_t selection = {0};
        bool selection_valid = false;
        bool capture_requested = false;
        uint32_t mjpeg_quality = SI_CFG_VIDEO_JPEG_QUALITY;
        esp_err_t selection_ret = get_uvc_selection(
            &selection, &selection_valid, &capture_requested,
            &mjpeg_quality);
        if (selection_ret != ESP_OK) {
            /* A busy application-state mutex is not evidence that the
             * physical selection disappeared.  Keep the working stream
             * untouched and retry instead of turning a 50 ms observation
             * timeout into a visible stop/restart cycle. */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!selection_valid) {
            /* No physical UVC selection exists.  Stop any old alternate
             * setting, but never destroy a retained handle merely because no
             * browser is connected. */
            if (s_uvc_session_state != SI_UVC_SESSION_OPEN_STOPPED &&
                s_uvc_session_state != SI_UVC_SESSION_CLOSED) {
                if (!stop_uvc_stream()) {
                    vTaskDelay(pdMS_TO_TICKS(250));
                    continue;
                }
            }
            /* set_capture(false) publishes idle before waking the supervisor.
             * The running-stream unwind temporarily publishes "restarting";
             * restore the terminal idle state even when stop already reached
             * OPEN_STOPPED before this iteration. */
            if (!capture_requested && s_lock &&
                xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                reset_frame_state_locked("video capture idle");
                xSemaphoreGive(s_lock);
            }
            TickType_t now = xTaskGetTickCount();
            bool recover = false;
            if (s_lock &&
                xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (s_capture_enabled && s_mode_count == 0U) {
                    if (missing_since == 0) {
                        missing_since = now;
                    }
                    if ((int32_t)(now - missing_since) >=
                            (int32_t)pdMS_TO_TICKS(
                                SI_UVC_MISSING_DEVICE_RECOVERY_DELAY_MS) &&
                        (last_missing_recovery == 0 ||
                         (int32_t)(now - last_missing_recovery) >=
                            (int32_t)pdMS_TO_TICKS(
                                SI_UVC_MISSING_DEVICE_RECOVERY_INTERVAL_MS))) {
                        recover = true;
                        last_missing_recovery = now;
                    }
                } else {
                    missing_since = 0;
                }
                xSemaphoreGive(s_lock);
            }
            if (recover && !s_uvc_stream) {
                (void)power_cycle_uvc_root_port();
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        missing_since = 0;

        uvc_host_stream_hdl_t opened =
            __atomic_load_n(&s_uvc_stream, __ATOMIC_ACQUIRE);
        if (opened && s_uvc_open_selection.valid &&
            (s_uvc_open_selection.dev_addr != selection.dev_addr ||
             s_uvc_open_selection.stream_index != selection.stream_index ||
             s_uvc_open_quality != mjpeg_quality)) {
            if (!close_uvc_stream(true)) {
                s_uvc_close_pending = true;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            opened = NULL;
        }

        esp_err_t ret = ESP_OK;
        if (!opened) {
            invalidate_stream_generation();
            __atomic_store_n(&s_uvc_invalid_streak, 0U, __ATOMIC_RELEASE);
            /* The boot reserve is released exactly once for the first ring.
             * Subsequent physical closes keep that stopped URB ring inside
             * usb_host_uvc and the next open rebinds it to the new device. */
            release_uvc_dma_budget_for_open();
            uvc_host_stream_config_t config = {
                .event_cb = uvc_stream_event_cb,
                .frame_cb = uvc_frame_cb,
                .usb = {
                    .dev_addr = selection.dev_addr,
                    .vid = UVC_HOST_ANY_VID,
                    .pid = UVC_HOST_ANY_PID,
                    .uvc_stream_index = selection.stream_index,
                },
                .vs_format = selection.format,
                .advanced = {
                    .number_of_frame_buffers = SI_CFG_VIDEO_FRAME_BUFFERS,
                    .frame_size = s_uvc_frame_capacity,
                    .frame_heap_caps = uvc_frame_heap_caps(),
                    .number_of_urbs = SI_CFG_VIDEO_URBS,
                    .urb_size = SI_CFG_VIDEO_URB_SIZE,
                    .user_frame_buffers = s_uvc_frame_buffers,
                },
            };
            ESP_LOGI(TAG,
                     "open retained UVC %ux%u logical=%.1f transport=%.1f buffers=%u frame=%u urbs=%u/%u",
                     selection.format.h_res, selection.format.v_res,
                     selection.logical_fps_x100 / 100.0f,
                     selection.format.fps,
                     (unsigned)SI_CFG_VIDEO_FRAME_BUFFERS,
                     (unsigned)s_uvc_frame_capacity,
                     (unsigned)SI_CFG_VIDEO_URBS,
                     (unsigned)SI_CFG_VIDEO_URB_SIZE);
            ret = uvc_host_stream_open(
                &config, pdMS_TO_TICKS(SI_UVC_OPEN_TIMEOUT_MS), &opened);
            if (ret != ESP_OK) {
                if (s_lock &&
                    xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                    set_error_locked("open UVC stream", ret);
                    xSemaphoreGive(s_lock);
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            __atomic_store_n(&s_uvc_stream, opened, __ATOMIC_RELEASE);
            s_uvc_session_state = SI_UVC_SESSION_OPEN_STOPPED;
            s_uvc_open_selection = selection;
            s_uvc_open_quality = mjpeg_quality;

            uvc_host_buf_info_t buf_info = {0};
            if (uvc_host_buf_info_get(opened, &buf_info) == ESP_OK &&
                buf_info.dwMaxVideoFrameSize > s_uvc_frame_capacity) {
                ESP_LOGE(TAG, "negotiated UVC frame %u exceeds arena %u",
                         (unsigned)buf_info.dwMaxVideoFrameSize,
                         (unsigned)s_uvc_frame_capacity);
                if (s_lock &&
                    xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                    set_error_locked("UVC frame exceeds arena",
                                     ESP_ERR_INVALID_SIZE);
                    xSemaphoreGive(s_lock);
                }
                if (!close_uvc_stream(false)) {
                    s_uvc_close_pending = true;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        } else if (!uvc_transport_matches(&selection,
                                          &s_uvc_open_selection)) {
            if (!stop_uvc_stream()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            uvc_host_stream_format_t requested = selection.format;
            ret = uvc_host_stream_format_select(opened, &requested);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "select retained UVC format: %s",
                         esp_err_to_name(ret));
                if (!close_uvc_stream(false)) {
                    s_uvc_close_pending = true;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            selection.format = requested;
            s_uvc_open_selection = selection;
        } else {
            /* Logical owner may change without renegotiating USB. */
            s_uvc_open_selection.logical_fps_x100 =
                selection.logical_fps_x100;
        }

        if (!capture_requested) {
            /* Enumerate and open once as soon as the camera appears so the URB
             * ring owns its scarce DMA memory before login/HTTP traffic.  Idle
             * means stopped, not closed: later viewers only start the already
             * allocated session. */
            if (s_uvc_session_state != SI_UVC_SESSION_OPEN_STOPPED &&
                !stop_uvc_stream()) {
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            if (s_lock &&
                xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                reset_frame_state_locked("video capture idle");
                xSemaphoreGive(s_lock);
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }

        if (s_uvc_session_state == SI_UVC_SESSION_DRAINING ||
            s_uvc_session_state == SI_UVC_SESSION_FAULTED) {
            if (!stop_uvc_stream()) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }
        if (s_uvc_session_state == SI_UVC_SESSION_OPEN_STOPPED) {
            ret = uvc_host_stream_start(opened);
        } else if (s_uvc_session_state != SI_UVC_SESSION_STARTED) {
            ret = ESP_ERR_INVALID_STATE;
        }
        if (ret != ESP_OK) {
            if (s_lock &&
                xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                set_error_locked("start UVC stream", ret);
                xSemaphoreGive(s_lock);
            }
            if (!close_uvc_stream(false)) {
                s_uvc_close_pending = true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        s_uvc_session_state = SI_UVC_SESSION_STARTED;
        __atomic_store_n(&s_uvc_accept_frames, true, __ATOMIC_RELEASE);
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_status.streaming = true;
            s_status.last_error[0] = '\0';
            xSemaphoreGive(s_lock);
        }
        __atomic_store_n(&s_uvc_transport_fault, false, __ATOMIC_RELEASE);
        uint32_t last_callbacks =
            __atomic_load_n(&s_uvc_callbacks, __ATOMIC_RELAXED);
        TickType_t no_frame_since = xTaskGetTickCount();
        TickType_t healthy_since = no_frame_since;
        uint32_t healthy_callbacks = 0U;
        bool no_frame_timeout = false;
        bool transport_fault = false;
        bool idle_requested = false;

        while (true) {
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) > 0U) {
                if (__atomic_exchange_n(&s_uvc_transport_fault, false,
                                        __ATOMIC_ACQ_REL)) {
                    transport_fault = true;
                    break;
                }
                si_uvc_selection_t requested = {0};
                bool requested_valid = false;
                bool requested_capture = false;
                uint32_t requested_quality = 0U;
                esp_err_t requested_ret = get_uvc_selection(
                    &requested, &requested_valid, &requested_capture,
                    &requested_quality);
                if (requested_ret != ESP_OK) {
                    /* Do not tear down a healthy stream merely because the
                     * short state snapshot lock was busy, but also do not
                     * consume and lose the only stop/mode-change wakeup. */
                    xTaskNotifyGive(s_uvc_supervisor_task);
                    continue;
                }
                if (requested_valid && requested_capture &&
                    requested_quality == s_uvc_open_quality &&
                    uvc_selection_matches(&selection, &requested)) {
                    continue;
                }
                idle_requested = !requested_capture;
                break;
            }
            uint32_t callbacks =
                __atomic_load_n(&s_uvc_callbacks, __ATOMIC_RELAXED);
            if (callbacks != last_callbacks) {
                healthy_callbacks += callbacks - last_callbacks;
                last_callbacks = callbacks;
                no_frame_since = xTaskGetTickCount();
                if (healthy_callbacks >= SI_UVC_HEALTHY_RESET_CALLBACKS &&
                    (int32_t)(no_frame_since - healthy_since) >=
                        (int32_t)pdMS_TO_TICKS(SI_UVC_HEALTHY_RESET_MS)) {
                    s_no_frame_reopen_count = 0U;
                }
                continue;
            }
            if ((int32_t)(xTaskGetTickCount() - no_frame_since) >=
                (int32_t)pdMS_TO_TICKS(SI_UVC_NO_FRAME_TIMEOUT_MS)) {
                no_frame_timeout = true;
                break;
            }
        }

        bool stopped = true;
        bool closed = false;
        if (transport_fault) {
            closed = close_uvc_stream(true);
            stopped = closed;
            s_uvc_close_pending = !closed;
        } else {
            stopped = stop_uvc_stream();
        }
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            reset_frame_state_locked(no_frame_timeout ?
                                     "UVC stream produced no frames" :
                                     transport_fault ?
                                     "UVC transport restarting" :
                                     idle_requested && stopped ?
                                     "video capture idle" :
                                     "UVC stream restarting");
            xSemaphoreGive(s_lock);
        }
        if (no_frame_timeout && stopped &&
            ++s_no_frame_reopen_count >= 2U) {
            s_no_frame_reopen_count = 0U;
            if (!closed) {
                closed = close_uvc_stream(false);
                s_uvc_close_pending = !closed;
            }
            if (closed) {
                (void)power_cycle_uvc_root_port();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(stopped ? 500 : 1500));
    }
}

static esp_err_t init_uvc(void)
{
    /* The immutable store has one maximum-frame arena shared by two variable
     * metadata slots.  It therefore accepts every frame the negotiated UVC
     * buffers accept without allocating a second 4.15 MiB copy. */
    ESP_RETURN_ON_ERROR(
        si_video_frame_store_init(s_uvc_frame_capacity),
                        TAG, "create immutable video frame store");
    s_uvc_frame_queue = xQueueCreate(SI_CFG_VIDEO_FRAME_BUFFERS,
                                     sizeof(si_uvc_frame_item_t));
    ESP_RETURN_ON_FALSE(s_uvc_frame_queue, ESP_ERR_NO_MEM, TAG,
                        "create UVC ingest queue");
    s_uvc_return_queue = xQueueCreate(SI_CFG_VIDEO_FRAME_BUFFERS + 1U,
                                      sizeof(si_uvc_return_item_t));
    ESP_RETURN_ON_FALSE(s_uvc_return_queue, ESP_ERR_NO_MEM, TAG,
                        "create UVC frame-return recovery queue");
#if SI_CFG_VIDEO_H264_ENABLED
    s_h264_frame_queue = xQueueCreate(1U, sizeof(si_uvc_frame_item_t));
    ESP_RETURN_ON_FALSE(s_h264_frame_queue, ESP_ERR_NO_MEM, TAG,
                        "create H.264 UVC lease queue");
    s_h264_frame_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_h264_frame_lock, ESP_ERR_NO_MEM, TAG,
                        "create H.264 UVC lease mutex");
#endif

    BaseType_t ok = xTaskCreatePinnedToCore(
        uvc_return_task, "si_uvc_return", 3072, NULL,
        SI_UVC_RETURN_PRIORITY, &s_uvc_return_task, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create UVC frame-return recovery task");
    ok = xTaskCreatePinnedToCore(
        uvc_ingest_task, "si_uvc_ingest", 6144, NULL,
        SI_UVC_INGEST_PRIORITY, &s_uvc_ingest_task, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create UVC ingest task");

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_config), TAG,
                        "install USB host");
    ok = xTaskCreatePinnedToCore(usb_host_task, "si_usb_host", 4096,
                                 NULL, SI_USB_HOST_PRIORITY,
                                 &s_usb_host_task, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create USB host task");

    const uvc_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = SI_UVC_DRIVER_PRIORITY,
        .xCoreID = 0,
        .create_background_task = true,
        .event_cb = uvc_driver_event_cb,
    };
    ESP_RETURN_ON_ERROR(uvc_host_install(&driver_config), TAG,
                        "install UVC host driver");
    ok = xTaskCreatePinnedToCore(
        uvc_supervisor_task, "si_uvc_supervisor", 6144, NULL,
        SI_UVC_SUPERVISOR_PRIORITY, &s_uvc_supervisor_task, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create UVC supervisor task");
    return ESP_OK;
}

esp_err_t si_video_reserve_boot_memory(void)
{
    if (!SI_CFG_VIDEO_ENABLED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (__atomic_load_n(&s_boot_memory_reserved, __ATOMIC_ACQUIRE)) {
        return ESP_OK;
    }

    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)SI_UVC_BOOT_MEMORY_WAIT_MS * 1000LL;
    for (;;) {
        bool expected_claim = false;
        if (__atomic_compare_exchange_n(
                &s_boot_memory_reserve_claimed, &expected_claim, true,
                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            esp_err_t err = reserve_uvc_frame_arena();
            if (err == ESP_OK) {
                err = reserve_uvc_dma_budget();
            }
            __atomic_store_n(&s_boot_memory_reserved, err == ESP_OK,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&s_boot_memory_reserve_claimed, false,
                             __ATOMIC_RELEASE);
            return err;
        }
        if (__atomic_load_n(&s_boot_memory_reserved, __ATOMIC_ACQUIRE)) {
            return ESP_OK;
        }
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

bool si_video_boot_memory_ready(void)
{
    return __atomic_load_n(&s_boot_memory_reserved, __ATOMIC_ACQUIRE);
}

esp_err_t si_video_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.enabled = SI_CFG_VIDEO_ENABLED;
    s_status.width = SI_CFG_VIDEO_WIDTH;
    s_status.height = SI_CFG_VIDEO_HEIGHT;
    s_status.target_width = SI_CFG_VIDEO_WIDTH;
    s_status.target_height = SI_CFG_VIDEO_HEIGHT;
    s_status.target_fps_x100 = SI_CFG_VIDEO_FPS * 100U;
    s_status.jpeg_quality = SI_CFG_VIDEO_JPEG_QUALITY;
    __atomic_store_n(&s_uvc_requested_mjpeg_quality,
                     SI_CFG_VIDEO_JPEG_QUALITY, __ATOMIC_RELEASE);
    strlcpy(s_status.source, "usb-uvc", sizeof(s_status.source));
    strlcpy(s_status.pixel_format, "MJPEG",
            sizeof(s_status.pixel_format));
    strlcpy(s_status.capture_owner, "off",
            sizeof(s_status.capture_owner));
    strlcpy(s_capture_owner, "off", sizeof(s_capture_owner));
    s_target_width = SI_CFG_VIDEO_WIDTH;
    s_target_height = SI_CFG_VIDEO_HEIGHT;
    s_target_fps_x100 = SI_CFG_VIDEO_FPS * 100U;
    s_target_exact_fps = true;

    if (!SI_CFG_VIDEO_ENABLED) {
        strlcpy(s_status.last_error, "video disabled by config",
                sizeof(s_status.last_error));
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG,
                        "create video state mutex");
    s_transport_gate = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_transport_gate, ESP_ERR_NO_MEM, TAG,
                        "create output transport gate");
    xSemaphoreGive(s_transport_gate);

    ESP_RETURN_ON_ERROR(si_video_reserve_boot_memory(), TAG,
                        "reserve UVC boot memory");
    esp_err_t ret = init_uvc();
    if (ret != ESP_OK) {
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            set_error_locked("init USB UVC", ret);
            xSemaphoreGive(s_lock);
        }
        return ret;
    }
    s_status.initialized = true;
    return ESP_OK;
}

void si_video_get_status(si_video_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        *status = s_status;
        status->frames_dropped +=
            __atomic_load_n(&s_uvc_queue_drops, __ATOMIC_RELAXED);
        status->uvc_callbacks =
            __atomic_load_n(&s_uvc_callbacks, __ATOMIC_RELAXED);
        status->uvc_queue_drops =
            __atomic_load_n(&s_uvc_queue_drops, __ATOMIC_RELAXED);
        status->h264_pressure_coalesces =
            __atomic_load_n(&s_h264_pressure_coalesces,
                            __ATOMIC_RELAXED);
        status->uvc_buffer_underflows =
            __atomic_load_n(&s_uvc_buffer_underflows, __ATOMIC_RELAXED);
        status->uvc_buffer_overflows =
            __atomic_load_n(&s_uvc_buffer_overflows, __ATOMIC_RELAXED);
        status->uvc_return_pending =
            __atomic_load_n(&s_uvc_return_pending, __ATOMIC_RELAXED);
        status->uvc_return_retries =
            __atomic_load_n(&s_uvc_return_retries, __ATOMIC_RELAXED);
        status->uvc_return_failures =
            __atomic_load_n(&s_uvc_return_failures, __ATOMIC_RELAXED);
        status->ingest_validate_last_us =
            __atomic_load_n(&s_ingest_validate_last_us, __ATOMIC_RELAXED);
        status->ingest_validate_max_us =
            __atomic_load_n(&s_ingest_validate_max_us, __ATOMIC_RELAXED);
        status->ingest_publish_last_us =
            __atomic_load_n(&s_ingest_publish_last_us, __ATOMIC_RELAXED);
        status->ingest_publish_max_us =
            __atomic_load_n(&s_ingest_publish_max_us, __ATOMIC_RELAXED);
        status->ingest_total_last_us =
            __atomic_load_n(&s_ingest_total_last_us, __ATOMIC_RELAXED);
        status->ingest_total_max_us =
            __atomic_load_n(&s_ingest_total_max_us, __ATOMIC_RELAXED);
        if (status->frame_ready && s_last_accepted_frame_ms != 0U) {
            status->last_frame_ms = now_ms() - s_last_accepted_frame_ms;
        }
        xSemaphoreGive(s_lock);
    }
}

esp_err_t si_video_get_modes(si_video_mode_t *modes, size_t max_modes,
                             size_t *out_count)
{
    if (!out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0U;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const size_t copy_count = s_mode_count < max_modes ?
                              s_mode_count : max_modes;
    for (size_t i = 0; modes && i < copy_count; i++) {
        modes[i] = s_modes[i].public_mode;
        modes[i].selected = s_uvc_selection.valid &&
            s_uvc_selection.format.h_res == modes[i].width &&
            s_uvc_selection.format.v_res == modes[i].height &&
            diff_u32(s_uvc_selection.logical_fps_x100,
                     modes[i].fps_x100) <= 50U;
    }
    *out_count = s_mode_count;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_video_set_capture(bool enabled, const char *owner,
                               uint32_t width, uint32_t height,
                               uint32_t fps_x100, bool exact_fps,
                               uint32_t stride_ms)
{
    if (enabled && (width < 160U || height < 120U ||
                    width > 3840U || height > 2160U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const bool was_enabled = s_capture_enabled;
    const si_uvc_selection_t previous = s_uvc_selection;

    if (!enabled) {
        s_capture_enabled = false;
        s_capture_stride_ms = 0U;
        strlcpy(s_capture_owner, "off", sizeof(s_capture_owner));
        s_status.capture_enabled = false;
        s_status.capture_stride_ms = 0U;
        strlcpy(s_status.capture_owner, "off",
                sizeof(s_status.capture_owner));
        s_uvc_selection.valid = false;
        reset_frame_state_locked("video capture idle");
        xSemaphoreGive(s_lock);
        if (was_enabled && s_uvc_supervisor_task) {
            xTaskNotifyGive(s_uvc_supervisor_task);
        }
        return ESP_OK;
    }

    const uint32_t requested_fps_x100 = fps_x100 > 0U ? fps_x100 :
                                         SI_CFG_VIDEO_FPS * 100U;

    si_uvc_selection_t selected = {0};
    if (!select_stored_mode_locked(width, height, true,
                                   requested_fps_x100, exact_fps,
                                   &selected)) {
        const bool unavailable = s_mode_count > 0U;
        if (unavailable) {
            /* Exact-mode changes are transactional.  A rejected request must
             * leave the active selection, capture owner, target and readable
             * frame untouched so a bad UI/API value cannot black out a
             * working Stable stream. */
            xSemaphoreGive(s_lock);
            return ESP_ERR_NOT_FOUND;
        }

        /* No device has enumerated yet.  Preserve the requested startup mode
         * so enumeration can apply it later; there is no working selection to
         * roll back in this state. */
        s_capture_enabled = true;
        s_capture_stride_ms = stride_ms;
        strlcpy(s_capture_owner, owner ? owner : "off",
                sizeof(s_capture_owner));
        s_status.capture_enabled = true;
        s_status.capture_stride_ms = stride_ms;
        strlcpy(s_status.capture_owner, s_capture_owner,
                sizeof(s_status.capture_owner));
        s_target_width = width;
        s_target_height = height;
        s_target_fps_x100 = requested_fps_x100;
        s_target_exact_fps = exact_fps;
        s_status.target_width = width;
        s_status.target_height = height;
        s_status.target_fps_x100 = requested_fps_x100;
        s_uvc_selection.valid = false;
        reset_frame_state_locked("waiting for USB UVC device");
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    /* Commit the complete logical request only after exact physical mode
     * selection succeeds. */
    s_capture_enabled = true;
    s_capture_stride_ms = stride_ms;
    strlcpy(s_capture_owner, owner ? owner : "off",
            sizeof(s_capture_owner));
    s_status.capture_enabled = true;
    s_status.capture_stride_ms = stride_ms;
    strlcpy(s_status.capture_owner, s_capture_owner,
            sizeof(s_status.capture_owner));
    s_target_width = width;
    s_target_height = height;
    s_target_fps_x100 = requested_fps_x100;
    s_target_exact_fps = exact_fps;
    s_status.target_width = width;
    s_status.target_height = height;
    s_uvc_selection = selected;
    s_status.width = selected.format.h_res;
    s_status.height = selected.format.v_res;
    s_status.target_fps_x100 = selected.logical_fps_x100;
    const bool changed = !was_enabled ||
        !uvc_selection_matches(&previous, &selected) ||
        diff_u32(previous.logical_fps_x100,
                 selected.logical_fps_x100) > 50U;
    if (changed) {
        reset_frame_state_locked("switching video capture");
    }
    xSemaphoreGive(s_lock);
    if (changed && s_uvc_supervisor_task) {
        xTaskNotifyGive(s_uvc_supervisor_task);
    }
    return ESP_OK;
}

esp_err_t si_video_validate_mode(uint32_t width, uint32_t height,
                                 uint32_t fps_x100, bool exact_fps)
{
    if (width < 160U || height < 120U ||
        width > 3840U || height > 2160U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    si_uvc_selection_t selected = {0};
    const bool available = select_stored_mode_locked(
        width, height, true,
        fps_x100 > 0U ? fps_x100 : SI_CFG_VIDEO_FPS * 100U,
        exact_fps, &selected);
    xSemaphoreGive(s_lock);
    return available ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t si_video_set_mode(uint32_t width, uint32_t height,
                            uint32_t fps_x100)
{
    return si_video_set_capture(true, "kvm", width, height, fps_x100,
                                fps_x100 > 0U, 0U);
}

esp_err_t si_video_set_resolution(uint32_t width, uint32_t height)
{
    return si_video_set_mode(width, height, 0U);
}

esp_err_t si_video_set_quality(uint32_t quality)
{
    if (quality < 1U || quality > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const bool changed = s_status.jpeg_quality != quality;
    s_status.jpeg_quality = quality;
    __atomic_store_n(&s_uvc_requested_mjpeg_quality, quality,
                     __ATOMIC_RELEASE);
    xSemaphoreGive(s_lock);
    if (changed && s_uvc_supervisor_task) {
        xTaskNotifyGive(s_uvc_supervisor_task);
    }
    return ESP_OK;
}

esp_err_t si_video_request_jpeg_snapshot(void)
{
    /* Never let /api/snapshot return a frame from the previous output mode.
     * In MJPEG mode the normal next publication satisfies the request. In
     * H.264 mode the generation asks ingest to make exactly one validated
     * MJPEG copy before handing that same retained frame to the decoder. */
    si_video_frame_store_clear();
    if (__atomic_load_n(&s_h264_transport_active, __ATOMIC_ACQUIRE)) {
        uint32_t generation = __atomic_add_fetch(
            &s_mjpeg_snapshot_request_generation, 1U,
            __ATOMIC_ACQ_REL);
        if (generation == 0U) {
            __atomic_store_n(&s_mjpeg_snapshot_request_generation, 1U,
                             __ATOMIC_RELEASE);
        }
    }
    return ESP_OK;
}

/* Strong application hook consumed by the guarded usb_host_uvc negotiation
 * override. Keeping the preference outside the registry component preserves
 * the upstream ABI while allowing every UVC reopen to commit current policy. */
uint8_t si_uvc_mjpeg_quality_percent(void)
{
    return (uint8_t)__atomic_load_n(&s_uvc_requested_mjpeg_quality,
                                     __ATOMIC_ACQUIRE);
}

esp_err_t si_video_borrow_jpeg_if_new(uint32_t last_frame_id,
                                      si_video_jpeg_view_t *view)
{
    if (!view) {
        return ESP_ERR_INVALID_ARG;
    }
    si_video_frame_store_view_t stored = {0};
    esp_err_t ret = si_video_frame_store_acquire(last_frame_id, &stored);
    if (ret != ESP_OK) {
        memset(view, 0, sizeof(*view));
        return ret;
    }
    *view = (si_video_jpeg_view_t) {
        .data = stored.data,
        .len = stored.len,
        .frame_id = stored.frame_id,
        .token = stored.token,
    };
    return ESP_OK;
}

void si_video_release_jpeg(si_video_jpeg_view_t *view)
{
    if (!view) {
        return;
    }
    si_video_frame_store_view_t stored = {
        .data = view->data,
        .len = view->len,
        .frame_id = view->frame_id,
        .token = view->token,
    };
    si_video_frame_store_release(&stored);
    memset(view, 0, sizeof(*view));
}

esp_err_t si_video_wait_h264_jpeg_ready(uint32_t timeout_ms)
{
    if (!s_h264_frame_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!__atomic_load_n(&s_h264_transport_active, __ATOMIC_ACQUIRE)) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0U && wait_ticks == 0U) {
        wait_ticks = 1U;
    }
    si_uvc_frame_item_t observed = {0};
    if (xQueuePeek(s_h264_frame_queue, &observed, wait_ticks) != pdPASS) {
        return __atomic_load_n(&s_h264_transport_active, __ATOMIC_ACQUIRE) ?
                   ESP_ERR_TIMEOUT :
                   ESP_ERR_INVALID_STATE;
    }

    /* Do not inspect h264_epoch here: it is protected by
     * s_h264_frame_lock, and waiting while holding that mutex would block the
     * producer and session flush.  xQueuePeek is deliberately just an event
     * wait.  The following borrow owns dequeue plus all generation checks. */
    return __atomic_load_n(&s_h264_transport_active, __ATOMIC_ACQUIRE) ?
               ESP_OK :
               ESP_ERR_INVALID_STATE;
}

esp_err_t si_video_borrow_h264_jpeg_if_new(uint32_t last_frame_id,
                                           si_video_jpeg_view_t *view)
{
    if (!view) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(view, 0, sizeof(*view));
    if (!s_h264_frame_lock || !s_h264_frame_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_h264_frame_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!__atomic_load_n(&s_h264_transport_active, __ATOMIC_ACQUIRE)) {
        xSemaphoreGive(s_h264_frame_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_h264_borrowed.active) {
        xSemaphoreGive(s_h264_frame_lock);
        return ESP_ERR_NOT_FINISHED;
    }

    si_uvc_frame_item_t item = {0};
    if (xQueueReceive(s_h264_frame_queue, &item, 0) != pdPASS) {
        xSemaphoreGive(s_h264_frame_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    const uint32_t active_uvc_epoch =
        __atomic_load_n(&s_uvc_epoch, __ATOMIC_ACQUIRE);
    const uvc_host_stream_hdl_t active_stream =
        __atomic_load_n(&s_uvc_stream, __ATOMIC_ACQUIRE);
    const bool current = item.frame && item.frame->data &&
                         item.frame->data_len > 0U &&
                         item.h264_epoch == s_h264_frame_epoch &&
                         item.epoch == active_uvc_epoch &&
                         item.stream == active_stream;
    if (!current ||
        (last_frame_id != UINT32_MAX && item.frame_id == last_frame_id)) {
        xSemaphoreGive(s_h264_frame_lock);
        return_retained_uvc_frame(&item);
        return current ? ESP_ERR_NOT_FINISHED : ESP_ERR_NOT_FOUND;
    }

    s_h264_borrowed.item = item;
    s_h264_borrowed.active = true;
    view->data = item.frame->data;
    view->len = item.frame->data_len;
    view->frame_id = item.frame_id;
    view->token = &s_h264_borrowed;
    xSemaphoreGive(s_h264_frame_lock);
    return ESP_OK;
}

esp_err_t si_video_release_h264_jpeg(si_video_jpeg_view_t *view)
{
    if (!view || !view->token) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Returning a retained UVC frame is an ownership obligation, not a
     * best-effort operation.  A short mutex timeout here would permanently
     * strand the only direct lease and make stream close wait forever. */
    if (!s_h264_frame_lock ||
        xSemaphoreTake(s_h264_frame_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    const si_uvc_frame_item_t *borrowed = &s_h264_borrowed.item;
    const bool matches = s_h264_borrowed.active &&
                         view->token == &s_h264_borrowed &&
                         view->frame_id == borrowed->frame_id &&
                         view->data == borrowed->frame->data &&
                         view->len == borrowed->frame->data_len;
    if (!matches) {
        xSemaphoreGive(s_h264_frame_lock);
        return ESP_ERR_INVALID_STATE;
    }

    si_uvc_frame_item_t item = *borrowed;
    memset(&s_h264_borrowed, 0, sizeof(s_h264_borrowed));
    xSemaphoreGive(s_h264_frame_lock);
    return_retained_uvc_frame(&item);
    memset(view, 0, sizeof(*view));
    return ESP_OK;
}

esp_err_t si_video_flush_h264_jpeg(void)
{
    if (!s_h264_frame_lock || !s_h264_frame_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    invalidate_h264_frame_queue();
    return ESP_OK;
}

esp_err_t si_video_copy_jpeg(uint8_t *buf, size_t buf_cap,
                             size_t *out_len, uint32_t *out_frame_id)
{
    return si_video_frame_store_copy(UINT32_MAX, false, buf, buf_cap,
                                     out_len, out_frame_id);
}

esp_err_t si_video_copy_jpeg_if_new(uint32_t last_frame_id, uint8_t *buf,
                                    size_t buf_cap, size_t *out_len,
                                    uint32_t *out_frame_id)
{
    return si_video_frame_store_copy(last_frame_id, true, buf, buf_cap,
                                     out_len, out_frame_id);
}

esp_err_t si_video_acquire_jpeg(uint8_t **out_buf, size_t *out_len,
                                uint32_t *out_frame_id)
{
    if (!out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0U;
    si_video_jpeg_view_t view = {0};
    esp_err_t ret = si_video_borrow_jpeg_if_new(UINT32_MAX, &view);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t *copy = heap_caps_malloc(view.len,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        copy = malloc(view.len);
    }
    if (!copy) {
        si_video_release_jpeg(&view);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, view.data, view.len);
    *out_buf = copy;
    *out_len = view.len;
    if (out_frame_id) {
        *out_frame_id = view.frame_id;
    }
    si_video_release_jpeg(&view);
    return ESP_OK;
}

esp_err_t si_video_begin_mjpeg_send(uint32_t timeout_ms)
{
    if (!s_transport_gate) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_transport_gate,
                       pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (__atomic_load_n(&s_h264_transport_active, __ATOMIC_ACQUIRE)) {
        xSemaphoreGive(s_transport_gate);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void si_video_end_mjpeg_send(void)
{
    if (s_transport_gate) {
        xSemaphoreGive(s_transport_gate);
    }
}

esp_err_t si_video_set_h264_transport_active(bool active)
{
    const bool current = __atomic_load_n(&s_h264_transport_active,
                                         __ATOMIC_ACQUIRE);
    if (active && current) {
        return try_enable_uvc_retention_cap() ? ESP_OK :
                                                ESP_ERR_INVALID_STATE;
    }
    if (!s_h264_frame_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    si_uvc_frame_item_t queued = {0};
    bool have_queued = false;
    if (active) {
        if (!s_transport_gate ||
            xSemaphoreTake(s_transport_gate,
                           pdMS_TO_TICKS(1500)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        if (xSemaphoreTake(s_h264_frame_lock,
                           pdMS_TO_TICKS(1500)) != pdTRUE) {
            xSemaphoreGive(s_transport_gate);
            return ESP_ERR_TIMEOUT;
        }
        const TickType_t cap_deadline =
            xTaskGetTickCount() + pdMS_TO_TICKS(1500);
        while (!try_enable_uvc_retention_cap()) {
            if ((int32_t)(xTaskGetTickCount() - cap_deadline) >= 0) {
                xSemaphoreGive(s_h264_frame_lock);
                xSemaphoreGive(s_transport_gate);
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        advance_h264_frame_epoch_locked();
        have_queued = take_queued_h264_frame_locked(&queued);
        si_video_frame_store_clear();
        __atomic_store_n(&s_h264_transport_active, true,
                         __ATOMIC_RELEASE);
        xSemaphoreGive(s_h264_frame_lock);
    } else {
        if (xSemaphoreTake(s_h264_frame_lock,
                           pdMS_TO_TICKS(1500)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        const bool was_active = __atomic_load_n(
            &s_h264_transport_active, __ATOMIC_ACQUIRE);
        __atomic_store_n(&s_h264_transport_active, false,
                         __ATOMIC_RELEASE);
        disable_uvc_retention_cap();
        advance_h264_frame_epoch_locked();
        have_queued = take_queued_h264_frame_locked(&queued);
        si_video_frame_store_clear();
        xSemaphoreGive(s_h264_frame_lock);
        if (was_active && s_transport_gate) {
            xSemaphoreGive(s_transport_gate);
        }
    }
    if (have_queued) {
        return_retained_uvc_frame(&queued);
    }
    return ESP_OK;
}

size_t si_video_jpeg_capacity(void)
{
    return si_video_frame_store_capacity();
}
