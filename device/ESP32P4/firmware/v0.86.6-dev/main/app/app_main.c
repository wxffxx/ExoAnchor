// Firmware composition root.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "auth_service.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "diag_cli.h"
#include "device_settings.h"
#include "hid_device.h"
#if SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED
#include "ms2109_eeprom_emulator.h"
#endif
#if SI_CFG_MS2109_POWER_ENABLED
#include "ms2109_power.h"
#endif
#if SI_CFG_MS2109_TEST_ENABLED
#include "ms2109_test.h"
#endif
#include "net_manager.h"
#include "network_discovery.h"
#include "nvs_flash.h"
#include "power_control.h"
#include "settings_schema.h"
#include "settings_store.h"
#include "storage_manager.h"
#include "target_uart.h"
#include "video_input.h"
#if CONFIG_SI_VIDEO_H264_EXPERIMENT
#include "video_h264_stream.h"
#endif
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
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t err =
        si_settings_store_open_read(&store, SETTINGS_RESET_NAMESPACE);
    if (err != ESP_OK) {
        return false;
    }

    char stored[128] = {0};
    err = si_settings_store_get_string(
        &store, SETTINGS_RESET_IMAGE_KEY, stored, sizeof(stored));
    si_settings_store_close(&store);

    return err == ESP_OK && strcmp(stored, image_id) == 0;
}

static void store_settings_reset_marker(const char *image_id)
{
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t err =
        si_settings_store_open_write(&store, SETTINGS_RESET_NAMESPACE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open reset marker settings: %s",
                 esp_err_to_name(err));
        return;
    }

    err = si_settings_store_set_string(
        &store, SETTINGS_RESET_IMAGE_KEY, image_id);
    if (err == ESP_OK) {
        err = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);

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

static void confirm_running_ota_image(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running || running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        return;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t ret = esp_ota_get_state_partition(running, &state);
    if (ret == ESP_ERR_NOT_SUPPORTED || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read running OTA image state: %s",
                 esp_err_to_name(ret));
        return;
    }

    /*
     * Reaching this point proves the settings schema, recovery UART,
     * peripheral managers, network manager and authenticated Web/API server
     * all initialized. Optional target-side signals are deliberately not
     * required: unplugged HDMI or Ethernet must not reject valid firmware.
     */
    ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA image %s passed startup health and is now valid",
                 running->label);
        si_web_log("INFO", "OTA startup health confirmed");
    } else {
        ESP_LOGE(TAG, "Cannot confirm OTA image %s: %s", running->label,
                 esp_err_to_name(ret));
        si_web_log("ERROR", "OTA startup health confirmation failed");
    }
#endif
}

void app_main(void)
{
#if CONFIG_SI_VIDEO_H264_EXPERIMENT
    /* Claim the UVC baseline and latency-critical H.264 reference SRAM before
     * any optional subsystem can create a task or fragment the internal heap.
     * Ordinary Stable builds do not compile this early barrier. */
    esp_err_t video_boot_memory_ret = si_video_reserve_boot_memory();
    if (video_boot_memory_ret != ESP_OK) {
        ESP_LOGW(TAG, "UVC boot-memory reserve returned %s",
                 esp_err_to_name(video_boot_memory_ret));
    }

    /* Always call the H.264 initializer so a failed UVC preflight is latched
     * as unavailable for this boot; a later WebSocket must not retry the
     * reference allocation after runtime tasks have started. */
    esp_err_t h264_ret = si_h264_stream_initialize();
    if (h264_ret == ESP_OK) {
        si_web_log("INFO", "Optional H.264 reference workspace reserved; remaining resources are lazy");
    } else {
        si_web_log("WARNING", "Optional H.264 service unavailable; MJPEG remains available");
        ESP_LOGW(TAG, "H.264 service init returned %s; continuing with MJPEG",
                 esp_err_to_name(h264_ret));
    }
#endif

#if SI_CFG_MS2109_POWER_ENABLED
    esp_err_t ms_power_ret = si_ms2109_power_init();
    if (ms_power_ret != ESP_OK && ms_power_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "MS2109 power init returned %s",
                 esp_err_to_name(ms_power_ret));
    }
