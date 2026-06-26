#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <sys/param.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hid_device.h"
#include "mbedtls/sha256.h"
#include "net_manager.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "video_input.h"

#ifndef CONFIG_SI_AUTH_USERNAME
#define CONFIG_SI_AUTH_USERNAME "admin"
#endif
#ifndef CONFIG_SI_VIDEO_WIDTH
#define CONFIG_SI_VIDEO_WIDTH 1280
#endif
#ifndef CONFIG_SI_VIDEO_HEIGHT
#define CONFIG_SI_VIDEO_HEIGHT 720
#endif
#ifndef CONFIG_SI_VIDEO_UVC_FPS
#define CONFIG_SI_VIDEO_UVC_FPS 30
#endif

static const char *TAG = "si-web";

extern const uint8_t www_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t www_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t www_kvm_html_start[] asm("_binary_kvm_html_start");
extern const uint8_t www_kvm_html_end[] asm("_binary_kvm_html_end");
extern const uint8_t www_settings_html_start[] asm("_binary_settings_html_start");
extern const uint8_t www_settings_html_end[] asm("_binary_settings_html_end");

typedef struct {
    char time[16];
    char level[12];
    char message[128];
} log_entry_t;

typedef struct {
    bool valid;
    double overall_percent;
    uint32_t core_count;
    uint32_t sample_window_ms;
} cpu_usage_sample_t;

#define MAX_LOGS 80
#define SCRATCH_BUFSIZE 1024
#define AUTH_NAMESPACE "si_auth"
#define AUTH_USERNAME_KEY "username"
#define AUTH_PASSWORD_HASH_KEY "password_hash"
#define AUTH_LOGIN_COUNT_KEY "login_count"
#define AUTH_HASH_HEX_LEN 64
#define AUTH_USERNAME_MAX_LEN 32
#define AUTH_PASSWORD_MAX_LEN 64
#define DEVICE_NAMESPACE "si_device"
#define DEVICE_LABEL_KEY "label"
#define DEVICE_LABEL_DEFAULT "ESP32-P4"
#define DEVICE_LABEL_MAX_LEN 48
#define HID_WS_MAX_FRAME 1024
#define VIDEO_KVM_GRACE_MS 10000U
#define VIDEO_PREVIEW_WIDTH 1280U
#define VIDEO_PREVIEW_HEIGHT 720U
#define VIDEO_PREVIEW_FPS_X100 100U
#define VIDEO_PREVIEW_STRIDE_MS 1000U
#define OTA_NAMESPACE "si_ota"
#define OTA_MANIFEST_URL_KEY "manifest_url"
#define OTA_CHANNEL_KEY "channel"
#define OTA_AUTO_CHECK_KEY "auto_check"
#define OTA_MANIFEST_URL_DEFAULT "https://github.com/wxffxx/simpleipmi/releases/latest/download/esp32p4-manifest.json"
#define OTA_CHANNEL_DEFAULT "stable"
#define OTA_URL_MAX_LEN 256
#define OTA_VERSION_MAX_LEN 32
#define OTA_CHANNEL_MAX_LEN 16
#define OTA_SHA256_HEX_LEN 64
#define OTA_NOTES_MAX_LEN 128
#define OTA_MANIFEST_MAX_SIZE 4096
#define OTA_IO_BUFFER_SIZE 4096

static httpd_handle_t s_server;
static httpd_handle_t s_stream_server;
static SemaphoreHandle_t s_video_control_lock;
static SemaphoreHandle_t s_ota_lock;
static TaskHandle_t s_video_control_task;
static log_entry_t s_logs[MAX_LOGS];
static size_t s_log_head;
static size_t s_log_count;
static char s_auth_username[AUTH_USERNAME_MAX_LEN + 1];
static char s_auth_hash[AUTH_HASH_HEX_LEN + 1];
static bool s_auth_loaded;
static bool s_auth_using_default;
static bool s_auth_login_count_loaded;
static uint32_t s_auth_login_count;
static char s_device_label[DEVICE_LABEL_MAX_LEN + 1];
static bool s_device_label_loaded;
static bool s_cpu_usage_valid;
static int64_t s_cpu_prev_wall_us;
static configRUN_TIME_COUNTER_TYPE s_cpu_prev_idle;
static bool s_preview_enabled;
static bool s_kvm_seen;
static uint32_t s_kvm_last_ms;
static uint32_t s_kvm_width = CONFIG_SI_VIDEO_WIDTH;
static uint32_t s_kvm_height = CONFIG_SI_VIDEO_HEIGHT;
static uint32_t s_kvm_fps_x100 = CONFIG_SI_VIDEO_UVC_FPS * 100U;
static bool s_video_active_enabled;
static uint32_t s_video_active_width;
static uint32_t s_video_active_height;
static uint32_t s_video_active_fps_x100;
static uint32_t s_video_active_stride_ms;
static bool s_video_active_exact_fps;
static char s_video_active_owner[16] = "off";
static bool s_ota_settings_loaded;
static char s_ota_manifest_url[OTA_URL_MAX_LEN + 1];
static char s_ota_channel[OTA_CHANNEL_MAX_LEN + 1];
static bool s_ota_auto_check;
static bool s_ota_busy;
static bool s_ota_last_checked;
static bool s_ota_last_update_available;
static char s_ota_last_version[OTA_VERSION_MAX_LEN + 1];
static char s_ota_last_url[OTA_URL_MAX_LEN + 1];
static char s_ota_last_sha256[OTA_SHA256_HEX_LEN + 1];
static char s_ota_last_notes[OTA_NOTES_MAX_LEN + 1];
static uint32_t s_ota_last_size;
static uint32_t s_ota_last_check_sec;
static char s_ota_last_message[128] = "not checked";

static uint32_t uptime_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void format_uptime(uint32_t sec, char *out, size_t out_size)
{
    uint32_t days = sec / 86400;
    uint32_t hours = (sec % 86400) / 3600;
    uint32_t minutes = (sec % 3600) / 60;
    snprintf(out, out_size, "%" PRIu32 "d %" PRIu32 "h %" PRIu32 "m", days, hours, minutes);
}

typedef struct {
    bool enabled;
    bool exact_fps;
    uint32_t width;
    uint32_t height;
    uint32_t fps_x100;
    uint32_t stride_ms;
    char owner[16];
} video_control_request_t;

static uint32_t video_control_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool video_control_kvm_active_locked(uint32_t now_ms)
{
    return s_kvm_seen && (uint32_t)(now_ms - s_kvm_last_ms) <= VIDEO_KVM_GRACE_MS;
}

static uint32_t video_control_kvm_remaining_locked(uint32_t now_ms)
{
    if (!video_control_kvm_active_locked(now_ms)) {
        return 0;
    }
    return VIDEO_KVM_GRACE_MS - (uint32_t)(now_ms - s_kvm_last_ms);
}

static void video_control_desired_locked(video_control_request_t *request, uint32_t now_ms)
{
    memset(request, 0, sizeof(*request));
    strlcpy(request->owner, "off", sizeof(request->owner));

    if (s_ota_busy) {
        return;
    }

    if (video_control_kvm_active_locked(now_ms)) {
        request->enabled = true;
        request->exact_fps = true;
        request->width = s_kvm_width;
        request->height = s_kvm_height;
        request->fps_x100 = s_kvm_fps_x100;
        strlcpy(request->owner, "kvm", sizeof(request->owner));
        return;
    }

    if (s_preview_enabled) {
        request->enabled = true;
        request->exact_fps = false;
        request->width = VIDEO_PREVIEW_WIDTH;
        request->height = VIDEO_PREVIEW_HEIGHT;
        request->fps_x100 = VIDEO_PREVIEW_FPS_X100;
        request->stride_ms = VIDEO_PREVIEW_STRIDE_MS;
        strlcpy(request->owner, "preview", sizeof(request->owner));
    }
}

static bool video_control_matches_active_locked(const video_control_request_t *request)
{
    if (request->enabled != s_video_active_enabled) {
        return false;
    }
    if (!request->enabled) {
        return strcmp(s_video_active_owner, "off") == 0;
    }
    return strcmp(request->owner, s_video_active_owner) == 0 &&
           request->width == s_video_active_width &&
           request->height == s_video_active_height &&
           request->fps_x100 == s_video_active_fps_x100 &&
           request->stride_ms == s_video_active_stride_ms &&
           request->exact_fps == s_video_active_exact_fps;
}

static void video_control_store_active_locked(const video_control_request_t *request)
{
    s_video_active_enabled = request->enabled;
    s_video_active_width = request->width;
    s_video_active_height = request->height;
    s_video_active_fps_x100 = request->fps_x100;
    s_video_active_stride_ms = request->stride_ms;
    s_video_active_exact_fps = request->exact_fps;
    strlcpy(s_video_active_owner, request->enabled ? request->owner : "off",
            sizeof(s_video_active_owner));
}

