#include "video_input.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "driver/jpeg_encode.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

static const char *TAG = "si-video";

#ifdef CONFIG_SI_VIDEO_ENABLE
#define SI_VIDEO_CFG_ENABLE 1
#else
#define SI_VIDEO_CFG_ENABLE 0
#endif

#ifndef CONFIG_SI_VIDEO_WIDTH
#define CONFIG_SI_VIDEO_WIDTH 640
#endif
#ifndef CONFIG_SI_VIDEO_HEIGHT
#define CONFIG_SI_VIDEO_HEIGHT 480
#endif
#ifndef CONFIG_SI_VIDEO_JPEG_QUALITY
#define CONFIG_SI_VIDEO_JPEG_QUALITY 75
#endif
#ifndef CONFIG_SI_VIDEO_UVC_FPS
#define CONFIG_SI_VIDEO_UVC_FPS 30
#endif
#ifndef CONFIG_SI_VIDEO_UVC_FRAME_BUFFERS
#define CONFIG_SI_VIDEO_UVC_FRAME_BUFFERS 3
#endif
#ifndef CONFIG_SI_VIDEO_UVC_URBS
#define CONFIG_SI_VIDEO_UVC_URBS 4
#endif
#ifndef CONFIG_SI_VIDEO_UVC_URB_SIZE
#define CONFIG_SI_VIDEO_UVC_URB_SIZE 16384
#endif
#ifndef CONFIG_SI_VIDEO_UVC_JPEG_BUFFER_SIZE
#define CONFIG_SI_VIDEO_UVC_JPEG_BUFFER_SIZE (512 * 1024)
#endif

#define SI_UVC_TASK_PRIORITY 13
#define SI_YUY2_ENCODE_TASK_PRIORITY (SI_UVC_TASK_PRIORITY - 2)
#define SI_YUY2_ENCODE_TASK_STACK 8192
#define SI_YUY2_BUFFER_COUNT 2
#define SI_UVC_OPEN_TIMEOUT_MS 5000
#define SI_UVC_NO_FRAME_TIMEOUT_MS 5000
#define SI_UVC_MJPEG_ONLY 1
#define SI_UVC_PREFER_YUY2 0
#define SI_YUY2_CONVERT_TO_RGB565_FOR_JPEG 1
#define SI_YUY2_SWAP_UV_FOR_JPEG 1

static si_video_status_t s_status;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_yuy2_lock;
static TaskHandle_t s_usb_host_task;
static TaskHandle_t s_uvc_stream_task;
static TaskHandle_t s_yuy2_encode_task;
static uvc_host_stream_hdl_t s_uvc_stream;
static jpeg_encoder_handle_t s_jpeg_encoder;
static uint8_t *s_latest_jpeg;
static uint8_t *s_encode_jpeg;
static uint8_t *s_rgb565_encode;
static size_t s_latest_jpeg_cap;
static size_t s_encode_jpeg_cap;
static size_t s_rgb565_encode_cap;
static uint32_t s_no_frame_reopen_count;
static uint8_t *s_yuy2_buffers[SI_YUY2_BUFFER_COUNT];
static size_t s_yuy2_buffer_cap;
static int s_yuy2_write_idx;
static int s_yuy2_ready_idx = -1;
static int s_yuy2_encoding_idx = -1;
static size_t s_yuy2_ready_len;
static uint32_t s_yuy2_ready_width;
static uint32_t s_yuy2_ready_height;

typedef struct {
    bool valid;
    uint8_t dev_addr;
    uint8_t stream_index;
    uvc_host_stream_format_t format;
} si_uvc_selection_t;

typedef struct {
    si_video_mode_t public_mode;
    uint8_t dev_addr;
    uint8_t stream_index;
    uvc_host_stream_format_t format;
} si_uvc_mode_t;

static si_uvc_selection_t s_uvc_selection;
static si_uvc_mode_t s_modes[SI_VIDEO_MAX_MODES];
static size_t s_mode_count;
static uint32_t s_target_width;
static uint32_t s_target_height;
static uint32_t s_target_fps_x100;
static bool s_target_exact_fps;
static bool s_capture_enabled;
static uint32_t s_capture_stride_ms;
static uint32_t s_last_accepted_frame_ms;
static uint32_t s_fps_last_ms;
static uint32_t s_fps_last_frames;
static char s_capture_owner[16];

static void update_fps(uint32_t now_ms)
{
    if (s_fps_last_ms == 0) {
        s_fps_last_ms = now_ms;
        s_fps_last_frames = s_status.frames_encoded;
        return;
    }

    uint32_t elapsed = now_ms - s_fps_last_ms;
    if (elapsed >= 1000) {
        uint32_t frames = s_status.frames_encoded - s_fps_last_frames;
        s_status.fps_x100 = (frames * 100000U) / elapsed;
        s_fps_last_ms = now_ms;
        s_fps_last_frames = s_status.frames_encoded;
    }
}

static void reset_frame_state_locked(const char *message)
{
    s_status.streaming = false;
    s_status.frame_ready = false;
    s_status.last_jpeg_size = 0;
    s_status.last_frame_ms = 0;
    s_status.fps_x100 = 0;
    s_last_accepted_frame_ms = 0;
    s_fps_last_ms = 0;
    s_fps_last_frames = s_status.frames_encoded;
    if (message) {
        strlcpy(s_status.last_error, message, sizeof(s_status.last_error));
    } else {
        s_status.last_error[0] = '\0';
    }
}

