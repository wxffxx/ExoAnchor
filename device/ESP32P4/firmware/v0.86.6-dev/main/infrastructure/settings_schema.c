#include "settings_schema.h"

#include "settings_store.h"

#define SETTINGS_SCHEMA_NAMESPACE "si_schema"
#define SETTINGS_SCHEMA_VERSION_KEY "version"
#define SETTINGS_SCHEMA_LAST_GOOD_KEY "last_good"
#define SETTINGS_SCHEMA_PENDING_KEY "pending"

static esp_err_t get_optional_u32(si_settings_store_t *store, const char *key,
                                  uint32_t *value, bool *found)
{
    esp_err_t ret = si_settings_store_get_u32(store, key, value);
    if (ret == ESP_ERR_NOT_FOUND) {
        *value = 0;
        *found = false;
        return ESP_OK;
    }
    *found = ret == ESP_OK;
    return ret;
}

static esp_err_t erase_optional(si_settings_store_t *store, const char *key)
{
    esp_err_t ret = si_settings_store_erase_key(store, key);
    return ret == ESP_ERR_NOT_FOUND ? ESP_OK : ret;
}

static esp_err_t run_migrations(uint32_t from_version, uint32_t to_version)
{
    while (from_version < to_version) {
        switch (from_version) {
        case 0:
            /*
             * Version 1 adopts the existing per-domain namespaces and keys.
             * No data rewrite is required, which makes this step idempotent.
             */
            from_version = 1;
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    return from_version == to_version ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_settings_schema_initialize(
    si_settings_schema_status_t *status_out)
{
    si_settings_schema_status_t status = {0};
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret =
        si_settings_store_open_write(&store, SETTINGS_SCHEMA_NAMESPACE);
    if (ret != ESP_OK) {
        return ret;
    }

    bool have_version = false;
    bool have_last_good = false;
    bool have_pending = false;
    ret = get_optional_u32(
        &store, SETTINGS_SCHEMA_VERSION_KEY, &status.version, &have_version);
    if (ret == ESP_OK) {
        ret = get_optional_u32(
            &store, SETTINGS_SCHEMA_LAST_GOOD_KEY,
            &status.last_good_version, &have_last_good);
    }
    if (ret == ESP_OK) {
        ret = get_optional_u32(
            &store, SETTINGS_SCHEMA_PENDING_KEY,
            &status.pending_version, &have_pending);
    }
    if (ret != ESP_OK) {
        si_settings_store_close(&store);
        return ret;
    }

    if (!have_version) {
        status.version = SI_SETTINGS_SCHEMA_CURRENT_VERSION;
        status.last_good_version = SI_SETTINGS_SCHEMA_CURRENT_VERSION;
        status.pending_version = 0;
        ret = si_settings_store_set_u32(
            &store, SETTINGS_SCHEMA_VERSION_KEY, status.version);
        if (ret == ESP_OK) {
            ret = si_settings_store_set_u32(
                &store, SETTINGS_SCHEMA_LAST_GOOD_KEY,
                status.last_good_version);
        }
        if (ret == ESP_OK) {
            ret = erase_optional(&store, SETTINGS_SCHEMA_PENDING_KEY);
        }
        if (ret == ESP_OK) {
            ret = si_settings_store_commit(&store);
        }
        status.initialized = ret == ESP_OK;
        si_settings_store_close(&store);
        if (status_out) {
            *status_out = status;
        }
        return ret;
    }

    if (!have_last_good) {
        status.last_good_version = status.version;
    }
    if (status.version > SI_SETTINGS_SCHEMA_CURRENT_VERSION ||
        (have_pending &&
         status.pending_version > SI_SETTINGS_SCHEMA_CURRENT_VERSION)) {
        status.recovery_required = true;
        si_settings_store_close(&store);
        if (status_out) {
            *status_out = status;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (have_pending && status.pending_version <= status.version) {
        ret = erase_optional(&store, SETTINGS_SCHEMA_PENDING_KEY);
        if (ret == ESP_OK) {
            ret = si_settings_store_commit(&store);
        }
        if (ret != ESP_OK) {
            status.recovery_required = true;
            si_settings_store_close(&store);
            if (status_out) {
                *status_out = status;
            }
            return ret;
        }
        have_pending = false;
        status.pending_version = 0;
    }

    if (status.version < SI_SETTINGS_SCHEMA_CURRENT_VERSION) {
        status.migration_resumed = have_pending;
        status.last_good_version = status.version;
        status.pending_version = SI_SETTINGS_SCHEMA_CURRENT_VERSION;
        ret = si_settings_store_set_u32(
            &store, SETTINGS_SCHEMA_LAST_GOOD_KEY,
            status.last_good_version);
        if (ret == ESP_OK) {
            ret = si_settings_store_set_u32(
                &store, SETTINGS_SCHEMA_PENDING_KEY,
                status.pending_version);
        }
        if (ret == ESP_OK) {
            ret = si_settings_store_commit(&store);
        }
        if (ret == ESP_OK) {
            ret = run_migrations(
                status.version, SI_SETTINGS_SCHEMA_CURRENT_VERSION);
        }
        if (ret == ESP_OK) {
            status.version = SI_SETTINGS_SCHEMA_CURRENT_VERSION;
            status.last_good_version = status.version;
            status.pending_version = 0;
            ret = si_settings_store_set_u32(
                &store, SETTINGS_SCHEMA_VERSION_KEY, status.version);
        }
        if (ret == ESP_OK) {
            ret = si_settings_store_set_u32(
                &store, SETTINGS_SCHEMA_LAST_GOOD_KEY,
                status.last_good_version);
        }
        if (ret == ESP_OK) {
            ret = erase_optional(&store, SETTINGS_SCHEMA_PENDING_KEY);
        }
        if (ret == ESP_OK) {
            ret = si_settings_store_commit(&store);
        }
        if (ret != ESP_OK) {
            status.recovery_required = true;
            si_settings_store_close(&store);
            if (status_out) {
                *status_out = status;
            }
            return ret;
        }
    }

    status.initialized = true;
    si_settings_store_close(&store);
    if (status_out) {
        *status_out = status;
    }
    return ESP_OK;
}