static esp_err_t video_control_apply_now(void)
{
    if (!s_video_control_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    video_control_request_t request;
    bool unchanged = false;
    if (xSemaphoreTake(s_video_control_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    video_control_desired_locked(&request, video_control_now_ms());
    unchanged = video_control_matches_active_locked(&request);
    xSemaphoreGive(s_video_control_lock);
    if (unchanged) {
        return ESP_OK;
    }

    esp_err_t ret = si_video_set_capture(request.enabled, request.owner, request.width, request.height,
                                         request.fps_x100, request.exact_fps, request.stride_ms);
    if (ret == ESP_OK &&
        xSemaphoreTake(s_video_control_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        video_control_store_active_locked(&request);
        xSemaphoreGive(s_video_control_lock);
    }
    return ret;
}

static void video_control_touch_kvm(void)
{
    if (!s_video_control_lock) {
        return;
    }
    if (xSemaphoreTake(s_video_control_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_kvm_seen = true;
        s_kvm_last_ms = video_control_now_ms();
        xSemaphoreGive(s_video_control_lock);
    }
}

static bool video_control_set_kvm_mode(uint32_t width, uint32_t height, uint32_t fps_x100)
{
    bool active = false;
    if (!s_video_control_lock) {
        return false;
    }
    if (xSemaphoreTake(s_video_control_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_kvm_width = width;
        s_kvm_height = height;
        s_kvm_fps_x100 = fps_x100 > 0 ? fps_x100 : s_kvm_fps_x100;
        if (s_kvm_fps_x100 == 0) {
            s_kvm_fps_x100 = CONFIG_SI_VIDEO_UVC_FPS * 100U;
        }
        active = video_control_kvm_active_locked(video_control_now_ms());
        xSemaphoreGive(s_video_control_lock);
    }
    return active;
}

static esp_err_t video_control_set_preview(bool enabled)
{
    if (!s_video_control_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_video_control_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_preview_enabled = enabled;
    xSemaphoreGive(s_video_control_lock);
    return video_control_apply_now();
}

static void video_control_task(void *arg)
{
    (void)arg;
    esp_err_t last_error = ESP_OK;
    while (true) {
        esp_err_t ret = video_control_apply_now();
        if (ret != ESP_OK && ret != last_error) {
            ESP_LOGW(TAG, "video control apply failed: %s", esp_err_to_name(ret));
        }
        last_error = ret;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t video_control_start(void)
{
    if (s_video_control_lock) {
        return ESP_OK;
    }
    s_video_control_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_video_control_lock, ESP_ERR_NO_MEM, TAG, "create video control mutex");

    video_control_request_t off = {0};
    strlcpy(off.owner, "off", sizeof(off.owner));
    if (xSemaphoreTake(s_video_control_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        video_control_store_active_locked(&off);
        xSemaphoreGive(s_video_control_lock);
    }

    BaseType_t ok = xTaskCreatePinnedToCore(video_control_task, "si_video_ctl", 3072, NULL,
                                            tskIDLE_PRIORITY + 2, &s_video_control_task,
                                            tskNO_AFFINITY);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create video control task");
    return video_control_apply_now();
}

void si_web_log(const char *level, const char *message)
{
    log_entry_t *entry = &s_logs[s_log_head];
    uint32_t sec = uptime_sec();
    snprintf(entry->time, sizeof(entry->time), "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
             sec / 3600, (sec % 3600) / 60, sec % 60);
    strlcpy(entry->level, level ? level : "INFO", sizeof(entry->level));
    strlcpy(entry->message, message ? message : "", sizeof(entry->message));

    s_log_head = (s_log_head + 1) % MAX_LOGS;
    if (s_log_count < MAX_LOGS) {
        s_log_count++;
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    if (!payload) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc failed");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, payload);
    free(payload);
    return ret;
}

static esp_err_t send_text_status(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message ? message : status);
}

static uint32_t fps_value_to_x100(double fps)
{
    return fps <= 0.0 ? 0 : (uint32_t)(fps * 100.0 + 0.5);
}

static bool secure_equal(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    unsigned char diff = (unsigned char)(alen ^ blen);
    size_t max_len = MAX(alen, blen);
    for (size_t i = 0; i < max_len; i++) {
        unsigned char ca = i < alen ? (unsigned char)a[i] : 0;
        unsigned char cb = i < blen ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

static esp_err_t auth_hash_password(const char *password, char out[AUTH_HASH_HEX_LEN + 1])
{
    if (!password) {
        return ESP_ERR_INVALID_ARG;
    }

    unsigned char digest[32];
    int ret = mbedtls_sha256((const unsigned char *)password, strlen(password), digest, 0);
    if (ret != 0) {
        return ESP_FAIL;
    }

    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(out + (i * 2), 3, "%02x", digest[i]);
    }
    out[AUTH_HASH_HEX_LEN] = '\0';
    return ESP_OK;
}

static const char *auth_default_username(void)
{
    return CONFIG_SI_AUTH_USERNAME[0] ? CONFIG_SI_AUTH_USERNAME : "admin";
}

static esp_err_t auth_validate_username(const char *username)
{
    if (!username) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(username);
    if (len < 1 || len > AUTH_USERNAME_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)username[i];
        if (ch < 33 || ch > 126) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static void auth_load_password(void)
{
    if (s_auth_loaded) {
        return;
    }

    strlcpy(s_auth_username, auth_default_username(), sizeof(s_auth_username));
    s_auth_hash[0] = '\0';
    s_auth_using_default = true;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AUTH_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        size_t hash_len = sizeof(s_auth_hash);
        err = nvs_get_str(nvs, AUTH_PASSWORD_HASH_KEY, s_auth_hash, &hash_len);
        char username[sizeof(s_auth_username)] = {0};
        size_t username_len = sizeof(username);
        esp_err_t username_err = nvs_get_str(nvs, AUTH_USERNAME_KEY, username, &username_len);
        nvs_close(nvs);
        if (err == ESP_OK && strlen(s_auth_hash) == AUTH_HASH_HEX_LEN) {
            if (username_err == ESP_OK && auth_validate_username(username) == ESP_OK) {
                strlcpy(s_auth_username, username, sizeof(s_auth_username));
            }
            s_auth_using_default = false;
            s_auth_loaded = true;
            return;
        }
    }

    if (CONFIG_SI_AUTH_PASSWORD[0] != '\0') {
        if (auth_hash_password(CONFIG_SI_AUTH_PASSWORD, s_auth_hash) != ESP_OK) {
            s_auth_hash[0] = '\0';
        }
    }
    s_auth_loaded = true;
}

static bool auth_is_enabled(void)
{
    auth_load_password();
    return s_auth_hash[0] != '\0';
}

static bool auth_password_matches(const char *password)
{
    char hash[AUTH_HASH_HEX_LEN + 1];
    if (!password || auth_hash_password(password, hash) != ESP_OK) {
        return false;
    }
    auth_load_password();
    return secure_equal(hash, s_auth_hash);
}

static bool auth_username_matches(const char *username)
{
    auth_load_password();
    return username && secure_equal(username, s_auth_username);
}

static bool auth_credentials_match(const char *username, const char *password)
{
    return auth_username_matches(username) && auth_password_matches(password);
}

static uint32_t auth_login_count(void)
{
    if (s_auth_login_count_loaded) {
        return s_auth_login_count;
    }
    nvs_handle_t nvs;
    if (nvs_open(AUTH_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        (void)nvs_get_u32(nvs, AUTH_LOGIN_COUNT_KEY, &s_auth_login_count);
        nvs_close(nvs);
    }
    s_auth_login_count_loaded = true;
    return s_auth_login_count;
}

static void auth_increment_login_count(void)
{
    uint32_t count = auth_login_count() + 1U;
    nvs_handle_t nvs;
    if (nvs_open(AUTH_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_u32(nvs, AUTH_LOGIN_COUNT_KEY, count) == ESP_OK &&
            nvs_commit(nvs) == ESP_OK) {
            s_auth_login_count = count;
        }
        nvs_close(nvs);
    } else {
        s_auth_login_count = count;
    }
}

static bool auth_token_matches(const char *token)
{
    if (!token || !auth_is_enabled()) {
        return false;
    }
    if (secure_equal(token, s_auth_hash)) {
        return true;
    }
    return false;
}

static bool check_auth(httpd_req_t *req)
{
    if (!auth_is_enabled()) {
        return true;
    }

    char auth[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) == ESP_OK) {
        const char *prefix = "Bearer ";
        if (strncmp(auth, prefix, strlen(prefix)) == 0 &&
            auth_token_matches(auth + strlen(prefix))) {
            return true;
        }
    }

    char query[192] = {0};
    char token[96] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "auth", token, sizeof(token)) == ESP_OK &&
        auth_token_matches(token)) {
        return true;
    }

    return false;
}

static esp_err_t require_auth(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    }
    return ESP_OK;
}

static esp_err_t auth_validate_new_password(const char *password)
{
    if (!password) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(password);
    if (len < 4 || len > AUTH_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)password[i];
        if (ch < 33 || ch > 126) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static esp_err_t auth_set_credentials(const char *username, const char *password)
{
    char hash[AUTH_HASH_HEX_LEN + 1];
    ESP_RETURN_ON_ERROR(auth_validate_username(username), TAG, "validate username");
    ESP_RETURN_ON_ERROR(auth_validate_new_password(password), TAG, "validate password");
    ESP_RETURN_ON_ERROR(auth_hash_password(password, hash), TAG, "hash password");

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(AUTH_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open auth NVS");
    esp_err_t ret = nvs_set_str(nvs, AUTH_USERNAME_KEY, username);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, AUTH_PASSWORD_HASH_KEY, hash);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(ret, TAG, "save auth NVS");

    strlcpy(s_auth_username, username, sizeof(s_auth_username));
    strlcpy(s_auth_hash, hash, sizeof(s_auth_hash));
    s_auth_loaded = true;
    s_auth_using_default = false;
    return ESP_OK;
}

static esp_err_t device_validate_label(const char *label)
{
    if (!label) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(label);
    if (len < 1 || len > DEVICE_LABEL_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)label[i];
        if (ch < 32 || ch == 127) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static void device_load_label(void)
{
    if (s_device_label_loaded) {
        return;
    }

    strlcpy(s_device_label, DEVICE_LABEL_DEFAULT, sizeof(s_device_label));
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DEVICE_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        char label[sizeof(s_device_label)] = {0};
        size_t label_len = sizeof(label);
        err = nvs_get_str(nvs, DEVICE_LABEL_KEY, label, &label_len);
        nvs_close(nvs);
        if (err == ESP_OK && device_validate_label(label) == ESP_OK) {
            strlcpy(s_device_label, label, sizeof(s_device_label));
        }
    }
    s_device_label_loaded = true;
}

static esp_err_t device_set_label(const char *label)
{
    ESP_RETURN_ON_ERROR(device_validate_label(label), TAG, "validate device label");

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(DEVICE_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open device NVS");
    esp_err_t ret = nvs_set_str(nvs, DEVICE_LABEL_KEY, label);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(ret, TAG, "save device label");

    strlcpy(s_device_label, label, sizeof(s_device_label));
    s_device_label_loaded = true;
    return ESP_OK;
}

static esp_err_t send_embedded_html(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return send_embedded_html(req, www_index_html_start, www_index_html_end);
}

static esp_err_t kvm_handler(httpd_req_t *req)
{
    return send_embedded_html(req, www_kvm_html_start, www_kvm_html_end);
}

static esp_err_t settings_handler(httpd_req_t *req)
{
    return send_embedded_html(req, www_settings_html_start, www_settings_html_end);
}

static esp_err_t recv_request_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (!req || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[received] = '\0';
    return ESP_OK;
}

static esp_err_t auth_login_handler(httpd_req_t *req)
{
    char buf[SCRATCH_BUFSIZE] = {0};
    esp_err_t body_ret = recv_request_body(req, buf, sizeof(buf));
    if (body_ret == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request body too large");
    }
    if (body_ret != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    bool ok = !auth_is_enabled();
    if (!ok) {
        cJSON *root = cJSON_Parse(buf);
        cJSON *username = root ? cJSON_GetObjectItemCaseSensitive(root, "username") : NULL;
        cJSON *password = root ? cJSON_GetObjectItemCaseSensitive(root, "password") : NULL;
        ok = cJSON_IsString(username) && cJSON_IsString(password) &&
             auth_credentials_match(username->valuestring, password->valuestring);
        cJSON_Delete(root);
    }

    if (!ok) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid credentials");
    }

    auth_load_password();
    auth_increment_login_count();
    cJSON_AddStringToObject(resp, "token", s_auth_hash);
    cJSON_AddStringToObject(resp, "type", "bearer");
    cJSON_AddNumberToObject(resp, "login_count", auth_login_count());
    cJSON_AddBoolToObject(resp, "enabled", auth_is_enabled());
    cJSON_AddStringToObject(resp, "username", s_auth_username);
    cJSON_AddBoolToObject(resp, "local_password", !s_auth_using_default);
    cJSON_AddBoolToObject(resp, "local_credentials", !s_auth_using_default);
    cJSON_AddBoolToObject(resp, "using_default", s_auth_using_default && auth_is_enabled());
    cJSON_AddBoolToObject(resp, "must_change_credentials", s_auth_using_default && auth_is_enabled());
    esp_err_t ret = send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t auth_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", auth_is_enabled());
    auth_load_password();
    cJSON_AddStringToObject(root, "username", s_auth_username);
    cJSON_AddStringToObject(root, "default_username", auth_default_username());
    cJSON_AddBoolToObject(root, "local_password", !s_auth_using_default);
    cJSON_AddBoolToObject(root, "local_credentials", !s_auth_using_default);
    cJSON_AddBoolToObject(root, "using_default", s_auth_using_default && auth_is_enabled());
    cJSON_AddBoolToObject(root, "must_change_credentials", s_auth_using_default && auth_is_enabled());
    cJSON_AddBoolToObject(root, "token_valid", check_auth(req));
    cJSON_AddNumberToObject(root, "login_count", auth_login_count());
    cJSON_AddNumberToObject(root, "username_max_length", AUTH_USERNAME_MAX_LEN);
    cJSON_AddNumberToObject(root, "min_length", 4);
    cJSON_AddNumberToObject(root, "max_length", AUTH_PASSWORD_MAX_LEN);
    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static const char *json_string_any(const cJSON *obj, const char *first, const char *second, const char *third)
{
    const char *names[] = {first, second, third};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!names[i]) {
            continue;
        }
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, names[i]);
        if (cJSON_IsString(item)) {
            return item->valuestring;
        }
    }
    return NULL;
}

static esp_err_t settings_password_handler(httpd_req_t *req)
{
    char buf[SCRATCH_BUFSIZE] = {0};
    esp_err_t body_ret = recv_request_body(req, buf, sizeof(buf));
    if (body_ret == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request body too large");
    }
    if (body_ret != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const char *current_username = json_string_any(root, "current_username", "currentUsername", "old_username");
    const char *current = json_string_any(root, "current_password", "currentPassword", "old_password");
    const char *username = json_string_any(root, "username", "new_username", "newUsername");
    const char *password = json_string_any(root, "new_password", "newPassword", "password");
    auth_load_password();
    if (!username) {
        username = s_auth_username;
    }
    bool authorized = check_auth(req) || !auth_is_enabled() ||
                      (current && auth_credentials_match(current_username, current));
    if (!authorized) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    }

    esp_err_t ret = auth_validate_username(username);
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "username must be 1..32 printable ASCII characters");
    }

    ret = auth_validate_new_password(password);
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "password must be 4..64 printable ASCII characters");
    }

    ret = auth_set_credentials(username, password);
    cJSON_Delete(root);
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
    }

    si_web_log("INFO", "Local web credentials updated");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "token", s_auth_hash);
    cJSON_AddStringToObject(resp, "type", "bearer");
    cJSON_AddStringToObject(resp, "username", s_auth_username);
    cJSON_AddBoolToObject(resp, "local_password", true);
    cJSON_AddBoolToObject(resp, "local_credentials", true);
    cJSON_AddBoolToObject(resp, "using_default", false);
    cJSON_AddBoolToObject(resp, "must_change_credentials", false);
    ret = send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t settings_device_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    device_load_label();

    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "label", s_device_label);
        cJSON_AddStringToObject(resp, "default_label", DEVICE_LABEL_DEFAULT);
        cJSON_AddNumberToObject(resp, "max_length", DEVICE_LABEL_MAX_LEN);
        esp_err_t ret = send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }

    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method not allowed");
    }
    char buf[SCRATCH_BUFSIZE] = {0};
    esp_err_t body_ret = recv_request_body(req, buf, sizeof(buf));
    if (body_ret == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request body too large");
    }
    if (body_ret != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    const char *label = root ? json_string_any(root, "label", "device_label", "name") : NULL;
    if (!label) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "label is required");
    }

    esp_err_t ret = device_set_label(label);
    cJSON_Delete(root);
    if (ret == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "label must be 1..48 bytes");
    }
    if (ret == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "label contains invalid characters");
    }
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
    }

    si_web_log("INFO", "Device label updated");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "label", s_device_label);
    cJSON_AddStringToObject(resp, "default_label", DEVICE_LABEL_DEFAULT);
    cJSON_AddNumberToObject(resp, "max_length", DEVICE_LABEL_MAX_LEN);
    ret = send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

