// TF/SDMMC storage driver and filesystem layout management.
#include "storage_manager.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/sdmmc_host.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"

#if SOC_SDMMC_IO_POWER_EXTERNAL && SOC_GP_LDO_SUPPORTED
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *TAG = "si-storage";

#define STORAGE_MAINTENANCE_INTERVAL_MS 10000U
#define STORAGE_MAINTENANCE_STACK_SIZE 8192U

static SemaphoreHandle_t s_storage_lock;
static sdmmc_card_t *s_tf_card;
static si_storage_tf_status_t s_tf_status;
static si_storage_tf_status_t s_tf_snapshot;
static portMUX_TYPE s_tf_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_storage_maintenance_task;
#if SOC_SDMMC_IO_POWER_EXTERNAL && SOC_GP_LDO_SUPPORTED
static sd_pwr_ctrl_handle_t s_tf_pwr_ctrl;
#endif

static void tf_status_defaults(si_storage_tf_status_t *status)
{
    memset(status, 0, sizeof(*status));
    status->supported = true;
    snprintf(status->mount_point, sizeof(status->mount_point), "%s", SI_TF_MOUNT_POINT);
    snprintf(status->card_name, sizeof(status->card_name), "--");
    snprintf(status->card_type, sizeof(status->card_type), "--");
    snprintf(status->bus_mode, sizeof(status->bus_mode), "unavailable");
}

static void publish_tf_status_locked(void)
{
    taskENTER_CRITICAL(&s_tf_snapshot_mux);
    s_tf_snapshot = s_tf_status;
    taskEXIT_CRITICAL(&s_tf_snapshot_mux);
}

static const char *tf_card_type(const sdmmc_card_t *card)
{
    if (!card) {
        return "--";
    }
    if (card->is_sdio) {
        return "SDIO";
    }
    if (card->is_mmc) {
        return "MMC";
    }
    return "SD";
}

static uint32_t fatfs_sector_size(const FATFS *fs)
{
#if FF_MAX_SS != FF_MIN_SS
    return fs && fs->ssize ? fs->ssize : FF_MAX_SS;
#else
    (void)fs;
    return FF_MAX_SS;
#endif
}

static void log_tf_bus_levels(const char *stage)
{
    const gpio_num_t pins[] = {
        (gpio_num_t)SI_CFG_TF_SDMMC_CMD_GPIO,
        (gpio_num_t)SI_CFG_TF_SDMMC_D0_GPIO,
        (gpio_num_t)SI_CFG_TF_SDMMC_D1_GPIO,
        (gpio_num_t)SI_CFG_TF_SDMMC_D2_GPIO,
        (gpio_num_t)SI_CFG_TF_SDMMC_D3_GPIO,
        (gpio_num_t)SI_CFG_TF_SDMMC_CLK_GPIO,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], GPIO_FLOATING);
    }

    ESP_LOGW(TAG, "TF bus levels (%s, external pulls only): cmd=%d d0=%d d1=%d d2=%d d3=%d clk=%d",
             stage,
             gpio_get_level(pins[0]), gpio_get_level(pins[1]),
             gpio_get_level(pins[2]), gpio_get_level(pins[3]),
             gpio_get_level(pins[4]), gpio_get_level(pins[5]));
}

static bool tf_transport_error_allows_low_speed(esp_err_t error)
{
    return error == ESP_ERR_TIMEOUT ||
           error == ESP_ERR_INVALID_RESPONSE ||
           error == ESP_ERR_INVALID_CRC;
}

static esp_err_t mount_tf_sdmmc(const esp_vfs_fat_sdmmc_mount_config_t *mount_config,
                                int bus_width, int max_freq_khz,
                                int card_detect_level, sdmmc_card_t **card)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = max_freq_khz;
