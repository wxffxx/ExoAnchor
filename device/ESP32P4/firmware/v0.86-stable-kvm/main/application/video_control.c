#include "video_control.h"

#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings_store.h"
#include "time_utils.h"
#include "video_input.h"

#define VIDEO_SETTINGS_NAMESPACE "si_video_cfg"
#define VIDEO_SETTINGS_KVM_WIDTH_KEY "kvm_width"
#define VIDEO_SETTINGS_KVM_HEIGHT_KEY "kvm_height"
#define VIDEO_SETTINGS_KVM_FPS_X100_KEY "kvm_fps_x100"
#define VIDEO_SETTINGS_QUALITY_KEY "quality"
#define VIDEO_SETTINGS_ALWAYS_ON_KEY "always_on"
#define VIDEO_SETTINGS_PREVIEW_FPS_X100_KEY "preview_fps100"

typedef struct {
    bool enabled;
    bool exact_fps;
    uint32_t width;
    uint32_t height;
    uint32_t fps_x100;
    uint32_t stride_ms;
    char owner[16];
} video_control_request_t;

static const char *TAG = "si-video-ctl";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static bool s_preview_enabled;
static bool s_always_on;
static bool s_suspended;
static bool s_kvm_seen;
static uint32_t s_kvm_last_ms;
static bool s_agent_seen;
static uint32_t s_agent_last_ms;
static bool s_agent_takeover;
static bool s_settings_loaded;
static uint32_t s_kvm_width = SI_CFG_VIDEO_WIDTH;
static uint32_t s_kvm_height = SI_CFG_VIDEO_HEIGHT;
static uint32_t s_kvm_fps_x100 = SI_CFG_VIDEO_FPS * 100U;
static uint32_t s_preview_fps_x100 = SI_VIDEO_CONTROL_PREVIEW_FPS_X100;
static bool s_active_enabled;
static uint32_t s_active_width;
static uint32_t s_active_height;
static uint32_t s_active_fps_x100;
static uint32_t s_active_stride_ms;
static bool s_active_exact_fps;
static char s_active_owner[16] = "off";

static uint32_t preview_stride_ms(uint32_t fps_x100)
{
    return fps_x100 > 0 ? (100000U + fps_x100 - 1U) / fps_x100 : 0;
}

static bool kvm_active_locked(uint32_t now_ms)
{
    return s_kvm_seen &&
           (uint32_t)(now_ms - s_kvm_last_ms) <= SI_VIDEO_CONTROL_KVM_GRACE_MS;
}

static uint32_t kvm_remaining_locked(uint32_t now_ms)
{
    if (!kvm_active_locked(now_ms)) {
        return 0;
    }
    return SI_VIDEO_CONTROL_KVM_GRACE_MS - (uint32_t)(now_ms - s_kvm_last_ms);
}

static bool agent_active_locked(uint32_t now_ms)
{
    return s_agent_seen &&
           (uint32_t)(now_ms - s_agent_last_ms) <= SI_VIDEO_CONTROL_AGENT_GRACE_MS;
}

static uint32_t agent_remaining_locked(uint32_t now_ms)
{
    if (!agent_active_locked(now_ms)) {
        return 0;
    }
    return SI_VIDEO_CONTROL_AGENT_GRACE_MS - (uint32_t)(now_ms - s_agent_last_ms);
}

static bool agent_takeover_active_locked(uint32_t now_ms)
{
    return s_agent_takeover && agent_active_locked(now_ms);
}