static void set_error(const char *message, esp_err_t err)
{
    if (!message) {
        message = "unknown";
    }
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s: %s", message, esp_err_to_name(err));
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

static float uvc_interval_to_fps(uint32_t interval)
{
    return interval == 0 ? 0.0f : 10000000.0f / (float)interval;
}

static uint32_t uvc_interval_to_fps_x100(uint32_t interval)
{
    return interval == 0 ? 0 : (uint32_t)((1000000000ULL + (interval / 2U)) / interval);
}

static uint32_t fps_float_to_x100(float fps)
{
    return fps <= 0.0f ? 0 : (uint32_t)(fps * 100.0f + 0.5f);
}

static uint32_t mode_area(const si_uvc_mode_t *mode)
{
    return mode->public_mode.width * mode->public_mode.height;
}

static uint32_t diff_u32(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

static bool mode_is_better(const si_uvc_mode_t *candidate, const si_uvc_mode_t *best,
                           uint32_t width, uint32_t height, uint32_t fps_x100)
{
    if (!best) {
        return true;
    }

#if SI_UVC_PREFER_YUY2
    if (candidate->format.format != best->format.format) {
        if (candidate->format.format == UVC_VS_FORMAT_YUY2) {
            return true;
        }
        if (best->format.format == UVC_VS_FORMAT_YUY2) {
            return false;
        }
    }
#endif

    const uint32_t target_area = width * height;
    const uint32_t candidate_area = mode_area(candidate);
    const uint32_t best_area = mode_area(best);

    uint32_t candidate_rank = candidate_area <= target_area ? 0 : 1;
    uint32_t best_rank = best_area <= target_area ? 0 : 1;
    if (candidate->public_mode.width == width && candidate->public_mode.height == height) {
        candidate_rank = 0;
    }
    if (best->public_mode.width == width && best->public_mode.height == height) {
        best_rank = 0;
    }
    if (candidate_rank != best_rank) {
        return candidate_rank < best_rank;
    }

    uint32_t candidate_area_diff = diff_u32(candidate_area, target_area);
    uint32_t best_area_diff = diff_u32(best_area, target_area);
    if (candidate_area_diff != best_area_diff) {
        return candidate_area_diff < best_area_diff;
    }

    if (fps_x100 > 0) {
        uint32_t candidate_fps_diff = diff_u32(candidate->public_mode.fps_x100, fps_x100);
        uint32_t best_fps_diff = diff_u32(best->public_mode.fps_x100, fps_x100);
        if (candidate_fps_diff != best_fps_diff) {
            return candidate_fps_diff < best_fps_diff;
        }
    }

    return candidate->public_mode.fps_x100 > best->public_mode.fps_x100;
}

static bool select_stored_mode_locked(uint32_t width, uint32_t height, bool exact_resolution,
                                      uint32_t fps_x100, bool exact_fps,
                                      si_uvc_selection_t *selection)
{
    if (!selection || s_mode_count == 0) {
        return false;
    }

    const si_uvc_mode_t *best = NULL;
    for (size_t i = 0; i < s_mode_count; i++) {
        const si_uvc_mode_t *mode = &s_modes[i];
        if (exact_resolution &&
            (mode->public_mode.width != width || mode->public_mode.height != height)) {
            continue;
        }
        if (exact_fps && fps_x100 > 0 && diff_u32(mode->public_mode.fps_x100, fps_x100) > 50) {
            continue;
        }
        if (mode_is_better(mode, best, width, height, fps_x100)) {
            best = mode;
        }
    }

    if (!best) {
        return false;
    }

    selection->valid = true;
    selection->dev_addr = best->dev_addr;
    selection->stream_index = best->stream_index;
    selection->format = best->format;
    return true;
}

static void add_uvc_mode_locked(const uvc_host_frame_info_t *info,
                                uint8_t dev_addr, uint8_t stream_index,
                                uint32_t fps_x100)
{
    if (!info || fps_x100 == 0 || s_mode_count >= SI_VIDEO_MAX_MODES) {
        return;
    }

    for (size_t i = 0; i < s_mode_count; i++) {
        si_uvc_mode_t *mode = &s_modes[i];
        if (mode->public_mode.width == info->h_res &&
            mode->public_mode.height == info->v_res &&
            mode->format.format == info->format &&
            diff_u32(mode->public_mode.fps_x100, fps_x100) <= 50) {
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
    mode->format.h_res = info->h_res;
    mode->format.v_res = info->v_res;
    mode->format.fps = fps_x100 / 100.0f;
    mode->format.format = info->format;
}

static void store_uvc_modes_locked(const uvc_host_frame_info_t *list, size_t list_size,
                                   uint8_t dev_addr, uint8_t stream_index)
{
    s_mode_count = 0;
    memset(s_modes, 0, sizeof(s_modes));

    for (size_t i = 0; i < list_size && s_mode_count < SI_VIDEO_MAX_MODES; i++) {
        const uvc_host_frame_info_t *info = &list[i];
        if (info->format != UVC_VS_FORMAT_MJPEG || info->h_res == 0 || info->v_res == 0) {
            continue;
        }

        if (info->interval_type == 0) {
            add_uvc_mode_locked(info, dev_addr, stream_index,
                                uvc_interval_to_fps_x100(info->default_interval));
            static const uint32_t common_fps_x100[] = {
                6000, 5000, 3000, 2500, 2400, 2000, 1500, 1000, 500, 100,
            };
            uint32_t min_fps = uvc_interval_to_fps_x100(info->interval_max);
            uint32_t max_fps = uvc_interval_to_fps_x100(info->interval_min);
            for (size_t j = 0; j < sizeof(common_fps_x100) / sizeof(common_fps_x100[0]); j++) {
                if (common_fps_x100[j] >= min_fps && common_fps_x100[j] <= max_fps) {
                    add_uvc_mode_locked(info, dev_addr, stream_index, common_fps_x100[j]);
                }
            }
        } else {
            uint8_t count = info->interval_type;
            if (count > CONFIG_UVC_INTERVAL_ARRAY_SIZE) {
                count = CONFIG_UVC_INTERVAL_ARRAY_SIZE;
            }
            for (uint8_t j = 0; j < count; j++) {
                add_uvc_mode_locked(info, dev_addr, stream_index,
                                    uvc_interval_to_fps_x100(info->interval[j]));
            }
            add_uvc_mode_locked(info, dev_addr, stream_index,
                                uvc_interval_to_fps_x100(info->default_interval));
        }
    }
    s_status.modes_count = (uint32_t)s_mode_count;
}

static uint32_t uvc_frame_heap_caps(void)
{
#ifdef CONFIG_SPIRAM
    return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    return MALLOC_CAP_8BIT;
#endif
}

static size_t yuy2_frame_len(uint32_t width, uint32_t height)
{
    return (size_t)width * height * 2U;
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static uint16_t yuv_to_rgb565(uint8_t y, uint8_t u, uint8_t v)
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;
    if (c < 0) {
        c = 0;
    }

    uint8_t r = clamp_u8((298 * c + 409 * e + 128) >> 8);
    uint8_t g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    uint8_t b = clamp_u8((298 * c + 516 * d + 128) >> 8);

    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      (b >> 3));
}

static void yuy2_to_rgb565(const uint8_t *src, uint8_t *dst, size_t len)
{
    for (size_t i = 0; i + 3 < len; i += 4) {
        uint8_t y0 = src[i];
        uint8_t u = src[i + 1];
        uint8_t y1 = src[i + 2];
        uint8_t v = src[i + 3];
        uint16_t rgb0 = yuv_to_rgb565(y0, u, v);
        uint16_t rgb1 = yuv_to_rgb565(y1, u, v);
        dst[i] = (uint8_t)(rgb0 & 0xFF);
        dst[i + 1] = (uint8_t)(rgb0 >> 8);
        dst[i + 2] = (uint8_t)(rgb1 & 0xFF);
        dst[i + 3] = (uint8_t)(rgb1 >> 8);
    }
}

static esp_err_t ensure_yuy2_buffers(size_t required)
{
    if (required == 0 || !s_yuy2_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_yuy2_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_yuy2_buffers[0] && s_yuy2_buffers[1] && s_yuy2_buffer_cap >= required) {
        s_yuy2_ready_idx = -1;
        s_yuy2_ready_len = 0;
        xSemaphoreGive(s_yuy2_lock);
        return ESP_OK;
    }

    if (s_yuy2_encoding_idx >= 0) {
        xSemaphoreGive(s_yuy2_lock);
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < SI_YUY2_BUFFER_COUNT; i++) {
        free(s_yuy2_buffers[i]);
        s_yuy2_buffers[i] = NULL;
    }
    free(s_rgb565_encode);
    s_rgb565_encode = NULL;
    s_rgb565_encode_cap = 0;
    s_yuy2_buffer_cap = 0;
    s_yuy2_ready_idx = -1;
    s_yuy2_ready_len = 0;
    s_yuy2_write_idx = 0;

    jpeg_encode_memory_alloc_cfg_t raw_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    for (size_t i = 0; i < SI_YUY2_BUFFER_COUNT; i++) {
        size_t allocated = 0;
        s_yuy2_buffers[i] = jpeg_alloc_encoder_mem(required, &raw_mem_cfg, &allocated);
        if (!s_yuy2_buffers[i] || allocated < required) {
            for (size_t j = 0; j < SI_YUY2_BUFFER_COUNT; j++) {
                free(s_yuy2_buffers[j]);
                s_yuy2_buffers[j] = NULL;
            }
            xSemaphoreGive(s_yuy2_lock);
            return ESP_ERR_NO_MEM;
        }
    }
#if SI_YUY2_CONVERT_TO_RGB565_FOR_JPEG
    size_t rgb_allocated = 0;
    s_rgb565_encode = jpeg_alloc_encoder_mem(required, &raw_mem_cfg, &rgb_allocated);
    if (!s_rgb565_encode || rgb_allocated < required) {
        for (size_t j = 0; j < SI_YUY2_BUFFER_COUNT; j++) {
            free(s_yuy2_buffers[j]);
            s_yuy2_buffers[j] = NULL;
        }
        free(s_rgb565_encode);
        s_rgb565_encode = NULL;
        xSemaphoreGive(s_yuy2_lock);
        return ESP_ERR_NO_MEM;
    }
    s_rgb565_encode_cap = rgb_allocated;
#endif
    s_yuy2_buffer_cap = required;
    xSemaphoreGive(s_yuy2_lock);
    ESP_LOGI(TAG, "allocated YUY2 encode buffers: %u bytes each", (unsigned)required);
    return ESP_OK;
}

static esp_err_t queue_yuy2_frame(const uvc_host_frame_t *frame)
{
    if (!frame || !frame->data || !s_yuy2_lock || !s_yuy2_encode_task) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t expected_len = yuy2_frame_len(frame->vs_format.h_res, frame->vs_format.v_res);
    if (frame->data_len < expected_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (xSemaphoreTake(s_yuy2_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_yuy2_buffers[0] || !s_yuy2_buffers[1] || s_yuy2_buffer_cap < expected_len) {
        xSemaphoreGive(s_yuy2_lock);
        return ESP_ERR_NO_MEM;
    }

    int idx = -1;
    for (int i = 0; i < SI_YUY2_BUFFER_COUNT; i++) {
        int candidate = (s_yuy2_write_idx + i) % SI_YUY2_BUFFER_COUNT;
        if (candidate != s_yuy2_encoding_idx) {
            idx = candidate;
            break;
        }
    }
    if (idx < 0) {
        xSemaphoreGive(s_yuy2_lock);
        return ESP_ERR_TIMEOUT;
    }

#if SI_YUY2_CONVERT_TO_RGB565_FOR_JPEG
    memcpy(s_yuy2_buffers[idx], frame->data, expected_len);
#elif SI_YUY2_SWAP_UV_FOR_JPEG
    const uint8_t *src = frame->data;
    uint8_t *dst = s_yuy2_buffers[idx];
    for (size_t i = 0; i + 3 < expected_len; i += 4) {
        dst[i] = src[i];
        dst[i + 1] = src[i + 3];
        dst[i + 2] = src[i + 2];
        dst[i + 3] = src[i + 1];
    }
#else
    memcpy(s_yuy2_buffers[idx], frame->data, expected_len);
#endif
    s_yuy2_ready_idx = idx;
    s_yuy2_ready_len = expected_len;
    s_yuy2_ready_width = frame->vs_format.h_res;
    s_yuy2_ready_height = frame->vs_format.v_res;
    s_yuy2_write_idx = (idx + 1) % SI_YUY2_BUFFER_COUNT;
    xSemaphoreGive(s_yuy2_lock);

    xTaskNotifyGive(s_yuy2_encode_task);
    return ESP_OK;
}

static void yuy2_encode_task(void *arg)
{
    (void)arg;
    uint32_t last_encoded_ms = 0;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (true) {
            int idx = -1;
            size_t raw_len = 0;
            uint32_t width = 0;
            uint32_t height = 0;

            if (xSemaphoreTake(s_yuy2_lock, portMAX_DELAY) == pdTRUE) {
                idx = s_yuy2_ready_idx;
                if (idx >= 0) {
                    raw_len = s_yuy2_ready_len;
                    width = s_yuy2_ready_width;
                    height = s_yuy2_ready_height;
                    s_yuy2_ready_idx = -1;
                    s_yuy2_ready_len = 0;
                    s_yuy2_encoding_idx = idx;
                }
                xSemaphoreGive(s_yuy2_lock);
            }

            if (idx < 0) {
                break;
            }

            uint32_t quality = CONFIG_SI_VIDEO_JPEG_QUALITY;
            if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
                quality = s_status.jpeg_quality;
                xSemaphoreGive(s_lock);
            }

#if SI_YUY2_CONVERT_TO_RGB565_FOR_JPEG
            const uint8_t *encode_input = s_rgb565_encode;
            jpeg_enc_input_format_t encode_format = JPEG_ENCODE_IN_FORMAT_RGB565;
            if (s_rgb565_encode && s_rgb565_encode_cap >= raw_len) {
                yuy2_to_rgb565(s_yuy2_buffers[idx], s_rgb565_encode, raw_len);
            } else {
                encode_input = NULL;
            }
#else
            const uint8_t *encode_input = s_yuy2_buffers[idx];
            jpeg_enc_input_format_t encode_format = JPEG_ENCODE_IN_FORMAT_YUV422;
#endif

            jpeg_encode_cfg_t enc_cfg = {
                .height = height,
                .width = width,
                .src_type = encode_format,
                .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
                .image_quality = quality,
            };
            uint32_t jpeg_size = 0;
            esp_err_t ret = ESP_ERR_INVALID_STATE;
            if (s_jpeg_encoder && s_encode_jpeg && encode_input && raw_len > 0) {
                ret = jpeg_encoder_process(s_jpeg_encoder, &enc_cfg,
                                           encode_input, (uint32_t)raw_len,
                                           s_encode_jpeg, (uint32_t)s_encode_jpeg_cap,
                                           &jpeg_size);
            }

            if (xSemaphoreTake(s_yuy2_lock, portMAX_DELAY) == pdTRUE) {
                if (s_yuy2_encoding_idx == idx) {
                    s_yuy2_encoding_idx = -1;
                }
                xSemaphoreGive(s_yuy2_lock);
            }

            uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (ret == ESP_OK && jpeg_size > 0 && jpeg_size <= s_latest_jpeg_cap) {
                    memcpy(s_latest_jpeg, s_encode_jpeg, jpeg_size);
                    s_status.width = width;
                    s_status.height = height;
                    s_status.last_jpeg_size = jpeg_size;
                    s_status.last_frame_ms = last_encoded_ms == 0 ? 0 : now_ms - last_encoded_ms;
                    s_status.frames_encoded++;
                    s_status.frame_ready = true;
                    s_status.streaming = true;
                    strlcpy(s_status.pixel_format, "YUY2", sizeof(s_status.pixel_format));
                    s_status.last_error[0] = '\0';
                    last_encoded_ms = now_ms;
                    update_fps(now_ms);
                } else {
                    s_status.frames_dropped++;
                    if (ret == ESP_OK) {
                        set_error("YUY2 JPEG output buffer too small", ESP_ERR_NO_MEM);
                    } else {
                        set_error("YUY2 JPEG encode", ret);
                    }
                }
                xSemaphoreGive(s_lock);
            }
        }
    }
}

static size_t mjpeg_complete_len(const uint8_t *data, size_t len)
{
    if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return 0;
    }

    for (size_t i = len - 1; i > 0; i--) {
        if (data[i - 1] == 0xFF && data[i] == 0xD9) {
            return i + 1;
        }
    }
    return 0;
}

static bool jpeg_marker_has_length(uint8_t marker)
{
    if (marker == 0x01 || marker == 0xD8 || marker == 0xD9 ||
        (marker >= 0xD0 && marker <= 0xD7)) {
        return false;
    }
    return true;
}

static bool jpeg_marker_is_sof(uint8_t marker)
{
    return marker >= 0xC0 && marker <= 0xCF &&
           marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

static bool mjpeg_header_valid(const uint8_t *data, size_t len,
                               uint32_t expected_width, uint32_t expected_height)
{
    if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8 ||
        data[len - 2] != 0xFF || data[len - 1] != 0xD9) {
        return false;
    }

    bool saw_sof = false;
    bool saw_sos = false;
    size_t pos = 2;

    while (pos + 1 < len) {
        if (data[pos] != 0xFF) {
            return false;
        }
        while (pos < len && data[pos] == 0xFF) {
            pos++;
        }
        if (pos >= len) {
            return false;
        }

        uint8_t marker = data[pos++];
        if (marker == 0xD9) {
            return saw_sof && saw_sos && pos == len;
        }
        if (!jpeg_marker_has_length(marker)) {
            continue;
        }
        if (pos + 2 > len) {
            return false;
        }

        uint16_t segment_len = ((uint16_t)data[pos] << 8) | data[pos + 1];
        if (segment_len < 2 || pos + segment_len > len) {
            return false;
        }

        if (jpeg_marker_is_sof(marker)) {
            if (segment_len < 8) {
                return false;
            }
            uint32_t height = ((uint32_t)data[pos + 3] << 8) | data[pos + 4];
            uint32_t width = ((uint32_t)data[pos + 5] << 8) | data[pos + 6];
            if ((expected_width && width != expected_width) ||
                (expected_height && height != expected_height)) {
                return false;
            }
            saw_sof = true;
        }

        pos += segment_len;
        if (marker == 0xDA) {
            saw_sos = true;
            return saw_sof;
        }
    }

    return false;
}

static void power_cycle_uvc_root_port(void)
{
    ESP_LOGW(TAG, "power cycling USB UVC root port");
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_uvc_selection.valid = false;
        s_mode_count = 0;
        s_status.modes_count = 0;
        s_status.streaming = false;
        s_status.frame_ready = false;
        s_status.last_jpeg_size = 0;
        s_status.fps_x100 = 0;
        strlcpy(s_status.last_error, "power cycling USB UVC port", sizeof(s_status.last_error));
        xSemaphoreGive(s_lock);
    }

    esp_err_t ret = usb_host_lib_set_root_port_power(false);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB root port power off returned %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(700));

    ret = usb_host_device_free_all();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "usb_host_device_free_all returned %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    ret = usb_host_lib_set_root_port_power(true);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB root port power on returned %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void usb_host_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t event_flags = 0;
        esp_err_t ret = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "USB host event handling returned %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB host: all devices freed");
        }
    }
}

