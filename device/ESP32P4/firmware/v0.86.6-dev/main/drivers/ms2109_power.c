#include "ms2109_power.h"

#include "board_config.h"

#if SI_CFG_MS2109_POWER_ENABLED

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "si-ms-power";
static SemaphoreHandle_t s_lock;
static si_ms2109_power_status_t s_status = {
    .enabled = true,
    .switch_gpio = SI_CFG_MS2109_SWITCH_GPIO,
    .core_enable_gpio = SI_CFG_MS2109_CORE_ENABLE_GPIO,
    .switch_output_level = -1,
    .core_enable_output_level = -1,
    .last_result = ESP_ERR_INVALID_STATE,
};

static int rail_level(bool on, bool active_high)
{
    return active_high ? (on ? 1 : 0) : (on ? 0 : 1);
}

static void sample_levels_locked(void)
{
    s_status.switch_output_level =
        gpio_get_level(SI_CFG_MS2109_SWITCH_GPIO);
    s_status.core_enable_output_level =
        gpio_get_level(SI_CFG_MS2109_CORE_ENABLE_GPIO);
    s_status.power_on =
        s_status.switch_output_level ==
            rail_level(true, SI_CFG_MS2109_SWITCH_ACTIVE_HIGH) &&
        s_status.core_enable_output_level ==
            rail_level(true, SI_CFG_MS2109_CORE_ENABLE_ACTIVE_HIGH);
}

static esp_err_t set_power_locked(bool on)
{
    esp_err_t ret;
    if (on) {
        ret = gpio_set_level(
            SI_CFG_MS2109_SWITCH_GPIO,
            rail_level(true, SI_CFG_MS2109_SWITCH_ACTIVE_HIGH));
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(SI_CFG_MS2109_POWER_SEQUENCE_DELAY_MS));
            ret = gpio_set_level(
                SI_CFG_MS2109_CORE_ENABLE_GPIO,
                rail_level(true, SI_CFG_MS2109_CORE_ENABLE_ACTIVE_HIGH));
        }
        if (ret != ESP_OK) {
            (void)gpio_set_level(
                SI_CFG_MS2109_CORE_ENABLE_GPIO,
                rail_level(false, SI_CFG_MS2109_CORE_ENABLE_ACTIVE_HIGH));
            (void)gpio_set_level(
                SI_CFG_MS2109_SWITCH_GPIO,
                rail_level(false, SI_CFG_MS2109_SWITCH_ACTIVE_HIGH));
        }
    } else {
        ret = gpio_set_level(
            SI_CFG_MS2109_CORE_ENABLE_GPIO,
            rail_level(false, SI_CFG_MS2109_CORE_ENABLE_ACTIVE_HIGH));
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(SI_CFG_MS2109_POWER_SEQUENCE_DELAY_MS));
            ret = gpio_set_level(
                SI_CFG_MS2109_SWITCH_GPIO,
                rail_level(false, SI_CFG_MS2109_SWITCH_ACTIVE_HIGH));
        }
    }
    sample_levels_locked();
    return ret;
}

esp_err_t si_ms2109_power_init(void)
{
    if (s_status.initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(
        GPIO_IS_VALID_OUTPUT_GPIO(SI_CFG_MS2109_SWITCH_GPIO) &&
        GPIO_IS_VALID_OUTPUT_GPIO(SI_CFG_MS2109_CORE_ENABLE_GPIO) &&
        SI_CFG_MS2109_SWITCH_GPIO != SI_CFG_MS2109_CORE_ENABLE_GPIO,
        ESP_ERR_INVALID_ARG, TAG, "invalid MS2109 power GPIO map");

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG,
                            "create MS2109 power lock");
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "MS2109 power busy during init");
    if (s_status.initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    const uint64_t pin_mask =
        (1ULL << SI_CFG_MS2109_SWITCH_GPIO) |
        (1ULL << SI_CFG_MS2109_CORE_ENABLE_GPIO);
    const gpio_config_t config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_set_level(
        SI_CFG_MS2109_SWITCH_GPIO,
        rail_level(false, SI_CFG_MS2109_SWITCH_ACTIVE_HIGH));
    if (ret == ESP_OK) {
        ret = gpio_set_level(
            SI_CFG_MS2109_CORE_ENABLE_GPIO,
            rail_level(false, SI_CFG_MS2109_CORE_ENABLE_ACTIVE_HIGH));
    }
    if (ret == ESP_OK) {
        ret = gpio_config(&config);
    }
    if (ret == ESP_OK) {
        ret = set_power_locked(true);
    }

    s_status.initialized = ret == ESP_OK;
    s_status.last_result = ret;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "MS2109 rails enabled: 3v3=GPIO%d level=%d, 1v2=GPIO%d level=%d, delay=%dms",
                 SI_CFG_MS2109_SWITCH_GPIO,
                 s_status.switch_output_level,
                 SI_CFG_MS2109_CORE_ENABLE_GPIO,
                 s_status.core_enable_output_level,
                 SI_CFG_MS2109_POWER_SEQUENCE_DELAY_MS);
    } else {
        ESP_LOGE(TAG, "MS2109 power initialization failed: %s",
                 esp_err_to_name(ret));
    }
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t si_ms2109_power_get_status(si_ms2109_power_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG,
                        "missing MS2109 power status output");
    *status = (si_ms2109_power_status_t) {
        .enabled = true,
        .switch_gpio = SI_CFG_MS2109_SWITCH_GPIO,
        .core_enable_gpio = SI_CFG_MS2109_CORE_ENABLE_GPIO,
        .switch_output_level = -1,
        .core_enable_output_level = -1,
        .last_result = ESP_ERR_TIMEOUT,
    };
    if (!s_lock || !s_status.initialized) {
        *status = s_status;
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "MS2109 power status busy");
    sample_levels_locked();
    *status = s_status;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_ms2109_power_set(bool on)
{
    ESP_RETURN_ON_FALSE(s_lock && s_status.initialized, ESP_ERR_INVALID_STATE,
                        TAG, "MS2109 power not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "MS2109 power busy");
    esp_err_t ret = set_power_locked(on);
    s_status.operation_count++;
    s_status.last_result = ret;
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t si_ms2109_power_cycle(uint32_t off_time_ms)
{
    ESP_RETURN_ON_FALSE(off_time_ms >= 10U && off_time_ms <= 5000U,
                        ESP_ERR_INVALID_ARG, TAG,
                        "MS2109 off time must be 10..5000 ms");
    ESP_RETURN_ON_FALSE(s_lock && s_status.initialized, ESP_ERR_INVALID_STATE,
                        TAG, "MS2109 power not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "MS2109 power busy");
    esp_err_t ret = set_power_locked(false);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(off_time_ms));
        ret = set_power_locked(true);
    }
    s_status.operation_count++;
    s_status.last_result = ret;
    xSemaphoreGive(s_lock);
    return ret;
}

#else

esp_err_t si_ms2109_power_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_ms2109_power_get_status(si_ms2109_power_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    *status = (si_ms2109_power_status_t) {
        .enabled = false,
        .switch_gpio = SI_CFG_MS2109_SWITCH_GPIO,
        .core_enable_gpio = SI_CFG_MS2109_CORE_ENABLE_GPIO,
        .switch_output_level = -1,
        .core_enable_output_level = -1,
        .last_result = ESP_ERR_NOT_SUPPORTED,
    };
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_ms2109_power_set(bool on)
{
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_ms2109_power_cycle(uint32_t off_time_ms)
{
    (void)off_time_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
