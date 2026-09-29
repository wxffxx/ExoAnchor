#include "auth_service.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "device_settings.h"
#include "esp_check.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha256.h"
#include "secret_store.h"
#include "settings_store.h"

#define AUTH_NAMESPACE "si_auth"
#define AUTH_USERNAME_KEY "username"
#define AUTH_PASSWORD_HASH_KEY "password_hash"
#define AUTH_LOGIN_COUNT_KEY "login_count"
#define AUTH_BOOTSTRAP_KEY "bootstrap"
#define AUTH_SALT_BYTES 16U
#define AUTH_SALT_HEX_LEN 32U
#define AUTH_VERIFIER_BYTES 32U
#define AUTH_ITERATIONS 60000U
#define AUTH_SESSION_SLOTS 16U
#define AUTH_ABSOLUTE_US (24LL * 60LL * 60LL * 1000000LL)
static const char *TAG = "si-auth";
static uint32_t s_credential_generation = 1U;
uint32_t si_auth_credential_generation(void) { return __atomic_load_n(&s_credential_generation, __ATOMIC_ACQUIRE); }
static char s_username[SI_AUTH_USERNAME_MAX_LEN + 1];
static char s_hash[SI_AUTH_TOKEN_LEN + 1];
static char s_salt[AUTH_SALT_HEX_LEN + 1];
static bool s_loaded, s_initialized, s_using_default, s_legacy, s_login_count_loaded;
static uint32_t s_login_count;
static bool s_setup_pending;
static SemaphoreHandle_t s_lock;
static uint32_t s_failures;
static int64_t s_failure_started, s_locked_until;
typedef struct {
    uint8_t digest[32];
    int64_t created, touched;
    bool used;
    bool browser;
} auth_session_t;
static auth_session_t s_sessions[AUTH_SESSION_SLOTS];
static esp_err_t set_credentials_internal(const char *, const char *, bool, bool, bool, bool);

static bool secure_equal(const char *a, const char *b)
{
    if (!a || !b) return false;
    size_t al = strlen(a), bl = strlen(b);
    unsigned diff = (unsigned)(al ^ bl);
    for (size_t i = 0; i < (al > bl ? al : bl); ++i)
        diff |= (i < al ? (unsigned char)a[i] : 0) ^
                (i < bl ? (unsigned char)b[i] : 0);
    return diff == 0;
}