static bool uvc_frame_cb(const uvc_host_frame_t *frame, void *user_ctx)
{
    (void)user_ctx;

    if (!frame || !frame->data || frame->data_len == 0) {
        return true;
    }

    if (frame->vs_format.format == UVC_VS_FORMAT_YUY2) {
        size_t expected_len = yuy2_frame_len(frame->vs_format.h_res, frame->vs_format.v_res);
        if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(30)) != pdTRUE) {
            s_status.frames_dropped++;
            return true;
        }
        s_status.frames_captured++;
        s_status.width = frame->vs_format.h_res;
        s_status.height = frame->vs_format.v_res;
        s_status.streaming = true;
        strlcpy(s_status.pixel_format, "YUY2", sizeof(s_status.pixel_format));
        if (frame->data_len < expected_len) {
            s_status.frames_dropped++;
            snprintf(s_status.last_error, sizeof(s_status.last_error),
                     "short UVC YUY2 frame: %u/%u", (unsigned)frame->data_len,
                     (unsigned)expected_len);
            xSemaphoreGive(s_lock);
            return true;
        }
        xSemaphoreGive(s_lock);

        esp_err_t ret = queue_yuy2_frame(frame);
        if (ret != ESP_OK && s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(30)) == pdTRUE) {
            s_status.frames_dropped++;
            set_error("queue YUY2 frame", ret);
            xSemaphoreGive(s_lock);
        }
        return true;
    }

    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(30)) != pdTRUE) {
        s_status.frames_dropped++;
        return true;
    }

    s_status.frames_captured++;
    if (!s_capture_enabled) {
        xSemaphoreGive(s_lock);
        return true;
    }
    if (frame->vs_format.format != UVC_VS_FORMAT_MJPEG) {
        s_status.frames_dropped++;
        snprintf(s_status.last_error, sizeof(s_status.last_error),
                 "unsupported UVC format: %s", uvc_format_name(frame->vs_format.format));
        xSemaphoreGive(s_lock);
        return true;
    }

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_capture_stride_ms > 0 && s_last_accepted_frame_ms != 0 &&
        (uint32_t)(now_ms - s_last_accepted_frame_ms) < s_capture_stride_ms) {
        xSemaphoreGive(s_lock);
        return true;
    }

    size_t jpeg_len = mjpeg_complete_len(frame->data, frame->data_len);
    if (jpeg_len == 0) {
        s_status.frames_dropped++;
        strlcpy(s_status.last_error, "incomplete UVC MJPEG frame", sizeof(s_status.last_error));
        xSemaphoreGive(s_lock);
        return true;
    }
    if (!mjpeg_header_valid(frame->data, jpeg_len, frame->vs_format.h_res, frame->vs_format.v_res)) {
        s_status.frames_dropped++;
        strlcpy(s_status.last_error, "invalid UVC MJPEG header", sizeof(s_status.last_error));
        xSemaphoreGive(s_lock);
        return true;
    }
    if (jpeg_len > s_latest_jpeg_cap) {
        s_status.frames_dropped++;
        set_error("UVC JPEG buffer too small", ESP_ERR_NO_MEM);
        xSemaphoreGive(s_lock);
        return true;
    }

    memcpy(s_latest_jpeg, frame->data, jpeg_len);
    s_status.width = frame->vs_format.h_res;
    s_status.height = frame->vs_format.v_res;
    s_status.last_jpeg_size = jpeg_len;
    s_status.last_frame_ms = s_last_accepted_frame_ms == 0 ? 0 : now_ms - s_last_accepted_frame_ms;
    s_status.frames_encoded++;
    s_status.frame_ready = true;
    s_status.streaming = true;
    s_status.last_error[0] = '\0';
    s_last_accepted_frame_ms = now_ms;
    update_fps(now_ms);
    xSemaphoreGive(s_lock);

    return true;
}

