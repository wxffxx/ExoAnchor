#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "console_credentials.h"
#include "nvs.h"
#include "secret_store.h"
#include "settings_schema.h"
#include "settings_store.h"

typedef struct {
    bool namespace_exists;
    bool open;
    bool writable;
    bool fail_commit;
    bool have_name;
    bool have_secret;
    bool have_count;
    bool have_console_username;
    bool have_console_password;
    bool have_schema_version;
    bool have_schema_last_good;
    bool have_schema_pending;
    char name[32];
    char secret[32];
    char console_username[SI_CONSOLE_USERNAME_MAX_LEN];
    char console_password[SI_CONSOLE_PASSWORD_MAX_LEN];
    uint32_t count;
    uint32_t schema_version;
    uint32_t schema_last_good;
    uint32_t schema_pending;
} fake_nvs_t;

static fake_nvs_t s_nvs;

static esp_err_t require_handle(nvs_handle_t handle, bool writable)
{
    if (handle != 1 || !s_nvs.open || (writable && !s_nvs.writable)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t mode,
                   nvs_handle_t *out_handle)
{
    if (!namespace_name || !out_handle ||
        (strcmp(namespace_name, "test") != 0 &&
         strcmp(namespace_name, "si_console") != 0 &&
         strcmp(namespace_name, "si_schema") != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode == NVS_READONLY && !s_nvs.namespace_exists) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    s_nvs.namespace_exists = true;
    s_nvs.open = true;
    s_nvs.writable = mode == NVS_READWRITE;
    *out_handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    if (handle == 1) {
        s_nvs.open = false;
        s_nvs.writable = false;
    }
}

static esp_err_t fake_string(const char *key, const char **value_out)
{
    if (strcmp(key, "name") == 0 && s_nvs.have_name) {
        *value_out = s_nvs.name;
        return ESP_OK;
    }
    if (strcmp(key, "secret") == 0 && s_nvs.have_secret) {
        *value_out = s_nvs.secret;
        return ESP_OK;
    }
    if (strcmp(key, "username") == 0 && s_nvs.have_console_username) {
        *value_out = s_nvs.console_username;
        return ESP_OK;
    }
    if (strcmp(key, "password") == 0 && s_nvs.have_console_password) {
        *value_out = s_nvs.console_password;
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out,
                      size_t *length)
{
    esp_err_t ret = require_handle(handle, false);
    if (ret != ESP_OK || !key || !length) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_ARG;
    }
    const char *value = NULL;
    ret = fake_string(key, &value);
    if (ret != ESP_OK) {
        return ret;
    }
    size_t required = strlen(value) + 1;
    if (!out) {
        *length = required;
        return ESP_OK;
    }
    if (*length < required) {
        *length = required;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, value, required);
    *length = required;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    esp_err_t ret = require_handle(handle, true);
    if (ret != ESP_OK || !key || !value) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_ARG;
    }
    if (strcmp(key, "name") == 0) {
        snprintf(s_nvs.name, sizeof(s_nvs.name), "%s", value);
        s_nvs.have_name = true;
        return ESP_OK;
    }
    if (strcmp(key, "secret") == 0) {
        snprintf(s_nvs.secret, sizeof(s_nvs.secret), "%s", value);
        s_nvs.have_secret = true;
        return ESP_OK;
    }
    if (strcmp(key, "username") == 0) {
        snprintf(s_nvs.console_username, sizeof(s_nvs.console_username),
                 "%s", value);
        s_nvs.have_console_username = true;
        return ESP_OK;
    }
    if (strcmp(key, "password") == 0) {
        snprintf(s_nvs.console_password, sizeof(s_nvs.console_password),
                 "%s", value);
        s_nvs.have_console_password = true;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out)
{
    esp_err_t ret = require_handle(handle, false);
    if (ret != ESP_OK || !key || !out) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_ARG;
    }
    if (strcmp(key, "count") == 0 && s_nvs.have_count) {
        *out = s_nvs.count;
        return ESP_OK;
    }
    if (strcmp(key, "version") == 0 && s_nvs.have_schema_version) {
        *out = s_nvs.schema_version;
        return ESP_OK;
    }
    if (strcmp(key, "last_good") == 0 && s_nvs.have_schema_last_good) {
        *out = s_nvs.schema_last_good;
        return ESP_OK;
    }
    if (strcmp(key, "pending") == 0 && s_nvs.have_schema_pending) {
        *out = s_nvs.schema_pending;
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    esp_err_t ret = require_handle(handle, true);
    if (ret != ESP_OK || !key) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_ARG;
    }
    if (strcmp(key, "count") == 0) {
        s_nvs.count = value;
        s_nvs.have_count = true;
        return ESP_OK;
    }
    if (strcmp(key, "version") == 0) {
        s_nvs.schema_version = value;
        s_nvs.have_schema_version = true;
        return ESP_OK;
    }
    if (strcmp(key, "last_good") == 0) {
        s_nvs.schema_last_good = value;
        s_nvs.have_schema_last_good = true;
        return ESP_OK;
    }
    if (strcmp(key, "pending") == 0) {
        s_nvs.schema_pending = value;
        s_nvs.have_schema_pending = true;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

#define DEFINE_UNUSED_NVS_GETTER(name, type)                                   \
    esp_err_t nvs_get_##name(nvs_handle_t handle, const char *key, type *out)   \
    {                                                                           \
        (void)handle;                                                           \
        (void)key;                                                              \
        (void)out;                                                              \
        return ESP_ERR_NVS_NOT_FOUND;                                           \
    }

#define DEFINE_UNUSED_NVS_SETTER(name, type)                                   \
    esp_err_t nvs_set_##name(nvs_handle_t handle, const char *key, type value)  \
    {                                                                           \
        (void)handle;                                                           \
        (void)key;                                                              \
        (void)value;                                                            \
        return ESP_OK;                                                          \
    }

DEFINE_UNUSED_NVS_GETTER(u8, uint8_t)
DEFINE_UNUSED_NVS_GETTER(u16, uint16_t)
DEFINE_UNUSED_NVS_GETTER(i32, int32_t)
DEFINE_UNUSED_NVS_SETTER(u8, uint8_t)
DEFINE_UNUSED_NVS_SETTER(u16, uint16_t)
DEFINE_UNUSED_NVS_SETTER(i32, int32_t)

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    esp_err_t ret = require_handle(handle, true);
    if (ret != ESP_OK || !key) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_ARG;
    }
    if (strcmp(key, "name") == 0 && s_nvs.have_name) {
        s_nvs.have_name = false;
        return ESP_OK;
    }
    if (strcmp(key, "secret") == 0 && s_nvs.have_secret) {
        s_nvs.have_secret = false;
        memset(s_nvs.secret, 0, sizeof(s_nvs.secret));
        return ESP_OK;
    }
    if (strcmp(key, "count") == 0 && s_nvs.have_count) {
        s_nvs.have_count = false;
        return ESP_OK;
    }
    if (strcmp(key, "username") == 0 && s_nvs.have_console_username) {
        s_nvs.have_console_username = false;
        memset(s_nvs.console_username, 0, sizeof(s_nvs.console_username));
        return ESP_OK;
    }
    if (strcmp(key, "password") == 0 && s_nvs.have_console_password) {
        s_nvs.have_console_password = false;
        memset(s_nvs.console_password, 0, sizeof(s_nvs.console_password));
        return ESP_OK;
    }
    if (strcmp(key, "version") == 0 && s_nvs.have_schema_version) {
        s_nvs.have_schema_version = false;
        return ESP_OK;
    }
    if (strcmp(key, "last_good") == 0 && s_nvs.have_schema_last_good) {
        s_nvs.have_schema_last_good = false;
        return ESP_OK;
    }
    if (strcmp(key, "pending") == 0 && s_nvs.have_schema_pending) {
        s_nvs.have_schema_pending = false;
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    esp_err_t ret = require_handle(handle, true);
    if (ret == ESP_OK) {
        s_nvs.have_name = false;
        s_nvs.have_secret = false;
        s_nvs.have_count = false;
        s_nvs.have_console_username = false;
        s_nvs.have_console_password = false;
        s_nvs.have_schema_version = false;
        s_nvs.have_schema_last_good = false;
        s_nvs.have_schema_pending = false;
        memset(s_nvs.console_username, 0, sizeof(s_nvs.console_username));
        memset(s_nvs.console_password, 0, sizeof(s_nvs.console_password));
    }
    return ret;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    esp_err_t ret = require_handle(handle, true);
    return ret == ESP_OK && s_nvs.fail_commit ? ESP_FAIL : ret;
}

static void test_missing_and_validation(void)
{
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    char value[8];
    assert(si_settings_store_open_read(&store, "test") == ESP_ERR_NOT_FOUND);
    assert(si_settings_store_get_string(&store, "name", value,
                                        sizeof(value)) == ESP_ERR_INVALID_STATE);
    assert(si_settings_store_open_write(NULL, "test") == ESP_ERR_INVALID_ARG);
    assert(si_settings_store_open_write(&store, "") == ESP_ERR_INVALID_ARG);
}

static void test_settings_round_trip(void)
{
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    assert(si_settings_store_open_write(&store, "test") == ESP_OK);
    assert(si_settings_store_set_string(&store, "name", "prototype0") == ESP_OK);
    assert(si_settings_store_set_u32(&store, "count", 32) == ESP_OK);
    assert(si_settings_store_commit(&store) == ESP_OK);
    si_settings_store_close(&store);

    assert(si_settings_store_open_read(&store, "test") == ESP_OK);
    char name[32];
    uint32_t count = 0;
    size_t stored_size = 0;
    assert(si_settings_store_get_string_size(&store, "name", &stored_size) == ESP_OK);
    assert(stored_size == strlen("prototype0") + 1);
    assert(si_settings_store_get_string(&store, "name", name,
                                        sizeof(name)) == ESP_OK);
    assert(strcmp(name, "prototype0") == 0);
    assert(si_settings_store_get_u32(&store, "count", &count) == ESP_OK);
    assert(count == 32);
    char too_small[4];
    assert(si_settings_store_get_string(&store, "name", too_small,
                                        sizeof(too_small)) == ESP_ERR_INVALID_SIZE);
    assert(si_settings_store_set_u32(&store, "count", 1) == ESP_ERR_INVALID_STATE);
    si_settings_store_close(&store);
}

static void test_secret_boundary(void)
{
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    assert(si_settings_store_open_write(&store, "test") == ESP_OK);
    assert(si_secret_store_set_string(&store, "secret", "") == ESP_ERR_INVALID_ARG);
    assert(si_secret_store_set_string(&store, "secret", "token") == ESP_OK);
    bool configured = false;
    assert(si_secret_store_is_configured(&store, "secret", &configured) == ESP_OK);
    assert(configured);
    char secret[16];
    assert(si_secret_store_get_string(&store, "secret", secret,
                                      sizeof(secret)) == ESP_OK);
    assert(strcmp(secret, "token") == 0);
    si_secret_store_clear(secret, sizeof(secret));
    for (size_t index = 0; index < sizeof(secret); index++) {
        assert(secret[index] == '\0');
    }
    assert(si_secret_store_erase(&store, "secret") == ESP_OK);
    assert(si_secret_store_erase(&store, "secret") == ESP_OK);
    assert(si_secret_store_is_configured(&store, "secret", &configured) == ESP_OK);
    assert(!configured);
    si_settings_store_close(&store);
}

static void test_commit_failure(void)
{
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    assert(si_settings_store_open_write(&store, "test") == ESP_OK);
    s_nvs.fail_commit = true;
    assert(si_settings_store_commit(&store) == ESP_FAIL);
    s_nvs.fail_commit = false;
    si_settings_store_close(&store);
}

static void test_console_credentials(void)
{
    si_console_credentials_status_t status;
    assert(si_console_credentials_get_status(&status) == ESP_OK);
    assert(!status.configured);
    assert(!status.password_configured);
    assert(status.username[0] == '\0');

    assert(si_console_credentials_save("", "secret", true, false) ==
           ESP_ERR_INVALID_ARG);
    assert(si_console_credentials_save(" console", "secret", true, false) ==
           ESP_ERR_INVALID_ARG);
    assert(si_console_credentials_save("console", "", true, false) ==
           ESP_ERR_INVALID_ARG);
    assert(si_console_credentials_save("console", "line\nbreak", true,
                                       false) == ESP_ERR_INVALID_ARG);
    assert(si_console_credentials_save("用户", "secret", true, false) ==
           ESP_ERR_INVALID_ARG);

    assert(si_console_credentials_save("ubuntu", "console-secret", true,
                                       false) == ESP_OK);
    assert(si_console_credentials_get_status(&status) == ESP_OK);
    assert(status.configured);
    assert(status.password_configured);
    assert(strcmp(status.username, "ubuntu") == 0);

    char password[SI_CONSOLE_PASSWORD_MAX_LEN];
    assert(si_console_credentials_get_password(password, sizeof(password)) ==
           ESP_OK);
    assert(strcmp(password, "console-secret") == 0);
    si_secret_store_clear(password, sizeof(password));

    assert(si_console_credentials_save("operator", NULL, false, false) ==
           ESP_OK);
    assert(si_console_credentials_get_status(&status) == ESP_OK);
    assert(strcmp(status.username, "operator") == 0);
    assert(status.password_configured);

    assert(si_console_credentials_save("operator", NULL, false, true) ==
           ESP_OK);
    assert(si_console_credentials_get_status(&status) == ESP_OK);
    assert(status.configured);
    assert(!status.password_configured);
    assert(si_console_credentials_get_password(password, sizeof(password)) ==
           ESP_ERR_NOT_FOUND);

    assert(si_console_credentials_save("operator", "new-secret", true,
                                       false) == ESP_OK);
    assert(si_console_credentials_clear() == ESP_OK);
    assert(si_console_credentials_get_status(&status) == ESP_OK);
    assert(!status.configured);
    assert(!status.password_configured);
}

static void test_settings_schema(void)
{
    si_settings_schema_status_t status;

    memset(&s_nvs, 0, sizeof(s_nvs));
    assert(si_settings_schema_initialize(&status) == ESP_OK);
    assert(status.initialized);
    assert(status.version == SI_SETTINGS_SCHEMA_CURRENT_VERSION);
    assert(status.last_good_version == SI_SETTINGS_SCHEMA_CURRENT_VERSION);
    assert(status.pending_version == 0);
    assert(s_nvs.have_schema_version);
    assert(s_nvs.have_schema_last_good);
    assert(!s_nvs.have_schema_pending);

    memset(&status, 0, sizeof(status));
    assert(si_settings_schema_initialize(&status) == ESP_OK);
    assert(status.initialized);
    assert(!status.migration_resumed);

    s_nvs.schema_version = SI_SETTINGS_SCHEMA_CURRENT_VERSION + 1;
    memset(&status, 0, sizeof(status));
    assert(si_settings_schema_initialize(&status) == ESP_ERR_NOT_SUPPORTED);
    assert(status.recovery_required);

    memset(&s_nvs, 0, sizeof(s_nvs));
    s_nvs.namespace_exists = true;
    s_nvs.have_schema_version = true;
    s_nvs.schema_version = 0;
    s_nvs.have_schema_pending = true;
    s_nvs.schema_pending = SI_SETTINGS_SCHEMA_CURRENT_VERSION;
    assert(si_settings_schema_initialize(&status) == ESP_OK);
    assert(status.initialized);
    assert(status.migration_resumed);
    assert(status.version == SI_SETTINGS_SCHEMA_CURRENT_VERSION);
    assert(!s_nvs.have_schema_pending);
}

int main(void)
{
    memset(&s_nvs, 0, sizeof(s_nvs));
    test_missing_and_validation();
    test_settings_round_trip();
    test_secret_boundary();
    test_console_credentials();
    test_commit_failure();
    test_settings_schema();
    puts("host settings store tests: PASS");
    return 0;
}
