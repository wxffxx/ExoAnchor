#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "network_config.h"
#include "network_settings.h"
#include "nvs.h"

typedef enum {
    VALUE_NONE,
    VALUE_U8,
    VALUE_U32,
    VALUE_STRING,
} value_type_t;

typedef struct {
    char key[16];
    value_type_t type;
    uint32_t number;
    char string[80];
} value_t;

static value_t s_values[32];
static bool s_open;
static bool s_writable;
static size_t s_nvs_read_calls;

static value_t *find_value(const char *key, bool create)
{
    value_t *empty = NULL;
    for (size_t i = 0; i < sizeof(s_values) / sizeof(s_values[0]); ++i) {
        if (s_values[i].type != VALUE_NONE &&
            strcmp(s_values[i].key, key) == 0) {
            return &s_values[i];
        }
        if (!empty && s_values[i].type == VALUE_NONE) {
            empty = &s_values[i];
        }
    }
    if (create && empty) {
        snprintf(empty->key, sizeof(empty->key), "%s", key);
        return empty;
    }
    return NULL;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t mode,
                   nvs_handle_t *out_handle)
{
    if (!namespace_name || strcmp(namespace_name, "si_net_cfg") != 0 ||
        !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    s_open = true;
    s_writable = mode == NVS_READWRITE;
    *out_handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    if (handle == 1) {
        s_open = false;
        s_writable = false;
    }
}

static esp_err_t require_handle(nvs_handle_t handle, bool writable)
{
    return handle == 1 && s_open && (!writable || s_writable)
               ? ESP_OK
               : ESP_ERR_INVALID_STATE;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out,
                      size_t *length)
{
    s_nvs_read_calls++;
    esp_err_t ret = require_handle(handle, false);
    value_t *value = find_value(key, false);
    if (ret != ESP_OK || !key || !length) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_ARG;
    }
    if (!value || value->type != VALUE_STRING) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t required = strlen(value->string) + 1;
    if (!out) {
        *length = required;
        return ESP_OK;
    }
    if (*length < required) {
        *length = required;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, value->string, required);
    *length = required;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key,
                      const char *input)
{
    esp_err_t ret = require_handle(handle, true);
    value_t *value = find_value(key, true);
    if (ret != ESP_OK || !value || !input ||
        strlen(input) >= sizeof(value->string)) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_ARG;
    }
    value->type = VALUE_STRING;
    snprintf(value->string, sizeof(value->string), "%s", input);
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    s_nvs_read_calls++;
    esp_err_t ret = require_handle(handle, false);
    value_t *value = find_value(key, false);
    if (ret != ESP_OK || !out) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_ARG;
    }
    if (!value || value->type != VALUE_U8) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out = (uint8_t)value->number;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t input)
{
    esp_err_t ret = require_handle(handle, true);
    value_t *value = find_value(key, true);
    if (ret != ESP_OK || !value) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_ARG;
    }
    value->type = VALUE_U8;
    value->number = input;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out)
{
    s_nvs_read_calls++;
    esp_err_t ret = require_handle(handle, false);
    value_t *value = find_value(key, false);
    if (ret != ESP_OK || !out) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_ARG;
    }
    if (!value || value->type != VALUE_U32) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out = value->number;
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t input)
{
    esp_err_t ret = require_handle(handle, true);
    value_t *value = find_value(key, true);
    if (ret != ESP_OK || !value) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_ARG;
    }
    value->type = VALUE_U32;
    value->number = input;
    return ESP_OK;
}

#define UNUSED_GETTER(name, type)                                              \
    esp_err_t nvs_get_##name(nvs_handle_t handle, const char *key, type *out)  \
    {                                                                          \
        (void)handle;                                                          \
        (void)key;                                                             \
        (void)out;                                                             \
        return ESP_ERR_NVS_NOT_FOUND;                                          \
    }

#define UNUSED_SETTER(name, type)                                              \
    esp_err_t nvs_set_##name(nvs_handle_t handle, const char *key, type input) \
    {                                                                          \
        (void)handle;                                                          \
        (void)key;                                                             \
        (void)input;                                                           \
        return ESP_OK;                                                         \
    }

