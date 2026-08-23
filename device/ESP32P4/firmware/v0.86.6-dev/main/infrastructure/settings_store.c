#include "settings_store.h"

#include <string.h>

#include "nvs.h"

#ifdef ESP_PLATFORM
#include "esp_memory_utils.h"
#include "sdkconfig.h"
#endif

/*
 * ESP-IDF disables the flash cache while NVS accesses the partition.  A task
 * whose current stack lives in PSRAM cannot survive that transition.  Keep
 * this invariant at the shared facade so a newly added external-stack caller
 * fails closed instead of reaching cache_utils.c's fatal assertion.
 */
static bool current_task_can_access_flash(void)
{
#ifdef ESP_PLATFORM
    volatile uint8_t stack_probe = 0;
    return esp_ptr_in_dram((const void *)&stack_probe)
#if CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP
           || esp_ptr_in_rtc_dram_fast((const void *)&stack_probe)
#endif
        ;
#else
    return true;
#endif
}

static esp_err_t normalize_error(esp_err_t error)
{
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (error == ESP_ERR_NVS_INVALID_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    return error;
}

static esp_err_t validate_store(const si_settings_store_t *store,
                                bool require_write)
{
    if (!store || !store->open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (require_write && !store->writable) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!current_task_can_access_flash()) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static nvs_handle_t raw_nvs_handle(const si_settings_store_t *store)
{
    return (nvs_handle_t)store->handle;
}

static esp_err_t open_store(si_settings_store_t *store,
                            const char *namespace_name, nvs_open_mode_t mode)
{
    if (!store || !namespace_name || namespace_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (store->open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!current_task_can_access_flash()) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    esp_err_t ret = normalize_error(nvs_open(namespace_name, mode, &handle));
    if (ret != ESP_OK) {
        return ret;
    }
    store->handle = (uintptr_t)handle;
    store->open = true;
    store->writable = mode == NVS_READWRITE;
    return ESP_OK;
}

esp_err_t si_settings_store_open_read(si_settings_store_t *store,
                                      const char *namespace_name)
{
    return open_store(store, namespace_name, NVS_READONLY);
}

esp_err_t si_settings_store_open_write(si_settings_store_t *store,
                                       const char *namespace_name)
{
    return open_store(store, namespace_name, NVS_READWRITE);
}

void si_settings_store_close(si_settings_store_t *store)
{
    if (!store || !store->open) {
        return;
    }
    nvs_close(raw_nvs_handle(store));
    memset(store, 0, sizeof(*store));
}

esp_err_t si_settings_store_get_string(si_settings_store_t *store,
                                       const char *key, char *out,
                                       size_t out_size)
{
    if (!key || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = validate_store(store, false);
    if (ret != ESP_OK) {
        return ret;
    }
    size_t length = out_size;
    return normalize_error(nvs_get_str(raw_nvs_handle(store), key, out, &length));
}

esp_err_t si_settings_store_get_string_size(si_settings_store_t *store,
                                            const char *key,
                                            size_t *size_out)
{
    if (!key || !size_out) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = validate_store(store, false);
    if (ret != ESP_OK) {
        return ret;
    }
    *size_out = 0;
    return normalize_error(nvs_get_str(raw_nvs_handle(store), key, NULL, size_out));
}

#define DEFINE_GETTER(name, type, nvs_fn)                                      \
    esp_err_t si_settings_store_get_##name(si_settings_store_t *store,          \
                                            const char *key, type *out)          \
    {                                                                           \
        if (!key || !out) {                                                     \
            return ESP_ERR_INVALID_ARG;                                         \
        }                                                                       \
        esp_err_t ret = validate_store(store, false);                            \
        return ret == ESP_OK ? normalize_error(                                \
            nvs_fn(raw_nvs_handle(store), key, out)) : ret;                     \
    }

DEFINE_GETTER(u8, uint8_t, nvs_get_u8)
DEFINE_GETTER(u16, uint16_t, nvs_get_u16)
DEFINE_GETTER(u32, uint32_t, nvs_get_u32)
DEFINE_GETTER(i32, int32_t, nvs_get_i32)

#undef DEFINE_GETTER

esp_err_t si_settings_store_set_string(si_settings_store_t *store,
                                       const char *key, const char *value)
{
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = validate_store(store, true);
    return ret == ESP_OK ? normalize_error(
        nvs_set_str(raw_nvs_handle(store), key, value)) : ret;
}

#define DEFINE_SETTER(name, type, nvs_fn)                                      \
    esp_err_t si_settings_store_set_##name(si_settings_store_t *store,          \
                                            const char *key, type value)         \
    {                                                                           \
        if (!key) {                                                             \
            return ESP_ERR_INVALID_ARG;                                         \
        }                                                                       \
        esp_err_t ret = validate_store(store, true);                             \
        return ret == ESP_OK ? normalize_error(                                \
            nvs_fn(raw_nvs_handle(store), key, value)) : ret;                   \
    }

DEFINE_SETTER(u8, uint8_t, nvs_set_u8)
DEFINE_SETTER(u16, uint16_t, nvs_set_u16)
DEFINE_SETTER(u32, uint32_t, nvs_set_u32)
DEFINE_SETTER(i32, int32_t, nvs_set_i32)

#undef DEFINE_SETTER

esp_err_t si_settings_store_erase_key(si_settings_store_t *store,
                                      const char *key)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = validate_store(store, true);
    return ret == ESP_OK ? normalize_error(
        nvs_erase_key(raw_nvs_handle(store), key)) : ret;
}

esp_err_t si_settings_store_erase_all(si_settings_store_t *store)
{
    esp_err_t ret = validate_store(store, true);
    return ret == ESP_OK ? normalize_error(
        nvs_erase_all(raw_nvs_handle(store))) : ret;
}

esp_err_t si_settings_store_commit(si_settings_store_t *store)
{
    esp_err_t ret = validate_store(store, true);
    return ret == ESP_OK ? normalize_error(nvs_commit(raw_nvs_handle(store))) : ret;
}
