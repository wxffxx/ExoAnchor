#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "settings_store.h"

/*
 * Access boundary for secret strings stored in an existing settings
 * transaction. It centralizes non-empty checks, presence probes, and explicit
 * buffer clearing. This facade does not encrypt NVS data at rest; encrypted
 * storage and key provisioning remain separate platform capabilities.
 */
esp_err_t si_secret_store_get_string(si_settings_store_t *store,
                                     const char *key, char *out,
                                     size_t out_size);
esp_err_t si_secret_store_set_string(si_settings_store_t *store,
                                     const char *key, const char *value);
esp_err_t si_secret_store_is_configured(si_settings_store_t *store,
                                        const char *key, bool *configured_out);
esp_err_t si_secret_store_erase(si_settings_store_t *store, const char *key);
void si_secret_store_clear(void *buffer, size_t size);