UNUSED_GETTER(u16, uint16_t)
UNUSED_GETTER(i32, int32_t)
UNUSED_SETTER(u16, uint16_t)
UNUSED_SETTER(i32, int32_t)

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    esp_err_t ret = require_handle(handle, true);
    value_t *value = find_value(key, false);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!value) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memset(value, 0, sizeof(*value));
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    esp_err_t ret = require_handle(handle, true);
    if (ret == ESP_OK) {
        memset(s_values, 0, sizeof(s_values));
    }
    return ret;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    return require_handle(handle, true);
}

static si_network_config_t factory_config(void)
{
    si_network_config_t config;
    si_network_config_default(&config, "exoanchor-123456");
    return config;
}

static si_network_config_t static_config(void)
{
    si_network_config_t config = factory_config();
    config.mode = SI_NETWORK_MODE_STATIC;
    snprintf(config.address, sizeof(config.address), "192.0.2.223");
    snprintf(config.netmask, sizeof(config.netmask), "255.255.255.0");
    snprintf(config.gateway, sizeof(config.gateway), "192.0.2.1");
    snprintf(config.dns_primary, sizeof(config.dns_primary), "1.1.1.1");
    return config;
}

int main(void)
{
    si_network_config_t defaults = factory_config();
    si_network_settings_status_t status;
    assert(si_network_settings_get(&status) == ESP_ERR_INVALID_STATE);
    assert(s_nvs_read_calls == 0);
    assert(si_network_settings_initialize(&defaults, &status) == ESP_OK);
    assert(status.active_slot == 1);
    assert(status.active.mode == SI_NETWORK_MODE_DHCP);
    assert(!status.have_staged);

    si_network_config_t static_ip = static_config();
    assert(si_network_settings_stage(&static_ip) == ESP_OK);
    size_t reads_before_snapshot = s_nvs_read_calls;
    assert(si_network_settings_get(&status) == ESP_OK);
    assert(s_nvs_read_calls == reads_before_snapshot);
    assert(status.have_staged);
    assert(status.staged.generation == 1);
    assert(si_network_settings_mark_pending() == ESP_OK);
    reads_before_snapshot = s_nvs_read_calls;
    assert(si_network_settings_get(&status) == ESP_OK);
    assert(s_nvs_read_calls == reads_before_snapshot);
    assert(status.pending);

    assert(si_network_settings_initialize(&defaults, &status) == ESP_OK);
    assert(status.recovered_pending);
    assert(status.active.mode == SI_NETWORK_MODE_DHCP);
    assert(!status.have_staged);

    assert(si_network_settings_stage(&static_ip) == ESP_OK);
    assert(si_network_settings_mark_pending() == ESP_OK);
    assert(si_network_settings_confirm() == ESP_OK);
    reads_before_snapshot = s_nvs_read_calls;
    assert(si_network_settings_get(&status) == ESP_OK);
    assert(s_nvs_read_calls == reads_before_snapshot);
    assert(status.active.mode == SI_NETWORK_MODE_STATIC);
    assert(status.active.generation == 1);

    si_network_config_t changed = defaults;
    snprintf(changed.hostname, sizeof(changed.hostname), "rack-kvm-2");
    assert(si_network_settings_stage(&changed) == ESP_OK);
    assert(si_network_settings_rollback() == ESP_OK);
    reads_before_snapshot = s_nvs_read_calls;
    assert(si_network_settings_get(&status) == ESP_OK);
    assert(s_nvs_read_calls == reads_before_snapshot);
    assert(status.active.mode == SI_NETWORK_MODE_STATIC);
    assert(!status.have_staged);

    assert(si_network_settings_reset(&defaults) == ESP_OK);
    reads_before_snapshot = s_nvs_read_calls;
    assert(si_network_settings_get(&status) == ESP_OK);
    assert(s_nvs_read_calls == reads_before_snapshot);
    assert(status.active.mode == SI_NETWORK_MODE_DHCP);
    assert(strcmp(status.active.hostname, "exoanchor-123456") == 0);
    puts("host network settings tests: PASS");
    return 0;
}
