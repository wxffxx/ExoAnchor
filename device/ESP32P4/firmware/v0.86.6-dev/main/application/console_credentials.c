#include "console_credentials.h"

#include <ctype.h>
#include <string.h>

#include "hid_ascii.h"
#include "secret_store.h"
#include "settings_store.h"

#define CONSOLE_CREDENTIALS_NAMESPACE "si_console"
#define CONSOLE_USERNAME_KEY "username"
#define CONSOLE_PASSWORD_KEY "password"

static bool console_username_valid(const char *username)
{
    if (!username) {
        return false;
    }
    size_t length = strlen(username);
    if (length == 0 || length >= SI_CONSOLE_USERNAME_MAX_LEN ||
        isspace((unsigned char)username[0]) ||
        isspace((unsigned char)username[length - 1])) {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        si_hid_ascii_key_t key;
        if (!si_hid_ascii_map((unsigned char)username[index], &key)) {
            return false;
        }
    }
    return true;
}

static bool console_password_valid(const char *password)
{
    if (!password) {
        return false;
    }
    size_t length = strlen(password);
    if (length == 0 || length >= SI_CONSOLE_PASSWORD_MAX_LEN) {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        si_hid_ascii_key_t key;
        if (!si_hid_ascii_map((unsigned char)password[index], &key)) {
            return false;
        }
    }
    return true;
}

esp_err_t si_console_credentials_get_status(
    si_console_credentials_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret =
        si_settings_store_open_read(&store, CONSOLE_CREDENTIALS_NAMESPACE);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = si_settings_store_get_string(&store, CONSOLE_USERNAME_KEY,
                                       status->username,
                                       sizeof(status->username));
    if (ret == ESP_ERR_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = si_secret_store_is_configured(&store, CONSOLE_PASSWORD_KEY,
                                            &status->password_configured);
    }
    si_settings_store_close(&store);
    if (ret != ESP_OK) {
        memset(status, 0, sizeof(*status));
        return ret;
    }
    status->configured = status->username[0] != '\0';
    return ESP_OK;
}

esp_err_t si_console_credentials_save(const char *username,
                                      const char *password,
                                      bool password_present,
                                      bool clear_password)
{
    if (!console_username_valid(username)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password_present && !console_password_valid(password)) {
        return ESP_ERR_INVALID_ARG;
    }

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret =
        si_settings_store_open_write(&store, CONSOLE_CREDENTIALS_NAMESPACE);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = si_settings_store_set_string(&store, CONSOLE_USERNAME_KEY, username);
    if (ret == ESP_OK && clear_password) {
        ret = si_secret_store_erase(&store, CONSOLE_PASSWORD_KEY);
    } else if (ret == ESP_OK && password_present) {
        ret = si_secret_store_set_string(&store, CONSOLE_PASSWORD_KEY,
                                         password);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}

esp_err_t si_console_credentials_clear(void)
{
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret =
        si_settings_store_open_write(&store, CONSOLE_CREDENTIALS_NAMESPACE);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = si_settings_store_erase_all(&store);
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}

esp_err_t si_console_credentials_get_password(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    si_secret_store_clear(out, out_size);

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret =
        si_settings_store_open_read(&store, CONSOLE_CREDENTIALS_NAMESPACE);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = si_secret_store_get_string(&store, CONSOLE_PASSWORD_KEY, out,
                                     out_size);
    si_settings_store_close(&store);
    return ret;
}