static void uvc_stream_event_cb(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (!event) {
        return;
    }

    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGW(TAG, "UVC transfer error: %s", esp_err_to_name(event->transfer_error.error));
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_status.frames_dropped++;
            set_error("UVC transfer", event->transfer_error.error);
            xSemaphoreGive(s_lock);
        }
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "UVC camera disconnected");
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_uvc_selection.valid = false;
            s_mode_count = 0;
            s_status.modes_count = 0;
            s_status.streaming = false;
            s_status.frame_ready = false;
            s_status.last_jpeg_size = 0;
            strlcpy(s_status.last_error, "UVC camera disconnected", sizeof(s_status.last_error));
            xSemaphoreGive(s_lock);
        }
        if (s_uvc_stream_task) {
            xTaskNotifyGive(s_uvc_stream_task);
        }
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "UVC frame buffer overflow");
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_status.frames_dropped++;
            strlcpy(s_status.last_error, "UVC frame buffer overflow", sizeof(s_status.last_error));
            xSemaphoreGive(s_lock);
        }
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGW(TAG, "UVC frame buffer underflow");
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_status.frames_dropped++;
            strlcpy(s_status.last_error, "UVC frame buffer underflow", sizeof(s_status.last_error));
            xSemaphoreGive(s_lock);
        }
        break;
