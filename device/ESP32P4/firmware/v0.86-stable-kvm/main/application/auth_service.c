#include "auth_service.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "mbedtls/sha256.h"
#include "secret_store.h"
#include "settings_store.h"

#define AUTH_NAMESPACE "si_auth"
#define AUTH_USERNAME_KEY "username"
#define AUTH_PASSWORD_HASH_KEY "password_hash"
#define AUTH_LOGIN_COUNT_KEY "login_count"

static const char *TAG = "si-auth";

static char s_username[SI_AUTH_USERNAME_MAX_LEN + 1];
static char s_hash[SI_AUTH_TOKEN_LEN + 1];
static bool s_loaded;
static bool s_using_default;
static bool s_login_count_loaded;
static uint32_t s_login_count;

static bool secure_equal(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    size_t a_len = strlen(a);
    size_t b_len = strlen(b);
    unsigned char diff = (unsigned char)(a_len ^ b_len);
    size_t max_len = a_len > b_len ? a_len : b_len;
    for (size_t i = 0; i < max_len; i++) {
        unsigned char a_ch = i < a_len ? (unsigned char)a[i] : 0;
        unsigned char b_ch = i < b_len ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(a_ch ^ b_ch);
    }
    return diff == 0;
}

static esp_err_t hash_password(const char *password, char out[SI_AUTH_TOKEN_LEN + 1])
{
    if (!password || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    unsigned char digest[32] = {0};
    int hash_ret = mbedtls_sha256((const unsigned char *)password,
                                  strlen(password), digest, 0);
    if (hash_ret != 0) {
        si_secret_store_clear(digest, sizeof(digest));
        return ESP_FAIL;
    }
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(out + (i * 2), 3, "%02x", digest[i]);
    }
    si_secret_store_clear(digest, sizeof(digest));
    out[SI_AUTH_TOKEN_LEN] = '\0';
    return ESP_OK;
}

const char *si_auth_default_username(void)
{
    return SI_CFG_AUTH_USERNAME[0] ? SI_CFG_AUTH_USERNAME : "admin";
}

esp_err_t si_auth_validate_username(const char *username)
{
    if (!username) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(username);
    if (len < 1 || len > SI_AUTH_USERNAME_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)username[i];
        if (ch < 33 || ch > 126) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t si_auth_validate_password(const char *password)
{
    if (!password) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(password);
    if (len < SI_AUTH_PASSWORD_MIN_LEN || len > SI_AUTH_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)password[i];
        if (ch < 33 || ch > 126) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static void load_credentials(void)
{
    if (s_loaded) {
        return;
    }

    strlcpy(s_username, si_auth_default_username(), sizeof(s_username));
    s_hash[0] = '\0';
    s_using_default = true;

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t err = si_settings_store_open_read(&store, AUTH_NAMESPACE);
    if (err == ESP_OK) {
        err = si_secret_store_get_string(&store, AUTH_PASSWORD_HASH_KEY,
                                         s_hash, sizeof(s_hash));
        char username[sizeof(s_username)] = {0};
        esp_err_t username_err = si_settings_store_get_string(
            &store, AUTH_USERNAME_KEY, username, sizeof(username));
        si_settings_store_close(&store);
        if (err == ESP_OK && strlen(s_hash) == SI_AUTH_TOKEN_LEN) {
            if (username_err == ESP_OK && si_auth_validate_username(username) == ESP_OK) {
                strlcpy(s_username, username, sizeof(s_username));
            }
            s_using_default = false;
            s_loaded = true;
            return;
        }
    }

    if (SI_CFG_AUTH_PASSWORD[0] != '\0' &&
        hash_password(SI_CFG_AUTH_PASSWORD, s_hash) != ESP_OK) {
        s_hash[0] = '\0';
    }
    s_loaded = true;
}

bool si_auth_is_enabled(void)
{
    load_credentials();
    return s_hash[0] != '\0';
}

bool si_auth_credentials_match(const char *username, const char *password)
{
    char hash[SI_AUTH_TOKEN_LEN + 1];
    if (!username || !password || hash_password(password, hash) != ESP_OK) {
        return false;
    }
    load_credentials();
    bool matches = secure_equal(username, s_username) && secure_equal(hash, s_hash);
    si_secret_store_clear(hash, sizeof(hash));
    return matches;
}

bool si_auth_token_matches(const char *token)
{
    if (!token || !si_auth_is_enabled()) {
        return false;
    }
    return secure_equal(token, s_hash);
}

uint32_t si_auth_login_count(void)
{
    if (s_login_count_loaded) {
        return s_login_count;
    }
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    if (si_settings_store_open_read(&store, AUTH_NAMESPACE) == ESP_OK) {
        (void)si_settings_store_get_u32(&store, AUTH_LOGIN_COUNT_KEY,
                                        &s_login_count);
        si_settings_store_close(&store);
    }
    s_login_count_loaded = true;
    return s_login_count;
}

void si_auth_record_login(void)
{
    uint32_t count = si_auth_login_count() + 1U;
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    if (si_settings_store_open_write(&store, AUTH_NAMESPACE) == ESP_OK) {
        if (si_settings_store_set_u32(&store, AUTH_LOGIN_COUNT_KEY, count) == ESP_OK &&
            si_settings_store_commit(&store) == ESP_OK) {
            s_login_count = count;
        }
        si_settings_store_close(&store);
    } else {
        s_login_count = count;
    }
}

esp_err_t si_auth_set_credentials(const char *username, const char *password)
{
    char hash[SI_AUTH_TOKEN_LEN + 1];
    ESP_RETURN_ON_ERROR(si_auth_validate_username(username), TAG, "validate username");
    ESP_RETURN_ON_ERROR(si_auth_validate_password(password), TAG, "validate password");
    ESP_RETURN_ON_ERROR(hash_password(password, hash), TAG, "hash password");

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(&store, AUTH_NAMESPACE);
    if (ret != ESP_OK) {
        si_secret_store_clear(hash, sizeof(hash));
        return ret;
    }
    ret = si_settings_store_set_string(&store, AUTH_USERNAME_KEY, username);
    if (ret == ESP_OK) {
        ret = si_secret_store_set_string(&store, AUTH_PASSWORD_HASH_KEY, hash);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    if (ret != ESP_OK) {
        si_secret_store_clear(hash, sizeof(hash));
        return ret;
    }

    strlcpy(s_username, username, sizeof(s_username));
    strlcpy(s_hash, hash, sizeof(s_hash));
    si_secret_store_clear(hash, sizeof(hash));
    s_loaded = true;
    s_using_default = false;
    return ESP_OK;
}

void si_auth_get_status(si_auth_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    load_credentials();
    status->enabled = s_hash[0] != '\0';
    status->using_default = s_using_default;
    status->login_count = si_auth_login_count();
    strlcpy(status->username, s_username, sizeof(status->username));
    strlcpy(status->token, s_hash, sizeof(status->token));
}