#if SOC_SDMMC_IO_POWER_EXTERNAL && SOC_GP_LDO_SUPPORTED
    host.pwr_ctrl_handle = s_tf_pwr_ctrl;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.cd = (gpio_num_t)SI_CFG_TF_CARD_DETECT_GPIO;
    slot_config.clk = (gpio_num_t)SI_CFG_TF_SDMMC_CLK_GPIO;
    slot_config.cmd = (gpio_num_t)SI_CFG_TF_SDMMC_CMD_GPIO;
    slot_config.d0 = (gpio_num_t)SI_CFG_TF_SDMMC_D0_GPIO;
    slot_config.d1 = (gpio_num_t)SI_CFG_TF_SDMMC_D1_GPIO;
    slot_config.d2 = (gpio_num_t)SI_CFG_TF_SDMMC_D2_GPIO;
    slot_config.d3 = (gpio_num_t)SI_CFG_TF_SDMMC_D3_GPIO;
    slot_config.width = bus_width;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG,
             "TF SDMMC: cd=%d level=%d present=%s clk=%d cmd=%d d0=%d d1=%d d2=%d d3=%d width=%d max_freq=%dkHz",
             SI_CFG_TF_CARD_DETECT_GPIO, card_detect_level,
             card_detect_level < 0 ? "unknown" : (card_detect_level == 0 ? "yes" : "no"),
             SI_CFG_TF_SDMMC_CLK_GPIO, SI_CFG_TF_SDMMC_CMD_GPIO,
             SI_CFG_TF_SDMMC_D0_GPIO, SI_CFG_TF_SDMMC_D1_GPIO,
             SI_CFG_TF_SDMMC_D2_GPIO, SI_CFG_TF_SDMMC_D3_GPIO,
             bus_width, max_freq_khz);

    *card = NULL;
    return esp_vfs_fat_sdmmc_mount(SI_TF_MOUNT_POINT, &host, &slot_config,
                                   mount_config, card);
}

static void refresh_tf_usage_locked(void)
{
    if (!s_tf_status.mounted) {
        return;
    }

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    FRESULT result = f_getfree(SI_TF_MOUNT_POINT, &free_clusters, &fs);
    if (result != FR_OK || !fs) {
        snprintf(s_tf_status.last_error, sizeof(s_tf_status.last_error),
                 "f_getfree failed: %d", (int)result);
        ESP_LOGW(TAG, "Failed to refresh TF card usage: %d", (int)result);
        return;
    }

    uint64_t sector_size = fatfs_sector_size(fs);
    uint64_t sectors_per_cluster = fs->csize;
    uint64_t total_clusters = fs->n_fatent >= 2 ? (uint64_t)fs->n_fatent - 2ULL : 0ULL;
    uint64_t total = total_clusters * sectors_per_cluster * sector_size;
    uint64_t free_bytes = (uint64_t)free_clusters * sectors_per_cluster * sector_size;
    if (free_bytes > total) {
        free_bytes = total;
    }

    s_tf_status.sector_size = (uint32_t)sector_size;
    s_tf_status.cluster_size = (uint32_t)(sectors_per_cluster * sector_size);
    s_tf_status.total_bytes = total;
    s_tf_status.free_bytes = free_bytes;
    s_tf_status.used_bytes = total - free_bytes;
    s_tf_status.usage_percent = total == 0 ? 0.0 :
        ((double)s_tf_status.used_bytes * 100.0) / (double)total;
    s_tf_status.last_error[0] = '\0';
}

esp_err_t si_storage_ensure_dir(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    errno = 0;
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Failed to create %s: errno=%d", path, errno);
    return ESP_FAIL;
}

esp_err_t si_storage_ensure_layout(void)
{
    si_storage_tf_status_t status;
    si_storage_get_tf_status(&status);
    if (!status.mounted) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(si_storage_ensure_dir(SI_STORAGE_ROOT), TAG, "create storage root");
    ESP_RETURN_ON_ERROR(si_storage_ensure_dir(SI_STORAGE_ASSETS_DIR), TAG, "create assets dir");
    ESP_RETURN_ON_ERROR(si_storage_ensure_dir(SI_STORAGE_AGENT_DIR), TAG, "create agent dir");
    ESP_RETURN_ON_ERROR(si_storage_ensure_dir(SI_STORAGE_LOGS_DIR), TAG, "create logs dir");
    ESP_RETURN_ON_ERROR(si_storage_ensure_dir(SI_STORAGE_SNAPSHOTS_DIR), TAG, "create snapshots dir");
    ESP_RETURN_ON_ERROR(si_storage_ensure_dir(SI_STORAGE_EXPORTS_DIR), TAG, "create exports dir");
    ESP_RETURN_ON_ERROR(si_storage_ensure_dir(SI_STORAGE_MCP_DIR), TAG, "create mcp dir");
    ESP_RETURN_ON_ERROR(si_storage_ensure_dir(SI_STORAGE_OTA_DIR), TAG, "create ota dir");
    return ESP_OK;
}