#ifdef UVC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case UVC_HOST_DEVICE_SUSPENDED:
        ESP_LOGI(TAG, "UVC camera suspended");
        break;
    case UVC_HOST_DEVICE_RESUMED:
        ESP_LOGI(TAG, "UVC camera resumed");
        break;
#endif
    default:
        break;
    }
}

static void uvc_driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (!event || event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        return;
    }

    ESP_LOGI(TAG, "UVC device connected addr=%u stream=%u frames=%u",
             event->device_connected.dev_addr,
             event->device_connected.uvc_stream_index,
             (unsigned)event->device_connected.frame_info_num);

    size_t list_size = event->device_connected.frame_info_num;
    if (list_size == 0) {
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_uvc_selection.valid = false;
            strlcpy(s_status.last_error, "UVC device reported no frame modes", sizeof(s_status.last_error));
            xSemaphoreGive(s_lock);
        }
        return;
    }
    uvc_host_frame_info_t *list = calloc(list_size, sizeof(uvc_host_frame_info_t));
    if (!list) {
        return;
    }
    if (uvc_host_get_frame_list(event->device_connected.dev_addr,
                                event->device_connected.uvc_stream_index,
                                (uvc_host_frame_info_t (*)[])list,
                                &list_size) == ESP_OK) {
        for (size_t i = 0; i < list_size; i++) {
            ESP_LOGI(TAG, "UVC frame[%u]: %s %ux%u %.1ffps",
                     (unsigned)i, uvc_format_name(list[i].format),
                     list[i].h_res, list[i].v_res,
                     uvc_interval_to_fps(list[i].default_interval));
        }

        si_uvc_selection_t selected = {0};
        bool has_selection = false;
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            store_uvc_modes_locked(list, list_size,
                                   event->device_connected.dev_addr,
                                   event->device_connected.uvc_stream_index);
            if (!s_capture_enabled) {
                s_uvc_selection.valid = false;
                reset_frame_state_locked("video capture idle");
                xSemaphoreGive(s_lock);
                free(list);
                return;
            }
            has_selection = select_stored_mode_locked(s_target_width, s_target_height, true,
                                                      s_target_fps_x100, s_target_exact_fps, &selected);
            if (has_selection) {
                s_uvc_selection.valid = true;
                s_uvc_selection = selected;
                s_status.width = selected.format.h_res;
                s_status.height = selected.format.v_res;
                s_status.target_width = s_target_width;
                s_status.target_height = s_target_height;
                s_status.target_fps_x100 = fps_float_to_x100(selected.format.fps);
                strlcpy(s_status.pixel_format, uvc_format_name(selected.format.format),
                        sizeof(s_status.pixel_format));
                s_status.streaming = false;
                s_status.frame_ready = false;
                s_status.last_jpeg_size = 0;
                s_status.last_error[0] = '\0';
            }
            xSemaphoreGive(s_lock);
        }

        if (has_selection) {
            ESP_LOGI(TAG, "selected UVC %s mode addr=%u stream=%u %ux%u %.1ffps",
                     uvc_format_name(selected.format.format),
                     selected.dev_addr, selected.stream_index,
                     selected.format.h_res, selected.format.v_res,
                     selected.format.fps);
            if (s_uvc_stream_task) {
                xTaskNotifyGive(s_uvc_stream_task);
            }
        } else {
            if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                s_uvc_selection.valid = false;
                s_status.streaming = false;
                s_status.frame_ready = false;
                strlcpy(s_status.last_error, "UVC device has no supported video mode", sizeof(s_status.last_error));
                xSemaphoreGive(s_lock);
            }
            ESP_LOGW(TAG, "UVC device has no supported video mode");
        }
    } else if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_uvc_selection.valid = false;
        set_error("read UVC frame list", ESP_FAIL);
        xSemaphoreGive(s_lock);
    }
    free(list);
}

