// ATX control, sensing, and runtime GPIO mapping.
#include "power_control.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "si-power";

#define SI_POWER_CFG_ENABLE SI_CFG_POWER_ENABLED
#define SI_POWER_MIN_PRESS_MS 50U
#define SI_POWER_MAX_PRESS_MS 15000U
#define SI_POWER_NVS_NAMESPACE "si_power"

typedef enum {
    POWER_ROLE_POWER_BUTTON = 0,
    POWER_ROLE_RESET_BUTTON,
    POWER_ROLE_POWER_DETECT,
    POWER_ROLE_STANDBY_DETECT,
    POWER_ROLE_RESERVE1,
    POWER_ROLE_RESERVE2,
    POWER_ROLE_RESERVE3,
    POWER_ROLE_RESERVE4,
    POWER_ROLE_LOCATOR,
    POWER_ROLE_COUNT,
} power_role_t;

typedef enum {
    POWER_DIRECTION_INPUT = 0,
    POWER_DIRECTION_OUTPUT,
} power_direction_t;

typedef struct {
    const char *role;
    const char *label;
    const char *gpio_key;
    const char *active_key;
    power_direction_t direction;
    bool required;
    int default_gpio;
    bool default_active_high;
} power_gpio_role_def_t;

typedef struct {
    int gpio;
    bool active_high;
    bool logical_on;
} power_gpio_cfg_t;

static const power_gpio_role_def_t s_role_defs[POWER_ROLE_COUNT] = {
    [POWER_ROLE_POWER_BUTTON] = {
        .role = "power_button",
        .label = "Power button",
        .gpio_key = "pwr_btn",
        .active_key = "pwr_btn_ah",
        .direction = POWER_DIRECTION_OUTPUT,
        .required = true,
        .default_gpio = SI_CFG_POWER_BUTTON_GPIO,
        .default_active_high = SI_CFG_POWER_ACTIVE_HIGH,
    },
    [POWER_ROLE_RESET_BUTTON] = {
        .role = "reset_button",
        .label = "Reset button",
        .gpio_key = "rst_btn",
        .active_key = "rst_btn_ah",
        .direction = POWER_DIRECTION_OUTPUT,
        .required = true,
        .default_gpio = SI_CFG_RESET_BUTTON_GPIO,
        .default_active_high = SI_CFG_POWER_ACTIVE_HIGH,
    },
    [POWER_ROLE_POWER_DETECT] = {
        .role = "power_detect",
        .label = "Power detect",
        .gpio_key = "pwr_det",
        .active_key = "pwr_det_ah",
        .direction = POWER_DIRECTION_INPUT,
        .required = false,
        .default_gpio = SI_CFG_POWER_DETECT_GPIO,
        .default_active_high = SI_CFG_POWER_DETECT_ACTIVE_HIGH,
    },
    [POWER_ROLE_STANDBY_DETECT] = {
        .role = "standby_detect",
        .label = "Power Standby detect",
        .gpio_key = "stby_det",
        .active_key = "stby_det_ah",
        .direction = POWER_DIRECTION_INPUT,
        .required = false,
        .default_gpio = SI_CFG_STANDBY_DETECT_GPIO,
        .default_active_high = SI_CFG_STANDBY_DETECT_ACTIVE_HIGH,
    },
    [POWER_ROLE_RESERVE1] = {
        .role = "reserve1",
        .label = "Reserve GPIO 1",
        .gpio_key = "reserve1",
        .active_key = "res1_ah",
        .direction = POWER_DIRECTION_INPUT,
        .required = false,
        .default_gpio = SI_CFG_POWER_RESERVE1_GPIO,
        .default_active_high = true,
    },
    [POWER_ROLE_RESERVE2] = {
        .role = "reserve2",
        .label = "Reserve GPIO 2",
        .gpio_key = "reserve2",
        .active_key = "res2_ah",
        .direction = POWER_DIRECTION_INPUT,
        .required = false,
        .default_gpio = SI_CFG_POWER_RESERVE2_GPIO,
        .default_active_high = true,
    },
    [POWER_ROLE_RESERVE3] = {
        .role = "reserve3",
        .label = "Reserve GPIO 3",
        .gpio_key = "reserve3",
        .active_key = "res3_ah",
        .direction = POWER_DIRECTION_INPUT,
        .required = false,
        .default_gpio = SI_CFG_POWER_RESERVE3_GPIO,
        .default_active_high = true,
    },
    [POWER_ROLE_RESERVE4] = {
        .role = "reserve4",
        .label = "Reserve GPIO 4",
        .gpio_key = "reserve4",
        .active_key = "res4_ah",
        .direction = POWER_DIRECTION_INPUT,
        .required = false,
        .default_gpio = SI_CFG_POWER_RESERVE4_GPIO,
        .default_active_high = true,
    },
    [POWER_ROLE_LOCATOR] = {
        .role = "locator",
        .label = "定位灯",
        .gpio_key = "locator",
        .active_key = "loc_ah",
        .direction = POWER_DIRECTION_OUTPUT,
        .required = false,
        .default_gpio = SI_CFG_POWER_LOCATOR_GPIO,
        .default_active_high = SI_CFG_POWER_LOCATOR_ACTIVE_HIGH,
    },
};

