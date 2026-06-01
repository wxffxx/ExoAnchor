#include "video_input.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "driver/jpeg_encode.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/mipi_csi_types.h"
#include "sdkconfig.h"

static const char *TAG = "si-video";

#ifdef CONFIG_SI_VIDEO_ENABLE
#define SI_VIDEO_CFG_ENABLE 1
#else
#define SI_VIDEO_CFG_ENABLE 0
#endif

#ifdef CONFIG_SI_VIDEO_BYTE_SWAP
#define SI_VIDEO_CFG_BYTE_SWAP 1
#else
#define SI_VIDEO_CFG_BYTE_SWAP 0
#endif

#ifdef CONFIG_SI_VIDEO_CAMERA_SENSOR
#define SI_VIDEO_CFG_CAMERA_SENSOR 1
#else
#define SI_VIDEO_CFG_CAMERA_SENSOR 0
#endif

#ifdef CONFIG_SI_VIDEO_MIPI_LDO_ENABLE
#define SI_VIDEO_CFG_MIPI_LDO_ENABLE 1
#else
#define SI_VIDEO_CFG_MIPI_LDO_ENABLE 0
#endif

#ifndef CONFIG_SI_VIDEO_WIDTH
#define CONFIG_SI_VIDEO_WIDTH 0
#endif
#ifndef CONFIG_SI_VIDEO_HEIGHT
#define CONFIG_SI_VIDEO_HEIGHT 0
#endif
#ifndef CONFIG_SI_VIDEO_DATA_LANES
#define CONFIG_SI_VIDEO_DATA_LANES 2
#endif
#ifndef CONFIG_SI_VIDEO_LANE_BITRATE_MBPS
#define CONFIG_SI_VIDEO_LANE_BITRATE_MBPS 800
#endif
#ifndef CONFIG_SI_VIDEO_JPEG_QUALITY
#define CONFIG_SI_VIDEO_JPEG_QUALITY 75
#endif
#ifndef CONFIG_SI_VIDEO_SENSOR_FORMAT
#define CONFIG_SI_VIDEO_SENSOR_FORMAT ""
#endif
#ifndef CONFIG_SI_VIDEO_SCCB_SCL_GPIO
#define CONFIG_SI_VIDEO_SCCB_SCL_GPIO 8
#endif
#ifndef CONFIG_SI_VIDEO_SCCB_SDA_GPIO
#define CONFIG_SI_VIDEO_SCCB_SDA_GPIO 7
#endif
#ifndef CONFIG_SI_VIDEO_SCCB_FREQ_HZ
#define CONFIG_SI_VIDEO_SCCB_FREQ_HZ 10000
#endif
#ifndef CONFIG_SI_VIDEO_SENSOR_RESET_GPIO
#define CONFIG_SI_VIDEO_SENSOR_RESET_GPIO -1
#endif
#ifndef CONFIG_SI_VIDEO_SENSOR_PWDN_GPIO
#define CONFIG_SI_VIDEO_SENSOR_PWDN_GPIO -1
#endif
#ifndef CONFIG_SI_VIDEO_STREAM_DELAY_MS
#define CONFIG_SI_VIDEO_STREAM_DELAY_MS 120
#endif
#ifndef CONFIG_SI_VIDEO_MIPI_LDO_CHAN_ID
#define CONFIG_SI_VIDEO_MIPI_LDO_CHAN_ID 3
#endif
#ifndef CONFIG_SI_VIDEO_MIPI_LDO_VOLTAGE_MV
#define CONFIG_SI_VIDEO_MIPI_LDO_VOLTAGE_MV 2500
#endif

static si_video_status_t s_status;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_capture_task;
static esp_cam_ctlr_handle_t s_cam;
static esp_ldo_channel_handle_t s_mipi_ldo;
static isp_proc_handle_t s_isp;
static i2c_master_bus_handle_t s_sensor_i2c_bus;
static esp_sccb_io_handle_t s_sensor_sccb;
static esp_cam_sensor_device_t *s_sensor;
static jpeg_encoder_handle_t s_jpeg;
static const uint8_t *s_csi_frame;
static uint8_t *s_encode_frame;
static uint8_t *s_jpeg_work;
static uint8_t *s_latest_jpeg;
static size_t s_raw_len;
static size_t s_jpeg_work_cap;
static size_t s_latest_jpeg_cap;
static volatile uint32_t s_frame_irq_count;

static void set_error(const char *message, esp_err_t err)
{
    if (!message) {
        message = "unknown";
    }
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s: %s", message, esp_err_to_name(err));
}