typedef struct {
    char manifest_url[OTA_URL_MAX_LEN + 1];
    char channel[OTA_CHANNEL_MAX_LEN + 1];
    bool auto_check;
} ota_settings_t;

typedef struct {
    char version[OTA_VERSION_MAX_LEN + 1];
    char url[OTA_URL_MAX_LEN + 1];
    char sha256[OTA_SHA256_HEX_LEN + 1];
    char notes[OTA_NOTES_MAX_LEN + 1];
    uint32_t size;
    bool update_available;
} ota_manifest_t;

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    size_t max_bytes = (out_size - 1) / 2;
    size_t count = MIN(len, max_bytes);
    for (size_t i = 0; i < count; i++) {
        snprintf(out + (i * 2), 3, "%02x", bytes[i]);
    }
    out[count * 2] = '\0';
}

static bool ota_url_is_valid(const char *url)
{
    if (!url || url[0] == '\0' || strlen(url) > OTA_URL_MAX_LEN) {
        return false;
    }
    return strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0;
}

static bool ota_channel_is_valid(const char *channel)
{
    if (!channel) {
        return false;
    }
    size_t len = strlen(channel);
    if (len < 1 || len > OTA_CHANNEL_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)channel[i];
        if (!isalnum(ch) && ch != '-' && ch != '_') {
            return false;
        }
    }
    return true;
}

static void ota_settings_load(void)
{
    if (s_ota_settings_loaded) {
        return;
    }

    strlcpy(s_ota_manifest_url, OTA_MANIFEST_URL_DEFAULT, sizeof(s_ota_manifest_url));
    strlcpy(s_ota_channel, OTA_CHANNEL_DEFAULT, sizeof(s_ota_channel));
    s_ota_auto_check = false;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(OTA_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        char url[sizeof(s_ota_manifest_url)] = {0};
        size_t url_len = sizeof(url);
        if (nvs_get_str(nvs, OTA_MANIFEST_URL_KEY, url, &url_len) == ESP_OK &&
            ota_url_is_valid(url)) {
            strlcpy(s_ota_manifest_url, url, sizeof(s_ota_manifest_url));
        }

        char channel[sizeof(s_ota_channel)] = {0};
        size_t channel_len = sizeof(channel);
        if (nvs_get_str(nvs, OTA_CHANNEL_KEY, channel, &channel_len) == ESP_OK &&
            ota_channel_is_valid(channel)) {
            strlcpy(s_ota_channel, channel, sizeof(s_ota_channel));
        }

        uint8_t auto_check = 0;
        if (nvs_get_u8(nvs, OTA_AUTO_CHECK_KEY, &auto_check) == ESP_OK) {
            s_ota_auto_check = auto_check != 0;
        }
        nvs_close(nvs);
    }

    s_ota_settings_loaded = true;
}

static void ota_settings_snapshot(ota_settings_t *settings)
{
    ota_settings_load();
    strlcpy(settings->manifest_url, s_ota_manifest_url, sizeof(settings->manifest_url));
    strlcpy(settings->channel, s_ota_channel, sizeof(settings->channel));
    settings->auto_check = s_ota_auto_check;
}

static esp_err_t ota_settings_save(const char *manifest_url, const char *channel, bool auto_check)
{
    if (!manifest_url || manifest_url[0] == '\0') {
        manifest_url = OTA_MANIFEST_URL_DEFAULT;
    }
    if (!channel || channel[0] == '\0') {
        channel = OTA_CHANNEL_DEFAULT;
    }
    if (!ota_url_is_valid(manifest_url)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ota_channel_is_valid(channel)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(OTA_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open ota NVS");
    esp_err_t ret = nvs_set_str(nvs, OTA_MANIFEST_URL_KEY, manifest_url);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, OTA_CHANNEL_KEY, channel);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, OTA_AUTO_CHECK_KEY, auto_check ? 1 : 0);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(ret, TAG, "save ota settings");

    strlcpy(s_ota_manifest_url, manifest_url, sizeof(s_ota_manifest_url));
    strlcpy(s_ota_channel, channel, sizeof(s_ota_channel));
    s_ota_auto_check = auto_check;
    s_ota_settings_loaded = true;
    return ESP_OK;
}

static esp_err_t ota_require_auth(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    }
    auth_load_password();
    if (auth_is_enabled() && s_auth_using_default) {
        return send_text_status(req, "403 Forbidden", "change default credentials before OTA");
    }
    return ESP_OK;
}

static void ota_set_last_message(const char *message)
{
    strlcpy(s_ota_last_message, message ? message : "", sizeof(s_ota_last_message));
    s_ota_last_check_sec = uptime_sec();
}

