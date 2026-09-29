#include "auth_service.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "control_lease.h"
#include "device_settings.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hid_device.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha256.h"
#include "secret_store.h"
#include "settings_store.h"

#define AUTH_NAMESPACE "si_auth"
#define AUTH_USERNAME_KEY "username"
#define AUTH_PASSWORD_HASH_KEY "password_hash"
#define AUTH_PASSWORD_SALT_KEY "password_salt"
#define AUTH_PASSWORD_ITERATIONS_KEY "password_iter"
#define AUTH_BOOTSTRAP_REQUIRED_KEY "bootstrap"
#define AUTH_LOGIN_COUNT_KEY "login_count"
#define AUTH_FACTORY_PASSWORD "admin"

#define AUTH_SALT_BYTES 16U
#define AUTH_SALT_HEX_LEN (AUTH_SALT_BYTES * 2U)
#define AUTH_VERIFIER_BYTES 32U
#define AUTH_PBKDF2_ITERATIONS 60000U
#define AUTH_BROWSER_SESSION_SLOTS 12U
#define AUTH_MCP_SESSION_SLOTS 4U
#define AUTH_SESSION_SLOTS (AUTH_BROWSER_SESSION_SLOTS + AUTH_MCP_SESSION_SLOTS)
#define AUTH_SESSION_ABSOLUTE_MS (24ULL * 60ULL * 60ULL * 1000ULL)
#define AUTH_SESSION_LOCK_WAIT_MS 1000U
#define AUTH_RETAINED_MAGIC 0x45415353U
#define AUTH_RETAINED_VERSION 2U
#define AUTH_FAILURE_WINDOW_MS 60000ULL
#define AUTH_FAILURE_LIMIT 5U
#define AUTH_LOCKOUT_MS 30000ULL

typedef struct {
    bool used;
    si_principal_kind_t principal;
    si_capability_set_t capabilities;
    uint32_t generation;
    char session_id[SI_AUTH_SESSION_ID_MAX_LEN + 1];
    uint8_t token_digest[AUTH_VERIFIER_BYTES];
    int64_t created_us;
    int64_t last_seen_us;
} auth_session_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t checksum;
    uint32_t next_generation;
    int64_t clock_us;
    auth_session_t sessions[AUTH_SESSION_SLOTS];
} auth_retained_state_t;

static const char *TAG = "si-auth";

static char s_username[SI_AUTH_USERNAME_MAX_LEN + 1];
static char s_verifier[SI_AUTH_TOKEN_LEN + 1];
static char s_salt[AUTH_SALT_HEX_LEN + 1];
static uint32_t s_iterations;
static bool s_loaded;
static bool s_using_default;
static bool s_legacy_password;
static bool s_login_count_loaded;
static uint32_t s_login_count;
static SemaphoreHandle_t s_lock;
static RTC_NOINIT_ATTR auth_retained_state_t s_retained;
static bool s_retained_initialized;
static uint32_t s_failure_count;
static int64_t s_failure_window_started_us;
static int64_t s_lockout_until_us;
static uint32_t s_session_evictions;
static uint32_t s_session_expirations;
static uint32_t s_session_restores;
static uint32_t s_validation_misses;
static uint32_t s_lock_timeouts;

#define s_sessions s_retained.sessions

typedef struct {
    size_t count;
    struct {
        char session_id[SI_AUTH_SESSION_ID_MAX_LEN + 1];
        uint32_t generation;
    } sessions[AUTH_SESSION_SLOTS];
} auth_revoked_sessions_t;

static void revoked_sessions_add(auth_revoked_sessions_t *revoked,
                                 const auth_session_t *session)
{
    if (!revoked || !session || !session->used || !session->session_id[0] ||
        revoked->count >= AUTH_SESSION_SLOTS) {
        return;
    }
    strlcpy(revoked->sessions[revoked->count].session_id,
            session->session_id,
            sizeof(revoked->sessions[revoked->count].session_id));
    revoked->sessions[revoked->count].generation = session->generation;
    revoked->count++;
}

static void session_clear_locked(auth_session_t *session,
                                 auth_revoked_sessions_t *revoked)
{
    revoked_sessions_add(revoked, session);
    si_secret_store_clear(session, sizeof(*session));
}

