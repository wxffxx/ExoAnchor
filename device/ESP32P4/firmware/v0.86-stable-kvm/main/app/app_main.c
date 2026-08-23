// Firmware composition root.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "diag_cli.h"
#include "hid_device.h"
#include "net_manager.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "power_control.h"
#include "video_input.h"
#include "web_server.h"

static const char *TAG = "esphost-p4";

#define SETTINGS_RESET_NAMESPACE "si_boot"
#define SETTINGS_RESET_IMAGE_KEY "reset_image"

static void log_base_mac(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BASE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read base MAC: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Base MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void log_chip_info(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "Board profile: %s (%s), silicon=%s, hostname=%s",
             SI_BMC_BOARD, SI_BOARD_ID, SI_SILICON_TARGET, SI_BMC_HOSTNAME);
    ESP_LOGI(TAG, "CPU cores: %d", chip_info.cores);
    ESP_LOGI(TAG, "Silicon revision: v%u.%u",
             chip_info.revision / 100, chip_info.revision % 100);

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "Flash: %" PRIu32 " MB %s",
                 flash_size / (1024U * 1024U),
                 (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    } else {
        ESP_LOGW(TAG, "Flash size read failed");
    }

    ESP_LOGI(TAG, "Minimum free heap: %" PRIu32 " bytes", esp_get_minimum_free_heap_size());
    log_base_mac();
}

static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

#if CONFIG_SI_RESET_SETTINGS_ON_NEW_IMAGE
static void build_settings_reset_image_id(char *out, size_t out_size)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        snprintf(out, out_size, "%s:%s:%s:%s",
                 desc->project_name, desc->version, desc->date, desc->time);
        return;
    }

    snprintf(out, out_size, "%s:%s:%s", SI_BMC_VERSION, __DATE__, __TIME__);
}

static bool settings_reset_marker_matches(const char *image_id)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_RESET_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    char stored[128] = {0};
    size_t stored_len = sizeof(stored);
    err = nvs_get_str(nvs, SETTINGS_RESET_IMAGE_KEY, stored, &stored_len);
    nvs_close(nvs);

    return err == ESP_OK && strcmp(stored, image_id) == 0;
}

static void store_settings_reset_marker(const char *image_id)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_RESET_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open reset marker NVS: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(nvs, SETTINGS_RESET_IMAGE_KEY, image_id);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Stored settings reset marker for image %s", image_id);
    } else {
        ESP_LOGE(TAG, "Failed to store reset marker: %s", esp_err_to_name(err));
    }
}
#endif

static void maybe_reset_settings_for_new_image(void)
{
#if CONFIG_SI_RESET_SETTINGS_ON_NEW_IMAGE
    char image_id[128] = {0};
    build_settings_reset_image_id(image_id, sizeof(image_id));

    if (settings_reset_marker_matches(image_id)) {
        ESP_LOGI(TAG, "NVS settings reset already applied for this firmware image");
        return;
    }

    ESP_LOGW(TAG, "Erasing all NVS settings for firmware image %s", image_id);
    ESP_ERROR_CHECK(nvs_flash_deinit());
    ESP_ERROR_CHECK(nvs_flash_erase());
    init_nvs();
    store_settings_reset_marker(image_id);
#else
    ESP_LOGD(TAG, "NVS settings reset-on-new-image disabled");
#endif
}

void app_main(void)
{
    ESP_LOGI(TAG, "SI ESP32-P4 host firmware v%s starting", SI_BMC_VERSION);
    log_chip_info();

    init_nvs();
    maybe_reset_settings_for_new_image();

    ESP_LOGI(TAG, "USB UVC + HID firmware; HID DM=%d DP=%d",
             SI_CFG_HID_DM_GPIO, SI_CFG_HID_DP_GPIO);
    si_web_log("INFO", "USB UVC + HID firmware");

    esp_err_t video_ret = si_video_init();
    if (video_ret == ESP_OK) {
        si_web_log("INFO", "USB UVC video input initialized");
    } else {
        si_web_log("WARNING", "Video input not ready");
        ESP_LOGW(TAG, "Video init returned %s", esp_err_to_name(video_ret));
    }

    esp_err_t hid_ret = si_hid_init();
    if (hid_ret == ESP_OK) {
        si_web_log("INFO", "USB HID device initialized");
    } else {
        si_web_log("WARNING", "USB HID device not ready");
        ESP_LOGW(TAG, "HID init returned %s", esp_err_to_name(hid_ret));
    }

    esp_err_t power_ret = si_power_init();
    if (power_ret == ESP_OK) {
        si_web_log("INFO", "Power control initialized");
    } else {
        si_web_log("WARNING", "Power control not ready");
        ESP_LOGW(TAG, "Power control init returned %s", esp_err_to_name(power_ret));
    }

    esp_err_t net_ret = si_net_init();
    if (net_ret == ESP_OK) {
        si_web_log("INFO", "Ethernet connected");
    } else {
        si_web_log("WARNING", "Ethernet not connected; check cable, DHCP, and PHY link");
        ESP_LOGW(TAG, "Network init returned %s", esp_err_to_name(net_ret));
    }

    ESP_ERROR_CHECK(si_web_server_start());
    ESP_ERROR_CHECK(si_diag_cli_start());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