static const char *json_string_any4(const cJSON *obj, const char *first, const char *second,
                                    const char *third, const char *fourth)
{
    const char *value = json_string_any(obj, first, second, third);
    if (value) {
        return value;
    }
    const cJSON *item = fourth ? cJSON_GetObjectItemCaseSensitive(obj, fourth) : NULL;
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static const cJSON *json_number_any(const cJSON *obj, const char *first, const char *second)
{
    const cJSON *item = first ? cJSON_GetObjectItemCaseSensitive(obj, first) : NULL;
    if (cJSON_IsNumber(item)) {
        return item;
    }
    item = second ? cJSON_GetObjectItemCaseSensitive(obj, second) : NULL;
    return cJSON_IsNumber(item) ? item : NULL;
}

static bool ota_board_matches(const char *board)
{
    if (!board || board[0] == '\0') {
        return true;
    }
    return strcasecmp(board, "esp32p4") == 0 ||
           strcasecmp(board, "esp32-p4") == 0 ||
           strcasecmp(board, "esphost-esp32p4") == 0;
}

static int version_compare(const char *a, const char *b)
{
    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    if (*a == 'v' || *a == 'V') {
        a++;
    }
    if (*b == 'v' || *b == 'V') {
        b++;
    }

    const char *pa = a;
    const char *pb = b;
    while (*pa || *pb) {
        while (*pa && !isalnum((unsigned char)*pa)) {
            pa++;
        }
        while (*pb && !isalnum((unsigned char)*pb)) {
            pb++;
        }
        if (isdigit((unsigned char)*pa) && isdigit((unsigned char)*pb)) {
            unsigned long va = strtoul(pa, (char **)&pa, 10);
            unsigned long vb = strtoul(pb, (char **)&pb, 10);
            if (va != vb) {
                return va > vb ? 1 : -1;
            }
            continue;
        }
        int ca = tolower((unsigned char)*pa);
        int cb = tolower((unsigned char)*pb);
        if (ca != cb) {
            return ca > cb ? 1 : -1;
        }
        if (*pa) {
            pa++;
        }
        if (*pb) {
            pb++;
        }
    }
    return 0;
}

static esp_err_t ota_parse_manifest(const char *json, const ota_settings_t *settings,
                                    ota_manifest_t *manifest, char *err, size_t err_size)
{
    memset(manifest, 0, sizeof(*manifest));
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        strlcpy(err, "invalid manifest json", err_size);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *firmware = cJSON_GetObjectItemCaseSensitive(root, "firmware");
    if (!cJSON_IsObject(firmware)) {
        firmware = root;
    }

    const char *board = json_string_any(firmware, "board", "target", "device");
    if (!board) {
        board = json_string_any(root, "board", "target", "device");
    }
    if (!ota_board_matches(board)) {
        strlcpy(err, "manifest board mismatch", err_size);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const char *channel = json_string_any(firmware, "channel", "track", "release_channel");
    if (!channel) {
        channel = json_string_any(root, "channel", "track", "release_channel");
    }
    if (channel && settings && settings->channel[0] &&
        strcasecmp(channel, settings->channel) != 0) {
        strlcpy(err, "manifest channel mismatch", err_size);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const char *version = json_string_any4(firmware, "version", "tag", "name", "firmware_version");
    const char *url = json_string_any4(firmware, "url", "bin_url", "firmware_url", "download_url");
    const char *sha = json_string_any4(firmware, "sha256", "sha256sum", "hash", "digest");
    const char *notes = json_string_any(firmware, "notes", "changelog", "description");
    const cJSON *size = json_number_any(firmware, "size", "bytes");

    if (!version || !version[0]) {
        strlcpy(err, "manifest version missing", err_size);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!url || !ota_url_is_valid(url)) {
        strlcpy(err, "manifest firmware url missing", err_size);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (sha && strlen(sha) != OTA_SHA256_HEX_LEN) {
        strlcpy(err, "manifest sha256 must be 64 hex chars", err_size);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(manifest->version, version, sizeof(manifest->version));
    strlcpy(manifest->url, url, sizeof(manifest->url));
    if (sha) {
        strlcpy(manifest->sha256, sha, sizeof(manifest->sha256));
    }
    if (notes) {
        strlcpy(manifest->notes, notes, sizeof(manifest->notes));
    }
    if (size && size->valuedouble > 0) {
        manifest->size = (uint32_t)size->valuedouble;
    }
    manifest->update_available = version_compare(manifest->version, SI_BMC_VERSION) > 0;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t ota_fetch_manifest(const ota_settings_t *settings, ota_manifest_t *manifest,
                                    char *err, size_t err_size)
{
    char *payload = calloc(1, OTA_MANIFEST_MAX_SIZE + 1);
    if (!payload) {
        strlcpy(err, "manifest buffer alloc failed", err_size);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = settings->manifest_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 12000,
        .buffer_size = 1024,
        .user_agent = "ExoAnchor-ESP32P4-OTA/" SI_BMC_VERSION,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(payload);
        strlcpy(err, "http client init failed", err_size);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        snprintf(err, err_size, "manifest open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        free(payload);
        return ret;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        snprintf(err, err_size, "manifest http status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(payload);
        return ESP_FAIL;
    }
    if (content_length > OTA_MANIFEST_MAX_SIZE) {
        strlcpy(err, "manifest too large", err_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(payload);
        return ESP_ERR_INVALID_SIZE;
    }

    int total = 0;
    while (total < OTA_MANIFEST_MAX_SIZE) {
        int read_len = esp_http_client_read(client, payload + total,
                                            OTA_MANIFEST_MAX_SIZE - total);
        if (read_len < 0) {
            strlcpy(err, "manifest read failed", err_size);
            ret = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            ret = ESP_OK;
            break;
        }
        total += read_len;
    }
    if (ret == ESP_OK && total >= OTA_MANIFEST_MAX_SIZE &&
        !esp_http_client_is_complete_data_received(client)) {
        strlcpy(err, "manifest too large", err_size);
        ret = ESP_ERR_INVALID_SIZE;
    }
    payload[MIN(total, OTA_MANIFEST_MAX_SIZE)] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ret == ESP_OK) {
        ret = ota_parse_manifest(payload, settings, manifest, err, err_size);
    }
    free(payload);
    return ret;
}

static void ota_store_last_manifest(const ota_manifest_t *manifest, const char *message)
{
    s_ota_last_checked = true;
    s_ota_last_update_available = manifest->update_available;
    strlcpy(s_ota_last_version, manifest->version, sizeof(s_ota_last_version));
    strlcpy(s_ota_last_url, manifest->url, sizeof(s_ota_last_url));
    strlcpy(s_ota_last_sha256, manifest->sha256, sizeof(s_ota_last_sha256));
    strlcpy(s_ota_last_notes, manifest->notes, sizeof(s_ota_last_notes));
    s_ota_last_size = manifest->size;
    ota_set_last_message(message);
}

static esp_err_t ota_begin_update(uint32_t expected_size, esp_ota_handle_t *handle,
                                  const esp_partition_t **partition)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_size > 0 && expected_size > next->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = esp_ota_begin(next, expected_size > 0 ? expected_size : OTA_SIZE_UNKNOWN, handle);
    if (ret != ESP_OK) {
        return ret;
    }
    *partition = next;
    return ESP_OK;
}

static esp_err_t ota_finish_update(esp_ota_handle_t handle, const esp_partition_t *partition)
{
    ESP_RETURN_ON_ERROR(esp_ota_end(handle), TAG, "end ota");
    ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(partition), TAG, "set ota boot partition");
    return ESP_OK;
}

static esp_err_t ota_apply_http_firmware(const ota_manifest_t *manifest,
                                         char sha_hex[OTA_SHA256_HEX_LEN + 1],
                                         uint32_t *written_out)
{
    uint8_t *buffer = malloc(OTA_IO_BUFFER_SIZE);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = manifest->url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 30000,
        .buffer_size = OTA_IO_BUFFER_SIZE,
        .user_agent = "ExoAnchor-ESP32P4-OTA/" SI_BMC_VERSION,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        free(buffer);
        return ret;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(buffer);
        return ESP_FAIL;
    }

    uint32_t expected_size = manifest->size;
    if (content_length > 0) {
        expected_size = (uint32_t)content_length;
    }

    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *partition = NULL;
    ret = ota_begin_update(expected_size, &ota_handle, &partition);
    if (ret != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(buffer);
        return ret;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    ret = mbedtls_sha256_starts(&sha, 0) == 0 ? ESP_OK : ESP_FAIL;

    uint32_t written = 0;
    while (ret == ESP_OK) {
        int read_len = esp_http_client_read(client, (char *)buffer, OTA_IO_BUFFER_SIZE);
        if (read_len < 0) {
            ret = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            break;
        }
        if ((uint32_t)read_len > partition->size - written) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        ret = esp_ota_write(ota_handle, buffer, read_len);
        if (ret != ESP_OK) {
            break;
        }
        if (mbedtls_sha256_update(&sha, buffer, read_len) != 0) {
            ret = ESP_FAIL;
            break;
        }
        written += (uint32_t)read_len;
    }

    if (ret == ESP_OK && content_length > 0 && written != (uint32_t)content_length) {
        ret = ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[32] = {0};
    if (ret == ESP_OK && mbedtls_sha256_finish(&sha, digest) != 0) {
        ret = ESP_FAIL;
    }
    mbedtls_sha256_free(&sha);
    bytes_to_hex(digest, sizeof(digest), sha_hex, OTA_SHA256_HEX_LEN + 1);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buffer);

    if (ret == ESP_OK && manifest->sha256[0] &&
        strcasecmp(sha_hex, manifest->sha256) != 0) {
        ret = ESP_ERR_INVALID_CRC;
    }

    if (ret == ESP_OK) {
        ret = ota_finish_update(ota_handle, partition);
    } else {
        (void)esp_ota_abort(ota_handle);
    }
    if (written_out) {
        *written_out = written;
    }
    return ret;
}

static esp_err_t ota_apply_uploaded_firmware(httpd_req_t *req,
                                             char sha_hex[OTA_SHA256_HEX_LEN + 1],
                                             uint32_t *written_out)
{
    if (req->content_len <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buffer = malloc(OTA_IO_BUFFER_SIZE);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *partition = NULL;
    esp_err_t ret = ota_begin_update((uint32_t)req->content_len, &ota_handle, &partition);
    if (ret != ESP_OK) {
        free(buffer);
        return ret;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    ret = mbedtls_sha256_starts(&sha, 0) == 0 ? ESP_OK : ESP_FAIL;

    uint32_t remaining = (uint32_t)req->content_len;
    uint32_t written = 0;
    while (ret == ESP_OK && remaining > 0) {
        size_t to_read = MIN((size_t)remaining, (size_t)OTA_IO_BUFFER_SIZE);
        int read_len = httpd_req_recv(req, (char *)buffer, to_read);
        if (read_len <= 0) {
            ret = ESP_FAIL;
            break;
        }
        ret = esp_ota_write(ota_handle, buffer, read_len);
        if (ret != ESP_OK) {
            break;
        }
        if (mbedtls_sha256_update(&sha, buffer, read_len) != 0) {
            ret = ESP_FAIL;
            break;
        }
        remaining -= (uint32_t)read_len;
        written += (uint32_t)read_len;
    }

    uint8_t digest[32] = {0};
    if (ret == ESP_OK && mbedtls_sha256_finish(&sha, digest) != 0) {
        ret = ESP_FAIL;
    }
    mbedtls_sha256_free(&sha);
    bytes_to_hex(digest, sizeof(digest), sha_hex, OTA_SHA256_HEX_LEN + 1);
    free(buffer);

    if (ret == ESP_OK) {
        ret = ota_finish_update(ota_handle, partition);
    } else {
        (void)esp_ota_abort(ota_handle);
    }
    if (written_out) {
        *written_out = written;
    }
    return ret;
}

static void add_ota_json(cJSON *root)
{
    ota_settings_t settings;
    ota_settings_snapshot(&settings);

    cJSON_AddStringToObject(root, "current_version", SI_BMC_VERSION);
    cJSON_AddBoolToObject(root, "busy", s_ota_busy);
    cJSON_AddBoolToObject(root, "auto_check_supported", false);

    cJSON *settings_json = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddStringToObject(settings_json, "manifest_url", settings.manifest_url);
    cJSON_AddStringToObject(settings_json, "default_manifest_url", OTA_MANIFEST_URL_DEFAULT);
    cJSON_AddStringToObject(settings_json, "channel", settings.channel);
    cJSON_AddBoolToObject(settings_json, "auto_check", settings.auto_check);

    cJSON *last = cJSON_AddObjectToObject(root, "last_check");
    cJSON_AddBoolToObject(last, "checked", s_ota_last_checked);
    cJSON_AddBoolToObject(last, "update_available", s_ota_last_update_available);
    cJSON_AddStringToObject(last, "version", s_ota_last_version);
    cJSON_AddStringToObject(last, "url", s_ota_last_url);
    cJSON_AddStringToObject(last, "sha256", s_ota_last_sha256);
    cJSON_AddStringToObject(last, "notes", s_ota_last_notes);
    cJSON_AddNumberToObject(last, "size", s_ota_last_size);
    cJSON_AddNumberToObject(last, "checked_uptime_seconds", s_ota_last_check_sec);
    cJSON_AddStringToObject(last, "message", s_ota_last_message);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    cJSON *partition = cJSON_AddObjectToObject(root, "partition");
    cJSON_AddStringToObject(partition, "running", running ? running->label : "--");
    cJSON_AddStringToObject(partition, "boot", boot ? boot->label : "--");
    cJSON_AddNumberToObject(partition, "running_address", running ? running->address : 0);
    cJSON_AddNumberToObject(partition, "boot_address", boot ? boot->address : 0);

    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "idf_app_version", desc ? desc->version : "");
}

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    cJSON *root = cJSON_CreateObject();
    add_ota_json(root);
    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t ota_settings_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }

    char buf[SCRATCH_BUFSIZE] = {0};
    esp_err_t body_ret = recv_request_body(req, buf, sizeof(buf));
    if (body_ret == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request body too large");
    }
    if (body_ret != ESP_OK) {
        return ESP_FAIL;
    }

    ota_settings_t current;
    ota_settings_snapshot(&current);
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const char *url = json_string_any(root, "manifest_url", "url", "manifest");
    const char *channel = json_string_any(root, "channel", "track", "release_channel");
    cJSON *auto_check = cJSON_GetObjectItemCaseSensitive(root, "auto_check");
    ret = ota_settings_save(url ? url : current.manifest_url,
                            channel ? channel : current.channel,
                            cJSON_IsBool(auto_check) ? cJSON_IsTrue(auto_check) : current.auto_check);
    cJSON_Delete(root);
    if (ret == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid OTA settings");
    }
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
    }

    si_web_log("INFO", "OTA settings updated");
    return ota_status_handler(req);
}

static esp_err_t ota_check_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    if (xSemaphoreTake(s_ota_lock, 0) != pdTRUE) {
        return send_text_status(req, "409 Conflict", "OTA operation already running");
    }

    ota_settings_t settings;
    ota_settings_snapshot(&settings);
    ota_manifest_t manifest;
    char err[128] = {0};
    ret = ota_fetch_manifest(&settings, &manifest, err, sizeof(err));
    if (ret == ESP_OK) {
        ota_store_last_manifest(&manifest, manifest.update_available ? "update available" : "already latest");
        si_web_log("INFO", manifest.update_available ? "OTA update available" : "OTA already latest");
    } else {
        ota_set_last_message(err);
        si_web_log("WARN", "OTA manifest check failed");
    }
    xSemaphoreGive(s_ota_lock);

    if (ret != ESP_OK) {
        return send_text_status(req, "502 Bad Gateway", err[0] ? err : esp_err_to_name(ret));
    }
    return ota_status_handler(req);
}

static esp_err_t ota_install_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    bool force = false;
    if (req->content_len > 0 && req->content_len < 128) {
        char buf[128] = {0};
        if (recv_request_body(req, buf, sizeof(buf)) == ESP_OK) {
            cJSON *root = cJSON_Parse(buf);
            cJSON *force_json = root ? cJSON_GetObjectItemCaseSensitive(root, "force") : NULL;
            force = cJSON_IsTrue(force_json);
            cJSON_Delete(root);
        }
    }

    if (xSemaphoreTake(s_ota_lock, 0) != pdTRUE) {
        return send_text_status(req, "409 Conflict", "OTA operation already running");
    }
    s_ota_busy = true;
    (void)video_control_apply_now();

    ota_settings_t settings;
    ota_settings_snapshot(&settings);
    ota_manifest_t manifest;
    char err[128] = {0};
    ret = ota_fetch_manifest(&settings, &manifest, err, sizeof(err));
    if (ret == ESP_OK) {
        ota_store_last_manifest(&manifest, manifest.update_available ? "update available" : "already latest");
    }
    if (ret == ESP_OK && !manifest.update_available && !force) {
        ret = ESP_ERR_INVALID_STATE;
        strlcpy(err, "already latest", sizeof(err));
    }

    char sha_hex[OTA_SHA256_HEX_LEN + 1] = {0};
    uint32_t written = 0;
    if (ret == ESP_OK) {
        si_web_log("INFO", "OTA firmware download started");
        ret = ota_apply_http_firmware(&manifest, sha_hex, &written);
        if (ret == ESP_OK) {
            ota_set_last_message("firmware installed, reboot required");
            si_web_log("INFO", "OTA firmware installed");
        } else {
            snprintf(err, sizeof(err), "firmware install failed: %s", esp_err_to_name(ret));
            ota_set_last_message(err);
            si_web_log("WARN", "OTA firmware install failed");
        }
    }

    s_ota_busy = false;
    (void)video_control_apply_now();
    xSemaphoreGive(s_ota_lock);

    if (ret != ESP_OK) {
        return send_text_status(req, ret == ESP_ERR_INVALID_STATE ? "409 Conflict" : "500 Internal Server Error",
                                err[0] ? err : esp_err_to_name(ret));
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "version", manifest.version);
    cJSON_AddStringToObject(resp, "sha256", sha_hex);
    cJSON_AddNumberToObject(resp, "size", written);
    cJSON_AddBoolToObject(resp, "reboot_required", true);
    ret = send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    if (xSemaphoreTake(s_ota_lock, 0) != pdTRUE) {
        return send_text_status(req, "409 Conflict", "OTA operation already running");
    }
    s_ota_busy = true;
    (void)video_control_apply_now();

    char sha_hex[OTA_SHA256_HEX_LEN + 1] = {0};
    uint32_t written = 0;
    si_web_log("INFO", "OTA upload started");
    ret = ota_apply_uploaded_firmware(req, sha_hex, &written);
    if (ret == ESP_OK) {
        ota_set_last_message("uploaded firmware installed, reboot required");
        si_web_log("INFO", "OTA upload installed");
    } else {
        char err[96];
        snprintf(err, sizeof(err), "OTA upload failed: %s", esp_err_to_name(ret));
        ota_set_last_message(err);
        si_web_log("WARN", "OTA upload failed");
    }

    s_ota_busy = false;
    (void)video_control_apply_now();
    xSemaphoreGive(s_ota_lock);

    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(ret));
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "sha256", sha_hex);
    cJSON_AddNumberToObject(resp, "size", written);
    cJSON_AddBoolToObject(resp, "reboot_required", true);
    ret = send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static void ota_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(650));
    esp_restart();
}