static void revoked_sessions_apply(
    const auth_revoked_sessions_t *revoked)
{
    if (!revoked) {
        return;
    }
    for (size_t i = 0; i < revoked->count; i++) {
        (void)si_control_lease_revoke_auth_session(
            revoked->sessions[i].session_id,
            revoked->sessions[i].generation);
    }
}

static uint32_t next_session_generation_locked(void)
{
    if (++s_retained.next_generation == 0U) {
        s_retained.next_generation = 1U;
    }
    return s_retained.next_generation;
}

static esp_err_t set_credentials_internal(const char *username,
                                          const char *password,
                                          bool bootstrap_required);

static esp_err_t ensure_lock(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static uint32_t retained_checksum(const auth_retained_state_t *state)
{
    const uint8_t *bytes = (const uint8_t *)state;
    const size_t checksum_offset =
        offsetof(auth_retained_state_t, checksum);
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < sizeof(*state); i++) {
        uint8_t value =
            i >= checksum_offset &&
                    i < checksum_offset + sizeof(state->checksum) ?
                0U : bytes[i];
        hash ^= value;
        hash *= 16777619U;
    }
    return hash;
}

static bool retained_reset_allowed(esp_reset_reason_t reason)
{
    return reason == ESP_RST_SW || reason == ESP_RST_PANIC ||
           reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT;
}

static void retained_commit_locked(int64_t now_us)
{
    s_retained.magic = AUTH_RETAINED_MAGIC;
    s_retained.version = AUTH_RETAINED_VERSION;
    s_retained.clock_us = now_us;
    s_retained.checksum = 0;
    s_retained.checksum = retained_checksum(&s_retained);
}

static bool retained_session_sane(const auth_session_t *session,
                                  int64_t retained_clock_us)
{
    if (!session || !session->used ||
        (session->principal != SI_PRINCIPAL_BROWSER &&
         session->principal != SI_PRINCIPAL_MCP) ||
        session->generation == 0U ||
        session->last_seen_us < session->created_us ||
        session->last_seen_us > retained_clock_us ||
        strnlen(session->session_id, sizeof(session->session_id)) == 0 ||
        strnlen(session->session_id, sizeof(session->session_id)) >=
            sizeof(session->session_id)) {
        return false;
    }
    const int64_t absolute_us =
        (int64_t)(AUTH_SESSION_ABSOLUTE_MS * 1000ULL);
    return session->created_us >= retained_clock_us - absolute_us;
}

static void retained_init_locked(void)
{
    if (s_retained_initialized) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    uint32_t expected_checksum = s_retained.checksum;
    bool restore =
        retained_reset_allowed(esp_reset_reason()) &&
        s_retained.magic == AUTH_RETAINED_MAGIC &&
        s_retained.version == AUTH_RETAINED_VERSION &&
        s_retained.clock_us >= 0 &&
        expected_checksum == retained_checksum(&s_retained);
    int64_t previous_clock_us = s_retained.clock_us;
    if (!restore) {
        si_secret_store_clear(&s_retained, sizeof(s_retained));
    } else {
        for (size_t i = 0; i < AUTH_SESSION_SLOTS; i++) {
            auth_session_t *session = &s_sessions[i];
            if (!session->used) {
                continue;
            }
            if (!retained_session_sane(session, previous_clock_us)) {
                si_secret_store_clear(session, sizeof(*session));
                continue;
            }
            int64_t created_age_us =
                previous_clock_us - session->created_us;
            int64_t idle_age_us =
                previous_clock_us - session->last_seen_us;
            session->created_us = now_us - created_age_us;
            session->last_seen_us = now_us - idle_age_us;
            s_session_restores++;
        }
    }
    s_retained_initialized = true;
    retained_commit_locked(now_us);
}

