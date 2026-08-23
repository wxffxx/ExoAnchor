#include "device_settings.h"

#include <string.h>

#include "esp_check.h"
#include "settings_store.h"

#define DEVICE_NAMESPACE "si_device"
#define DEVICE_LABEL_KEY "label"
#define SESSION_NAMESPACE "si_session"
#define SESSION_AUTO_LOGOUT_ENABLED_KEY "auto_logout"
#define SESSION_AUTO_LOGOUT_MINUTES_KEY "auto_minutes"

static const char *TAG = "si-settings";

static char s_device_label[SI_DEVICE_LABEL_MAX_LEN + 1];
static bool s_device_label_loaded;
static si_session_settings_t s_session_settings = {
    .auto_logout_enabled = SI_SESSION_LOGOUT_DEFAULT_ENABLED,
    .auto_logout_minutes = SI_SESSION_LOGOUT_DEFAULT_MINUTES,
};
static bool s_session_settings_loaded;

static uint32_t clamp_u32(uint32_t value, uint32_t minimum, uint32_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static esp_err_t validate_device_label(const char *label)
{
    if (!label) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(label);
    if (len < 1 || len > SI_DEVICE_LABEL_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)label[i];
        if (ch < 32 || ch == 127) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static void load_device_label(void)
{
    if (s_device_label_loaded) {
        return;
    }

    strlcpy(s_device_label, SI_DEVICE_LABEL_DEFAULT, sizeof(s_device_label));
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t err = si_settings_store_open_read(&store, DEVICE_NAMESPACE);
    if (err == ESP_OK) {
        char label[sizeof(s_device_label)] = {0};
        err = si_settings_store_get_string(&store, DEVICE_LABEL_KEY,
                                           label, sizeof(label));
        si_settings_store_close(&store);
        if (err == ESP_OK && validate_device_label(label) == ESP_OK) {
            strlcpy(s_device_label, label, sizeof(s_device_label));
        }
    }
    s_device_label_loaded = true;
}

void si_device_label_get(char *label, size_t label_size)
{
    if (!label || label_size == 0) {
        return;
    }
    load_device_label();
    strlcpy(label, s_device_label, label_size);
}

esp_err_t si_device_label_set(const char *label)
{
    ESP_RETURN_ON_ERROR(validate_device_label(label), TAG, "validate device label");

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store, DEVICE_NAMESPACE),
                        TAG, "open device settings");
    esp_err_t ret = si_settings_store_set_string(&store, DEVICE_LABEL_KEY, label);
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    ESP_RETURN_ON_ERROR(ret, TAG, "save device settings");

    strlcpy(s_device_label, label, sizeof(s_device_label));
    s_device_label_loaded = true;
    return ESP_OK;
}

static void load_session_settings(void)
{
    if (s_session_settings_loaded) {
        return;
    }

    s_session_settings.auto_logout_enabled = SI_SESSION_LOGOUT_DEFAULT_ENABLED;
    s_session_settings.auto_logout_minutes = SI_SESSION_LOGOUT_DEFAULT_MINUTES;

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t err = si_settings_store_open_read(&store, SESSION_NAMESPACE);
    if (err == ESP_OK) {
        uint8_t enabled = 0;
        if (si_settings_store_get_u8(&store, SESSION_AUTO_LOGOUT_ENABLED_KEY,
                                     &enabled) == ESP_OK) {
            s_session_settings.auto_logout_enabled = enabled != 0;
        }
        uint32_t minutes = 0;
        if (si_settings_store_get_u32(&store, SESSION_AUTO_LOGOUT_MINUTES_KEY,
                                      &minutes) == ESP_OK) {
            s_session_settings.auto_logout_minutes =
                clamp_u32(minutes, SI_SESSION_LOGOUT_MIN_MINUTES,
                          SI_SESSION_LOGOUT_MAX_MINUTES);
        }
        si_settings_store_close(&store);
    }
    s_session_settings_loaded = true;
}

void si_session_settings_get(si_session_settings_t *settings)
{
    if (!settings) {
        return;
    }
    load_session_settings();
    *settings = s_session_settings;
}

esp_err_t si_session_settings_set(const si_session_settings_t *settings)
{
    if (!settings || settings->auto_logout_minutes < SI_SESSION_LOGOUT_MIN_MINUTES ||
        settings->auto_logout_minutes > SI_SESSION_LOGOUT_MAX_MINUTES) {
        return ESP_ERR_INVALID_ARG;
    }

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store, SESSION_NAMESPACE),
                        TAG, "open session settings");
    esp_err_t ret = si_settings_store_set_u8(
        &store, SESSION_AUTO_LOGOUT_ENABLED_KEY,
        settings->auto_logout_enabled ? 1 : 0);
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u32(&store, SESSION_AUTO_LOGOUT_MINUTES_KEY,
                                        settings->auto_logout_minutes);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    ESP_RETURN_ON_ERROR(ret, TAG, "save session settings");

    s_session_settings = *settings;
    s_session_settings_loaded = true;
    return ESP_OK;
}