static bool lock_auth(void)
{
    return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static esp_err_t hash_password(const char *password, char out[65])
{
    uint8_t digest[32];
    if (!password || mbedtls_sha256((const uint8_t *)password,
                                    strlen(password), digest, 0) != 0)
        return ESP_FAIL;
    for (size_t i = 0; i < sizeof(digest); ++i)
        snprintf(out + 2 * i, 3, "%02x", digest[i]);
    si_secret_store_clear(digest, sizeof(digest));
    return ESP_OK;
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out,
                         size_t out_size)
{
    if (!bytes || !out || out_size < (len * 2U) + 1U) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        snprintf(out + (i * 2U), 3, "%02x", bytes[i]);
    }
    out[len * 2U] = '\0';
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool hex_to_bytes(const char *hex, size_t expected_len, uint8_t *out)
{
    if (!hex || !out || strlen(hex) != expected_len * 2U) {
        return false;
    }
    for (size_t i = 0; i < expected_len; i++) {
        int high = hex_nibble(hex[i * 2U]);
        int low = hex_nibble(hex[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}


static esp_err_t derive_password(const char *password, const char *salt_hex, char out[65])
{
    uint8_t salt[AUTH_SALT_BYTES], digest[32];
    if (!password || !hex_to_bytes(salt_hex, sizeof(salt), salt))
        return ESP_ERR_INVALID_ARG;
    int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
        (const uint8_t *)password, strlen(password), salt, sizeof(salt),
        AUTH_ITERATIONS, sizeof(digest), digest);
    si_secret_store_clear(salt, sizeof(salt));
    if (!ret) bytes_to_hex(digest, sizeof(digest), out, 65);
    si_secret_store_clear(digest, sizeof(digest));
    return ret == 0 ? ESP_OK : ESP_FAIL;
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


esp_err_t si_auth_initialize(void)
{
    if (s_loaded) return s_initialized ? ESP_OK : ESP_FAIL;
    s_loaded = true;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    strlcpy(s_username, si_auth_default_username(), sizeof(s_username));
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, AUTH_NAMESPACE);
    if (ret == ESP_OK) {
        ret = si_secret_store_get_string(&store, AUTH_PASSWORD_HASH_KEY, s_hash, sizeof(s_hash));
        char username[sizeof(s_username)] = {0};
        esp_err_t ur = si_settings_store_get_string(&store, AUTH_USERNAME_KEY, username, sizeof(username));
        esp_err_t sr = si_secret_store_get_string(&store, "password_salt", s_salt, sizeof(s_salt));
        uint8_t bootstrap_flag = 0;
        esp_err_t br = si_settings_store_get_u8(&store, AUTH_BOOTSTRAP_KEY, &bootstrap_flag);
        uint8_t setup_flag = 0;
        esp_err_t setup_err = si_settings_store_get_u8(&store, "setup", &setup_flag);
        si_settings_store_close(&store);
        if (ret == ESP_OK && strlen(s_hash) == SI_AUTH_TOKEN_LEN) {
            if (ur != ESP_OK || si_auth_validate_username(username) != ESP_OK) return ESP_FAIL;
            strlcpy(s_username, username, sizeof(s_username));
            if (br != ESP_OK && br != ESP_ERR_NOT_FOUND) return ESP_FAIL;
            if (bootstrap_flag > 1) return ESP_FAIL;
            s_using_default = bootstrap_flag == 1;
            if ((setup_err != ESP_OK && setup_err != ESP_ERR_NOT_FOUND) || setup_flag > 1 ||
                (setup_err == ESP_OK && setup_flag && !s_using_default)) return ESP_FAIL;
            s_setup_pending = setup_err == ESP_OK ? setup_flag == 1 :
                s_using_default && si_auth_validate_password(SI_CFG_AUTH_PASSWORD) != ESP_OK;
            s_legacy = sr == ESP_ERR_NOT_FOUND;
            uint8_t salt[AUTH_SALT_BYTES];
            if (!s_legacy && (sr != ESP_OK || !hex_to_bytes(s_salt, sizeof(salt), salt))) return ESP_FAIL;
            s_initialized = true;
            return ESP_OK;
        }
        if (ret != ESP_ERR_NOT_FOUND) return ESP_FAIL;
    } else if (ret != ESP_ERR_NOT_FOUND) return ret;
    s_using_default = true;
    char bootstrap[33] = {0};
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    bytes_to_hex(random, sizeof(random), bootstrap, sizeof(bootstrap));
    si_secret_store_clear(random, sizeof(random));
    bool configured = si_auth_validate_password(SI_CFG_AUTH_PASSWORD) == ESP_OK;
    ret = set_credentials_internal(si_auth_default_username(), configured ? SI_CFG_AUTH_PASSWORD : bootstrap, true, true, false, !configured);
    s_initialized = ret == ESP_OK;
    s_using_default = true;
    si_secret_store_clear(bootstrap, sizeof(bootstrap));
    return ret;
}

bool si_auth_is_enabled(void)
{
    /* An unavailable credential store must never turn authentication off. */
    return true;
}

uint32_t si_auth_login_retry_after_ms(void)
{
    if (!lock_auth()) return 1000;
    int64_t remaining = s_locked_until - esp_timer_get_time();
    xSemaphoreGive(s_lock);
    return remaining > 0 ? (uint32_t)((remaining + 999) / 1000) : 0;
}

void si_auth_record_login_failure(void) { /* Recorded atomically by credential verification. */ }
void si_auth_clear_login_failures(void) { /* Cleared atomically by credential verification. */ }

bool si_auth_credentials_match(const char *username, const char *password)
{
    if (!username || !password || strlen(password) > SI_AUTH_PASSWORD_MAX_LEN || si_auth_setup_required() || !lock_auth()) return false;
    int64_t now = esp_timer_get_time();
    if (now < s_locked_until) { xSemaphoreGive(s_lock); return false; }
    char hash[65] = {0};
    esp_err_t ret = s_legacy ? hash_password(password, hash) : derive_password(password, s_salt, hash);
    bool matches = ret == ESP_OK && secure_equal(username, s_username) && secure_equal(hash, s_hash);
    si_secret_store_clear(hash, sizeof(hash));
    bool migrate = matches && s_legacy;
    if (matches) { s_failures = 0; s_failure_started = 0; }
    else {
        if (!s_failure_started || now - s_failure_started > 60000000LL) {
            s_failure_started = now; s_failures = 0;
        }
        if (++s_failures >= 5) { s_locked_until = now + 30000000LL; s_failures = 0; }
    }
    /* Hash migration preserves the logical credential generation and stays
       under the same lock as verification, so it cannot restore an old password. */
    if (migrate && set_credentials_internal(username, password, s_using_default, false, true, s_setup_pending) != ESP_OK) matches = false;
    xSemaphoreGive(s_lock);
    return matches;
}

static bool session_live(auth_session_t *session, int64_t now, const si_session_settings_t *settings)
{
    if (!session->used) return false;
    if (now < session->created || now < session->touched || now - session->created >= AUTH_ABSOLUTE_US ||
        (settings->auto_logout_enabled && now - session->touched >= (int64_t)settings->auto_logout_minutes * 60000000LL)) {
        si_secret_store_clear(session, sizeof(*session));
        return false;
    }
    return true;
}

static bool token_lookup(const char *token, bool recent, bool revoke)
{
    if (!token || strlen(token) != SI_AUTH_TOKEN_LEN) return false;
    uint8_t digest[32];
    if (mbedtls_sha256((const uint8_t *)token, strlen(token), digest, 0) != 0) return false;
    si_session_settings_t settings;
    si_session_settings_get(&settings);
    if (!lock_auth()) { si_secret_store_clear(digest, sizeof(digest)); return false; }
    int64_t now = esp_timer_get_time();
    bool found = false;
    for (size_t i = 0; i < AUTH_SESSION_SLOTS; ++i) {
        if (!session_live(&s_sessions[i], now, &settings)) continue;
        unsigned diff = 0;
        for (size_t j = 0; j < sizeof(digest); ++j) diff |= digest[j] ^ s_sessions[i].digest[j];
        if (diff) continue;
        found = !recent || (s_sessions[i].browser && now - s_sessions[i].created <= 60000000LL);
        if (revoke) si_secret_store_clear(&s_sessions[i], sizeof(s_sessions[i]));
        else if (found) s_sessions[i].touched = now;
        break;
    }
    xSemaphoreGive(s_lock);
    si_secret_store_clear(digest, sizeof(digest));
    return found;
}

bool si_auth_token_matches(const char *token) { return token_lookup(token, false, false); }
bool si_auth_token_is_recent(const char *token) { return token_lookup(token, true, false); }
void si_auth_revoke_session(const char *token) { (void)token_lookup(token, false, true); }

esp_err_t si_auth_create_session_for_generation(const char *client, uint32_t expected_generation, char out_token[65])
{
    uint8_t random[32], digest[32];
    if (!out_token || si_auth_setup_required() || !s_initialized) return ESP_ERR_INVALID_STATE;
    esp_fill_random(random, sizeof(random));
    bytes_to_hex(random, sizeof(random), out_token, 65);
    si_secret_store_clear(random, sizeof(random));
    if (mbedtls_sha256((const uint8_t *)out_token, 64, digest, 0) != 0 || !lock_auth()) {
        si_secret_store_clear(out_token, 65); return ESP_FAIL;
    }
    if (expected_generation && expected_generation != si_auth_credential_generation()) {
        xSemaphoreGive(s_lock);
        si_secret_store_clear(digest, sizeof(digest));
        si_secret_store_clear(out_token, 65);
        return ESP_ERR_INVALID_STATE;
    }
    size_t slot = 0;
    for (size_t i = 0; i < AUTH_SESSION_SLOTS; ++i) {
        if (!s_sessions[i].used) { slot = i; break; }
        if (s_sessions[i].touched < s_sessions[slot].touched) slot = i;
    }
    si_secret_store_clear(&s_sessions[slot], sizeof(s_sessions[slot]));
    memcpy(s_sessions[slot].digest, digest, sizeof(digest));
    s_sessions[slot].created = s_sessions[slot].touched = esp_timer_get_time();
    s_sessions[slot].used = true;
    s_sessions[slot].browser = !client || strcmp(client, "exoanchor-mcp") != 0;
    xSemaphoreGive(s_lock);
    si_secret_store_clear(digest, sizeof(digest));
    return ESP_OK;
}

esp_err_t si_auth_create_session(char out_token[65]) { return si_auth_create_session_for_client(NULL, out_token); }

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


static esp_err_t set_credentials_internal(const char *username, const char *password,
                                          bool bootstrap, bool rotate, bool locked, bool allow_setup)
{
    char verifier[65] = {0}, salt_hex[33] = {0};
    uint8_t salt[16];
    ESP_RETURN_ON_ERROR(si_auth_validate_username(username), TAG, "username");
    ESP_RETURN_ON_ERROR(si_auth_validate_password(password), TAG, "password");
    esp_fill_random(salt, sizeof(salt));
    bytes_to_hex(salt, sizeof(salt), salt_hex, sizeof(salt_hex));
    si_secret_store_clear(salt, sizeof(salt));
    ESP_RETURN_ON_ERROR(derive_password(password, salt_hex, verifier), TAG, "derive password");
    char next_username[sizeof(s_username)];
    strlcpy(next_username, username, sizeof(next_username));
    if (!locked && !lock_auth()) return ESP_ERR_TIMEOUT;
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(&store, AUTH_NAMESPACE);
    if (ret == ESP_OK) ret = si_settings_store_set_string(&store, AUTH_USERNAME_KEY, username);
    if (ret == ESP_OK) ret = si_secret_store_set_string(&store, AUTH_PASSWORD_HASH_KEY, verifier);
    if (ret == ESP_OK) ret = si_secret_store_set_string(&store, "password_salt", salt_hex);
    if (ret == ESP_OK) ret = si_settings_store_set_u8(&store, AUTH_BOOTSTRAP_KEY, bootstrap ? 1 : 0);
    if (ret == ESP_OK) ret = si_settings_store_set_u8(&store, "setup", bootstrap && allow_setup ? 1 : 0);
    if (ret == ESP_OK) ret = si_settings_store_commit(&store);
    si_settings_store_close(&store);
    if (ret == ESP_OK) {
        strlcpy(s_username, next_username, sizeof(s_username));
        strlcpy(s_hash, verifier, sizeof(s_hash));
        strlcpy(s_salt, salt_hex, sizeof(s_salt));
        s_legacy = false;
        s_using_default = bootstrap;
        s_setup_pending = bootstrap && allow_setup;
        if (rotate) {
            __atomic_add_fetch(&s_credential_generation, 1U, __ATOMIC_RELEASE);
            si_secret_store_clear(s_sessions, sizeof(s_sessions));
        }
    }
    if (!locked) xSemaphoreGive(s_lock);
    si_secret_store_clear(verifier, sizeof(verifier));
    si_secret_store_clear(salt_hex, sizeof(salt_hex));
    return ret;
}

esp_err_t si_auth_set_credentials(const char *username, const char *password)
{
    return set_credentials_internal(username, password, false, true, false, false);
}

void si_auth_get_status(si_auth_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    status->enabled = true;
    status->using_default = s_using_default;
    status->setup_required = si_auth_setup_required();
    status->login_count = si_auth_login_count();
    strlcpy(status->username, s_username, sizeof(status->username));
}

esp_err_t si_auth_create_session_for_client(const char *client, char out_token[65]) { return si_auth_create_session_for_generation(client, 0, out_token); }

bool si_auth_setup_required(void)
{
    return s_initialized && s_hash[0] && s_using_default && s_setup_pending;
}

esp_err_t si_auth_setup_credentials(const char *username, const char *password,
                                    uint32_t expected_generation)
{
    if (!lock_auth()) return ESP_ERR_TIMEOUT;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (si_auth_setup_required() && expected_generation == si_auth_credential_generation()) {
        ret = set_credentials_internal(username, password, false, true, true, false);
    }
    xSemaphoreGive(s_lock);
    return ret;
}