static bool secure_equal(const void *a, size_t a_len, const void *b, size_t b_len)
{
    if (!a || !b) {
        return false;
    }
    const uint8_t *a_bytes = (const uint8_t *)a;
    const uint8_t *b_bytes = (const uint8_t *)b;
    uint8_t diff = (uint8_t)(a_len ^ b_len);
    size_t max_len = a_len > b_len ? a_len : b_len;
    for (size_t i = 0; i < max_len; i++) {
        uint8_t a_ch = i < a_len ? a_bytes[i] : 0;
        uint8_t b_ch = i < b_len ? b_bytes[i] : 0;
        diff |= (uint8_t)(a_ch ^ b_ch);
    }
    return diff == 0;
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

static esp_err_t sha256_text(const char *text,
                             uint8_t digest[AUTH_VERIFIER_BYTES])
{
    if (!text || !digest) {
        return ESP_ERR_INVALID_ARG;
    }
    return mbedtls_sha256((const unsigned char *)text, strlen(text),
                          digest, 0) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t legacy_hash_password(const char *password,
                                      char out[SI_AUTH_TOKEN_LEN + 1])
{
    uint8_t digest[AUTH_VERIFIER_BYTES] = {0};
    ESP_RETURN_ON_ERROR(sha256_text(password, digest), TAG, "legacy sha256");
    bytes_to_hex(digest, sizeof(digest), out, SI_AUTH_TOKEN_LEN + 1);
    si_secret_store_clear(digest, sizeof(digest));
    return ESP_OK;
}

static esp_err_t derive_password(const char *password, const char *salt_hex,
                                 uint32_t iterations,
                                 char out[SI_AUTH_TOKEN_LEN + 1])
{
    uint8_t salt[AUTH_SALT_BYTES] = {0};
    uint8_t verifier[AUTH_VERIFIER_BYTES] = {0};
    if (!password || !salt_hex || !hex_to_bytes(salt_hex, sizeof(salt), salt) ||
        iterations < 10000U) {
        return ESP_ERR_INVALID_ARG;
    }
    int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256, (const uint8_t *)password, strlen(password),
        salt, sizeof(salt), iterations, sizeof(verifier), verifier);
    si_secret_store_clear(salt, sizeof(salt));
    if (ret != 0) {
        si_secret_store_clear(verifier, sizeof(verifier));
        return ESP_FAIL;
    }
    bytes_to_hex(verifier, sizeof(verifier), out, SI_AUTH_TOKEN_LEN + 1);
    si_secret_store_clear(verifier, sizeof(verifier));
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

static void generate_bootstrap_password(
    char out[SI_AUTH_BOOTSTRAP_PASSWORD_LEN + 1])
{
    uint32_t random_value = 0;
    const uint32_t range = 1000000U;
    const uint32_t unbiased_limit = UINT32_MAX - (UINT32_MAX % range);
    do {
        esp_fill_random(&random_value, sizeof(random_value));
    } while (random_value >= unbiased_limit);
    snprintf(out, SI_AUTH_BOOTSTRAP_PASSWORD_LEN + 1, "%06" PRIu32,
             random_value % range);
}

static void load_credentials(void)
{
    if (s_loaded) {
        return;
    }

    strlcpy(s_username, si_auth_default_username(), sizeof(s_username));
    s_verifier[0] = '\0';
    s_salt[0] = '\0';
    s_iterations = 0;
    s_using_default = true;
    s_legacy_password = false;

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t err = si_settings_store_open_read(&store, AUTH_NAMESPACE);
    if (err == ESP_OK) {
        char username[sizeof(s_username)] = {0};
        esp_err_t verifier_err = si_secret_store_get_string(&store, AUTH_PASSWORD_HASH_KEY,
                                                            s_verifier, sizeof(s_verifier));
        esp_err_t salt_err = si_secret_store_get_string(
            &store, AUTH_PASSWORD_SALT_KEY, s_salt, sizeof(s_salt));
        esp_err_t iteration_err = si_settings_store_get_u32(
            &store, AUTH_PASSWORD_ITERATIONS_KEY, &s_iterations);
        esp_err_t username_err = si_settings_store_get_string(
            &store, AUTH_USERNAME_KEY, username, sizeof(username));
        uint8_t bootstrap_required = 0;
        esp_err_t bootstrap_err = si_settings_store_get_u8(
            &store, AUTH_BOOTSTRAP_REQUIRED_KEY, &bootstrap_required);
        si_settings_store_close(&store);

        if (verifier_err == ESP_OK && strlen(s_verifier) == SI_AUTH_TOKEN_LEN) {
            if (username_err == ESP_OK &&
                si_auth_validate_username(username) == ESP_OK) {
                strlcpy(s_username, username, sizeof(s_username));
            }
            s_legacy_password = salt_err != ESP_OK ||
                                strlen(s_salt) != AUTH_SALT_HEX_LEN ||
                                iteration_err != ESP_OK ||
                                s_iterations < 10000U;
            if (s_legacy_password) {
                s_salt[0] = '\0';
                s_iterations = 0;
            }
            s_using_default = bootstrap_err == ESP_OK &&
                              bootstrap_required == 1U;
            s_loaded = true;
            return;
        }
    }

    const char *bootstrap_password =
        si_auth_validate_password(SI_CFG_AUTH_PASSWORD) == ESP_OK ?
            SI_CFG_AUTH_PASSWORD : AUTH_FACTORY_PASSWORD;

    esp_err_t bootstrap_ret = set_credentials_internal(
        s_username, bootstrap_password, true);
    if (bootstrap_ret != ESP_OK) {
        /* Fail closed for this boot even if NVS is temporarily unavailable. */
        if (legacy_hash_password(bootstrap_password, s_verifier) == ESP_OK) {
            s_legacy_password = true;
            s_using_default = true;
        }
        s_loaded = true;
        ESP_LOGE(TAG, "Could not persist bootstrap credential: %s",
                 esp_err_to_name(bootstrap_ret));
    }
}

esp_err_t si_auth_initialize(void)
{
    load_credentials();
    return s_verifier[0] != '\0' ? ESP_OK : ESP_FAIL;
}

bool si_auth_is_enabled(void)
{
    load_credentials();
    (void)ensure_lock();
    return s_verifier[0] != '\0';
}

bool si_auth_credentials_match(const char *username, const char *password)
{
    if (!username || !password) {
        return false;
    }
    load_credentials();
    if (!secure_equal(username, strlen(username),
                      s_username, strlen(s_username))) {
        return false;
    }

    char verifier[SI_AUTH_TOKEN_LEN + 1] = {0};
    esp_err_t ret = s_legacy_password
        ? legacy_hash_password(password, verifier)
        : derive_password(password, s_salt, s_iterations, verifier);
    bool matches = ret == ESP_OK &&
                   secure_equal(verifier, strlen(verifier),
                                s_verifier, strlen(s_verifier));
    si_secret_store_clear(verifier, sizeof(verifier));

    if (matches && s_legacy_password &&
        si_auth_validate_password(password) == ESP_OK) {
        (void)si_auth_set_credentials(s_username, password);
    }
    return matches;
}

static bool session_expired(const auth_session_t *session, int64_t now_us,
                            const si_session_settings_t *settings)
{
    if (!session || !session->used || now_us < session->created_us ||
        now_us < session->last_seen_us) {
        return true;
    }
    uint64_t age_ms = (uint64_t)(now_us - session->created_us) / 1000ULL;
    if (age_ms > AUTH_SESSION_ABSOLUTE_MS) {
        return true;
    }

    if (!settings || !settings->auto_logout_enabled) {
        return false;
    }
    uint64_t idle_ms = (uint64_t)(now_us - session->last_seen_us) / 1000ULL;
    return idle_ms > (uint64_t)settings->auto_logout_minutes * 60000ULL;
}

esp_err_t si_auth_create_session_for_client(
    const char *client, char out_token[SI_AUTH_TOKEN_LEN + 1])
{
    if (!out_token || !si_auth_is_enabled()) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_lock(), TAG, "create auth lock");

    uint8_t token[AUTH_VERIFIER_BYTES] = {0};
    uint8_t digest[AUTH_VERIFIER_BYTES] = {0};
    esp_fill_random(token, sizeof(token));
    bytes_to_hex(token, sizeof(token), out_token, SI_AUTH_TOKEN_LEN + 1);
    esp_err_t ret = sha256_text(out_token, digest);
    si_secret_store_clear(token, sizeof(token));
    if (ret != ESP_OK) {
        si_secret_store_clear(out_token, SI_AUTH_TOKEN_LEN + 1);
        return ret;
    }

    si_session_settings_t settings;
    si_session_settings_get(&settings);
    auth_revoked_sessions_t revoked = {0};
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(AUTH_SESSION_LOCK_WAIT_MS)) !=
        pdTRUE) {
        __atomic_add_fetch(&s_lock_timeouts, 1U, __ATOMIC_RELAXED);
        si_secret_store_clear(digest, sizeof(digest));
        si_secret_store_clear(out_token, SI_AUTH_TOKEN_LEN + 1);
        return ESP_ERR_TIMEOUT;
    }
    retained_init_locked();
    bool is_mcp = client && strcasecmp(client, "exoanchor-mcp") == 0;
    size_t slot_begin = is_mcp ? AUTH_BROWSER_SESSION_SLOTS : 0U;
    size_t slot_end = is_mcp ? AUTH_SESSION_SLOTS :
                                   AUTH_BROWSER_SESSION_SLOTS;
    int slot = -1;
    int64_t oldest = INT64_MAX;
    int64_t now_us = esp_timer_get_time();
    for (size_t i = slot_begin; i < slot_end; i++) {
        if (s_sessions[i].used &&
            session_expired(&s_sessions[i], now_us, &settings)) {
            session_clear_locked(&s_sessions[i], &revoked);
            s_session_expirations++;
        }
        if (!s_sessions[i].used) {
            slot = (int)i;
            break;
        }
        if (s_sessions[i].last_seen_us < oldest) {
            oldest = s_sessions[i].last_seen_us;
            slot = (int)i;
        }
    }
    if (slot < 0) {
        xSemaphoreGive(s_lock);
        si_secret_store_clear(digest, sizeof(digest));
        si_secret_store_clear(out_token, SI_AUTH_TOKEN_LEN + 1);
        return ESP_ERR_NO_MEM;
    }
    if (s_sessions[slot].used) {
        revoked_sessions_add(&revoked, &s_sessions[slot]);
        s_session_evictions++;
    }
    s_sessions[slot].used = true;
    s_sessions[slot].principal =
        is_mcp ? SI_PRINCIPAL_MCP : SI_PRINCIPAL_BROWSER;
    s_sessions[slot].capabilities =
        is_mcp ? SI_CAPABILITIES_MCP_DEFAULT : SI_CAPABILITIES_WEB_DEFAULT;
    s_sessions[slot].generation = next_session_generation_locked();
    snprintf(s_sessions[slot].session_id,
             sizeof(s_sessions[slot].session_id), "s%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    memcpy(s_sessions[slot].token_digest, digest, sizeof(digest));
    s_sessions[slot].created_us = now_us;
    s_sessions[slot].last_seen_us = now_us;
    retained_commit_locked(now_us);
    xSemaphoreGive(s_lock);
    revoked_sessions_apply(&revoked);
    si_secret_store_clear(digest, sizeof(digest));
    return ESP_OK;
}