static esp_err_t ota_reboot_handler(httpd_req_t *req)
{
    esp_err_t ret = ota_require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "rebooting");
    ret = send_json(req, resp);
    cJSON_Delete(resp);
    (void)xTaskCreate(ota_restart_task, "si_ota_reboot", 2048, NULL, tskIDLE_PRIORITY + 3, NULL);
    return ret;
}

static esp_err_t system_info_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);

    si_net_status_t net;
    si_net_get_status(&net);
    char uptime[32];
    format_uptime(uptime_sec(), uptime, sizeof(uptime));

    cJSON *root = cJSON_CreateObject();
    cJSON *cpu = cJSON_AddObjectToObject(root, "cpu");
    cJSON_AddNumberToObject(cpu, "usage_percent", 0);
    cJSON_AddNumberToObject(cpu, "cores", chip_info.cores);
    cJSON_AddNumberToObject(cpu, "freq_mhz", 0);

    cJSON *memory = cJSON_AddObjectToObject(root, "memory");
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    cJSON_AddNumberToObject(memory, "total_mb", 0);
    cJSON_AddNumberToObject(memory, "used_mb", 0);
    cJSON_AddNumberToObject(memory, "available_mb", free_heap / 1024 / 1024);
    cJSON_AddNumberToObject(memory, "free_bytes", free_heap);
    cJSON_AddNumberToObject(memory, "minimum_free_bytes", min_heap);
    cJSON_AddNumberToObject(memory, "usage_percent", 0);

    cJSON *temp = cJSON_AddObjectToObject(root, "temperature");
    cJSON_AddNumberToObject(temp, "celsius", 0);
    cJSON_AddStringToObject(temp, "source", "unavailable");

    cJSON *disk = cJSON_AddObjectToObject(root, "disk");
    cJSON_AddNumberToObject(disk, "total_gb", flash_size / 1024.0 / 1024.0 / 1024.0);
    cJSON_AddNumberToObject(disk, "used_gb", 0);
    cJSON_AddNumberToObject(disk, "free_gb", 0);
    cJSON_AddNumberToObject(disk, "usage_percent", 0);

    cJSON *network = cJSON_AddObjectToObject(root, "network");
    cJSON *eth = cJSON_AddObjectToObject(network, "ethernet");
    cJSON_AddBoolToObject(eth, "up", net.connected);
    cJSON_AddBoolToObject(eth, "link_up", net.link_up);
    cJSON_AddStringToObject(eth, "phy", net.phy);
    cJSON_AddNumberToObject(eth, "speed_mbps", net.speed_mbps);
    cJSON_AddBoolToObject(eth, "full_duplex", net.full_duplex);
    cJSON_AddStringToObject(eth, "ipv4", net.ip[0] ? net.ip : "No IP");
    cJSON_AddStringToObject(eth, "netmask", net.netmask);
    cJSON_AddStringToObject(eth, "gateway", net.gateway);
    cJSON_AddStringToObject(eth, "mac", net.mac);

    cJSON *uptime_obj = cJSON_AddObjectToObject(root, "uptime");
    cJSON_AddNumberToObject(uptime_obj, "seconds", uptime_sec());
    cJSON_AddStringToObject(uptime_obj, "formatted", uptime);

    cJSON *auth = cJSON_AddObjectToObject(root, "auth");
    cJSON_AddNumberToObject(auth, "login_count", auth_login_count());

    cJSON *load = cJSON_AddObjectToObject(root, "load");
    cJSON_AddNumberToObject(load, "1min", 0);
    cJSON_AddNumberToObject(load, "5min", 0);
    cJSON_AddNumberToObject(load, "15min", 0);
    device_load_label();
    cJSON_AddStringToObject(root, "device_label", s_device_label);
    cJSON_AddStringToObject(root, "label", s_device_label);
    cJSON_AddStringToObject(root, "hostname", SI_BMC_HOSTNAME);

    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t logs_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    int count = 50;
    char query[64] = {0};
    char nbuf[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "n", nbuf, sizeof(nbuf)) == ESP_OK) {
        count = atoi(nbuf);
        if (count < 1) {
            count = 1;
        } else if (count > MAX_LOGS) {
            count = MAX_LOGS;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *logs = cJSON_AddArrayToObject(root, "logs");
    size_t total = MIN((size_t)count, s_log_count);
    size_t start = (s_log_head + MAX_LOGS - total) % MAX_LOGS;
    for (size_t i = 0; i < total; i++) {
        size_t idx = (start + i) % MAX_LOGS;
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "time", s_logs[idx].time);
        cJSON_AddStringToObject(entry, "level", s_logs[idx].level);
        cJSON_AddStringToObject(entry, "message", s_logs[idx].message);
        cJSON_AddItemToArray(logs, entry);
    }
    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static void add_video_control_json(cJSON *root)
{
    bool preview_enabled = false;
    bool kvm_active = false;
    bool active_enabled = false;
    uint32_t kvm_remaining_ms = 0;
    uint32_t kvm_width = CONFIG_SI_VIDEO_WIDTH;
    uint32_t kvm_height = CONFIG_SI_VIDEO_HEIGHT;
    uint32_t kvm_fps_x100 = CONFIG_SI_VIDEO_UVC_FPS * 100U;
    uint32_t active_width = 0;
    uint32_t active_height = 0;
    uint32_t active_fps_x100 = 0;
    uint32_t active_stride_ms = 0;
    char active_owner[16] = "off";

    if (s_video_control_lock &&
        xSemaphoreTake(s_video_control_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t now_ms = video_control_now_ms();
        preview_enabled = s_preview_enabled;
        kvm_active = video_control_kvm_active_locked(now_ms);
        kvm_remaining_ms = video_control_kvm_remaining_locked(now_ms);
        kvm_width = s_kvm_width;
        kvm_height = s_kvm_height;
        kvm_fps_x100 = s_kvm_fps_x100;
        active_enabled = s_video_active_enabled;
        active_width = s_video_active_width;
        active_height = s_video_active_height;
        active_fps_x100 = s_video_active_fps_x100;
        active_stride_ms = s_video_active_stride_ms;
        strlcpy(active_owner, s_video_active_owner, sizeof(active_owner));
        xSemaphoreGive(s_video_control_lock);
    }

    cJSON *control = cJSON_AddObjectToObject(root, "control");
    cJSON_AddBoolToObject(control, "preview_enabled", preview_enabled);
    cJSON_AddBoolToObject(control, "kvm_active", kvm_active);
    cJSON_AddNumberToObject(control, "kvm_grace_ms", VIDEO_KVM_GRACE_MS);
    cJSON_AddNumberToObject(control, "kvm_grace_remaining_ms", kvm_remaining_ms);
    cJSON_AddBoolToObject(control, "active_enabled", active_enabled);
    cJSON_AddStringToObject(control, "active_owner", active_owner);

    cJSON *preview = cJSON_AddObjectToObject(control, "preview_mode");
    cJSON_AddNumberToObject(preview, "width", VIDEO_PREVIEW_WIDTH);
    cJSON_AddNumberToObject(preview, "height", VIDEO_PREVIEW_HEIGHT);
    cJSON_AddNumberToObject(preview, "fps", VIDEO_PREVIEW_FPS_X100 / 100.0);
    cJSON_AddNumberToObject(preview, "fps_x100", VIDEO_PREVIEW_FPS_X100);
    cJSON_AddNumberToObject(preview, "stride_ms", VIDEO_PREVIEW_STRIDE_MS);

    cJSON *kvm = cJSON_AddObjectToObject(control, "kvm_mode");
    cJSON_AddNumberToObject(kvm, "width", kvm_width);
    cJSON_AddNumberToObject(kvm, "height", kvm_height);
    cJSON_AddNumberToObject(kvm, "fps", kvm_fps_x100 / 100.0);
    cJSON_AddNumberToObject(kvm, "fps_x100", kvm_fps_x100);

    cJSON *active = cJSON_AddObjectToObject(control, "active_mode");
    cJSON_AddNumberToObject(active, "width", active_width);
    cJSON_AddNumberToObject(active, "height", active_height);
    cJSON_AddNumberToObject(active, "fps", active_fps_x100 / 100.0);
    cJSON_AddNumberToObject(active, "fps_x100", active_fps_x100);
    cJSON_AddNumberToObject(active, "stride_ms", active_stride_ms);
}

static void add_video_json(cJSON *root)
{
    si_video_status_t st;
    si_video_get_status(&st);
    si_video_mode_t *modes = calloc(SI_VIDEO_MAX_MODES, sizeof(si_video_mode_t));
    size_t mode_count = 0;
    if (modes) {
        (void)si_video_get_modes(modes, SI_VIDEO_MAX_MODES, &mode_count);
    }

    cJSON_AddBoolToObject(root, "enabled", st.enabled);
    cJSON_AddBoolToObject(root, "connected", st.frame_ready);
    cJSON_AddBoolToObject(root, "initialized", st.initialized);
    cJSON_AddBoolToObject(root, "streaming", st.streaming);
    cJSON_AddBoolToObject(root, "capture_enabled", st.capture_enabled);
    cJSON_AddNumberToObject(root, "capture_stride_ms", st.capture_stride_ms);
    cJSON_AddStringToObject(root, "capture_owner", st.capture_owner);
    cJSON_AddStringToObject(root, "source", st.source);
    cJSON_AddStringToObject(root, "pixel_format", st.pixel_format);
    cJSON_AddNumberToObject(root, "width", st.width);
    cJSON_AddNumberToObject(root, "height", st.height);
    char resolution[24];
    snprintf(resolution, sizeof(resolution), "%" PRIu32 "x%" PRIu32, st.width, st.height);
    cJSON_AddStringToObject(root, "resolution", resolution);
    cJSON_AddNumberToObject(root, "target_width", st.target_width);
    cJSON_AddNumberToObject(root, "target_height", st.target_height);
    cJSON_AddNumberToObject(root, "target_fps", st.target_fps_x100 / 100.0);
    cJSON_AddNumberToObject(root, "target_fps_x100", st.target_fps_x100);
    char target_resolution[24];
    snprintf(target_resolution, sizeof(target_resolution), "%" PRIu32 "x%" PRIu32,
             st.target_width, st.target_height);
    cJSON_AddStringToObject(root, "target_resolution", target_resolution);
    cJSON_AddNumberToObject(root, "modes_count", st.modes_count);
    cJSON_AddNumberToObject(root, "data_lanes", st.data_lanes);
    cJSON_AddNumberToObject(root, "lane_bitrate_mbps", st.lane_bitrate_mbps);
    cJSON_AddNumberToObject(root, "quality", st.jpeg_quality);
    cJSON_AddNumberToObject(root, "frames_captured", st.frames_captured);
    cJSON_AddNumberToObject(root, "frames_encoded", st.frames_encoded);
    cJSON_AddNumberToObject(root, "frames_dropped", st.frames_dropped);
    cJSON_AddNumberToObject(root, "last_jpeg_size", st.last_jpeg_size);
    cJSON_AddNumberToObject(root, "last_frame_ms", st.last_frame_ms);
    cJSON_AddNumberToObject(root, "fps", st.fps_x100 / 100.0);
    cJSON_AddStringToObject(root, "last_error", st.last_error);
    add_video_control_json(root);

    cJSON *modes_json = cJSON_AddArrayToObject(root, "modes");
    size_t copy_count = modes && mode_count < SI_VIDEO_MAX_MODES ? mode_count : SI_VIDEO_MAX_MODES;
    for (size_t i = 0; modes && i < copy_count; i++) {
        cJSON *mode = cJSON_CreateObject();
        cJSON_AddStringToObject(mode, "pixel_format", modes[i].pixel_format);
        cJSON_AddNumberToObject(mode, "width", modes[i].width);
        cJSON_AddNumberToObject(mode, "height", modes[i].height);
        char mode_resolution[24];
        snprintf(mode_resolution, sizeof(mode_resolution), "%" PRIu32 "x%" PRIu32,
                 modes[i].width, modes[i].height);
        cJSON_AddStringToObject(mode, "resolution", mode_resolution);
        cJSON_AddNumberToObject(mode, "fps", modes[i].fps_x100 / 100.0);
        cJSON_AddNumberToObject(mode, "fps_x100", modes[i].fps_x100);
        cJSON_AddBoolToObject(mode, "selected", modes[i].selected);
        cJSON_AddItemToArray(modes_json, mode);
    }
    free(modes);
}

static esp_err_t video_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    cJSON *root = cJSON_CreateObject();
    add_video_json(root);
    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static void add_hid_json(cJSON *root)
{
    si_hid_status_t st;
    si_hid_get_status(&st);

    cJSON_AddBoolToObject(root, "enabled", st.enabled);
    cJSON_AddBoolToObject(root, "initialized", st.initialized);
    cJSON_AddBoolToObject(root, "connected", st.mounted);
    cJSON_AddBoolToObject(root, "mounted", st.mounted);
    cJSON_AddBoolToObject(root, "ready", st.ready);
    cJSON_AddBoolToObject(root, "hid_ready", st.ready);
    cJSON_AddStringToObject(root, "mode", st.mode);
    cJSON_AddStringToObject(root, "port", st.port);
    cJSON_AddStringToObject(root, "transport", st.transport);

    cJSON *pins = cJSON_AddObjectToObject(root, "pins");
    cJSON_AddNumberToObject(pins, "dm_gpio", st.dm_gpio);
    cJSON_AddNumberToObject(pins, "dp_gpio", st.dp_gpio);

    cJSON *keyboard = cJSON_AddObjectToObject(root, "keyboard");
    cJSON_AddBoolToObject(keyboard, "available", st.ready);
    cJSON *mouse = cJSON_AddObjectToObject(root, "mouse");
    cJSON_AddBoolToObject(mouse, "available", st.ready);

    cJSON *stats = cJSON_AddObjectToObject(root, "stats");
    cJSON_AddNumberToObject(stats, "tx_messages", st.tx_messages);
    cJSON_AddNumberToObject(stats, "failed_messages", st.failed_messages);
    cJSON_AddStringToObject(root, "last_error", st.last_error);
}

static esp_err_t hid_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    cJSON *root = cJSON_CreateObject();
    add_hid_json(root);
    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t video_quality_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    }

    int quality = -1;
    char query[64] = {0};
    char qbuf[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "quality", qbuf, sizeof(qbuf)) == ESP_OK) {
        quality = atoi(qbuf);
    } else if (req->content_len > 0) {
        char buf[128] = {0};
        int len = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
        if (len < 0) {
            return ESP_FAIL;
        }
        cJSON *root = cJSON_Parse(buf);
        cJSON *q = root ? cJSON_GetObjectItemCaseSensitive(root, "quality") : NULL;
        if (cJSON_IsNumber(q)) {
            quality = q->valueint;
        }
        cJSON_Delete(root);
    }

    if (quality < 1 || quality > 100) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "quality must be 1..100");
    }

    ESP_RETURN_ON_ERROR(si_video_set_quality((uint32_t)quality), TAG, "set video quality");
    si_web_log("INFO", "Video JPEG quality updated");
    return video_status_handler(req);
}