static bool get_uvc_selection(si_uvc_selection_t *selection)
{
    if (!selection) {
        return false;
    }
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    bool valid = s_capture_enabled && s_uvc_selection.valid;
    if (valid) {
        *selection = s_uvc_selection;
    }
    xSemaphoreGive(s_lock);
    return valid;
}

static void uvc_stream_task(void *arg)
{
    (void)arg;

    while (true) {
        si_uvc_selection_t selection = {0};
        if (!get_uvc_selection(&selection)) {
            if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
                s_status.streaming = false;
                if (!s_capture_enabled) {
                    strlcpy(s_status.last_error, "video capture idle", sizeof(s_status.last_error));
                } else if (s_status.last_error[0] == '\0') {
                    strlcpy(s_status.last_error, "waiting for USB UVC device", sizeof(s_status.last_error));
                }
                xSemaphoreGive(s_lock);
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }

        uvc_host_stream_config_t stream_cfg = {
            .event_cb = uvc_stream_event_cb,
            .frame_cb = uvc_frame_cb,
            .usb = {
                .dev_addr = selection.dev_addr,
                .vid = UVC_HOST_ANY_VID,
                .pid = UVC_HOST_ANY_PID,
                .uvc_stream_index = selection.stream_index,
            },
            .vs_format = {
                .h_res = selection.format.h_res,
                .v_res = selection.format.v_res,
                .fps = selection.format.fps,
                .format = selection.format.format,
            },
            .advanced = {
                .number_of_frame_buffers = CONFIG_SI_VIDEO_UVC_FRAME_BUFFERS,
                .frame_size = 0,
                .frame_heap_caps = uvc_frame_heap_caps(),
                .number_of_urbs = CONFIG_SI_VIDEO_UVC_URBS,
                .urb_size = CONFIG_SI_VIDEO_UVC_URB_SIZE,
            },
        };

        ESP_LOGI(TAG, "opening USB UVC camera addr=%u stream=%u: %ux%u %s %.1ffps",
                 selection.dev_addr, selection.stream_index,
                 selection.format.h_res, selection.format.v_res,
                 uvc_format_name(selection.format.format), selection.format.fps);
        esp_err_t ret = uvc_host_stream_open(&stream_cfg, pdMS_TO_TICKS(SI_UVC_OPEN_TIMEOUT_MS), &s_uvc_stream);
        if (ret != ESP_OK) {
            set_error("open UVC stream", ret);
            ESP_LOGW(TAG, "open UVC stream failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uvc_host_stream_format_t active_format = stream_cfg.vs_format;
        uvc_host_stream_format_t actual_format = {0};
        if (uvc_host_stream_format_get(s_uvc_stream, &actual_format) == ESP_OK) {
            active_format = actual_format;
            if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                s_uvc_selection.format = actual_format;
                s_status.width = actual_format.h_res;
                s_status.height = actual_format.v_res;
                s_status.target_fps_x100 = fps_float_to_x100(actual_format.fps);
                strlcpy(s_status.pixel_format, uvc_format_name(actual_format.format),
                        sizeof(s_status.pixel_format));
                xSemaphoreGive(s_lock);
            }
        }

            if (!SI_UVC_MJPEG_ONLY && active_format.format == UVC_VS_FORMAT_YUY2) {
                ret = ensure_yuy2_buffers(yuy2_frame_len(active_format.h_res, active_format.v_res));
                if (ret != ESP_OK) {
                    set_error("alloc YUY2 encode buffers", ret);
                ESP_LOGW(TAG, "alloc YUY2 encode buffers failed: %s", esp_err_to_name(ret));
                uvc_host_stream_close(s_uvc_stream);
                s_uvc_stream = NULL;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        ret = uvc_host_stream_start(s_uvc_stream);
        if (ret != ESP_OK) {
            set_error("start UVC stream", ret);
            ESP_LOGW(TAG, "start UVC stream failed: %s", esp_err_to_name(ret));
            uvc_host_stream_close(s_uvc_stream);
            s_uvc_stream = NULL;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_status.streaming = true;
            s_status.last_error[0] = '\0';
            xSemaphoreGive(s_lock);
        }
        ESP_LOGI(TAG, "USB UVC stream started");

        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
        }

        uint32_t last_stream_frames = 0;
        TickType_t no_frame_since = xTaskGetTickCount();
        bool no_frame_timeout = false;
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            last_stream_frames = (!SI_UVC_MJPEG_ONLY && active_format.format == UVC_VS_FORMAT_YUY2) ?
                                 s_status.frames_captured : s_status.frames_encoded;
            xSemaphoreGive(s_lock);
        }

        while (true) {
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) > 0) {
                break;
            }

            TickType_t now = xTaskGetTickCount();
            uint32_t stream_frames = last_stream_frames;
            if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                stream_frames = (!SI_UVC_MJPEG_ONLY && active_format.format == UVC_VS_FORMAT_YUY2) ?
                                s_status.frames_captured : s_status.frames_encoded;
                xSemaphoreGive(s_lock);
            }

            if (stream_frames != last_stream_frames) {
                last_stream_frames = stream_frames;
                no_frame_since = now;
                s_no_frame_reopen_count = 0;
                continue;
            }

            if ((int32_t)(now - no_frame_since) >= pdMS_TO_TICKS(SI_UVC_NO_FRAME_TIMEOUT_MS)) {
                if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                    strlcpy(s_status.last_error, "UVC stream produced no complete frames",
                            sizeof(s_status.last_error));
                    xSemaphoreGive(s_lock);
                }
                ESP_LOGW(TAG, "UVC stream produced no complete frames; reopening stream");
                no_frame_timeout = true;
                break;
            }
        }

        if (s_uvc_stream) {
            (void)uvc_host_stream_stop(s_uvc_stream);
            ret = uvc_host_stream_close(s_uvc_stream);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "close UVC stream returned %s", esp_err_to_name(ret));
            }
            s_uvc_stream = NULL;
        }
        if (no_frame_timeout) {
            s_no_frame_reopen_count++;
            if (!SI_UVC_MJPEG_ONLY && s_no_frame_reopen_count >= 2) {
                s_no_frame_reopen_count = 0;
                power_cycle_uvc_root_port();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static esp_err_t init_uvc(void)
{
    s_latest_jpeg_cap = CONFIG_SI_VIDEO_UVC_JPEG_BUFFER_SIZE;
    s_latest_jpeg = heap_caps_malloc(s_latest_jpeg_cap, uvc_frame_heap_caps());
    if (!s_latest_jpeg) {
        s_latest_jpeg = heap_caps_malloc(s_latest_jpeg_cap, MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_latest_jpeg, ESP_ERR_NO_MEM, TAG, "alloc UVC latest JPEG buffer");

    BaseType_t ok;
    if (!SI_UVC_MJPEG_ONLY) {
        jpeg_encode_memory_alloc_cfg_t jpeg_mem_cfg = {
            .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
        };
        s_encode_jpeg = jpeg_alloc_encoder_mem(s_latest_jpeg_cap, &jpeg_mem_cfg, &s_encode_jpeg_cap);
        ESP_RETURN_ON_FALSE(s_encode_jpeg && s_encode_jpeg_cap >= s_latest_jpeg_cap,
                            ESP_ERR_NO_MEM, TAG, "alloc YUY2 JPEG encode buffer");

        jpeg_encode_engine_cfg_t jpeg_engine_cfg = {
            .timeout_ms = 200,
        };
        ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&jpeg_engine_cfg, &s_jpeg_encoder),
                            TAG, "create JPEG encoder");

        ok = xTaskCreatePinnedToCore(yuy2_encode_task, "si_yuy2_jpeg",
                                     SI_YUY2_ENCODE_TASK_STACK, NULL,
                                     SI_YUY2_ENCODE_TASK_PRIORITY,
                                     &s_yuy2_encode_task,
                                     tskNO_AFFINITY);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create YUY2 encode task");
    }

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_config), TAG, "install USB host");

    ok = xTaskCreatePinnedToCore(usb_host_task, "si_usb_host", 4096, NULL,
                                 SI_UVC_TASK_PRIORITY, &s_usb_host_task,
                                 tskNO_AFFINITY);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create USB host task");

    const uvc_host_driver_config_t driver_cfg = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = SI_UVC_TASK_PRIORITY + 1,
        .xCoreID = tskNO_AFFINITY,
        .create_background_task = true,
        .event_cb = uvc_driver_event_cb,
    };
    ESP_RETURN_ON_ERROR(uvc_host_install(&driver_cfg), TAG, "install UVC host driver");

    ok = xTaskCreatePinnedToCore(uvc_stream_task, "si_uvc_stream", 6144, NULL,
                                 SI_UVC_TASK_PRIORITY - 1, &s_uvc_stream_task,
                                 tskNO_AFFINITY);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create UVC stream task");

    ESP_LOGI(TAG, "USB UVC host initialized");
    return ESP_OK;
}