esp_err_t si_auth_create_session(char out_token[SI_AUTH_TOKEN_LEN + 1])
{
    return si_auth_create_session_for_client(NULL, out_token);
}

bool si_auth_token_get_context(const char *token, bool touch,
                               si_auth_session_context_t *context)
{
    if (context) {
        memset(context, 0, sizeof(*context));
        context->principal = SI_PRINCIPAL_ANONYMOUS;
    }
    if (!token || strlen(token) != SI_AUTH_TOKEN_LEN || !si_auth_is_enabled() ||
        ensure_lock() != ESP_OK || !context) {
        return false;
    }
    uint8_t digest[AUTH_VERIFIER_BYTES] = {0};
    if (sha256_text(token, digest) != ESP_OK) {
        return false;
    }
    si_session_settings_t settings;
    si_session_settings_get(&settings);
    auth_revoked_sessions_t revoked = {0};
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(AUTH_SESSION_LOCK_WAIT_MS)) !=
        pdTRUE) {
        __atomic_add_fetch(&s_lock_timeouts, 1U, __ATOMIC_RELAXED);
        si_secret_store_clear(digest, sizeof(digest));
        return false;
    }
    retained_init_locked();

    bool matches = false;
    int64_t now_us = esp_timer_get_time();
    for (size_t i = 0; i < AUTH_SESSION_SLOTS; i++) {
        if (s_sessions[i].used &&
            session_expired(&s_sessions[i], now_us, &settings)) {
            session_clear_locked(&s_sessions[i], &revoked);
            s_session_expirations++;
            continue;
        }
        if (s_sessions[i].used &&
            secure_equal(digest, sizeof(digest),
                         s_sessions[i].token_digest,
                         sizeof(s_sessions[i].token_digest))) {
            if (touch) {
                s_sessions[i].last_seen_us = now_us;
            }
            context->authenticated = true;
            context->principal = s_sessions[i].principal;
            context->capabilities = s_sessions[i].capabilities;
            context->generation = s_sessions[i].generation;
            strlcpy(context->session_id, s_sessions[i].session_id,
                    sizeof(context->session_id));
            matches = true;
            break;
        }
    }
    if (!matches) {
        s_validation_misses++;
    }
    retained_commit_locked(now_us);
    xSemaphoreGive(s_lock);
    revoked_sessions_apply(&revoked);
    si_secret_store_clear(digest, sizeof(digest));
    return matches;
}