static void desired_locked(video_control_request_t *request, uint32_t now_ms)
{
    memset(request, 0, sizeof(*request));
    strlcpy(request->owner, "off", sizeof(request->owner));

    if (s_suspended) {
        return;
    }
    if (agent_takeover_active_locked(now_ms)) {
        request->enabled = true;
        request->width = SI_VIDEO_CONTROL_PREVIEW_WIDTH;
        request->height = SI_VIDEO_CONTROL_PREVIEW_HEIGHT;
        request->fps_x100 = SI_VIDEO_CONTROL_PREVIEW_FPS_X100;
        request->stride_ms = SI_VIDEO_CONTROL_PREVIEW_STRIDE_MS;
        strlcpy(request->owner, "agent", sizeof(request->owner));
        return;
    }
    if (kvm_active_locked(now_ms)) {
        request->enabled = true;
        request->exact_fps = true;
        request->width = s_kvm_width;
        request->height = s_kvm_height;
        request->fps_x100 = s_kvm_fps_x100;
        strlcpy(request->owner, "kvm", sizeof(request->owner));
        return;
    }
    if (agent_active_locked(now_ms)) {
        request->enabled = true;
        request->width = SI_VIDEO_CONTROL_PREVIEW_WIDTH;
        request->height = SI_VIDEO_CONTROL_PREVIEW_HEIGHT;
        request->fps_x100 = SI_VIDEO_CONTROL_PREVIEW_FPS_X100;
        request->stride_ms = SI_VIDEO_CONTROL_PREVIEW_STRIDE_MS;
        strlcpy(request->owner, "agent", sizeof(request->owner));
        return;
    }
    if (s_preview_enabled) {
        request->enabled = true;
        request->width = SI_VIDEO_CONTROL_PREVIEW_WIDTH;
        request->height = SI_VIDEO_CONTROL_PREVIEW_HEIGHT;
        request->fps_x100 = s_preview_fps_x100;
        request->stride_ms = preview_stride_ms(s_preview_fps_x100);
        strlcpy(request->owner, "preview", sizeof(request->owner));
        return;
    }
    if (s_always_on) {
        request->enabled = true;
        request->width = SI_VIDEO_CONTROL_PREVIEW_WIDTH;
        request->height = SI_VIDEO_CONTROL_PREVIEW_HEIGHT;
        request->fps_x100 = SI_VIDEO_CONTROL_PREVIEW_FPS_X100;
        request->stride_ms = SI_VIDEO_CONTROL_PREVIEW_STRIDE_MS;
        strlcpy(request->owner, "always", sizeof(request->owner));
    }
}

static bool matches_active_locked(const video_control_request_t *request)
{
    if (request->enabled != s_active_enabled) {
        return false;
    }
    if (!request->enabled) {
        return strcmp(s_active_owner, "off") == 0;
    }
    return strcmp(request->owner, s_active_owner) == 0 &&
           request->width == s_active_width &&
           request->height == s_active_height &&
           request->fps_x100 == s_active_fps_x100 &&
           request->stride_ms == s_active_stride_ms &&
           request->exact_fps == s_active_exact_fps;
}

static void store_active_locked(const video_control_request_t *request)
{
    s_active_enabled = request->enabled;
    s_active_width = request->width;
    s_active_height = request->height;
    s_active_fps_x100 = request->fps_x100;
    s_active_stride_ms = request->stride_ms;
    s_active_exact_fps = request->exact_fps;
    strlcpy(s_active_owner, request->enabled ? request->owner : "off",
            sizeof(s_active_owner));
}

esp_err_t si_video_control_apply(void)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    video_control_request_t request;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    desired_locked(&request, si_monotonic_ms());
    bool unchanged = matches_active_locked(&request);
    xSemaphoreGive(s_lock);
    if (unchanged) {
        return ESP_OK;
    }

    esp_err_t ret = si_video_set_capture(request.enabled, request.owner,
                                         request.width, request.height,
                                         request.fps_x100, request.exact_fps,
                                         request.stride_ms);
    if (ret == ESP_OK && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        store_active_locked(&request);
        xSemaphoreGive(s_lock);
    }
    return ret;
}

bool si_video_control_touch_kvm(void)
{
    if (!s_lock) {
        return false;
    }
    bool accepted = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t now_ms = si_monotonic_ms();
        if (s_agent_takeover && !agent_active_locked(now_ms)) {
            s_agent_takeover = false;
        }
        if (!agent_takeover_active_locked(now_ms)) {
            s_kvm_seen = true;
            s_kvm_last_ms = now_ms;
            accepted = true;
        }
        xSemaphoreGive(s_lock);
    }
    return accepted;
}

void si_video_control_release_kvm(void)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_kvm_seen = false;
        xSemaphoreGive(s_lock);
    }
}

void si_video_control_touch_agent(bool takeover)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_agent_seen = true;
        s_agent_last_ms = si_monotonic_ms();
        s_agent_takeover = takeover;
        if (takeover) {
            s_kvm_seen = false;
        }
        xSemaphoreGive(s_lock);
    }
}

void si_video_control_keep_agent_alive(void)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_agent_seen = true;
        s_agent_last_ms = si_monotonic_ms();
        xSemaphoreGive(s_lock);
    }
}

void si_video_control_release_agent(void)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_agent_seen = false;
        s_agent_takeover = false;
        xSemaphoreGive(s_lock);
    }
}