esp_err_t si_video_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.enabled = SI_VIDEO_CFG_ENABLE;
    s_status.width = CONFIG_SI_VIDEO_WIDTH;
    s_status.height = CONFIG_SI_VIDEO_HEIGHT;
    s_status.target_width = CONFIG_SI_VIDEO_WIDTH;
    s_status.target_height = CONFIG_SI_VIDEO_HEIGHT;
    s_status.target_fps_x100 = CONFIG_SI_VIDEO_UVC_FPS * 100U;
    s_status.capture_enabled = false;
    s_status.capture_stride_ms = 0;
    s_status.data_lanes = 0;
    s_status.lane_bitrate_mbps = 0;
    s_status.jpeg_quality = CONFIG_SI_VIDEO_JPEG_QUALITY;
    s_target_width = CONFIG_SI_VIDEO_WIDTH;
    s_target_height = CONFIG_SI_VIDEO_HEIGHT;
    s_target_fps_x100 = CONFIG_SI_VIDEO_UVC_FPS * 100U;
    s_target_exact_fps = true;
    s_capture_enabled = false;
    s_capture_stride_ms = 0;
    strlcpy(s_status.source, "usb-uvc", sizeof(s_status.source));
    strlcpy(s_status.pixel_format, "MJPEG", sizeof(s_status.pixel_format));
    strlcpy(s_capture_owner, "off", sizeof(s_capture_owner));
    strlcpy(s_status.capture_owner, s_capture_owner, sizeof(s_status.capture_owner));

    if (!SI_VIDEO_CFG_ENABLE) {
        strlcpy(s_status.last_error, "video disabled by config", sizeof(s_status.last_error));
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create video mutex");
    if (!SI_UVC_MJPEG_ONLY) {
        s_yuy2_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_yuy2_lock, ESP_ERR_NO_MEM, TAG, "create YUY2 mutex");
    }

    esp_err_t ret = init_uvc();
    if (ret != ESP_OK) {
        set_error("init USB UVC", ret);
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
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        *status = s_status;
        xSemaphoreGive(s_lock);
    } else {
        *status = s_status;
    }
}