bool si_auth_session_get_live_context(
    const char *session_id, uint32_t expected_generation,
    si_auth_session_context_t *context)
{
    if (context) {
        memset(context, 0, sizeof(*context));
        context->principal = SI_PRINCIPAL_ANONYMOUS;
    }
    if (!session_id || !session_id[0] || expected_generation == 0U ||
        !context) {
        return false;
    }
    if (!si_auth_is_enabled()) {
        if (strcmp(session_id, "auth-disabled") != 0 ||
            expected_generation != SI_AUTH_DISABLED_SESSION_GENERATION) {
            return false;
        }
        context->authenticated = true;
        context->principal = SI_PRINCIPAL_BROWSER;
        context->capabilities = SI_CAPABILITIES_WEB_DEFAULT;
        context->generation = SI_AUTH_DISABLED_SESSION_GENERATION;
        strlcpy(context->session_id, "auth-disabled",
                sizeof(context->session_id));
        return true;
    }
    if (ensure_lock() != ESP_OK) {
        return false;
    }

    si_session_settings_t settings;
    si_session_settings_get(&settings);
    auth_revoked_sessions_t revoked = {0};
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(AUTH_SESSION_LOCK_WAIT_MS)) !=
        pdTRUE) {
        __atomic_add_fetch(&s_lock_timeouts, 1U, __ATOMIC_RELAXED);
        return false;
    }
    retained_init_locked();

    bool matches = false;
    int64_t now_us = esp_timer_get_time();
    for (size_t i = 0; i < AUTH_SESSION_SLOTS; i++) {
        if (!s_sessions[i].used ||
            strcmp(s_sessions[i].session_id, session_id) != 0) {
            continue;
        }
        if (session_expired(&s_sessions[i], now_us, &settings)) {
            session_clear_locked(&s_sessions[i], &revoked);
            s_session_expirations++;
            break;
        }
        if (s_sessions[i].generation != expected_generation) {
            break;
        }
        context->authenticated = true;
        context->principal = s_sessions[i].principal;
        context->capabilities = s_sessions[i].capabilities;
        context->generation = s_sessions[i].generation;
        strlcpy(context->session_id, s_sessions[i].session_id,
                sizeof(context->session_id));
        matches = true;
        break;
    }
    retained_commit_locked(now_us);
    xSemaphoreGive(s_lock);
    revoked_sessions_apply(&revoked);
    return matches;
}