#endif

#if SI_CFG_MS2109_TEST_ENABLED
    esp_err_t ms_test_ret = si_ms2109_test_init();
    if (ms_test_ret == ESP_OK) {
        ESP_LOGI(TAG, "PrototypeV2.4 MS2109 test controls initialized");
    } else if (ms_test_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "MS2109 test init returned %s",
                 esp_err_to_name(ms_test_ret));
    }
#endif

#if SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED
    esp_err_t eeprom_emulator_ret = si_ms2109_eeprom_emulator_init();
    if (eeprom_emulator_ret != ESP_OK &&
        eeprom_emulator_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "MS2109 EEPROM emulator init returned %s",
                 esp_err_to_name(eeprom_emulator_ret));
    }
#endif

    ESP_LOGI(TAG, "SI ESP32-P4 host firmware v%s starting", SI_BMC_VERSION);
    ESP_LOGI(TAG, "Configured SDMMC pins: CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d",
             SI_CFG_TF_SDMMC_CLK_GPIO, SI_CFG_TF_SDMMC_CMD_GPIO,
             SI_CFG_TF_SDMMC_D0_GPIO, SI_CFG_TF_SDMMC_D1_GPIO,
             SI_CFG_TF_SDMMC_D2_GPIO, SI_CFG_TF_SDMMC_D3_GPIO);
    log_chip_info();

    init_nvs();
    maybe_reset_settings_for_new_image();
    si_settings_schema_status_t schema = {0};
    esp_err_t schema_ret = si_settings_schema_initialize(&schema);
    if (schema_ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Settings schema unavailable: %s version=%" PRIu32
                 " pending=%" PRIu32 " last_good=%" PRIu32,
                 esp_err_to_name(schema_ret), schema.version,
                 schema.pending_version, schema.last_good_version);
    }
    ESP_ERROR_CHECK(schema_ret);
    ESP_LOGI(TAG, "Settings schema v%" PRIu32 "%s",
             schema.version,
             schema.migration_resumed ? " (resumed migration)" : "");

    /* app_main owns an internal stack. Preload every feature gate here before
     * the PSRAM-backed diagnostics/Agent callers can observe it. A failed
     * preload intentionally leaves the cached gates all-false and retryable. */
    esp_err_t feature_ret = si_product_feature_settings_preload();
    if (feature_ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Product feature preload failed; optional features remain disabled: %s",
                 esp_err_to_name(feature_ret));
    }

    ESP_ERROR_CHECK(si_auth_initialize());

    esp_err_t target_uart_ret = si_target_uart_init();
    if (target_uart_ret == ESP_OK) {
        si_web_log("INFO", "Target UART bridge initialized");
    } else if (target_uart_ret != ESP_ERR_NOT_SUPPORTED) {
        si_web_log("WARNING", "Target UART bridge not ready");
        ESP_LOGW(TAG, "Target UART init returned %s",
                 esp_err_to_name(target_uart_ret));
    }

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

    esp_err_t storage_ret = si_storage_init();
    if (storage_ret == ESP_OK) {
        si_web_log("INFO", "TF card mounted at /sdcard");
    } else {
        si_web_log("WARNING", "TF card not mounted");
        ESP_LOGW(TAG, "TF card init returned %s", esp_err_to_name(storage_ret));
    }

    ESP_ERROR_CHECK(si_diag_cli_start());

    esp_err_t net_ret = si_net_init();
    if (net_ret == ESP_OK) {
        si_web_log("INFO", "Ethernet manager initialized");
        esp_err_t discovery_ret = si_network_discovery_start();
        if (discovery_ret != ESP_OK) {
            ESP_LOGW(TAG, "Network discovery init returned %s",
                     esp_err_to_name(discovery_ret));
        }
    } else {
        si_web_log("WARNING", "Ethernet manager not ready");
        ESP_LOGW(TAG, "Network init returned %s", esp_err_to_name(net_ret));
    }

    ESP_ERROR_CHECK(si_web_server_start());
    confirm_running_ota_image();
    ESP_LOGI(TAG, "Initialization complete; releasing app_main task stack");
}