static SemaphoreHandle_t s_lock;
static si_power_status_t s_status;
static power_gpio_cfg_t s_gpio_cfg[POWER_ROLE_COUNT];

static bool gpio_valid(int gpio)
{
    return gpio >= 0 && gpio < GPIO_NUM_MAX;
}

static bool role_is_output(power_role_t role)
{
    return s_role_defs[role].direction == POWER_DIRECTION_OUTPUT;
}

static const char *role_direction_name(power_role_t role)
{
    return role_is_output(role) ? "output" : "input";
}

static int role_from_name(const char *role)
{
    if (!role || !role[0]) {
        return -1;
    }
    for (int i = 0; i < POWER_ROLE_COUNT; i++) {
        if (strcasecmp(role, s_role_defs[i].role) == 0) {
            return i;
        }
    }
    return -1;
}

static int logical_level(bool active_high, bool on)
{
    return active_high ? (on ? 1 : 0) : (on ? 0 : 1);
}

static int role_output_level(power_role_t role, const power_gpio_cfg_t *cfg)
{
    bool on = role == POWER_ROLE_LOCATOR ? cfg[role].logical_on : false;
    return logical_level(cfg[role].active_high, on);
}

static uint32_t normalize_duration(uint32_t duration_ms, uint32_t fallback_ms)
{
    uint32_t duration = duration_ms > 0 ? duration_ms : fallback_ms;
    if (duration < SI_POWER_MIN_PRESS_MS) {
        duration = SI_POWER_MIN_PRESS_MS;
    }
    if (duration > SI_POWER_MAX_PRESS_MS) {
        duration = SI_POWER_MAX_PRESS_MS;
    }
    return duration;
}

static void set_last_error(const char *message, esp_err_t err)
{
    if (err == ESP_OK) {
        s_status.last_error[0] = '\0';
        return;
    }
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s: %s",
             message ? message : "power control", esp_err_to_name(err));
}

static void set_gpio_defaults(void)
{
    for (int i = 0; i < POWER_ROLE_COUNT; i++) {
        s_gpio_cfg[i].gpio = s_role_defs[i].default_gpio;
        s_gpio_cfg[i].active_high = s_role_defs[i].default_active_high;
        s_gpio_cfg[i].logical_on = false;
    }
}