bool si_auth_token_matches(const char *token)
{
    si_auth_session_context_t context;
    return si_auth_token_get_context(token, true, &context);
}

bool si_auth_token_is_mcp(const char *token)
{
    si_auth_session_context_t context;
    return si_auth_token_get_context(token, false, &context) &&
           context.principal == SI_PRINCIPAL_MCP;
}

void si_auth_revoke_session(const char *token)
{
    if (!token || ensure_lock() != ESP_OK) {
        return;
    }
    uint8_t digest[AUTH_VERIFIER_BYTES] = {0};
    if (sha256_text(token, digest) != ESP_OK) {
        return;
    }
    auth_revoked_sessions_t revoked = {0};
    /* Explicit logout is a synchronous security boundary.  Unlike ordinary
     * validation it must not silently keep a live token because the auth mutex
     * was briefly busy. */
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        retained_init_locked();
        for (size_t i = 0; i < AUTH_SESSION_SLOTS; i++) {
            if (s_sessions[i].used &&
                secure_equal(digest, sizeof(digest),
                             s_sessions[i].token_digest,
                             sizeof(s_sessions[i].token_digest))) {
                session_clear_locked(&s_sessions[i], &revoked);
                break;
            }
        }
        retained_commit_locked(esp_timer_get_time());
        xSemaphoreGive(s_lock);
    }
    /* Auth is cleared before entering HID authority.  Explicit logout does
     * not return until the exact session generation is fenced and its report
     * owner is synchronously neutralized (or left neutral-pending). */
    revoked_sessions_apply(&revoked);
    si_secret_store_clear(digest, sizeof(digest));
}

