#include "two_step.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "auth_service.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "secret_store.h"
#include "settings_store.h"

#define RECORD_BYTES (1 + 20 + 8 + 1 + 32 * SI_TWO_STEP_RECOVERY_COUNT)
#define ENROLLMENT_US (300LL * 1000000LL)
#define MIN_UNIX_TIME 1704067200LL

typedef struct {
    bool enabled;
    uint8_t secret[20];
    uint64_t last_step;
    uint8_t used;
    uint8_t recovery[SI_TWO_STEP_RECOVERY_COUNT][32];
} two_step_state_t;

static SemaphoreHandle_t s_two_lock;
static bool s_two_loaded, s_two_storage_ok;
static two_step_state_t s_two_state, s_two_pending;
static char s_two_pending_id[33];
static uint32_t s_two_pending_generation;
static int64_t s_two_pending_until;
/* Guessing budgets are per secret: codes for the active secret (login,
 * management, disable) share one; a pending enrollment secret has its own. */
typedef struct {
    unsigned failures;
    int64_t locked_until;
} two_throttle_t;
static two_throttle_t s_two_active, s_two_enroll;

static bool two_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    unsigned difference = 0;
    for (size_t i = 0; i < n; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}

static void two_hex(const uint8_t *input, size_t n, char *output)
{
    const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        output[2 * i] = hex[input[i] >> 4];
        output[2 * i + 1] = hex[input[i] & 15];
    }
    output[2 * n] = 0;
}

