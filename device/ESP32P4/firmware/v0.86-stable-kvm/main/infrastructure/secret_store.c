#include "secret_store.h"

#include <string.h>

esp_err_t si_secret_store_get_string(si_settings_store_t *store,
                                     const char *key, char *out,
                                     size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, out_size);
    esp_err_t ret = si_settings_store_get_string(store, key, out, out_size);
    if (ret != ESP_OK || out[0] == '\0') {
        si_secret_store_clear(out, out_size);
        return ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;
    }
    return ESP_OK;
}

esp_err_t si_secret_store_set_string(si_settings_store_t *store,
                                     const char *key, const char *value)
{
    if (!value || value[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return si_settings_store_set_string(store, key, value);
}

esp_err_t si_secret_store_is_configured(si_settings_store_t *store,
                                        const char *key, bool *configured_out)
{
    if (!configured_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *configured_out = false;
    size_t stored_size = 0;
    esp_err_t ret = si_settings_store_get_string_size(store, key, &stored_size);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret == ESP_OK) {
        *configured_out = stored_size > 1;
    }
    return ret;
}

esp_err_t si_secret_store_erase(si_settings_store_t *store, const char *key)
{
    esp_err_t ret = si_settings_store_erase_key(store, key);
    return ret == ESP_ERR_NOT_FOUND ? ESP_OK : ret;
}

void si_secret_store_clear(void *buffer, size_t size)
{
    volatile unsigned char *bytes = buffer;
    while (bytes && size > 0) {
        *bytes++ = 0;
        size--;
    }
}