static esp_err_t mount_tf_locked(void)
{
    if (s_tf_status.mounted) {
        refresh_tf_usage_locked();
        return ESP_OK;
    }

    esp_err_t ret;
#if SOC_SDMMC_IO_POWER_EXTERNAL && SOC_GP_LDO_SUPPORTED
    if (!s_tf_pwr_ctrl) {
        sd_pwr_ctrl_ldo_config_t ldo_config = {
            .ldo_chan_id = SI_CFG_TF_LDO_CHANNEL,
        };
        ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_tf_pwr_ctrl);
        if (ret != ESP_OK) {
            snprintf(s_tf_status.last_error, sizeof(s_tf_status.last_error),
                     "ldo failed: %s", esp_err_to_name(ret));
            ESP_LOGW(TAG, "TF card LDO init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "TF card SDMMC power uses on-chip LDO channel %d",
                 SI_CFG_TF_LDO_CHANNEL);
    }
#endif

    int card_detect_level = -1;
    if (SI_CFG_TF_CARD_DETECT_GPIO >= 0) {
        gpio_set_direction((gpio_num_t)SI_CFG_TF_CARD_DETECT_GPIO, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)SI_CFG_TF_CARD_DETECT_GPIO, GPIO_PULLUP_ONLY);
        card_detect_level = gpio_get_level((gpio_num_t)SI_CFG_TF_CARD_DETECT_GPIO);
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    const char *active_mode = "sdmmc-standard";
    int active_width = SI_CFG_TF_SDMMC_BUS_WIDTH;
    int active_freq_khz = SDMMC_FREQ_DEFAULT;
    bool degraded_mode = false;

    s_tf_status.fallback_reason[0] = '\0';
    ret = mount_tf_sdmmc(&mount_config, SI_CFG_TF_SDMMC_BUS_WIDTH,
                         SDMMC_FREQ_DEFAULT, card_detect_level, &card);
    esp_err_t standard_ret = ret;
    if (ret != ESP_OK && SI_CFG_TF_SDMMC_LOW_SPEED_FALLBACK &&
        SI_CFG_TF_SDMMC_BUS_WIDTH > 1 &&
        tf_transport_error_allows_low_speed(ret)) {
        snprintf(s_tf_status.fallback_reason, sizeof(s_tf_status.fallback_reason),
                 "standard failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG,
                 "Standard %d-bit SDMMC init failed (%s); retrying 1-bit at %dkHz",
                 SI_CFG_TF_SDMMC_BUS_WIDTH, esp_err_to_name(ret),
                 SI_CFG_TF_SDMMC_LOW_SPEED_FREQ_KHZ);

        ret = mount_tf_sdmmc(&mount_config, 1,
                             SI_CFG_TF_SDMMC_LOW_SPEED_FREQ_KHZ,
                             card_detect_level, &card);
        if (ret == ESP_OK) {
            active_mode = "sdmmc-low-speed";
            active_width = 1;
            active_freq_khz = SI_CFG_TF_SDMMC_LOW_SPEED_FREQ_KHZ;
            degraded_mode = true;
            ESP_LOGW(TAG, "TF card initialized in automatic low-speed 1-bit fallback mode");
        } else {
            snprintf(s_tf_status.fallback_reason, sizeof(s_tf_status.fallback_reason),
                     "standard %s; low-speed %s",
                     esp_err_to_name(standard_ret), esp_err_to_name(ret));
            ESP_LOGW(TAG, "Low-speed 1-bit SDMMC fallback failed: %s",
                     esp_err_to_name(ret));
        }
    }
    if (ret != ESP_OK && card_detect_level == 0) {
        log_tf_bus_levels("after SDMMC failure");
        ESP_LOGW(TAG, "SDMMC init failed (%s); trying SDSPI fallback", esp_err_to_name(ret));

        sdmmc_host_t spi_host = SDSPI_HOST_DEFAULT();
        spi_host.max_freq_khz = 10 * 1000;
#if SOC_SDMMC_IO_POWER_EXTERNAL && SOC_GP_LDO_SUPPORTED
        spi_host.pwr_ctrl_handle = s_tf_pwr_ctrl;
#endif

        spi_bus_config_t bus_config = {
            .mosi_io_num = SI_CFG_TF_SDMMC_CMD_GPIO,
            .miso_io_num = SI_CFG_TF_SDMMC_D0_GPIO,
            .sclk_io_num = SI_CFG_TF_SDMMC_CLK_GPIO,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 16 * 1024,
        };
        esp_err_t bus_ret = spi_bus_initialize(spi_host.slot, &bus_config, SDSPI_DEFAULT_DMA);
        if (bus_ret == ESP_OK) {
            sdspi_device_config_t device_config = SDSPI_DEVICE_CONFIG_DEFAULT();
            device_config.host_id = spi_host.slot;
            device_config.gpio_cs = (gpio_num_t)SI_CFG_TF_SDMMC_D3_GPIO;
            device_config.gpio_cd = (gpio_num_t)SI_CFG_TF_CARD_DETECT_GPIO;

            card = NULL;
            ret = esp_vfs_fat_sdspi_mount(SI_TF_MOUNT_POINT, &spi_host, &device_config,
                                          &mount_config, &card);
            if (ret == ESP_OK) {
                active_mode = "sdspi-low-speed";
                active_width = 1;
                active_freq_khz = 10 * 1000;
                degraded_mode = true;
                ESP_LOGW(TAG, "TF card initialized through SDSPI fallback at 10MHz");
            } else {
                ESP_LOGW(TAG, "SDSPI fallback failed: %s", esp_err_to_name(ret));
                spi_bus_free(spi_host.slot);
                log_tf_bus_levels("after SDSPI failure");
            }
        } else {
            ret = bus_ret;
            ESP_LOGW(TAG, "SDSPI bus init failed: %s", esp_err_to_name(bus_ret));
        }
    }
    if (ret != ESP_OK) {
        s_tf_card = NULL;
        s_tf_status.mounted = false;
        s_tf_status.degraded_mode = false;
        s_tf_status.bus_width = 0;
        s_tf_status.bus_frequency_khz = 0;
        snprintf(s_tf_status.bus_mode, sizeof(s_tf_status.bus_mode), "unavailable");
        snprintf(s_tf_status.card_type, sizeof(s_tf_status.card_type), "--");
        snprintf(s_tf_status.card_name, sizeof(s_tf_status.card_name), "--");
        snprintf(s_tf_status.last_error, sizeof(s_tf_status.last_error),
                 "mount failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "TF card mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_tf_card = card;
    s_tf_status.mounted = true;
    s_tf_status.degraded_mode = degraded_mode;
    s_tf_status.bus_width = (uint8_t)active_width;
    s_tf_status.bus_frequency_khz = card->real_freq_khz > 0 ?
        card->real_freq_khz : (uint32_t)active_freq_khz;
    snprintf(s_tf_status.bus_mode, sizeof(s_tf_status.bus_mode), "%s", active_mode);
    snprintf(s_tf_status.card_type, sizeof(s_tf_status.card_type), "%s", tf_card_type(card));
    snprintf(s_tf_status.card_name, sizeof(s_tf_status.card_name), "%.*s",
             (int)strnlen(card->cid.name, sizeof(card->cid.name)), card->cid.name);
    refresh_tf_usage_locked();

    ESP_LOGI(TAG, "TF card mounted at %s: name=%s type=%s mode=%s width=%u freq=%" PRIu32 "kHz used=%llu total=%llu",
             SI_TF_MOUNT_POINT,
             s_tf_status.card_name,
             s_tf_status.card_type,
             s_tf_status.bus_mode,
             (unsigned)s_tf_status.bus_width,
             s_tf_status.bus_frequency_khz,
             (unsigned long long)s_tf_status.used_bytes,
             (unsigned long long)s_tf_status.total_bytes);
    return ESP_OK;
}

static void storage_maintenance_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(STORAGE_MAINTENANCE_INTERVAL_MS));
        xSemaphoreTake(s_storage_lock, portMAX_DELAY);
        bool was_mounted = s_tf_status.mounted;
        esp_err_t ret = mount_tf_locked();
        publish_tf_status_locked();
        xSemaphoreGive(s_storage_lock);
        if (ret == ESP_OK && !was_mounted) {
            (void)si_storage_ensure_layout();
        }
    }
}