static esp_err_t video_resolution_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    }

    int width = -1;
    int height = -1;
    uint32_t fps_x100 = 0;
    char query[128] = {0};
    char wbuf[12] = {0};
    char hbuf[12] = {0};
    char fbuf[16] = {0};
    char pfbuf[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "width", wbuf, sizeof(wbuf)) == ESP_OK &&
        httpd_query_key_value(query, "height", hbuf, sizeof(hbuf)) == ESP_OK) {
        width = atoi(wbuf);
        height = atoi(hbuf);
        if (httpd_query_key_value(query, "fps", fbuf, sizeof(fbuf)) == ESP_OK) {
            fps_x100 = fps_value_to_x100(atof(fbuf));
        }
        (void)httpd_query_key_value(query, "pixel_format", pfbuf, sizeof(pfbuf));
    } else if (req->content_len > 0) {
        char buf[192] = {0};
        int len = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
        if (len < 0) {
            return ESP_FAIL;
        }
        cJSON *root = cJSON_Parse(buf);
        cJSON *w = root ? cJSON_GetObjectItemCaseSensitive(root, "width") : NULL;
        cJSON *h = root ? cJSON_GetObjectItemCaseSensitive(root, "height") : NULL;
        cJSON *fps = root ? cJSON_GetObjectItemCaseSensitive(root, "fps") : NULL;
        cJSON *fps_raw = root ? cJSON_GetObjectItemCaseSensitive(root, "fps_x100") : NULL;
        cJSON *pixel_format = root ? cJSON_GetObjectItemCaseSensitive(root, "pixel_format") : NULL;
        if (cJSON_IsNumber(w) && cJSON_IsNumber(h)) {
            width = w->valueint;
            height = h->valueint;
        }
        if (cJSON_IsNumber(fps_raw)) {
            fps_x100 = (uint32_t)fps_raw->valueint;
        } else if (cJSON_IsNumber(fps)) {
            fps_x100 = fps_value_to_x100(fps->valuedouble);
        }
        if (cJSON_IsString(pixel_format) && pixel_format->valuestring) {
            strlcpy(pfbuf, pixel_format->valuestring, sizeof(pfbuf));
        }
        cJSON_Delete(root);
    }

    if (width < 160 || height < 120) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "width/height required");
    }
    if (pfbuf[0] != '\0' && strcasecmp(pfbuf, "MJPEG") != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "only MJPEG video modes are supported");
    }

    bool kvm_active = video_control_set_kvm_mode((uint32_t)width, (uint32_t)height, fps_x100);
    if (kvm_active) {
        esp_err_t ret = video_control_apply_now();
        if (ret == ESP_ERR_NOT_FOUND) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "video mode is not available");
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "set video mode");
    }

    char msg[96];
    if (fps_x100 > 0) {
        snprintf(msg, sizeof(msg), "KVM video mode saved as %dx%d @ %.2ffps",
                 width, height, fps_x100 / 100.0);
    } else {
        snprintf(msg, sizeof(msg), "KVM video resolution saved as %dx%d", width, height);
    }
    si_web_log("INFO", msg);
    return video_status_handler(req);
}