bool si_video_control_set_kvm_mode(uint32_t width, uint32_t height, uint32_t fps_x100)
{
    if (!s_lock) {
        return false;
    }
    bool active = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_kvm_width = width;
        s_kvm_height = height;
        s_kvm_fps_x100 = fps_x100 > 0 ? fps_x100 : s_kvm_fps_x100;
        if (s_kvm_fps_x100 == 0) {
            s_kvm_fps_x100 = SI_CFG_VIDEO_FPS * 100U;
        }
        active = kvm_active_locked(si_monotonic_ms());
        xSemaphoreGive(s_lock);
    }
    return active;
}

esp_err_t si_video_control_get_kvm_mode(uint32_t *width, uint32_t *height,
                                        uint32_t *fps_x100)
{
    if (!width || !height || !fps_x100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *width = s_kvm_width;
    *height = s_kvm_height;
    *fps_x100 = s_kvm_fps_x100;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool si_video_control_mode_valid(uint32_t width, uint32_t height, uint32_t fps_x100)
{
    return width >= SI_VIDEO_CONTROL_WIDTH_MIN && width <= SI_VIDEO_CONTROL_WIDTH_MAX &&
           height >= SI_VIDEO_CONTROL_HEIGHT_MIN && height <= SI_VIDEO_CONTROL_HEIGHT_MAX &&
           fps_x100 > 0 && fps_x100 <= SI_VIDEO_CONTROL_FPS_X100_MAX;
}

esp_err_t si_video_control_save_kvm_mode(uint32_t width, uint32_t height,
                                         uint32_t fps_x100)
{
    if (!si_video_control_mode_valid(width, height, fps_x100)) {
        return ESP_ERR_INVALID_ARG;
    }
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store,
                                                      VIDEO_SETTINGS_NAMESPACE),
                        TAG, "open video settings");
    esp_err_t ret = si_settings_store_set_u32(&store,
                                              VIDEO_SETTINGS_KVM_WIDTH_KEY,
                                              width);
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u32(&store,
                                        VIDEO_SETTINGS_KVM_HEIGHT_KEY, height);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u32(&store,
                                        VIDEO_SETTINGS_KVM_FPS_X100_KEY,
                                        fps_x100);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}

esp_err_t si_video_control_save_quality(uint32_t quality)
{
    if (quality < 1 || quality > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store,
                                                      VIDEO_SETTINGS_NAMESPACE),
                        TAG, "open video settings");
    esp_err_t ret = si_settings_store_set_u32(&store,
                                              VIDEO_SETTINGS_QUALITY_KEY,
                                              quality);
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}

bool si_video_control_preview_fps_valid(uint32_t fps_x100)
{
    return fps_x100 >= SI_VIDEO_CONTROL_PREVIEW_FPS_X100_MIN &&
           fps_x100 <= SI_VIDEO_CONTROL_PREVIEW_FPS_X100_MAX &&
           fps_x100 % 100U == 0;
}

esp_err_t si_video_control_save_runtime_settings(bool always_on,
                                                  uint32_t preview_fps_x100)
{
    if (!si_video_control_preview_fps_valid(preview_fps_x100)) {
        return ESP_ERR_INVALID_ARG;
    }
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store,
                                                      VIDEO_SETTINGS_NAMESPACE),
                        TAG, "open video settings");
    esp_err_t ret = si_settings_store_set_u8(&store,
                                             VIDEO_SETTINGS_ALWAYS_ON_KEY,
                                             always_on ? 1 : 0);
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u32(&store,
                                        VIDEO_SETTINGS_PREVIEW_FPS_X100_KEY,
                                        preview_fps_x100);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}