esp_err_t si_video_get_modes(si_video_mode_t *modes, size_t max_modes, size_t *out_count)
{
    if (!out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t count = s_mode_count;
    if (modes && max_modes > 0) {
        size_t copy_count = count < max_modes ? count : max_modes;
        for (size_t i = 0; i < copy_count; i++) {
            modes[i] = s_modes[i].public_mode;
            modes[i].selected =
                s_uvc_selection.valid &&
                s_uvc_selection.format.h_res == modes[i].width &&
                s_uvc_selection.format.v_res == modes[i].height &&
                diff_u32(fps_float_to_x100(s_uvc_selection.format.fps), modes[i].fps_x100) <= 50;
        }
    }
    *out_count = count;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_video_set_capture(bool enabled, const char *owner, uint32_t width, uint32_t height,
                               uint32_t fps_x100, bool exact_fps, uint32_t stride_ms)
{
    if (enabled && (width < 160 || height < 120 || width > 3840 || height > 2160)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (enabled && fps_x100 > 0 && (fps_x100 < 100 || fps_x100 > 24000)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    bool notify = false;
    bool was_enabled = s_capture_enabled;
    si_uvc_selection_t previous = s_uvc_selection;
    uint32_t previous_stride = s_capture_stride_ms;

    s_capture_enabled = enabled;
    s_capture_stride_ms = enabled ? stride_ms : 0;
    strlcpy(s_capture_owner, enabled && owner ? owner : "off", sizeof(s_capture_owner));
    s_status.capture_enabled = s_capture_enabled;
    s_status.capture_stride_ms = s_capture_stride_ms;
    strlcpy(s_status.capture_owner, s_capture_owner, sizeof(s_status.capture_owner));

    if (!enabled) {
        reset_frame_state_locked("video capture idle");
        notify = was_enabled;
        xSemaphoreGive(s_lock);
        if (notify && s_uvc_stream_task) {
            xTaskNotifyGive(s_uvc_stream_task);
        }
        return ESP_OK;
    }

    uint32_t requested_fps_x100 = fps_x100 > 0 ? fps_x100 : s_target_fps_x100;
    s_target_width = width;
    s_target_height = height;
    s_target_fps_x100 = requested_fps_x100;
    s_target_exact_fps = exact_fps;
    s_status.target_width = width;
    s_status.target_height = height;
    s_status.target_fps_x100 = requested_fps_x100;

    si_uvc_selection_t selection = {0};
    if (!select_stored_mode_locked(width, height, true, requested_fps_x100, exact_fps, &selection)) {
        if (s_mode_count > 0) {
            s_capture_enabled = false;
            s_capture_stride_ms = 0;
            s_status.capture_enabled = false;
            s_status.capture_stride_ms = 0;
            strlcpy(s_capture_owner, "off", sizeof(s_capture_owner));
            strlcpy(s_status.capture_owner, s_capture_owner, sizeof(s_status.capture_owner));
            reset_frame_state_locked("video mode is not available");
            xSemaphoreGive(s_lock);
            if (was_enabled && s_uvc_stream_task) {
                xTaskNotifyGive(s_uvc_stream_task);
            }
            return ESP_ERR_NOT_FOUND;
        }

        s_uvc_selection.valid = false;
        reset_frame_state_locked("waiting for USB UVC device");
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    uint32_t selected_fps_x100 = fps_float_to_x100(selection.format.fps);
    s_target_fps_x100 = selected_fps_x100;
    s_status.target_fps_x100 = selected_fps_x100;
    s_uvc_selection = selection;
    s_status.width = selection.format.h_res;
    s_status.height = selection.format.v_res;
    strlcpy(s_status.pixel_format, uvc_format_name(selection.format.format),
            sizeof(s_status.pixel_format));

    bool changed = !was_enabled ||
                   !previous.valid ||
                   previous.format.h_res != selection.format.h_res ||
                   previous.format.v_res != selection.format.v_res ||
                   previous.format.format != selection.format.format ||
                   diff_u32(fps_float_to_x100(previous.format.fps), selected_fps_x100) > 50 ||
                   previous_stride != s_capture_stride_ms;
    if (changed) {
        reset_frame_state_locked("switching video capture");
        notify = true;
    }

    xSemaphoreGive(s_lock);

    if (notify && s_uvc_stream_task) {
        xTaskNotifyGive(s_uvc_stream_task);
    }
    return ESP_OK;
}

esp_err_t si_video_set_mode(uint32_t width, uint32_t height, uint32_t fps_x100)
{
    return si_video_set_capture(true, "kvm", width, height, fps_x100, fps_x100 > 0, 0);
}

esp_err_t si_video_set_resolution(uint32_t width, uint32_t height)
{
    return si_video_set_mode(width, height, 0);
}

esp_err_t si_video_set_quality(uint32_t quality)
{
    if (quality < 1 || quality > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.jpeg_quality = quality;
        xSemaphoreGive(s_lock);
    } else {
        s_status.jpeg_quality = quality;
    }
    return ESP_OK;
}

esp_err_t si_video_acquire_jpeg(uint8_t **out_buf, size_t *out_len, uint32_t *out_frame_id)
{
    if (!out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }

        if (!s_status.frame_ready || s_status.last_jpeg_size == 0) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NOT_FOUND;
        }

        size_t need = s_status.last_jpeg_size;
        xSemaphoreGive(s_lock);

        uint8_t *copy = malloc(need);
        if (!copy) {
            return ESP_ERR_NO_MEM;
        }

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
            free(copy);
            return ESP_ERR_TIMEOUT;
        }

        if (!s_status.frame_ready || s_status.last_jpeg_size == 0) {
            xSemaphoreGive(s_lock);
            free(copy);
            return ESP_ERR_NOT_FOUND;
        }
        if (s_status.last_jpeg_size > need) {
            xSemaphoreGive(s_lock);
            free(copy);
            continue;
        }

        memcpy(copy, s_latest_jpeg, s_status.last_jpeg_size);
        *out_len = s_status.last_jpeg_size;
        if (out_frame_id) {
            *out_frame_id = s_status.frames_encoded;
        }
        xSemaphoreGive(s_lock);

        *out_buf = copy;
        return ESP_OK;
    }

    return ESP_ERR_NO_MEM;
}