static esp_err_t validate_gpio_cfg(const power_gpio_cfg_t *cfg)
{
    for (int i = 0; i < POWER_ROLE_COUNT; i++) {
        if (s_role_defs[i].required && !gpio_valid(cfg[i].gpio)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg[i].gpio < -1 || cfg[i].gpio >= GPIO_NUM_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (int i = 0; i < POWER_ROLE_COUNT; i++) {
        if (!gpio_valid(cfg[i].gpio)) {
            continue;
        }
        for (int j = i + 1; j < POWER_ROLE_COUNT; j++) {
            if (cfg[i].gpio == cfg[j].gpio) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

static bool gpio_used_in_cfg(const power_gpio_cfg_t *cfg, int gpio)
{
    if (!gpio_valid(gpio)) {
        return false;
    }
    for (int i = 0; i < POWER_ROLE_COUNT; i++) {
        if (cfg[i].gpio == gpio) {
            return true;
        }
    }
    return false;
}

static void release_unused_gpios(const power_gpio_cfg_t *old_cfg, const power_gpio_cfg_t *new_cfg)
{
    for (int i = 0; i < POWER_ROLE_COUNT; i++) {
        int gpio = old_cfg[i].gpio;
        if (gpio_valid(gpio) && !gpio_used_in_cfg(new_cfg, gpio)) {
            (void)gpio_reset_pin((gpio_num_t)gpio);
        }
    }
}

static esp_err_t configure_output_role(power_role_t role, const power_gpio_cfg_t *cfg)
{
    int gpio = cfg[role].gpio;
    if (!gpio_valid(gpio)) {
        return s_role_defs[role].required ? ESP_ERR_INVALID_ARG : ESP_OK;
    }
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpio_cfg), TAG, "configure output gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)gpio, role_output_level(role, cfg)), TAG, "set output idle");
    return ESP_OK;
}

static esp_err_t configure_input_role(power_role_t role, const power_gpio_cfg_t *cfg)
{
    int gpio = cfg[role].gpio;
    if (!gpio_valid(gpio)) {
        return ESP_OK;
    }
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&gpio_cfg);
}

static esp_err_t apply_gpio_cfg(const power_gpio_cfg_t *cfg)
{
    ESP_RETURN_ON_ERROR(validate_gpio_cfg(cfg), TAG, "validate gpio map");
    for (int i = 0; i < POWER_ROLE_COUNT; i++) {
        if (!gpio_valid(cfg[i].gpio) && !s_role_defs[i].required) {
            continue;
        }
        esp_err_t ret = role_is_output((power_role_t)i)
                            ? configure_output_role((power_role_t)i, cfg)
                            : configure_input_role((power_role_t)i, cfg);
        ESP_RETURN_ON_ERROR(ret, TAG, "configure gpio map");
    }
    return ESP_OK;
}

static bool read_active_gpio(power_role_t role)
{
    int gpio = s_gpio_cfg[role].gpio;
    if (!gpio_valid(gpio)) {
        return false;
    }
    int level = gpio_get_level((gpio_num_t)gpio);
    return s_gpio_cfg[role].active_high ? level != 0 : level == 0;
}

static void fill_gpio_map_entry_locked(power_role_t role)
{
    si_power_gpio_map_entry_t *entry = &s_status.gpio_map[role];
    const power_gpio_role_def_t *def = &s_role_defs[role];
    const power_gpio_cfg_t *cfg = &s_gpio_cfg[role];
    bool valid = gpio_valid(cfg->gpio);

    memset(entry, 0, sizeof(*entry));
    strlcpy(entry->role, def->role, sizeof(entry->role));
    strlcpy(entry->label, def->label, sizeof(entry->label));
    strlcpy(entry->direction, role_direction_name(role), sizeof(entry->direction));
    entry->gpio = cfg->gpio;
    entry->enabled = SI_POWER_CFG_ENABLE && valid;
    entry->implemented = entry->enabled;
    entry->active_high = cfg->active_high;
    entry->configurable = true;
    entry->required = def->required;

    if (!valid) {
        strlcpy(entry->state, def->required ? "invalid" : "not-wired", sizeof(entry->state));
        return;
    }

    if (role_is_output(role)) {
        if (role == POWER_ROLE_LOCATOR) {
            entry->active = cfg->logical_on;
            strlcpy(entry->state, cfg->logical_on ? "on" : "off", sizeof(entry->state));
        } else {
            entry->active = false;
            strlcpy(entry->state, s_status.busy ? "busy" : "idle", sizeof(entry->state));
        }
        return;
    }

    entry->active = read_active_gpio(role);
    strlcpy(entry->state, entry->active ? "on" : "off", sizeof(entry->state));
}

static void sync_status_locked(void)
{
    s_status.enabled = SI_POWER_CFG_ENABLE;
    s_status.active_high = s_gpio_cfg[POWER_ROLE_POWER_BUTTON].active_high;
    s_status.power_button_gpio = s_gpio_cfg[POWER_ROLE_POWER_BUTTON].gpio;
    s_status.reset_button_gpio = s_gpio_cfg[POWER_ROLE_RESET_BUTTON].gpio;
    s_status.power_detect_gpio = s_gpio_cfg[POWER_ROLE_POWER_DETECT].gpio;
    s_status.standby_detect_gpio = s_gpio_cfg[POWER_ROLE_STANDBY_DETECT].gpio;
    s_status.reserve1_gpio = s_gpio_cfg[POWER_ROLE_RESERVE1].gpio;
    s_status.reserve2_gpio = s_gpio_cfg[POWER_ROLE_RESERVE2].gpio;
    s_status.reserve3_gpio = s_gpio_cfg[POWER_ROLE_RESERVE3].gpio;
    s_status.reserve4_gpio = s_gpio_cfg[POWER_ROLE_RESERVE4].gpio;
    s_status.locator_gpio = s_gpio_cfg[POWER_ROLE_LOCATOR].gpio;
    s_status.power_detect_active_high = s_gpio_cfg[POWER_ROLE_POWER_DETECT].active_high;
    s_status.standby_detect_active_high = s_gpio_cfg[POWER_ROLE_STANDBY_DETECT].active_high;
    s_status.locator_active_high = s_gpio_cfg[POWER_ROLE_LOCATOR].active_high;
    s_status.power_detect_supported = gpio_valid(s_gpio_cfg[POWER_ROLE_POWER_DETECT].gpio);
    s_status.standby_detect_supported = gpio_valid(s_gpio_cfg[POWER_ROLE_STANDBY_DETECT].gpio);
    s_status.locator_supported = gpio_valid(s_gpio_cfg[POWER_ROLE_LOCATOR].gpio);
    s_status.power_on = read_active_gpio(POWER_ROLE_POWER_DETECT);
    s_status.standby_on = read_active_gpio(POWER_ROLE_STANDBY_DETECT);
    s_status.locator_on = s_status.locator_supported && s_gpio_cfg[POWER_ROLE_LOCATOR].logical_on;
    s_status.gpio_map_count = POWER_ROLE_COUNT;
    for (int i = 0; i < POWER_ROLE_COUNT; i++) {
        fill_gpio_map_entry_locked((power_role_t)i);
    }
}

static void load_gpio_map_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SI_POWER_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return;
    }
    for (int i = 0; i < POWER_ROLE_COUNT; i++) {
        int32_t gpio = 0;
        uint8_t active_high = 0;
        if (nvs_get_i32(nvs, s_role_defs[i].gpio_key, &gpio) == ESP_OK) {
            s_gpio_cfg[i].gpio = (int)gpio;
        }
        if (nvs_get_u8(nvs, s_role_defs[i].active_key, &active_high) == ESP_OK) {
            s_gpio_cfg[i].active_high = active_high != 0;
        }
    }
    nvs_close(nvs);

    if (validate_gpio_cfg(s_gpio_cfg) != ESP_OK) {
        set_gpio_defaults();
        set_last_error("invalid saved gpio map", ESP_ERR_INVALID_ARG);
    }
}

static esp_err_t save_gpio_map_to_nvs(const power_gpio_cfg_t *cfg)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(SI_POWER_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open power NVS");
    esp_err_t ret = ESP_OK;
    for (int i = 0; i < POWER_ROLE_COUNT && ret == ESP_OK; i++) {
        ret = nvs_set_i32(nvs, s_role_defs[i].gpio_key, cfg[i].gpio);
        if (ret == ESP_OK) {
            ret = nvs_set_u8(nvs, s_role_defs[i].active_key, cfg[i].active_high ? 1 : 0);
        }
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t pulse_role(power_role_t role, uint32_t duration_ms, const char *action)
{
    if (!SI_POWER_CFG_ENABLE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t duration = normalize_duration(duration_ms, SI_CFG_POWER_PRESS_MS);
    int gpio = -1;
    bool active_high = true;
    esp_err_t ret = ESP_OK;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_status.initialized || s_status.busy) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    gpio = s_gpio_cfg[role].gpio;
    active_high = s_gpio_cfg[role].active_high;
    if (!gpio_valid(gpio)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    s_status.busy = true;
    strlcpy(s_status.last_action, action ? action : "press", sizeof(s_status.last_action));
    set_last_error(NULL, ESP_OK);
    sync_status_locked();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "pressing %s on GPIO%d for %" PRIu32 " ms", action, gpio, duration);
    ret = gpio_set_level((gpio_num_t)gpio, logical_level(active_high, true));
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(duration));
        ret = gpio_set_level((gpio_num_t)gpio, logical_level(active_high, false));
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_status.busy = false;
        if (ret == ESP_OK) {
            s_status.action_count++;
            set_last_error(NULL, ESP_OK);
        } else {
            set_last_error("gpio pulse", ret);
        }
        sync_status_locked();
        xSemaphoreGive(s_lock);
    }
    return ret;
}

esp_err_t si_power_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    set_gpio_defaults();
    s_status.default_press_ms = SI_CFG_POWER_PRESS_MS;
    s_status.force_off_ms = SI_CFG_POWER_FORCE_OFF_MS;
    strlcpy(s_status.last_action, "idle", sizeof(s_status.last_action));
    load_gpio_map_from_nvs();

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create power mutex");
    }

    if (!SI_POWER_CFG_ENABLE) {
        strlcpy(s_status.last_error, "power control disabled by config", sizeof(s_status.last_error));
        sync_status_locked();
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = apply_gpio_cfg(s_gpio_cfg);
    if (ret != ESP_OK) {
        set_last_error("init power gpio", ret);
        sync_status_locked();
        return ret;
    }

    s_status.initialized = true;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        sync_status_locked();
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "power control initialized: power=GPIO%d reset=GPIO%d detect=%d standby=%d locator=%d",
             s_gpio_cfg[POWER_ROLE_POWER_BUTTON].gpio,
             s_gpio_cfg[POWER_ROLE_RESET_BUTTON].gpio,
             s_gpio_cfg[POWER_ROLE_POWER_DETECT].gpio,
             s_gpio_cfg[POWER_ROLE_STANDBY_DETECT].gpio,
             s_gpio_cfg[POWER_ROLE_LOCATOR].gpio);
    return ESP_OK;
}

