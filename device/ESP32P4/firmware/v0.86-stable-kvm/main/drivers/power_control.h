#pragma once

// ATX control and sensing public API.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#define SI_POWER_GPIO_MAP_MAX 9
#define SI_POWER_GPIO_ROLE_MAX 24
#define SI_POWER_GPIO_LABEL_MAX 32
#define SI_POWER_GPIO_DIRECTION_MAX 12
#define SI_POWER_GPIO_STATE_MAX 24

typedef struct {
    char role[SI_POWER_GPIO_ROLE_MAX];
    char label[SI_POWER_GPIO_LABEL_MAX];
    char direction[SI_POWER_GPIO_DIRECTION_MAX];
    int gpio;
    bool enabled;
    bool implemented;
    bool active_high;
    bool active;
    bool configurable;
    bool required;
    char state[SI_POWER_GPIO_STATE_MAX];
} si_power_gpio_map_entry_t;

typedef struct {
    const char *role;
    int gpio;
    bool active_high;
    bool active_high_set;
} si_power_gpio_update_t;

typedef struct {
    bool enabled;
    bool initialized;
    bool busy;
    bool active_high;
    bool power_detect_supported;
    bool standby_detect_supported;
    bool power_on;
    bool standby_on;
    bool locator_supported;
    bool locator_on;
    int power_button_gpio;
    int reset_button_gpio;
    int power_detect_gpio;
    int standby_detect_gpio;
    int reserve1_gpio;
    int reserve2_gpio;
    int reserve3_gpio;
    int reserve4_gpio;
    int locator_gpio;
    bool power_detect_active_high;
    bool standby_detect_active_high;
    bool locator_active_high;
    uint32_t default_press_ms;
    uint32_t force_off_ms;
    uint32_t action_count;
    char last_action[24];
    char last_error[96];
    size_t gpio_map_count;
    si_power_gpio_map_entry_t gpio_map[SI_POWER_GPIO_MAP_MAX];
} si_power_status_t;

esp_err_t si_power_init(void);
void si_power_get_status(si_power_status_t *status);
esp_err_t si_power_press_power(uint32_t duration_ms);
esp_err_t si_power_press_reset(uint32_t duration_ms);
esp_err_t si_power_force_off(uint32_t duration_ms);
esp_err_t si_power_set_gpio_map(const si_power_gpio_update_t *updates, size_t count, bool persist);
esp_err_t si_power_reset_gpio_map(bool persist);
esp_err_t si_power_set_locator(bool on);
esp_err_t si_power_toggle_locator(void);