static esp_err_t video_lease_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    }
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char buf[192] = {0};
    int len = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (len < 0) {
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(buf);
    cJSON *owner = root ? cJSON_GetObjectItemCaseSensitive(root, "owner") : NULL;
    if (!cJSON_IsString(owner) || !owner->valuestring) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "owner required");
    }

    esp_err_t ret = ESP_OK;
    if (strcasecmp(owner->valuestring, "preview") == 0) {
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        if (!cJSON_IsBool(enabled)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled required");
        }
        bool on = cJSON_IsTrue(enabled);
        ret = video_control_set_preview(on);
        si_web_log("INFO", on ? "Dashboard preview enabled" : "Dashboard preview disabled");
    } else if (strcasecmp(owner->valuestring, "kvm") == 0) {
        cJSON *active = cJSON_GetObjectItemCaseSensitive(root, "active");
        bool on = !cJSON_IsBool(active) || cJSON_IsTrue(active);
        video_control_touch_kvm();
        if (on) {
            ret = video_control_apply_now();
        }
    } else {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown owner");
    }
    cJSON_Delete(root);

    if (ret == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "video mode is not available");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "set video lease");
    return video_status_handler(req);
}

static esp_err_t snapshot_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    esp_err_t ret = si_video_acquire_jpeg(&jpeg, &jpeg_len, NULL);
    if (ret != ESP_OK) {
        return send_text_status(req, "503 Service Unavailable", esp_err_to_name(ret));
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send(req, (const char *)jpeg, jpeg_len);
    free(jpeg);
    return ret;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    video_control_touch_kvm();
    (void)video_control_apply_now();

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");

    uint32_t last_frame_id = 0;
    TickType_t first_frame_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    TickType_t next_send_tick = 0;
    const TickType_t frame_interval = pdMS_TO_TICKS(CONFIG_SI_VIDEO_STREAM_DELAY_MS);
    uint32_t last_kvm_touch_ms = 0;

    while (true) {
        uint32_t now_ms = video_control_now_ms();
        if (last_kvm_touch_ms == 0 ||
            (uint32_t)(now_ms - last_kvm_touch_ms) >= 1000U) {
            video_control_touch_kvm();
            last_kvm_touch_ms = now_ms;
        }

        uint8_t *jpeg = NULL;
        size_t jpeg_len = 0;
        uint32_t frame_id = 0;
        esp_err_t ret = si_video_acquire_jpeg(&jpeg, &jpeg_len, &frame_id);
        if (ret != ESP_OK) {
            if (last_frame_id == 0 &&
                (int32_t)(xTaskGetTickCount() - first_frame_deadline) >= 0) {
                return send_text_status(req, "503 Service Unavailable", esp_err_to_name(ret));
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (frame_id == last_frame_id) {
            free(jpeg);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        last_frame_id = frame_id;

        TickType_t now = xTaskGetTickCount();
        if (frame_interval > 0 && next_send_tick != 0 &&
            (int32_t)(now - next_send_tick) < 0) {
            free(jpeg);
            vTaskDelay(next_send_tick - now);
            continue;
        }

        char header[128];
        int header_len = snprintf(header, sizeof(header),
                                  "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                                  jpeg_len);
        if (httpd_resp_send_chunk(req, header, header_len) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)jpeg, jpeg_len) != ESP_OK ||
            httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
            free(jpeg);
            return ESP_OK;
        }
        free(jpeg);
        if (frame_interval > 0) {
            next_send_tick = now + frame_interval;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static esp_err_t stream_redirect_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    char host[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || host[0] == '\0') {
        si_net_status_t net;
        si_net_get_status(&net);
        strlcpy(host, net.ip[0] ? net.ip : "127.0.0.1", sizeof(host));
    }
    char *port = strchr(host, ':');
    if (port) {
        *port = '\0';
    }

    char query[192] = {0};
    char token[96] = {0};
    bool has_token = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                     httpd_query_key_value(query, "auth", token, sizeof(token)) == ESP_OK;
    char location[192];
    if (has_token) {
        snprintf(location, sizeof(location), "http://%s:%u/api/stream?auth=%s",
                 host, (unsigned)SI_STREAM_HTTP_PORT, token);
    } else {
        snprintf(location, sizeof(location), "http://%s:%u/api/stream",
                 host, (unsigned)SI_STREAM_HTTP_PORT);
    }
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, location);
}

static int httpd_client_count(httpd_handle_t server)
{
    if (!server) {
        return 0;
    }
    size_t clients = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(server, &clients, fds) != ESP_OK) {
        return 0;
    }
    return (int)clients;
}

int si_web_client_count(void)
{
    return httpd_client_count(s_server) + httpd_client_count(s_stream_server);
}

static double used_percent(uint32_t total, uint32_t free_bytes)
{
    if (total == 0) {
        return 0.0;
    }
    if (free_bytes > total) {
        free_bytes = total;
    }
    return ((double)(total - free_bytes) * 100.0) / (double)total;
}

static void add_heap_caps_json(cJSON *root, const char *name, uint32_t caps)
{
    uint32_t total = heap_caps_get_total_size(caps);
    uint32_t free_bytes = heap_caps_get_free_size(caps);
    uint32_t minimum_free = heap_caps_get_minimum_free_size(caps);
    uint32_t largest_free = heap_caps_get_largest_free_block(caps);
    uint32_t used = total > free_bytes ? total - free_bytes : 0;

    cJSON *heap = cJSON_AddObjectToObject(root, name);
    cJSON_AddNumberToObject(heap, "total_bytes", total);
    cJSON_AddNumberToObject(heap, "used_bytes", used);
    cJSON_AddNumberToObject(heap, "free_bytes", free_bytes);
    cJSON_AddNumberToObject(heap, "minimum_free_bytes", minimum_free);
    cJSON_AddNumberToObject(heap, "largest_free_block", largest_free);
    cJSON_AddNumberToObject(heap, "usage_percent", used_percent(total, free_bytes));
}

static double clamp_percent(double value)
{
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 100.0) {
        return 100.0;
    }
    return value;
}

static uint64_t runtime_delta(configRUN_TIME_COUNTER_TYPE now, configRUN_TIME_COUNTER_TYPE prev)
{
    return (uint64_t)(now - prev);
}

static void sample_cpu_usage(cpu_usage_sample_t *out)
{
    memset(out, 0, sizeof(*out));
    out->core_count = CONFIG_FREERTOS_NUMBER_OF_CORES;

#if (configGENERATE_RUN_TIME_STATS == 1) && (INCLUDE_xTaskGetIdleTaskHandle == 1)
    int64_t wall_us = esp_timer_get_time();
    configRUN_TIME_COUNTER_TYPE idle_runtime = ulTaskGetIdleRunTimeCounter();
    uint64_t wall_delta = s_cpu_usage_valid && wall_us > s_cpu_prev_wall_us ?
                          (uint64_t)(wall_us - s_cpu_prev_wall_us) : 0;
    uint64_t idle_delta = s_cpu_usage_valid ?
                          runtime_delta(idle_runtime, s_cpu_prev_idle) : 0;
    if (wall_delta > 0) {
        double capacity = (double)wall_delta * (double)CONFIG_FREERTOS_NUMBER_OF_CORES;
        out->overall_percent = clamp_percent(100.0 -
            (((double)idle_delta * 100.0) / capacity));
        out->sample_window_ms = (uint32_t)(wall_delta / 1000U);
        out->valid = true;
    }

    s_cpu_prev_wall_us = wall_us;
    s_cpu_prev_idle = idle_runtime;
    s_cpu_usage_valid = true;
#endif
}

static void add_performance_json(cJSON *root)
{
    si_video_status_t video;
    si_video_get_status(&video);

    double fps = video.fps_x100 / 100.0;
    double target_fps = video.target_fps_x100 / 100.0;
    double estimated_mbps = fps * (double)video.last_jpeg_size * 8.0 / 1000000.0;
    uint32_t frame_total = video.frames_captured;
    if (frame_total == 0) {
        frame_total = video.frames_encoded + video.frames_dropped;
    }
    double drop_percent = frame_total == 0 ? 0.0 :
                          ((double)video.frames_dropped * 100.0) / (double)frame_total;

    cJSON_AddNumberToObject(root, "uptime_seconds", uptime_sec());
    cJSON_AddNumberToObject(root, "active_connections", si_web_client_count());
    cJSON_AddNumberToObject(root, "task_count", uxTaskGetNumberOfTasks());

    cpu_usage_sample_t cpu_sample;
    sample_cpu_usage(&cpu_sample);
    cJSON *cpu = cJSON_AddObjectToObject(root, "cpu");
    cJSON_AddBoolToObject(cpu, "valid", cpu_sample.valid);
    cJSON_AddNumberToObject(cpu, "cores", cpu_sample.core_count);
    cJSON_AddNumberToObject(cpu, "usage_percent", cpu_sample.overall_percent);
    cJSON_AddNumberToObject(cpu, "sample_window_ms", cpu_sample.sample_window_ms);
    cJSON *cores = cJSON_AddArrayToObject(cpu, "core_usage_percent");
    (void)cores;

    cJSON *memory = cJSON_AddObjectToObject(root, "memory");
    add_heap_caps_json(memory, "heap", MALLOC_CAP_8BIT);
    add_heap_caps_json(memory, "internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    add_heap_caps_json(memory, "psram", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    cJSON *stream = cJSON_AddObjectToObject(root, "video");
    cJSON_AddNumberToObject(stream, "fps", fps);
    cJSON_AddNumberToObject(stream, "target_fps", target_fps);
    cJSON_AddNumberToObject(stream, "estimated_mbps", estimated_mbps);
    cJSON_AddNumberToObject(stream, "last_jpeg_size", video.last_jpeg_size);
    cJSON_AddNumberToObject(stream, "frame_interval_ms", video.last_frame_ms);
    cJSON_AddNumberToObject(stream, "expected_frame_interval_ms",
                            target_fps > 0.0 ? 1000.0 / target_fps : 0.0);
    cJSON_AddNumberToObject(stream, "frames_captured", video.frames_captured);
    cJSON_AddNumberToObject(stream, "frames_encoded", video.frames_encoded);
    cJSON_AddNumberToObject(stream, "frames_dropped", video.frames_dropped);
    cJSON_AddNumberToObject(stream, "drop_percent", drop_percent);
}

static esp_err_t overall_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_net_status_t net;
    si_net_get_status(&net);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "server_uptime", uptime_sec());
    cJSON *video = cJSON_AddObjectToObject(root, "video");
    add_video_json(video);
    cJSON *hid = cJSON_AddObjectToObject(root, "hid");
    add_hid_json(hid);

    cJSON *network = cJSON_AddObjectToObject(root, "network");
    cJSON_AddBoolToObject(network, "configured", net.configured);
    cJSON_AddBoolToObject(network, "connected", net.connected);
    cJSON_AddBoolToObject(network, "link_up", net.link_up);
    cJSON_AddBoolToObject(network, "full_duplex", net.full_duplex);
    cJSON_AddNumberToObject(network, "speed_mbps", net.speed_mbps);
    cJSON_AddStringToObject(network, "interface", net.interface);
    cJSON_AddStringToObject(network, "driver", net.driver);
    cJSON_AddStringToObject(network, "phy", net.phy);
    cJSON_AddStringToObject(network, "ip", net.ip);
    cJSON_AddStringToObject(network, "netmask", net.netmask);
    cJSON_AddStringToObject(network, "gateway", net.gateway);
    cJSON_AddStringToObject(network, "mac", net.mac);

    cJSON_AddNumberToObject(root, "active_connections", si_web_client_count());
    cJSON_AddBoolToObject(root, "authEnabled", auth_is_enabled());
    cJSON *performance = cJSON_AddObjectToObject(root, "performance");
    add_performance_json(performance);

    esp_err_t ret = send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

#ifdef CONFIG_HTTPD_WS_SUPPORT
static esp_err_t hid_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (!check_auth(req)) {
            return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
        }
        si_web_log("INFO", "KVM HID WebSocket connected");
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }
    if (frame.len == 0) {
        return ESP_OK;
    }
    if (frame.len > HID_WS_MAX_FRAME) {
        ESP_LOGW(TAG, "HID WebSocket frame too large: %u", (unsigned)frame.len);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK && frame.type == HTTPD_WS_TYPE_TEXT) {
        cJSON *root = cJSON_Parse((const char *)buf);
        if (root) {
            esp_err_t hid_ret = si_hid_handle_json(root);
            if (hid_ret != ESP_OK && hid_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "HID command failed: %s", esp_err_to_name(hid_ret));
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "Invalid HID WebSocket JSON");
        }
    }
    free(buf);
    return ret;
}
#endif