void si_power_get_status(si_power_status_t *status)
{
    if (!status) {
        return;
    }
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        sync_status_locked();
        *status = s_status;
        xSemaphoreGive(s_lock);
    } else {
        *status = s_status;
    }
}

esp_err_t si_power_press_power(uint32_t duration_ms)
{
    return pulse_role(POWER_ROLE_POWER_BUTTON, duration_ms, "power");
}

esp_err_t si_power_press_reset(uint32_t duration_ms)
{
    return pulse_role(POWER_ROLE_RESET_BUTTON, duration_ms, "reset");
}

esp_err_t si_power_force_off(uint32_t duration_ms)
{
    return pulse_role(POWER_ROLE_POWER_BUTTON,
                      normalize_duration(duration_ms, SI_CFG_POWER_FORCE_OFF_MS),
                      "force_off");
}

esp_err_t si_power_set_gpio_map(const si_power_gpio_update_t *updates, size_t count, bool persist)
{
    if (!SI_POWER_CFG_ENABLE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!updates && count > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    power_gpio_cfg_t next_cfg[POWER_ROLE_COUNT];
    power_gpio_cfg_t old_cfg[POWER_ROLE_COUNT];
    esp_err_t ret = ESP_OK;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_status.initialized || s_status.busy) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(next_cfg, s_gpio_cfg, sizeof(next_cfg));
    memcpy(old_cfg, s_gpio_cfg, sizeof(old_cfg));
    for (size_t i = 0; i < count; i++) {
        int role = role_from_name(updates[i].role);
        if (role < 0) {
            ret = ESP_ERR_INVALID_ARG;
            break;
        }
        next_cfg[role].gpio = updates[i].gpio;
        if (updates[i].active_high_set) {
            next_cfg[role].active_high = updates[i].active_high;
        }
        if (role != POWER_ROLE_LOCATOR) {
            next_cfg[role].logical_on = false;
        }
    }
    if (ret == ESP_OK) {
        ret = apply_gpio_cfg(next_cfg);
    }
    if (ret == ESP_OK) {
        release_unused_gpios(old_cfg, next_cfg);
        memcpy(s_gpio_cfg, next_cfg, sizeof(s_gpio_cfg));
        set_last_error(NULL, ESP_OK);
    } else {
        set_last_error("update gpio map", ret);
    }
    sync_status_locked();
    xSemaphoreGive(s_lock);

    if (ret == ESP_OK && persist) {
        esp_err_t save_ret = save_gpio_map_to_nvs(next_cfg);
        if (save_ret != ESP_OK) {
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                set_last_error("save gpio map", save_ret);
                sync_status_locked();
                xSemaphoreGive(s_lock);
            }
            return save_ret;
        }
    }
    return ret;
}