static bool IRAM_ATTR csi_trans_finished_cb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    (void)handle;
    (void)trans;
    (void)user_data;

    s_frame_irq_count++;
    BaseType_t high_task_woken = pdFALSE;
    if (s_capture_task) {
        vTaskNotifyGiveFromISR(s_capture_task, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

static esp_err_t init_camera_sensor(void)
{
#if !SI_VIDEO_CFG_CAMERA_SENSOR
    return ESP_OK;
#else
    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = CONFIG_SI_VIDEO_SCCB_SDA_GPIO,
        .scl_io_num = CONFIG_SI_VIDEO_SCCB_SCL_GPIO,
        .i2c_port = I2C_NUM_0,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_conf, &s_sensor_i2c_bus),
                        TAG, "create camera SCCB/I2C bus");

    esp_cam_sensor_config_t cam_cfg = {
        .reset_pin = CONFIG_SI_VIDEO_SENSOR_RESET_GPIO,
        .pwdn_pin = CONFIG_SI_VIDEO_SENSOR_PWDN_GPIO,
        .xclk_pin = -1,
    };

    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        sccb_i2c_config_t i2c_cfg = {
            .scl_speed_hz = CONFIG_SI_VIDEO_SCCB_FREQ_HZ,
            .device_address = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        esp_err_t ret = sccb_new_i2c_io(s_sensor_i2c_bus, &i2c_cfg, &cam_cfg.sccb_handle);
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "SCCB addr 0x%02x unavailable: %s", p->sccb_addr, esp_err_to_name(ret));
            continue;
        }

        cam_cfg.sensor_port = p->port;
        s_sensor = (*(p->detect))(&cam_cfg);
        if (s_sensor) {
            if (p->port != ESP_CAM_SENSOR_MIPI_CSI) {
                esp_sccb_del_i2c_io(cam_cfg.sccb_handle);
                cam_cfg.sccb_handle = NULL;
                s_sensor = NULL;
                return ESP_ERR_NOT_SUPPORTED;
            }
            s_sensor_sccb = cam_cfg.sccb_handle;
            ESP_LOGI(TAG, "camera sensor detected at SCCB addr 0x%02x", p->sccb_addr);
            break;
        }

        esp_sccb_del_i2c_io(cam_cfg.sccb_handle);
        cam_cfg.sccb_handle = NULL;
    }

    ESP_RETURN_ON_FALSE(s_sensor, ESP_ERR_NOT_FOUND, TAG, "detect MIPI camera sensor");

    esp_cam_sensor_format_array_t fmt_array = {0};
    ESP_RETURN_ON_ERROR(esp_cam_sensor_query_format(s_sensor, &fmt_array),
                        TAG, "query camera formats");

    const esp_cam_sensor_format_t *selected = NULL;
    const esp_cam_sensor_format_t *formats = fmt_array.format_array;
    for (int i = 0; i < fmt_array.count; i++) {
        ESP_LOGI(TAG, "camera fmt[%d]: %s", i, formats[i].name);
        if (strcmp(formats[i].name, CONFIG_SI_VIDEO_SENSOR_FORMAT) == 0) {
            selected = &formats[i];
        }
    }

    ESP_RETURN_ON_FALSE(selected, ESP_ERR_INVALID_ARG, TAG,
                        "camera format not supported: %s", CONFIG_SI_VIDEO_SENSOR_FORMAT);
    ESP_RETURN_ON_ERROR(esp_cam_sensor_set_format(s_sensor, selected),
                        TAG, "set camera sensor format");

    int stream_on = 1;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(s_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_on),
                        TAG, "start camera sensor stream");

    ESP_LOGI(TAG, "camera sensor stream started: %s", selected->name);
    return ESP_OK;
#endif
}

static esp_err_t init_isp(void)
{
#if !SI_VIDEO_CFG_CAMERA_SENSOR
    return ESP_OK;
#else
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = CONFIG_SI_VIDEO_WIDTH,
        .v_res = CONFIG_SI_VIDEO_HEIGHT,
    };
    ESP_RETURN_ON_ERROR(esp_isp_new_processor(&isp_cfg, &s_isp), TAG, "create ISP processor");
    ESP_RETURN_ON_ERROR(esp_isp_enable(s_isp), TAG, "enable ISP processor");
    ESP_LOGI(TAG, "ISP enabled: RAW8 -> RGB565");
    return ESP_OK;
#endif
}