static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"error\":\"not found\"}");
}

static void register_uri(httpd_handle_t server, const char *uri, httpd_method_t method,
                         esp_err_t (*handler)(httpd_req_t *), bool websocket)
{
    httpd_uri_t cfg = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = NULL,
    };
#ifdef CONFIG_HTTPD_WS_SUPPORT
    cfg.is_websocket = websocket;
#else
    (void)websocket;
#endif
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &cfg));
}

static esp_err_t start_stream_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SI_STREAM_HTTP_PORT;
    config.ctrl_port = (uint16_t)(config.ctrl_port + 1);
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 6144;
    config.max_uri_handlers = 2;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.send_wait_timeout = 10;

    ESP_RETURN_ON_ERROR(httpd_start(&s_stream_server, &config), TAG, "httpd_start stream");
    register_uri(s_stream_server, "/api/stream", HTTP_GET, stream_handler, false);
    register_uri(s_stream_server, "/stream", HTTP_GET, stream_handler, false);
    ESP_LOGI(TAG, "MJPEG stream server started on port %d", SI_STREAM_HTTP_PORT);
    return ESP_OK;
}

esp_err_t si_web_server_start(void)
{
    auth_load_password();
    ota_settings_load();
    if (!s_ota_lock) {
        s_ota_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_ota_lock, ESP_ERR_NO_MEM, TAG, "create ota mutex");
    }
    ESP_RETURN_ON_ERROR(video_control_start(), TAG, "start video control");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SI_DEFAULT_HTTP_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 12288;
    config.max_uri_handlers = 42;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start");

    register_uri(s_server, "/", HTTP_GET, index_handler, false);
    register_uri(s_server, "/kvm", HTTP_GET, kvm_handler, false);
    register_uri(s_server, "/settings", HTTP_GET, settings_handler, false);
    register_uri(s_server, "/api/auth/login", HTTP_POST, auth_login_handler, false);
    register_uri(s_server, "/api/auth/status", HTTP_GET, auth_status_handler, false);
    register_uri(s_server, "/api/settings/account", HTTP_POST, settings_password_handler, false);
    register_uri(s_server, "/api/settings/password", HTTP_POST, settings_password_handler, false);
    register_uri(s_server, "/api/settings/device", HTTP_GET, settings_device_handler, false);
    register_uri(s_server, "/api/settings/device", HTTP_POST, settings_device_handler, false);
    register_uri(s_server, "/api/status", HTTP_GET, overall_status_handler, false);
    register_uri(s_server, "/api/system/info", HTTP_GET, system_info_handler, false);
    register_uri(s_server, "/api/system/logs", HTTP_GET, logs_handler, false);
    register_uri(s_server, "/api/hid/status", HTTP_GET, hid_status_handler, false);
    register_uri(s_server, "/api/ota/status", HTTP_GET, ota_status_handler, false);
    register_uri(s_server, "/api/ota/settings", HTTP_POST, ota_settings_handler, false);
    register_uri(s_server, "/api/ota/check", HTTP_POST, ota_check_handler, false);
    register_uri(s_server, "/api/ota/install", HTTP_POST, ota_install_handler, false);
    register_uri(s_server, "/api/ota/upload", HTTP_POST, ota_upload_handler, false);
    register_uri(s_server, "/api/ota/reboot", HTTP_POST, ota_reboot_handler, false);
#ifdef CONFIG_HTTPD_WS_SUPPORT
    register_uri(s_server, "/api/ws/hid", HTTP_GET, hid_ws_handler, true);
#endif
    register_uri(s_server, "/api/video/status", HTTP_GET, video_status_handler, false);
    register_uri(s_server, "/api/video/quality", HTTP_POST, video_quality_handler, false);
    register_uri(s_server, "/api/video/resolution", HTTP_POST, video_resolution_handler, false);
    register_uri(s_server, "/api/video/lease", HTTP_POST, video_lease_handler, false);
    register_uri(s_server, "/api/stream", HTTP_GET, stream_redirect_handler, false);
    register_uri(s_server, "/api/snapshot", HTTP_GET, snapshot_handler, false);

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler);
    ESP_RETURN_ON_ERROR(start_stream_server(), TAG, "start stream server");
    si_web_log("INFO", "HTTP server started");
    ESP_LOGI(TAG, "HTTP server started on port %d", SI_DEFAULT_HTTP_PORT);
    return ESP_OK;
}