esp_err_t si_power_reset_gpio_map(bool persist)
{
    power_gpio_cfg_t defaults[POWER_ROLE_COUNT];
    for (int i = 0; i < POWER_ROLE_COUNT; i++) {
        defaults[i].gpio = s_role_defs[i].default_gpio;
        defaults[i].active_high = s_role_defs[i].default_active_high;
        defaults[i].logical_on = false;
    }

    si_power_gpio_update_t updates[POWER_ROLE_COUNT];
    for (int i = 0; i < POWER_ROLE_COUNT; i++) {
        updates[i].role = s_role_defs[i].role;
        updates[i].gpio = defaults[i].gpio;
        updates[i].active_high = defaults[i].active_high;
        updates[i].active_high_set = true;
    }
    return si_power_set_gpio_map(updates, POWER_ROLE_COUNT, persist);
}

esp_err_t si_power_set_locator(bool on)
{
    if (!SI_POWER_CFG_ENABLE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    int gpio = -1;
    bool active_high = true;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_status.initialized || s_status.busy) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    gpio = s_gpio_cfg[POWER_ROLE_LOCATOR].gpio;
    active_high = s_gpio_cfg[POWER_ROLE_LOCATOR].active_high;
    if (!gpio_valid(gpio)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreGive(s_lock);

    esp_err_t ret = gpio_set_level((gpio_num_t)gpio, logical_level(active_high, on));

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (ret == ESP_OK) {
            s_gpio_cfg[POWER_ROLE_LOCATOR].logical_on = on;
            strlcpy(s_status.last_action, on ? "locator_on" : "locator_off",
                    sizeof(s_status.last_action));
            s_status.action_count++;
            set_last_error(NULL, ESP_OK);
        } else {
            set_last_error("locator output", ret);
        }
        sync_status_locked();
        xSemaphoreGive(s_lock);
    }
    return ret;
}

esp_err_t si_power_toggle_locator(void)
{
    bool next = true;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        next = !s_gpio_cfg[POWER_ROLE_LOCATOR].logical_on;
        xSemaphoreGive(s_lock);
    }
    return si_power_set_locator(next);
}