static esp_err_t alloc_buffers(void)
{
    size_t fb_len = 0;
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_get_frame_buffer_len(s_cam, &fb_len),
                        TAG, "get CSI frame buffer length");
    s_raw_len = fb_len;
    if (s_raw_len == 0) {
        s_raw_len = (size_t)CONFIG_SI_VIDEO_WIDTH * CONFIG_SI_VIDEO_HEIGHT * 2U;
    }
    s_jpeg_work_cap = s_raw_len / 2U;
    if (s_jpeg_work_cap < 256U * 1024U) {
        s_jpeg_work_cap = 256U * 1024U;
    }
    s_latest_jpeg_cap = s_jpeg_work_cap;

    const void *frame = NULL;
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_get_frame_buffer(s_cam, 1, &frame),
                        TAG, "get CSI backup frame buffer");
    s_csi_frame = (const uint8_t *)frame;

    jpeg_encode_memory_alloc_cfg_t in_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    jpeg_encode_memory_alloc_cfg_t out_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t allocated = 0;
    s_encode_frame = jpeg_alloc_encoder_mem(s_raw_len, &in_cfg, &allocated);
    ESP_RETURN_ON_FALSE(s_encode_frame && allocated >= s_raw_len, ESP_ERR_NO_MEM,
                        TAG, "alloc raw encode buffer");

    allocated = 0;
    s_jpeg_work = jpeg_alloc_encoder_mem(s_jpeg_work_cap, &out_cfg, &allocated);
    ESP_RETURN_ON_FALSE(s_jpeg_work && allocated >= s_jpeg_work_cap, ESP_ERR_NO_MEM,
                        TAG, "alloc JPEG work buffer");

    s_latest_jpeg = heap_caps_malloc(s_latest_jpeg_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_latest_jpeg, ESP_ERR_NO_MEM, TAG, "alloc latest JPEG buffer");

    ESP_LOGI(TAG, "video buffers: raw=%zu jpeg_cap=%zu csi_frame=%p",
             s_raw_len, s_jpeg_work_cap, s_csi_frame);
    return ESP_OK;
}

static esp_err_t init_csi(void)
{
#if SI_VIDEO_CFG_MIPI_LDO_ENABLE
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = CONFIG_SI_VIDEO_MIPI_LDO_CHAN_ID,
        .voltage_mv = CONFIG_SI_VIDEO_MIPI_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_mipi_ldo),
                        TAG, "enable MIPI PHY LDO");
#endif

    ESP_RETURN_ON_ERROR(init_camera_sensor(), TAG, "init MIPI camera sensor");

    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id = 0,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res = CONFIG_SI_VIDEO_WIDTH,
        .v_res = CONFIG_SI_VIDEO_HEIGHT,
        .data_lane_num = CONFIG_SI_VIDEO_DATA_LANES,
        .lane_bit_rate_mbps = CONFIG_SI_VIDEO_LANE_BITRATE_MBPS,
#if SI_VIDEO_CFG_CAMERA_SENSOR
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
#else
        .input_data_color_type = CAM_CTLR_COLOR_YUV422,
        .output_data_color_type = CAM_CTLR_COLOR_YUV422,
#endif
        .queue_items = 2,
        .byte_swap_en = SI_VIDEO_CFG_BYTE_SWAP,
        .bk_buffer_dis = 0,
    };
    ESP_RETURN_ON_ERROR(esp_cam_new_csi_ctlr(&csi_cfg, &s_cam), TAG, "create CSI controller");

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_trans_finished = csi_trans_finished_cb,
    };
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(s_cam, &cbs, NULL),
                        TAG, "register CSI callbacks");
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(s_cam), TAG, "enable CSI controller");
    ESP_RETURN_ON_ERROR(alloc_buffers(), TAG, "alloc video buffers");
    ESP_RETURN_ON_ERROR(init_isp(), TAG, "init ISP");

    jpeg_encode_engine_cfg_t jpeg_cfg = {
        .timeout_ms = 120,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&jpeg_cfg, &s_jpeg), TAG, "create JPEG encoder");
    return ESP_OK;
}

static void update_fps(uint32_t now_ms)
{
    static uint32_t last_ms;
    static uint32_t last_frames;

    if (last_ms == 0) {
        last_ms = now_ms;
        last_frames = s_status.frames_encoded;
        return;
    }

    uint32_t elapsed = now_ms - last_ms;
    if (elapsed >= 1000) {
        uint32_t frames = s_status.frames_encoded - last_frames;
        s_status.fps_x100 = (frames * 100000U) / elapsed;
        last_ms = now_ms;
        last_frames = s_status.frames_encoded;
    }
}

