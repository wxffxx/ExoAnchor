#include "ms2109_power.h"

#include "board_config.h"

#if SI_CFG_MS2109_POWER_ENABLED

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "si-ms-power";
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_cold_start_timer;
static int64_t s_cold_start_until_us;
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

static void refresh_cold_start_locked(void)
{
    if (!s_status.cold_start_active) {
        s_status.cold_start_remaining_ms = 0;
        return;
    }
    int64_t remaining_us = s_cold_start_until_us - esp_timer_get_time();
    s_status.cold_start_remaining_ms =
        remaining_us > 0 ? (uint32_t)((remaining_us + 999) / 1000) : 0U;
}

/* Called with s_lock held: forget a pending cold start before a manual
 * on/off/cycle takes over. */
static void cancel_cold_start_locked(void)
{
    if (s_cold_start_timer) {
        (void)esp_timer_stop(s_cold_start_timer);
    }
    s_status.cold_start_active = false;
    s_status.cold_start_remaining_ms = 0;
}

static void cold_start_timer_cb(void *arg)
{
    (void)arg;
    /* esp_timer task context: set_power_locked() only toggles two GPIOs with
     * a 10 ms sequence delay, acceptable for a rare manual action. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "MS2109 cold start: power lock busy, rails stay off");
        return;
    }
    esp_err_t ret = ESP_OK;
    if (s_status.cold_start_active) {
        ret = set_power_locked(true);
        s_status.operation_count++;
        s_status.last_result = ret;
        s_status.cold_start_active = false;
        s_status.cold_start_remaining_ms = 0;
        ESP_LOGI(TAG, "MS2109 cold start: rails back on after %u ms off (%s)",
                 (unsigned)s_status.cold_start_off_ms, esp_err_to_name(ret));
    }
    xSemaphoreGive(s_lock);
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
    refresh_cold_start_locked();
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
    cancel_cold_start_locked();
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
    cancel_cold_start_locked();
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

esp_err_t si_ms2109_power_cold_start(uint32_t off_time_ms)
{
    ESP_RETURN_ON_FALSE(off_time_ms >= SI_MS2109_COLD_START_MIN_MS &&
                        off_time_ms <= SI_MS2109_COLD_START_MAX_MS,
                        ESP_ERR_INVALID_ARG, TAG,
                        "MS2109 cold start off time must be 30..60 s");
    ESP_RETURN_ON_FALSE(s_lock && s_status.initialized, ESP_ERR_INVALID_STATE,
                        TAG, "MS2109 power not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "MS2109 power busy");
    esp_err_t ret = ESP_OK;
    if (!s_cold_start_timer) {
        const esp_timer_create_args_t args = {
            .callback = cold_start_timer_cb,
            .name = "ms2109_cold",
        };
        ret = esp_timer_create(&args, &s_cold_start_timer);
    }
    if (ret == ESP_OK) {
        cancel_cold_start_locked();
        ret = set_power_locked(false);
    }
    if (ret == ESP_OK) {
        ret = esp_timer_start_once(s_cold_start_timer,
                                   (uint64_t)off_time_ms * 1000ULL);
    }
    if (ret == ESP_OK) {
        s_status.cold_start_active = true;
        s_status.cold_start_off_ms = off_time_ms;
        s_cold_start_until_us =
            esp_timer_get_time() + (int64_t)off_time_ms * 1000;
        refresh_cold_start_locked();
        ESP_LOGI(TAG, "MS2109 cold start: rails off for %u ms",
                 (unsigned)off_time_ms);
    } else {
        /* Never leave the capture card dark because the timer failed. */
        (void)set_power_locked(true);
        s_status.cold_start_active = false;
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

esp_err_t si_ms2109_power_cold_start(uint32_t off_time_ms)
{
    (void)off_time_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