esp_err_t si_storage_init(void)
{
    if (!s_storage_lock) {
        s_storage_lock = xSemaphoreCreateMutex();
        if (!s_storage_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_storage_lock, portMAX_DELAY);
    tf_status_defaults(&s_tf_status);
    esp_err_t ret = mount_tf_locked();
    publish_tf_status_locked();
    xSemaphoreGive(s_storage_lock);
    if (ret == ESP_OK) {
        ret = si_storage_ensure_layout();
    }
    if (!s_storage_maintenance_task) {
        BaseType_t task_ok = xTaskCreateWithCaps(
            storage_maintenance_task, "si_storage", STORAGE_MAINTENANCE_STACK_SIZE,
            NULL, tskIDLE_PRIORITY + 1, &s_storage_maintenance_task,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (task_ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create storage maintenance task");
            if (ret == ESP_OK) {
                ret = ESP_ERR_NO_MEM;
            }
        }
    }
    return ret;
}

void si_storage_get_tf_status(si_storage_tf_status_t *out)
{
    if (!out) {
        return;
    }

    if (!s_storage_lock) {
        tf_status_defaults(out);
        snprintf(out->last_error, sizeof(out->last_error), "storage not initialized");
        return;
    }
    taskENTER_CRITICAL(&s_tf_snapshot_mux);
    *out = s_tf_snapshot;
    taskEXIT_CRITICAL(&s_tf_snapshot_mux);
}