static void capture_task(void *arg)
{
    (void)arg;

    esp_err_t ret = esp_cam_ctlr_start(s_cam);
    if (ret != ESP_OK) {
        set_error("start CSI", ret);
        ESP_LOGE(TAG, "CSI start failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    s_status.streaming = true;
    ESP_LOGI(TAG, "CSI capture started: %" PRIu32 "x%" PRIu32 " %s lanes=%" PRIu32 " lane=%" PRIu32 "Mbps",
             s_status.width, s_status.height, s_status.pixel_format,
             s_status.data_lanes, s_status.lane_bitrate_mbps);

    uint32_t last_encode_ms = 0;
    while (true) {
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (notified == 0) {
            if (!s_status.frame_ready) {
                strlcpy(s_status.last_error, "waiting for CSI frame", sizeof(s_status.last_error));
                static uint32_t wait_timeouts;
                wait_timeouts++;
                if ((wait_timeouts % 3U) == 0) {
                    ESP_LOGW(TAG, "waiting for CSI frame; irq_count=%" PRIu32, s_frame_irq_count);
                }
            }
            update_fps(now_ms);
            continue;
        }

        s_status.frames_captured += notified;
        if (notified > 1) {
            s_status.frames_dropped += notified - 1;
        }
        if (last_encode_ms != 0 && (now_ms - last_encode_ms) < CONFIG_SI_VIDEO_STREAM_DELAY_MS) {
            s_status.frames_dropped += notified;
            update_fps(now_ms);
            continue;
        }
        last_encode_ms = now_ms;

        esp_cache_msync((void *)s_csi_frame, s_raw_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        memcpy(s_encode_frame, s_csi_frame, s_raw_len);

        jpeg_encode_cfg_t enc_cfg = {
            .height = CONFIG_SI_VIDEO_HEIGHT,
            .width = CONFIG_SI_VIDEO_WIDTH,
#if SI_VIDEO_CFG_CAMERA_SENSOR
            .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
#else
            .src_type = JPEG_ENCODE_IN_FORMAT_YUV422,
#endif
            .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
            .image_quality = s_status.jpeg_quality,
        };

        uint32_t jpeg_size = 0;
        int64_t start_us = esp_timer_get_time();
        ret = jpeg_encoder_process(s_jpeg, &enc_cfg, s_encode_frame, s_raw_len,
                                   s_jpeg_work, s_jpeg_work_cap, &jpeg_size);
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        if (ret != ESP_OK) {
            set_error("encode JPEG", ret);
            ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(ret));
            continue;
        }
        if (jpeg_size > s_latest_jpeg_cap) {
            s_status.frames_dropped++;
            set_error("JPEG buffer too small", ESP_ERR_NO_MEM);
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_latest_jpeg, s_jpeg_work, jpeg_size);
        s_status.last_jpeg_size = jpeg_size;
        s_status.last_frame_ms = elapsed_ms;
        s_status.frames_encoded++;
        s_status.frame_ready = true;
        s_status.last_error[0] = '\0';
        update_fps(now_ms);
        xSemaphoreGive(s_lock);
    }
}

esp_err_t si_video_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.enabled = SI_VIDEO_CFG_ENABLE;
    s_status.width = CONFIG_SI_VIDEO_WIDTH;
    s_status.height = CONFIG_SI_VIDEO_HEIGHT;
    s_status.data_lanes = CONFIG_SI_VIDEO_DATA_LANES;
    s_status.lane_bitrate_mbps = CONFIG_SI_VIDEO_LANE_BITRATE_MBPS;
    s_status.jpeg_quality = CONFIG_SI_VIDEO_JPEG_QUALITY;
#if SI_VIDEO_CFG_CAMERA_SENSOR
    strlcpy(s_status.source, "mipi-csi-camera", sizeof(s_status.source));
    strlcpy(s_status.pixel_format, "RAW8/RGB565", sizeof(s_status.pixel_format));
#else
    strlcpy(s_status.source, "mipi-csi", sizeof(s_status.source));
    strlcpy(s_status.pixel_format, "YUV422", sizeof(s_status.pixel_format));
#endif

#if !SI_VIDEO_CFG_ENABLE
    strlcpy(s_status.last_error, "video disabled by config", sizeof(s_status.last_error));
    return ESP_ERR_NOT_SUPPORTED;
#else
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create video mutex");

    esp_err_t ret = init_csi();
    if (ret != ESP_OK) {
        set_error("init CSI/JPEG", ret);
        return ret;
    }

    s_status.initialized = true;
    BaseType_t ok = xTaskCreatePinnedToCore(capture_task, "si_video", 8192, NULL, 8, &s_capture_task, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create video task");
    return ESP_OK;
#endif
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
    (void)s_frame_irq_count;
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

    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_status.frame_ready || s_status.last_jpeg_size == 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *copy = malloc(s_status.last_jpeg_size);
    if (!copy) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
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
