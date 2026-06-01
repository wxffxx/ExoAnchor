#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_device.h"
#include "net_manager.h"
#include "nvs_flash.h"
#include "video_input.h"
#include "web_server.h"

static const char *TAG = "esphost-p4";

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
    ESP_LOGI(TAG, "Board profile: %s", SI_BMC_BOARD);
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

void app_main(void)
{
    ESP_LOGI(TAG, "SI ESP32-P4 host firmware v%s starting", SI_BMC_VERSION);
    ESP_LOGI(TAG, "Reserved SDMMC pins: CLK=43 CMD=44 D0=39 D1=40 D2=41 D3=42");
    log_chip_info();

    init_nvs();

    ESP_ERROR_CHECK(si_hid_init());
    si_web_log("INFO", "TinyUSB HID initialized");

    esp_err_t video_ret = si_video_init();
    if (video_ret == ESP_OK) {
        si_web_log("INFO", "MIPI-CSI video input initialized");
    } else {
        si_web_log("WARNING", "MIPI-CSI video input not ready");
        ESP_LOGW(TAG, "Video init returned %s", esp_err_to_name(video_ret));
    }

    esp_err_t net_ret = si_net_init();
    if (net_ret == ESP_OK) {
        si_web_log("INFO", "Ethernet connected");
    } else {
        si_web_log("WARNING", "Ethernet not connected; check cable, DHCP, and PHY link");
        ESP_LOGW(TAG, "Network init returned %s", esp_err_to_name(net_ret));
    }

    ESP_ERROR_CHECK(si_web_server_start());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
