#include "target_profile_settings.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "settings_store.h"

#define TARGET_NAMESPACE "si_target"
#define TARGET_NAME_KEY "name"
#define TARGET_DEVICE_TYPE_KEY "device_type"
#define TARGET_OS_KEY "os"
#define TARGET_ENVIRONMENT_KEY "environment"
#define TARGET_LOCATION_KEY "location"
#define TARGET_CONFIGURATION_KEY "configuration"
#define TARGET_PURPOSE_KEY "purpose"
#define TARGET_NOTES_KEY "notes"

static const char *TAG = "si-target-profile";

static const char *const s_device_types[] = {
    "unknown", "desktop", "workstation", "server", "laptop", "embedded",
    "virtual_machine", "network_appliance", "other",
};
static const char *const s_operating_systems[] = {
    "unknown", "linux", "windows", "macos", "bsd", "appliance", "other",
};
static const char *const s_environments[] = {
    "unknown", "development", "testing", "staging", "production", "lab",
    "home", "other",
};

static bool value_in_list(const char *value, const char *const *values,
                          size_t value_count)
{
    if (!value) {
        return false;
    }
    for (size_t i = 0; i < value_count; i++) {
        if (strcmp(value, values[i]) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t validate_text(const char *value, size_t max_len,
                               bool allow_newlines)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(value);
    if (len > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (ch == 127 || (ch < 32 && !(allow_newlines &&
            (ch == '\n' || ch == '\r' || ch == '\t')))) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static esp_err_t validate_profile(const si_target_profile_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(validate_text(settings->name, SI_TARGET_NAME_MAX_LEN,
                                     false), TAG, "validate name");
    ESP_RETURN_ON_FALSE(
        value_in_list(settings->device_type, s_device_types,
                      sizeof(s_device_types) / sizeof(s_device_types[0])),
        ESP_ERR_INVALID_ARG, TAG, "invalid device type");
    ESP_RETURN_ON_FALSE(
        value_in_list(settings->operating_system, s_operating_systems,
                      sizeof(s_operating_systems) /
                      sizeof(s_operating_systems[0])),
        ESP_ERR_INVALID_ARG, TAG, "invalid operating system");
    ESP_RETURN_ON_FALSE(
        value_in_list(settings->environment, s_environments,
                      sizeof(s_environments) / sizeof(s_environments[0])),
        ESP_ERR_INVALID_ARG, TAG, "invalid environment");
    ESP_RETURN_ON_ERROR(validate_text(settings->location,
                                     SI_TARGET_LOCATION_MAX_LEN, false),
                        TAG, "validate location");
    ESP_RETURN_ON_ERROR(validate_text(settings->configuration,
                                     SI_TARGET_CONFIGURATION_MAX_LEN, true),
                        TAG, "validate configuration");
    ESP_RETURN_ON_ERROR(validate_text(settings->purpose,
                                     SI_TARGET_PURPOSE_MAX_LEN, true),
                        TAG, "validate purpose");
    return validate_text(settings->notes, SI_TARGET_NOTES_MAX_LEN, true);
}

void si_target_profile_defaults(si_target_profile_settings_t *settings)
{
    if (!settings) {
        return;
    }
    memset(settings, 0, sizeof(*settings));
    strlcpy(settings->device_type, "unknown",
            sizeof(settings->device_type));
    strlcpy(settings->operating_system, "unknown",
            sizeof(settings->operating_system));
    strlcpy(settings->environment, "unknown",
            sizeof(settings->environment));
}

static void read_string(si_settings_store_t *store, const char *key,
                        char *out, size_t out_size)
{
    char value[SI_TARGET_CONFIGURATION_MAX_LEN + 1] = {0};
    if (si_settings_store_get_string(store, key, value,
                                     sizeof(value)) == ESP_OK &&
        strlen(value) < out_size) {
        strlcpy(out, value, out_size);
    }
}

esp_err_t si_target_profile_load(si_target_profile_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    si_target_profile_defaults(settings);
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, TARGET_NAMESPACE);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "open target profile");
    read_string(&store, TARGET_NAME_KEY, settings->name,
                sizeof(settings->name));
    read_string(&store, TARGET_DEVICE_TYPE_KEY, settings->device_type,
                sizeof(settings->device_type));
    read_string(&store, TARGET_OS_KEY, settings->operating_system,
                sizeof(settings->operating_system));
    read_string(&store, TARGET_ENVIRONMENT_KEY, settings->environment,
                sizeof(settings->environment));
    read_string(&store, TARGET_LOCATION_KEY, settings->location,
                sizeof(settings->location));
    read_string(&store, TARGET_CONFIGURATION_KEY, settings->configuration,
                sizeof(settings->configuration));
    read_string(&store, TARGET_PURPOSE_KEY, settings->purpose,
                sizeof(settings->purpose));
    read_string(&store, TARGET_NOTES_KEY, settings->notes,
                sizeof(settings->notes));
    si_settings_store_close(&store);
    if (validate_profile(settings) != ESP_OK) {
        si_target_profile_defaults(settings);
    }
    return ESP_OK;
}

esp_err_t si_target_profile_save(const si_target_profile_settings_t *settings)
{
    ESP_RETURN_ON_ERROR(validate_profile(settings), TAG,
                        "validate target profile");
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store, TARGET_NAMESPACE),
                        TAG, "open target profile");

    esp_err_t ret = si_settings_store_set_string(&store, TARGET_NAME_KEY,
                                                 settings->name);
#define SAVE_FIELD(key, field)                                              \
    do {                                                                    \
        if (ret == ESP_OK) {                                                \
            ret = si_settings_store_set_string(&store, key, field);         \
        }                                                                   \
    } while (0)
    SAVE_FIELD(TARGET_DEVICE_TYPE_KEY, settings->device_type);
    SAVE_FIELD(TARGET_OS_KEY, settings->operating_system);
    SAVE_FIELD(TARGET_ENVIRONMENT_KEY, settings->environment);
    SAVE_FIELD(TARGET_LOCATION_KEY, settings->location);
    SAVE_FIELD(TARGET_CONFIGURATION_KEY, settings->configuration);
    SAVE_FIELD(TARGET_PURPOSE_KEY, settings->purpose);
    SAVE_FIELD(TARGET_NOTES_KEY, settings->notes);
#undef SAVE_FIELD
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}