static int two_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool two_unhex(const char *text, size_t n, uint8_t *out)
{
    if (!text || strlen(text) != 2 * n) return false;
    for (size_t i = 0; i < n; ++i) {
        int a = two_nibble(text[2 * i]), b = two_nibble(text[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return true;
}

static void two_base32(const uint8_t input[20], char output[33])
{
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    uint32_t buffer = 0;
    unsigned bits = 0, pos = 0;
    for (size_t i = 0; i < 20; ++i) {
        buffer = (buffer << 8) | input[i];
        bits += 8;
        while (bits >= 5) { bits -= 5; output[pos++] = alphabet[(buffer >> bits) & 31]; }
    }
    output[pos] = 0;
}

/* RFC 4226 dynamic truncation. All time counters are 64-bit (RFC 6238). */
static bool two_hotp(const uint8_t secret[20], uint64_t step, unsigned digits, uint32_t *out)
{
    uint8_t counter[8], digest[20];
    for (unsigned i = 0; i < 8; ++i) { counter[7 - i] = (uint8_t)step; step >>= 8; }
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!md || mbedtls_md_hmac(md, secret, 20, counter, sizeof(counter), digest) != 0) return false;
    unsigned offset = digest[19] & 15;
    uint32_t value = ((uint32_t)(digest[offset] & 127) << 24) |
        ((uint32_t)digest[offset + 1] << 16) | ((uint32_t)digest[offset + 2] << 8) | digest[offset + 3];
    *out = value % (digits == 8 ? 100000000U : 1000000U);
    si_secret_store_clear(digest, sizeof(digest));
    return true;
}

static bool two_time_ready(void) { return (int64_t)time(NULL) >= MIN_UNIX_TIME; }

#ifndef SI_TWO_STEP_SET_CLOCK
#define SI_TWO_STEP_SET_CLOCK(tv) settimeofday((tv), NULL)
#endif
/* Without an RTC or reachable NTP the clock restarts unset. A TOTP code that
 * verified against the client's clock attests that time, so adopt it. */
static void two_adopt_clock(int64_t unix_s)
{
    if (!unix_s || two_time_ready()) return;
    struct timeval now = {.tv_sec = (time_t)unix_s};
    (void)SI_TWO_STEP_SET_CLOCK(&now);
}

void si_two_step_clock_start(void)
{
    static bool started;
    if (started) return;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.wait_for_sync = false;
    if (esp_netif_sntp_init(&config) == ESP_OK) started = true;
}

static void two_load_locked(void)
{
    if (s_two_loaded) return;
    s_two_loaded = true;
    s_two_storage_ok = false;
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, "si_two_step");
    if (ret == ESP_ERR_NOT_FOUND) { s_two_storage_ok = true; return; }
    if (ret != ESP_OK) return;
    char text[RECORD_BYTES * 2 + 1] = {0};
    ret = si_secret_store_get_string(&store, "record", text, sizeof(text));
    si_settings_store_close(&store);
    /* Existing but unreadable/missing records fail closed. */
    if (ret == ESP_OK && strcmp(text, "off-v1") == 0) {
        s_two_storage_ok = true;
    } else if (ret == ESP_OK) {
        uint8_t raw[RECORD_BYTES] = {0};
        if (two_unhex(text, sizeof(raw), raw) && raw[0] == 1) {
            s_two_state.enabled = true;
            memcpy(s_two_state.secret, raw + 1, 20);
            s_two_state.last_step = 0;
            for (unsigned i = 0; i < 8; ++i) s_two_state.last_step = (s_two_state.last_step << 8) | raw[21 + i];
            s_two_state.used = raw[29];
            memcpy(s_two_state.recovery, raw + 30, sizeof(s_two_state.recovery));
            s_two_storage_ok = true;
        }
        si_secret_store_clear(raw, sizeof(raw));
    }
    si_secret_store_clear(text, sizeof(text));
}

esp_err_t si_two_step_initialize(void)
{
    if (!s_two_lock) s_two_lock = xSemaphoreCreateMutex();
    if (!s_two_lock || xSemaphoreTake(s_two_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_FAIL;
    two_load_locked();
    bool ok = s_two_storage_ok;
    xSemaphoreGive(s_two_lock);
    return ok ? ESP_OK : ESP_FAIL;
}

static bool two_lock(void)
{
    if (!s_two_lock || xSemaphoreTake(s_two_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (s_two_pending_until && (esp_timer_get_time() >= s_two_pending_until ||
        s_two_pending_generation != si_auth_credential_generation())) {
        si_secret_store_clear(&s_two_pending, sizeof(s_two_pending));
        si_secret_store_clear(s_two_pending_id, sizeof(s_two_pending_id));
        s_two_pending_until = 0;
    }
    return true;
}

/* Factor transitions (enable/disable) reload the authoritative record when a
 * commit fails; consumed-code updates fail closed to prevent replay. */
static esp_err_t two_save_locked(const two_step_state_t *state, bool transition)
{
    uint8_t raw[RECORD_BYTES] = {0};
    char text[RECORD_BYTES * 2 + 1];
    if (state->enabled) {
        raw[0] = 1;
        memcpy(raw + 1, state->secret, 20);
        uint64_t step = state->last_step;
        for (unsigned i = 0; i < 8; ++i) { raw[28 - i] = (uint8_t)step; step >>= 8; }
        raw[29] = state->used;
        memcpy(raw + 30, state->recovery, sizeof(state->recovery));
        two_hex(raw, sizeof(raw), text);
    } else strcpy(text, "off-v1");
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(&store, "si_two_step");
    if (ret == ESP_OK) ret = si_secret_store_set_string(&store, "record", text);
    if (ret == ESP_OK) ret = si_settings_store_commit(&store);
    si_settings_store_close(&store);
    si_secret_store_clear(raw, sizeof(raw));
    si_secret_store_clear(text, sizeof(text));
    if (ret == ESP_OK) {
        s_two_state = *state;
    } else if (transition) {
        si_secret_store_clear(&s_two_state, sizeof(s_two_state));
        s_two_loaded = false;
        two_load_locked();
    } else {
        s_two_storage_ok = false; /* An ambiguous commit must never permit replay. */
    }
    return ret;
}

static esp_err_t two_commit_transition(void *state) { return two_save_locked(state, true); }

void si_two_step_get_status(si_two_step_status_t *out)
{
    if (!out) return;
    *out = (si_two_step_status_t){.enabled = true, .time_ready = two_time_ready()};
    if (!two_lock()) return;
    out->storage_ok = s_two_storage_ok;
    out->enabled = !s_two_storage_ok || s_two_state.enabled;
    if (s_two_storage_ok && s_two_state.enabled)
        for (unsigned i = 0; i < SI_TWO_STEP_RECOVERY_COUNT; ++i)
            if (!(s_two_state.used & (1U << i))) out->recovery_remaining++;
    xSemaphoreGive(s_two_lock);
}

/* Stage the consumed code in a copy; publish only after durable commit. */
static bool two_check_code(two_step_state_t *candidate, const char *code, bool recovery,
                           int64_t client_unix, int64_t *adopt_unix)
{
    *adopt_unix = 0;
    if (!code) return false;
    if (recovery && strlen(code) == 32) {
        uint8_t bytes[16], digest[32];
        char normalized[33];
        if (!two_unhex(code, sizeof(bytes), bytes)) return false;
        two_hex(bytes, sizeof(bytes), normalized);
        int ret = mbedtls_sha256((const uint8_t *)normalized, 32, digest, 0);
        si_secret_store_clear(bytes, sizeof(bytes));
        si_secret_store_clear(normalized, sizeof(normalized));
        int match = -1;
        for (unsigned i = 0; i < SI_TWO_STEP_RECOVERY_COUNT; ++i)
            if (ret == 0 && two_equal(digest, candidate->recovery[i], 32) && !(candidate->used & (1U << i))) match = (int)i;
        si_secret_store_clear(digest, sizeof(digest));
        if (match < 0) return false;
        candidate->used |= (uint8_t)(1U << match);
        return true;
    }
    if (strlen(code) != 6) return false;
    for (unsigned i = 0; i < 6; ++i) if (code[i] < '0' || code[i] > '9') return false;
    int64_t now = (int64_t)time(NULL);
    bool claimed = now < MIN_UNIX_TIME;
    if (claimed) {
        if (client_unix < MIN_UNIX_TIME || client_unix > SI_TWO_STEP_CLIENT_TIME_MAX) return false;
        now = client_unix;
    }
    uint64_t current = (uint64_t)now / 30U, matched = 0;
    for (int drift = -1; drift <= 1; ++drift) {
        uint64_t step = drift < 0 ? current - 1 : current + (unsigned)drift;
        uint32_t value;
        char expected[7];
        if (!two_hotp(candidate->secret, step, 6, &value)) return false;
        snprintf(expected, sizeof(expected), "%06" PRIu32, value);
        if (two_equal((const uint8_t *)expected, (const uint8_t *)code, 6) && step > candidate->last_step) matched = step;
    }
    if (!matched) return false;
    candidate->last_step = matched;
    if (claimed) *adopt_unix = now;
    return true;
}

static bool two_attempt_allowed(const two_throttle_t *t) { return esp_timer_get_time() >= t->locked_until; }
static void two_attempt_result(two_throttle_t *t, bool ok)
{
    if (ok) t->failures = 0;
    else if (++t->failures >= 5) {
        t->locked_until = esp_timer_get_time() + 30000000LL;
        t->failures = 0;
    }
}

bool si_two_step_verify(const char *code, int64_t client_unix, uint32_t generation)
{
    if (!two_lock()) return false;
    bool ok = s_two_storage_ok && generation == si_auth_credential_generation();
    two_step_state_t candidate = s_two_state;
    if (ok && candidate.enabled) {
        int64_t adopt = 0;
        ok = two_attempt_allowed(&s_two_active) && two_check_code(&candidate, code, true, client_unix, &adopt);
        if (ok) ok = two_save_locked(&candidate, false) == ESP_OK;
        if (ok) two_adopt_clock(adopt);
        two_attempt_result(&s_two_active, ok);
    }
    si_secret_store_clear(&candidate, sizeof(candidate));
    xSemaphoreGive(s_two_lock);
    return ok;
}

esp_err_t si_two_step_begin(uint32_t generation, si_two_step_enrollment_t *out)
{
    if (!out || !two_lock()) return ESP_FAIL;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_two_storage_ok && !s_two_state.enabled && generation == si_auth_credential_generation()) {
        si_secret_store_clear(&s_two_pending, sizeof(s_two_pending));
        s_two_pending.enabled = true;
        esp_fill_random(s_two_pending.secret, sizeof(s_two_pending.secret));
        two_base32(s_two_pending.secret, out->secret);
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        two_hex(random, sizeof(random), out->id);
        ret = ESP_OK;
        for (unsigned i = 0; i < SI_TWO_STEP_RECOVERY_COUNT; ++i) {
            esp_fill_random(random, sizeof(random));
            two_hex(random, sizeof(random), out->recovery[i]);
            if (mbedtls_sha256((const uint8_t *)out->recovery[i], 32, s_two_pending.recovery[i], 0) != 0) ret = ESP_FAIL;
        }
        si_secret_store_clear(random, sizeof(random));
        if (ret == ESP_OK) {
            strcpy(s_two_pending_id, out->id);
            s_two_pending_generation = generation;
            s_two_pending_until = esp_timer_get_time() + ENROLLMENT_US;
        } else {
            si_secret_store_clear(out, sizeof(*out));
            si_secret_store_clear(&s_two_pending, sizeof(s_two_pending));
            s_two_pending_id[0] = 0;
        }
    }
    xSemaphoreGive(s_two_lock);
    return ret;
}

esp_err_t si_two_step_confirm(const char *id, const char *code, int64_t client_unix, uint32_t generation)
{
    if (!two_lock()) return ESP_FAIL;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_two_storage_ok && !s_two_state.enabled && id && strlen(id) == 32 &&
        two_equal((const uint8_t *)id, (const uint8_t *)s_two_pending_id, 32) &&
        generation == s_two_pending_generation && generation == si_auth_credential_generation() &&
        esp_timer_get_time() < s_two_pending_until && two_attempt_allowed(&s_two_enroll)) {
        two_step_state_t candidate = s_two_pending;
        int64_t adopt = 0;
        bool valid = two_check_code(&candidate, code, false, client_unix, &adopt);
        two_attempt_result(&s_two_enroll, valid);
        if (valid) {
            /* Persist inside the credential fence; sessions are revoked either way. */
            ret = si_auth_security_commit(generation, two_commit_transition, &candidate);
            if (ret == ESP_OK) two_adopt_clock(adopt);
            si_secret_store_clear(&s_two_pending, sizeof(s_two_pending));
            si_secret_store_clear(s_two_pending_id, sizeof(s_two_pending_id));
        }
        si_secret_store_clear(&candidate, sizeof(candidate));
    }
    xSemaphoreGive(s_two_lock);
    return ret;
}

esp_err_t si_two_step_disable(const char *code, int64_t client_unix, uint32_t generation)
{
    if (!two_lock()) return ESP_FAIL;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_two_storage_ok && s_two_state.enabled && generation == si_auth_credential_generation() && two_attempt_allowed(&s_two_active)) {
        two_step_state_t candidate = s_two_state;
        int64_t adopt = 0;
        bool valid = two_check_code(&candidate, code, true, client_unix, &adopt);
        two_attempt_result(&s_two_active, valid);
        if (valid) {
            si_secret_store_clear(&candidate, sizeof(candidate));
            ret = si_auth_security_commit(generation, two_commit_transition, &candidate);
            if (ret == ESP_OK) two_adopt_clock(adopt);
        }
        si_secret_store_clear(&candidate, sizeof(candidate));
    }
    xSemaphoreGive(s_two_lock);
    return ret;
}