esp_err_t si_video_control_set_always_on(bool enabled)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_always_on = enabled;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_video_control_set_preview_fps(uint32_t fps_x100)
{
    if (!si_video_control_preview_fps_valid(fps_x100)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_preview_fps_x100 = fps_x100;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void load_settings(void)
{
    if (s_settings_loaded) {
        return;
    }

    uint32_t width = SI_CFG_VIDEO_WIDTH;
    uint32_t height = SI_CFG_VIDEO_HEIGHT;
    uint32_t fps_x100 = SI_CFG_VIDEO_FPS * 100U;
    uint32_t preview_fps_x100 = SI_VIDEO_CONTROL_PREVIEW_FPS_X100;
    bool always_on = false;
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t err = si_settings_store_open_read(&store,
                                                 VIDEO_SETTINGS_NAMESPACE);
    if (err == ESP_OK) {
        uint32_t saved_width = 0;
        uint32_t saved_height = 0;
        uint32_t saved_fps_x100 = 0;
        bool have_width = si_settings_store_get_u32(
            &store, VIDEO_SETTINGS_KVM_WIDTH_KEY, &saved_width) == ESP_OK;
        bool have_height = si_settings_store_get_u32(
            &store, VIDEO_SETTINGS_KVM_HEIGHT_KEY, &saved_height) == ESP_OK;
        bool have_fps = si_settings_store_get_u32(
            &store, VIDEO_SETTINGS_KVM_FPS_X100_KEY,
            &saved_fps_x100) == ESP_OK;
        if (!have_fps) {
            saved_fps_x100 = fps_x100;
        }
        if (have_width && have_height &&
            si_video_control_mode_valid(saved_width, saved_height, saved_fps_x100)) {
            width = saved_width;
            height = saved_height;
            fps_x100 = saved_fps_x100;
        }

        uint32_t quality = 0;
        if (si_settings_store_get_u32(&store, VIDEO_SETTINGS_QUALITY_KEY,
                                      &quality) == ESP_OK &&
            quality >= 1 && quality <= 100) {
            (void)si_video_set_quality(quality);
        }
        uint8_t saved_always_on = 0;
        if (si_settings_store_get_u8(&store, VIDEO_SETTINGS_ALWAYS_ON_KEY,
                                     &saved_always_on) == ESP_OK) {
            always_on = saved_always_on != 0;
        }
        uint32_t saved_preview_fps_x100 = 0;
        if (si_settings_store_get_u32(&store,
                                      VIDEO_SETTINGS_PREVIEW_FPS_X100_KEY,
                                      &saved_preview_fps_x100) == ESP_OK &&
            si_video_control_preview_fps_valid(saved_preview_fps_x100)) {
            preview_fps_x100 = saved_preview_fps_x100;
        }
        si_settings_store_close(&store);
    }

    (void)si_video_control_set_kvm_mode(width, height, fps_x100);
    (void)si_video_control_set_always_on(always_on);
    (void)si_video_control_set_preview_fps(preview_fps_x100);
    s_settings_loaded = true;
}

esp_err_t si_video_control_set_preview(bool enabled)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_preview_enabled = enabled;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_video_control_set_suspended(bool suspended)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_suspended = suspended;
    xSemaphoreGive(s_lock);
    return si_video_control_apply();
}

void si_video_control_get_status(si_video_control_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    strlcpy(status->active_owner, "off", sizeof(status->active_owner));
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    uint32_t now_ms = si_monotonic_ms();
    status->preview_enabled = s_preview_enabled;
    status->always_on = s_always_on;
    status->suspended = s_suspended;
    status->kvm_active = kvm_active_locked(now_ms);
    status->agent_active = agent_active_locked(now_ms);
    status->agent_takeover = agent_takeover_active_locked(now_ms);
    status->kvm_remaining_ms = kvm_remaining_locked(now_ms);
    status->agent_remaining_ms = agent_remaining_locked(now_ms);
    status->kvm_width = s_kvm_width;
    status->kvm_height = s_kvm_height;
    status->kvm_fps_x100 = s_kvm_fps_x100;
    status->preview_fps_x100 = s_preview_fps_x100;
    status->preview_stride_ms = preview_stride_ms(s_preview_fps_x100);
    status->active_enabled = s_active_enabled;
    status->active_width = s_active_width;
    status->active_height = s_active_height;
    status->active_fps_x100 = s_active_fps_x100;
    status->active_stride_ms = s_active_stride_ms;
    strlcpy(status->active_owner, s_active_owner, sizeof(status->active_owner));
    xSemaphoreGive(s_lock);
}

static void control_task(void *arg)
{
    (void)arg;
    esp_err_t last_error = ESP_OK;
    while (true) {
        esp_err_t ret = si_video_control_apply();
        if (ret != ESP_OK && ret != last_error) {
            ESP_LOGW(TAG, "video control apply failed: %s", esp_err_to_name(ret));
        }
        last_error = ret;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t si_video_control_start(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create video control mutex");

    video_control_request_t off = {0};
    strlcpy(off.owner, "off", sizeof(off.owner));
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        store_active_locked(&off);
        xSemaphoreGive(s_lock);
    }

    BaseType_t ok = xTaskCreatePinnedToCore(control_task, "si_video_ctl", 3072, NULL,
                                            tskIDLE_PRIORITY + 2, &s_task,
                                            tskNO_AFFINITY);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create video control task");
    ESP_RETURN_ON_ERROR(si_video_control_apply(), TAG, "apply initial video state");
    load_settings();
    return si_video_control_apply();
}