void si_auth_revoke_all_sessions(void)
{
    auth_revoked_sessions_t revoked = {0};
    if (ensure_lock() != ESP_OK ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    retained_init_locked();
    for (size_t i = 0; i < AUTH_SESSION_SLOTS; i++) {
        if (s_sessions[i].used) {
            session_clear_locked(&s_sessions[i], &revoked);
        }
    }
    retained_commit_locked(esp_timer_get_time());
    xSemaphoreGive(s_lock);
    revoked_sessions_apply(&revoked);
}

uint32_t si_auth_login_retry_after_ms(void)
{
    if (ensure_lock() != ESP_OK ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }
    int64_t now_us = esp_timer_get_time();
    uint32_t retry_ms = 0;
    if (now_us < s_lockout_until_us) {
        retry_ms = (uint32_t)((s_lockout_until_us - now_us + 999LL) / 1000LL);
    }
    xSemaphoreGive(s_lock);
    return retry_ms;
}

void si_auth_record_login_failure(void)
{
    if (ensure_lock() != ESP_OK ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    if (s_failure_window_started_us == 0 ||
        now_us - s_failure_window_started_us >
            (int64_t)(AUTH_FAILURE_WINDOW_MS * 1000ULL)) {
        s_failure_window_started_us = now_us;
        s_failure_count = 0;
    }
    s_failure_count++;
    if (s_failure_count >= AUTH_FAILURE_LIMIT) {
        s_lockout_until_us = now_us + (int64_t)(AUTH_LOCKOUT_MS * 1000ULL);
        s_failure_count = 0;
        s_failure_window_started_us = now_us;
    }
    xSemaphoreGive(s_lock);
}

void si_auth_clear_login_failures(void)
{
    if (ensure_lock() == ESP_OK &&
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_failure_count = 0;
        s_failure_window_started_us = 0;
        s_lockout_until_us = 0;
        xSemaphoreGive(s_lock);
    }
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

static esp_err_t set_credentials_internal(const char *username,
                                          const char *password,
                                          bool bootstrap_required)
{
    uint8_t salt[AUTH_SALT_BYTES] = {0};
    char salt_hex[AUTH_SALT_HEX_LEN + 1] = {0};
    char verifier[SI_AUTH_TOKEN_LEN + 1] = {0};
    ESP_RETURN_ON_ERROR(si_auth_validate_username(username), TAG,
                        "validate username");
    bool factory_bootstrap =
        bootstrap_required && strcmp(password, AUTH_FACTORY_PASSWORD) == 0;
    if (!factory_bootstrap) {
        ESP_RETURN_ON_ERROR(si_auth_validate_password(password), TAG,
                            "validate password");
    }
    esp_fill_random(salt, sizeof(salt));
    bytes_to_hex(salt, sizeof(salt), salt_hex, sizeof(salt_hex));
    si_secret_store_clear(salt, sizeof(salt));
    ESP_RETURN_ON_ERROR(
        derive_password(password, salt_hex, AUTH_PBKDF2_ITERATIONS, verifier),
        TAG, "derive password");

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(&store, AUTH_NAMESPACE);
    if (ret == ESP_OK) {
        ret = si_settings_store_set_string(&store, AUTH_USERNAME_KEY, username);
    }
    if (ret == ESP_OK) {
        ret = si_secret_store_set_string(&store, AUTH_PASSWORD_HASH_KEY, verifier);
    }
    if (ret == ESP_OK) {
        ret = si_secret_store_set_string(&store, AUTH_PASSWORD_SALT_KEY, salt_hex);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u32(&store, AUTH_PASSWORD_ITERATIONS_KEY,
                                        AUTH_PBKDF2_ITERATIONS);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(
            &store, AUTH_BOOTSTRAP_REQUIRED_KEY,
            bootstrap_required ? 1U : 0U);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    if (ret != ESP_OK) {
        si_secret_store_clear(verifier, sizeof(verifier));
        si_secret_store_clear(salt_hex, sizeof(salt_hex));
        return ret;
    }

    strlcpy(s_username, username, sizeof(s_username));
    strlcpy(s_verifier, verifier, sizeof(s_verifier));
    strlcpy(s_salt, salt_hex, sizeof(s_salt));
    s_iterations = AUTH_PBKDF2_ITERATIONS;
    s_loaded = true;
    s_using_default = bootstrap_required;
    s_legacy_password = false;
    si_secret_store_clear(verifier, sizeof(verifier));
    si_secret_store_clear(salt_hex, sizeof(salt_hex));
    si_auth_revoke_all_sessions();
    return ESP_OK;
}

esp_err_t si_auth_set_credentials(const char *username, const char *password)
{
    return set_credentials_internal(username, password, false);
}

esp_err_t si_auth_reset_bootstrap(char *bootstrap_password_out,
                                  size_t out_size)
{
    if (!bootstrap_password_out ||
        out_size < SI_AUTH_BOOTSTRAP_PASSWORD_LEN + 1) {
        return ESP_ERR_INVALID_ARG;
    }
    char password[SI_AUTH_BOOTSTRAP_PASSWORD_LEN + 1] = {0};
    generate_bootstrap_password(password);
    esp_err_t ret = set_credentials_internal(
        si_auth_default_username(), password, true);
    if (ret == ESP_OK) {
        strlcpy(bootstrap_password_out, password, out_size);
    } else {
        bootstrap_password_out[0] = '\0';
    }
    si_secret_store_clear(password, sizeof(password));
    return ret;
}

void si_auth_get_status(si_auth_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    load_credentials();
    status->enabled = s_verifier[0] != '\0';
    status->using_default = s_using_default;
    status->legacy_password = s_legacy_password;
    status->login_count = si_auth_login_count();
    strlcpy(status->username, s_username, sizeof(status->username));
}

void si_auth_get_runtime_status(si_auth_runtime_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    status->evictions =
        __atomic_load_n(&s_session_evictions, __ATOMIC_RELAXED);
    status->expirations =
        __atomic_load_n(&s_session_expirations, __ATOMIC_RELAXED);
    status->restored =
        __atomic_load_n(&s_session_restores, __ATOMIC_RELAXED);
    status->validation_misses =
        __atomic_load_n(&s_validation_misses, __ATOMIC_RELAXED);
    status->lock_timeouts =
        __atomic_load_n(&s_lock_timeouts, __ATOMIC_RELAXED);
    si_session_settings_t settings;
    si_session_settings_get(&settings);
    auth_revoked_sessions_t revoked = {0};
    if (ensure_lock() != ESP_OK ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(AUTH_SESSION_LOCK_WAIT_MS)) !=
            pdTRUE) {
        __atomic_add_fetch(&s_lock_timeouts, 1U, __ATOMIC_RELAXED);
        status->lock_timeouts =
            __atomic_load_n(&s_lock_timeouts, __ATOMIC_RELAXED);
        return;
    }
    retained_init_locked();
    int64_t now_us = esp_timer_get_time();
    for (size_t i = 0; i < AUTH_SESSION_SLOTS; i++) {
        if (s_sessions[i].used &&
            session_expired(&s_sessions[i], now_us, &settings)) {
            session_clear_locked(&s_sessions[i], &revoked);
            s_session_expirations++;
            continue;
        }
        if (!s_sessions[i].used) {
            continue;
        }
        if (s_sessions[i].principal == SI_PRINCIPAL_MCP) {
            status->mcp_active++;
        } else if (s_sessions[i].principal == SI_PRINCIPAL_BROWSER) {
            status->browser_active++;
        }
    }
    status->expirations = s_session_expirations;
    status->restored = s_session_restores;
    retained_commit_locked(now_us);
    xSemaphoreGive(s_lock);
    revoked_sessions_apply(&revoked);
}
