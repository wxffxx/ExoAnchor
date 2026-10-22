/* Bounded WebAuthn RP for one local administrator. ES256, UV required,
 * discoverable credentials, none attestation. Private keys stay in authenticators.
 * All challenges are single-use, monotonic-time limited, and generation fenced.
 * Anonymous login challenges are stateless (boot-key MAC), so requests cannot
 * exhaust server slots; only accepted logins occupy the bounded spent set. */
#include "passkey.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "auth_service.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "settings_store.h"

#define PK_LABEL_MAX 48
#define PK_RECORD_MAX 3900
#define PK_CHALLENGES 1
#define PK_SPENT 8
#define PK_TIMEOUT_US 120000000LL
#define PK_DATA_MAX 1024
#define PK_B64_MAX(n) (((n) * 4 + 2) / 3 + 1)

typedef struct {
    uint8_t id[SI_PASSKEY_ID_MAX], public_key[65];
    size_t id_len;
    uint32_t counter;
    bool backup_eligible;
    char label[PK_LABEL_MAX + 1];
} pk_credential_t;
typedef struct {
    char origin[SI_PASSKEY_ORIGIN_MAX + 1], rp[254];
    uint8_t user[32];
    unsigned count;
    pk_credential_t keys[SI_PASSKEY_LIMIT];
} pk_state_t;
typedef struct {
    char challenge[44], session_id[SI_AUTH_TOKEN_LEN + 1], label[PK_LABEL_MAX + 1];
    bool registration;
    uint32_t generation;
    int64_t expires;
} pk_challenge_t;
typedef struct {
    uint8_t nonce[16];
    int64_t expires;
} pk_spent_t;
static pk_state_t s_pk;
static pk_challenge_t s_pk_pending[PK_CHALLENGES];
static pk_spent_t s_pk_spent[PK_SPENT];
static uint8_t s_pk_login_key[32];
static bool s_pk_login_key_ready;
static SemaphoreHandle_t s_pk_lock;
static bool s_pk_loaded, s_pk_storage_ok;

static const char *pk_string(const cJSON *j, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static bool pk_equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    unsigned diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= x[i] ^ y[i];
    return !diff;
}
static void pk_encode(const uint8_t *p, size_t n, char *out)
{
    const char *abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    uint32_t value = 0; unsigned bits = 0; size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        value = (value << 8) | p[i]; bits += 8;
        while (bits >= 6) { bits -= 6; out[pos++] = abc[(value >> bits) & 63]; }
    }
    if (bits) out[pos++] = abc[(value << (6 - bits)) & 63];
    out[pos] = 0;
}
bool si_passkey_session_binding(const char *token, char out[44])
{
    uint8_t hash[32];
    if (!token || !token[0] || mbedtls_sha256((const uint8_t *)token, strlen(token), hash, 0)) return false;
    pk_encode(hash, 32, out); return true;
}
static bool pk_decode(const char *s, uint8_t *out, size_t cap, size_t *len)
{
    if (!s) return false;
    size_t n = strlen(s), pos = 0;
    if (!n || n % 4 == 1 || n >= PK_B64_MAX(cap)) return false;
    uint32_t value = 0; unsigned bits = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i]; int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-') v = 62;
        else if (c == '_') v = 63;
        else return false;
        value = (value << 6) | (unsigned)v; bits += 6;
        if (bits >= 8) {
            bits -= 8; if (pos == cap) return false;
            out[pos++] = (uint8_t)(value >> bits);
        }
    }
    if (bits && (value & ((1U << bits) - 1))) return false;
    *len = pos; return true;
}
/* Login challenge = nonce[16] | expires_us[8] | MAC[8], with
 * MAC = HMAC-SHA256(boot key, nonce | expires | generation). */
static bool pk_login_mac(const uint8_t raw[24], uint32_t generation, uint8_t mac[8])
{
    uint8_t message[28], digest[32];
    memcpy(message, raw, 24);
    for (unsigned i = 0; i < 4; ++i) message[24 + i] = (uint8_t)(generation >> (24 - 8 * i));
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, s_pk_login_key, sizeof(s_pk_login_key), message, sizeof(message), digest)) return false;
    memcpy(mac, digest, 8); return true;
}
static bool pk_login_challenge(uint32_t generation, int64_t expires, char out[44])
{
    uint8_t raw[32];
    esp_fill_random(raw, 16);
    for (unsigned i = 0; i < 8; ++i) raw[16 + i] = (uint8_t)((uint64_t)expires >> (56 - 8 * i));
    if (!pk_login_mac(raw, generation, raw + 24)) return false;
    pk_encode(raw, sizeof(raw), out); return true;
}
/* Returns the spent-set slot to record on success, or -1 for an invalid,
 * expired, replayed, or (when 8 logins are live) unrecordable challenge. */
static int pk_login_ticket(const char *challenge, uint32_t generation, int64_t now, pk_challenge_t *ticket)
{
    uint8_t raw[32], mac[8]; size_t n; int64_t expires = 0;
    if (!pk_decode(challenge, raw, sizeof(raw), &n) || n != sizeof(raw) ||
        !pk_login_mac(raw, generation, mac) || !pk_equal(mac, raw + 24, 8)) return -1;
    for (unsigned i = 0; i < 8; ++i) expires = (int64_t)(((uint64_t)expires << 8) | raw[16 + i]);
    if (expires <= now || expires > now + PK_TIMEOUT_US) return -1;
    int slot = -1;
    for (unsigned i = 0; i < PK_SPENT; ++i) {
        if (s_pk_spent[i].expires > now) {
            if (pk_equal(s_pk_spent[i].nonce, raw, 16)) return -1;
        } else if (slot < 0) slot = (int)i;
    }
    if (slot >= 0) {
        memcpy(s_pk_spent[slot].nonce, raw, 16);
        *ticket = (pk_challenge_t){.generation = generation, .expires = expires};
    }
    return slot;
}
/* Canonical HTTPS origin, exact RP host (no parent-domain or IP RP IDs).
 * ASCII/punycode DNS only. Default port must be omitted as browsers do. */
static bool pk_origin(const char *origin, char rp[254])
{
    if (!origin || strlen(origin) > SI_PASSKEY_ORIGIN_MAX || strncmp(origin, "https://", 8)) return false;
    const char *host = origin + 8, *port = strchr(host, ':');
    size_t n = port ? (size_t)(port - host) : strlen(host);
    if (!n || n > 253) return false;
    size_t label = 0; bool alpha = false;
    for (size_t i = 0; i < n; ++i) {
        char c = host[i];
        if (c == '.') {
            if (!label || host[i - 1] == '-') return false;
            label = 0; alpha = false;
        } else {
            if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-') return false;
            if ((!label && c == '-') || ++label > 63) return false;
            if (c >= 'a' && c <= 'z') alpha = true;
        }
    }
    if (!label || !alpha || host[n - 1] == '-') return false;
    if (port) {
        unsigned value = 0; size_t digits = strlen(port + 1);
        if (!digits || digits > 5 || port[1] == '0') return false;
        for (const char *p = port + 1; *p; ++p) {
            if (*p < '0' || *p > '9') return false;
            value = value * 10 + (unsigned)(*p - '0');
        }
        if (!value || value > 65535 || value == 443) return false;
    }
    memcpy(rp, host, n); rp[n] = 0;
    return true;
}
bool si_passkey_origin_valid(const char *origin) { char rp[254]; return pk_origin(origin, rp); }

/* Reject duplicate keys, deeply nested data, trailing input, and embedded NULs.
 * cJSON otherwise accepts duplicate names and truncates decoded NUL strings. */
static bool pk_unique(const cJSON *j, unsigned depth)
{
    if (depth > 8) return false;
    for (const cJSON *a = j->child; a; a = a->next) {
        if (cJSON_IsObject(j)) {
            if (!a->string) return false;
            for (const cJSON *b = a->next; b; b = b->next)
                if (b->string && !strcmp(a->string, b->string)) return false;
        }
        if (!pk_unique(a, depth + 1)) return false;
    }
    return true;
}
cJSON *si_passkey_parse_json(const char *s)
{
    if (!s || strstr(s, "\\u0000")) return NULL;
    unsigned depth = 0; bool quoted = false, escaped = false;
    for (const char *p = s; *p; ++p) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (*p == '\\') escaped = true;
            else if (*p == '"') quoted = false;
        } else if (*p == '"') quoted = true;
        else if (*p == '{' || *p == '[') { if (++depth > 8) return NULL; }
        else if (*p == '}' || *p == ']') { if (!depth) return NULL; --depth; }
    }
    if (quoted || depth) return NULL;
    cJSON *j = cJSON_ParseWithOpts(s, NULL, true);
    if (!cJSON_IsObject(j) || !pk_unique(j, 0)) { cJSON_Delete(j); return NULL; }
    return j;
}
static bool pk_key_parse(const uint8_t point[65], mbedtls_pk_context *key)
{
    static const uint8_t prefix[] = {0x30,0x59,0x30,0x13,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,0x01,0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07,0x03,0x42,0x00};
    uint8_t der[sizeof(prefix) + 65];
    memcpy(der, prefix, sizeof(prefix)); memcpy(der + sizeof(prefix), point, 65);
    return point[0] == 4 && mbedtls_pk_parse_public_key(key, der, sizeof(der)) == 0;
}
static cJSON *pk_record(const pk_state_t *s)
{
    cJSON *j = cJSON_CreateObject();
    char text[PK_B64_MAX(SI_PASSKEY_ID_MAX)];
    if (!j) return NULL;
    cJSON_AddNumberToObject(j, "version", 1);
    cJSON_AddStringToObject(j, "origin", s->origin);
    pk_encode(s->user, 32, text); cJSON_AddStringToObject(j, "user", text);
    cJSON *keys = cJSON_AddArrayToObject(j, "keys");
    for (unsigned i = 0; i < s->count; ++i) {
        const pk_credential_t *k = &s->keys[i];
        cJSON *v = cJSON_CreateObject();
        pk_encode(k->id, k->id_len, text); cJSON_AddStringToObject(v, "id", text);
        pk_encode(k->public_key, 65, text); cJSON_AddStringToObject(v, "key", text);
        cJSON_AddStringToObject(v, "label", k->label);
        cJSON_AddNumberToObject(v, "counter", k->counter);
        cJSON_AddBoolToObject(v, "be", k->backup_eligible);
        cJSON_AddItemToArray(keys, v);
    }
    return j;
}
static bool pk_read_record(const cJSON *j, pk_state_t *s)
{
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(j, "version");
    const char *origin = pk_string(j, "origin"); size_t n;
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 || !pk_origin(origin, s->rp)) return false;
    strcpy(s->origin, origin);
    if (!pk_decode(pk_string(j, "user"), s->user, 32, &n) || n != 32) return false;
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(j, "keys");
    if (!cJSON_IsArray(keys) || cJSON_GetArraySize(keys) > SI_PASSKEY_LIMIT) return false;
    const cJSON *v;
    cJSON_ArrayForEach(v, keys) {
        pk_credential_t *k = &s->keys[s->count];
        const char *label = pk_string(v, "label");
        const cJSON *count = cJSON_GetObjectItemCaseSensitive(v, "counter");
        const cJSON *be = cJSON_GetObjectItemCaseSensitive(v, "be");
        if (!label || strlen(label) > PK_LABEL_MAX || !cJSON_IsBool(be) ||
            !cJSON_IsNumber(count) || count->valuedouble < 0 || count->valuedouble > UINT32_MAX ||
            count->valuedouble != (double)(uint32_t)count->valuedouble ||
            !pk_decode(pk_string(v, "id"), k->id, sizeof(k->id), &k->id_len) ||
            !pk_decode(pk_string(v, "key"), k->public_key, 65, &n) || n != 65) return false;
        for (unsigned i = 0; i < s->count; ++i)
            if (s->keys[i].id_len == k->id_len && pk_equal(s->keys[i].id, k->id, k->id_len)) return false;
        mbedtls_pk_context key; mbedtls_pk_init(&key);
        bool valid = pk_key_parse(k->public_key, &key); mbedtls_pk_free(&key);
        if (!valid) return false;
        strcpy(k->label, label); k->counter = (uint32_t)count->valuedouble;
        k->backup_eligible = cJSON_IsTrue(be); ++s->count;
    }
    return true;
}
static bool pk_lock(void)
{
    return s_pk_lock && xSemaphoreTake(s_pk_lock, pdMS_TO_TICKS(1000)) == pdTRUE;
}
esp_err_t si_passkey_initialize(void)
{
    if (!s_pk_lock) s_pk_lock = xSemaphoreCreateMutex();
    if (!pk_lock()) return ESP_FAIL;
    if (!s_pk_login_key_ready) {
        esp_fill_random(s_pk_login_key, sizeof(s_pk_login_key));
        s_pk_login_key_ready = true;
    }
    if (!s_pk_loaded) {
        s_pk_loaded = true;
        si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
        esp_err_t ret = si_settings_store_open_read(&store, "si_passkey");
        if (ret == ESP_ERR_NOT_FOUND) s_pk_storage_ok = true;
        else if (ret == ESP_OK) {
            char *record = calloc(1, PK_RECORD_MAX);
            if (record && si_settings_store_get_string(&store, "record", record, PK_RECORD_MAX) == ESP_OK) {
                cJSON *j = si_passkey_parse_json(record);
                if (j) s_pk_storage_ok = pk_read_record(j, &s_pk);
                cJSON_Delete(j);
            }
            free(record); si_settings_store_close(&store);
        }
    }
    bool ok = s_pk_storage_ok;
    xSemaphoreGive(s_pk_lock); return ok ? ESP_OK : ESP_FAIL;
}
static esp_err_t pk_save(const pk_state_t *candidate)
{
    cJSON *j = pk_record(candidate);
    /* Round-trip validates required fields even if cJSON allocation failed. */
    pk_state_t *check = calloc(1, sizeof(*check));
    bool valid = j && check && pk_read_record(j, check) && check->count == candidate->count &&
        !strcmp(check->origin, candidate->origin) && pk_equal(check->user, candidate->user, 32);
    for (unsigned i = 0; valid && i < candidate->count; ++i) {
        const pk_credential_t *a = &check->keys[i], *b = &candidate->keys[i];
        valid = a->id_len == b->id_len && a->counter == b->counter && a->backup_eligible == b->backup_eligible &&
            !strcmp(a->label, b->label) && pk_equal(a->id, b->id, b->id_len) && pk_equal(a->public_key, b->public_key, 65);
    }
    free(check);
    char *text = valid ? cJSON_PrintUnformatted(j) : NULL; cJSON_Delete(j);
    if (!text) return ESP_ERR_NO_MEM;
    if (strlen(text) >= PK_RECORD_MAX) { cJSON_free(text); return ESP_ERR_INVALID_SIZE; }
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(&store, "si_passkey");
    if (ret == ESP_OK) ret = si_settings_store_set_string(&store, "record", text);
    if (ret == ESP_OK) ret = si_settings_store_commit(&store);
    si_settings_store_close(&store); cJSON_free(text);
    if (ret == ESP_OK) s_pk = *candidate;
    else s_pk_storage_ok = false;
    return ret;
}
static esp_err_t pk_commit(void *context) { return pk_save(context); }

cJSON *si_passkey_status(bool list)
{
    if (!pk_lock()) return NULL;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "storage_ok", s_pk_storage_ok);
    cJSON_AddBoolToObject(j, "enabled", s_pk_storage_ok && s_pk.count > 0);
    cJSON_AddStringToObject(j, "origin", s_pk_storage_ok ? s_pk.origin : "");
    cJSON_AddNumberToObject(j, "limit", SI_PASSKEY_LIMIT);
    if (list) {
        cJSON *keys = cJSON_AddArrayToObject(j, "credentials");
        for (unsigned i = 0; s_pk_storage_ok && i < s_pk.count; ++i) {
            cJSON *k = cJSON_CreateObject(); char id[PK_B64_MAX(SI_PASSKEY_ID_MAX)];
            pk_encode(s_pk.keys[i].id, s_pk.keys[i].id_len, id);
            cJSON_AddStringToObject(k, "id", id); cJSON_AddStringToObject(k, "label", s_pk.keys[i].label);
            cJSON_AddItemToArray(keys, k);
        }
    }
    xSemaphoreGive(s_pk_lock); return j;
}
esp_err_t si_passkey_configure(const char *origin, uint32_t generation)
{
    char rp[254]; if (!pk_origin(origin, rp) || !pk_lock()) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_pk_storage_ok && !s_pk.count && generation == si_auth_credential_generation()) {
        pk_state_t *candidate = calloc(1, sizeof(*candidate));
        if (!candidate) ret = ESP_ERR_NO_MEM;
        else {
            strcpy(candidate->origin, origin); strcpy(candidate->rp, rp);
            esp_fill_random(candidate->user, sizeof(candidate->user));
            ret = si_auth_security_commit(generation, pk_commit, candidate);
            free(candidate); memset(s_pk_pending, 0, sizeof(s_pk_pending));
        }
    }
    xSemaphoreGive(s_pk_lock); return ret;
}
esp_err_t si_passkey_remove(const char *id, uint32_t generation)
{
    uint8_t bytes[SI_PASSKEY_ID_MAX]; size_t n;
    if (!pk_decode(id, bytes, sizeof(bytes), &n) || !pk_lock()) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; s_pk_storage_ok && generation == si_auth_credential_generation() && i < s_pk.count; ++i) {
        if (s_pk.keys[i].id_len != n || !pk_equal(bytes, s_pk.keys[i].id, n)) continue;
        pk_state_t *candidate = malloc(sizeof(*candidate));
        if (!candidate) { ret = ESP_ERR_NO_MEM; break; }
        *candidate = s_pk;
        memmove(&candidate->keys[i], &candidate->keys[i + 1], (candidate->count - i - 1) * sizeof(candidate->keys[0]));
        memset(&candidate->keys[--candidate->count], 0, sizeof(candidate->keys[0]));
        ret = si_auth_security_commit(generation, pk_commit, candidate);
        free(candidate); memset(s_pk_pending, 0, sizeof(s_pk_pending)); break;
    }
    xSemaphoreGive(s_pk_lock); return ret;
}
esp_err_t si_passkey_begin(bool registration, const char *origin, uint32_t generation,
                           const char *session_id, const char *label, cJSON **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!origin || !pk_lock()) return ESP_FAIL;
    int64_t now = esp_timer_get_time(); int slot = -1;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (!s_pk_storage_ok || !s_pk.origin[0] || strcmp(origin, s_pk.origin) || generation != si_auth_credential_generation() ||
        (registration ? (s_pk.count >= SI_PASSKEY_LIMIT || !session_id || !session_id[0] || strlen(session_id) > SI_AUTH_TOKEN_LEN || !label || !label[0] || strlen(label) > PK_LABEL_MAX) : !s_pk.count)) goto done;
    pk_challenge_t login = {0}, *p = &login;
    if (registration) {
        /* Authenticated enrollment owns server state; never evict another
         * session's live challenge. */
        for (unsigned i = 0; i < PK_CHALLENGES; ++i) {
            if (s_pk_pending[i].expires <= now || s_pk_pending[i].generation != generation ||
                !strcmp(s_pk_pending[i].session_id, session_id)) {
                memset(&s_pk_pending[i], 0, sizeof(s_pk_pending[i]));
                if (slot < 0) slot = (int)i;
            }
        }
        if (slot < 0) goto done;
        p = &s_pk_pending[slot]; uint8_t random[32]; esp_fill_random(random, sizeof(random));
        pk_encode(random, sizeof(random), p->challenge);
        p->generation = generation; p->registration = true; p->expires = now + PK_TIMEOUT_US;
    } else if (!pk_login_challenge(generation, now + PK_TIMEOUT_US, login.challenge)) {
        ret = ESP_FAIL; goto done;
    }
    cJSON *j = cJSON_CreateObject();
    if (!j) { memset(p, 0, sizeof(*p)); ret = ESP_ERR_NO_MEM; goto done; }
    cJSON_AddStringToObject(j, "challenge", p->challenge);
    cJSON_AddNumberToObject(j, "timeout", PK_TIMEOUT_US / 1000);
    if (registration) {
        strcpy(p->session_id, session_id); strcpy(p->label, label);
        cJSON *rp = cJSON_AddObjectToObject(j, "rp"), *user = cJSON_AddObjectToObject(j, "user");
        cJSON_AddStringToObject(rp, "id", s_pk.rp); cJSON_AddStringToObject(rp, "name", "ExoAnchor");
        char uid[44]; pk_encode(s_pk.user, 32, uid);
        si_auth_status_t status; si_auth_get_status(&status);
        cJSON_AddStringToObject(user, "id", uid); cJSON_AddStringToObject(user, "name", status.username);
        cJSON_AddStringToObject(user, "displayName", status.username);
        cJSON *params = cJSON_AddArrayToObject(j, "pubKeyCredParams"), *param = cJSON_CreateObject();
        cJSON_AddStringToObject(param, "type", "public-key"); cJSON_AddNumberToObject(param, "alg", -7);
        cJSON_AddItemToArray(params, param);
        cJSON *selection = cJSON_AddObjectToObject(j, "authenticatorSelection");
        cJSON_AddStringToObject(selection, "residentKey", "required");
        cJSON_AddBoolToObject(selection, "requireResidentKey", true);
        cJSON_AddStringToObject(selection, "userVerification", "required");
        cJSON_AddStringToObject(j, "attestation", "none");
        cJSON *excluded = cJSON_AddArrayToObject(j, "excludeCredentials");
        for (unsigned i = 0; i < s_pk.count; ++i) {
            char id[PK_B64_MAX(SI_PASSKEY_ID_MAX)]; pk_encode(s_pk.keys[i].id, s_pk.keys[i].id_len, id);
            cJSON *v = cJSON_CreateObject(); cJSON_AddStringToObject(v, "type", "public-key");
            cJSON_AddStringToObject(v, "id", id); cJSON_AddItemToArray(excluded, v);
        }
    } else {
        cJSON_AddStringToObject(j, "rpId", s_pk.rp);
        cJSON_AddStringToObject(j, "userVerification", "required");
    }
    *out = j; ret = ESP_OK;
done:
    xSemaphoreGive(s_pk_lock); return ret;
}

/* Small definite-length CBOR reader; no allocations, tags, indefinite lengths,
 * or unbounded recursion. Only ES256 COSE keys and none attestation are accepted. */
typedef struct { const uint8_t *p, *end; } pk_cbor_t;
static bool pk_head(pk_cbor_t *c, unsigned *type, uint32_t *n)
{
    if (c->p == c->end) return false;
    unsigned b = *c->p++; *type = b >> 5; b &= 31;
    if (b < 24) { *n = b; return true; }
    unsigned bytes = b == 24 ? 1 : b == 25 ? 2 : b == 26 ? 4 : 0;
    if (!bytes || (size_t)(c->end - c->p) < bytes) return false;
    *n = 0; for (unsigned i = 0; i < bytes; ++i) *n = (*n << 8) | *c->p++;
    return true;
}
static bool pk_skip(pk_cbor_t *c, unsigned depth)
{
    unsigned type; uint32_t n;
    if (depth > 6 || !pk_head(c, &type, &n)) return false;
    if (type <= 1 || (type == 7 && n <= 23)) return true;
    if (type == 2 || type == 3) {
        if (n > (size_t)(c->end - c->p)) return false;
        c->p += n; return true;
    }
    if ((type != 4 && type != 5) || n > 32) return false;
    for (uint32_t i = 0; i < n * (type == 5 ? 2U : 1U); ++i)
        if (!pk_skip(c, depth + 1)) return false;
    return true;
}
static bool pk_bytes(pk_cbor_t *c, unsigned expected, const uint8_t **p, size_t *len)
{
    unsigned type; uint32_t n;
    if (!pk_head(c, &type, &n) || type != expected || n > (size_t)(c->end - c->p)) return false;
    *p = c->p; *len = n; c->p += n; return true;
}
static bool pk_integer(pk_cbor_t *c, int *value)
{
    unsigned type; uint32_t n;
    if (!pk_head(c, &type, &n) || type > 1 || n > 1000) return false;
    *value = type == 0 ? (int)n : -1 - (int)n; return true;
}
static bool pk_cose(pk_cbor_t *c, uint8_t key[65])
{
    unsigned type, seen = 0; uint32_t count;
    if (!pk_head(c, &type, &count) || type != 5 || count != 5) return false;
    for (unsigned i = 0; i < count; ++i) {
        int name, value; unsigned bit;
        if (!pk_integer(c, &name)) return false;
        switch (name) { case 1: bit=1; break; case 3: bit=2; break; case -1: bit=4; break; case -2: bit=8; break; case -3: bit=16; break; default: return false; }
        if (seen & bit) return false;
        seen |= bit;
        if (name >= -1) {
            if (!pk_integer(c, &value) || value != (name == 1 ? 2 : name == 3 ? -7 : 1)) return false;
        } else {
            const uint8_t *p; size_t n;
            if (!pk_bytes(c, 2, &p, &n) || n != 32) return false;
            memcpy(key + (name == -2 ? 1 : 33), p, 32);
        }
    }
    key[0] = 4; return seen == 31;
}
static bool pk_attestation(const uint8_t *bytes, size_t len, const uint8_t **data, size_t *data_len)
{
    pk_cbor_t c = {bytes, bytes + len}; unsigned type, seen = 0; uint32_t count;
    if (!pk_head(&c, &type, &count) || type != 5 || count != 3) return false;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *name, *value; size_t n, v; unsigned bit;
        if (!pk_bytes(&c, 3, &name, &n)) return false;
        if (n == 3 && !memcmp(name, "fmt", 3)) {
            bit = 1;
            if (!pk_bytes(&c, 3, &value, &v) || v != 4 || memcmp(value, "none", 4)) return false;
        } else if (n == 7 && !memcmp(name, "attStmt", 7)) {
            bit = 2; uint32_t size;
            if (!pk_head(&c, &type, &size) || type != 5 || size) return false;
        } else if (n == 8 && !memcmp(name, "authData", 8)) {
            bit = 4; if (!pk_bytes(&c, 2, data, data_len)) return false;
        } else return false;
        if (seen & bit) return false;
        seen |= bit;
    }
    return seen == 7 && c.p == c.end;
}
static uint32_t pk_u32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static bool pk_auth_data(const uint8_t *data, size_t n, bool registration, pk_credential_t *key)
{
    uint8_t hash[32];
    if (n < 37 || mbedtls_sha256((const uint8_t *)s_pk.rp, strlen(s_pk.rp), hash, 0) || !pk_equal(hash, data, 32)) return false;
    unsigned flags = data[32];
    if ((flags & 5) != 5 || (flags & 0x22) || ((flags & 0x10) && !(flags & 8)) || !!(flags & 0x40) != registration) return false;
    pk_cbor_t c = {data + 37, data + n};
    if (registration) {
        if (n < 55) return false;
        size_t id_len = ((size_t)data[53] << 8) | data[54];
        if (id_len != key->id_len || n < 55 + id_len || !pk_equal(data + 55, key->id, id_len)) return false;
        c.p = data + 55 + id_len;
        if (!pk_cose(&c, key->public_key)) return false;
        mbedtls_pk_context parsed; mbedtls_pk_init(&parsed);
        bool valid = pk_key_parse(key->public_key, &parsed); mbedtls_pk_free(&parsed);
        if (!valid) return false;
        key->backup_eligible = !!(flags & 8);
    } else if (!!(flags & 8) != key->backup_eligible) return false;
    if (flags & 0x80) {
        if (c.p == c.end || (*c.p >> 5) != 5 || !pk_skip(&c, 0)) return false;
    }
    if (c.p != c.end) return false;
    uint32_t counter = pk_u32(data + 33);
    if (!registration && (counter || key->counter) && counter <= key->counter) return false;
    key->counter = counter; return true;
}
esp_err_t si_passkey_finish(bool registration, const char *origin, const char *session_id,
                            const cJSON *response, uint32_t *accepted_generation)
{
    if (!origin || !response || !accepted_generation || !pk_lock()) return ESP_FAIL;
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    uint8_t *client = calloc(1, PK_DATA_MAX + 1), *bytes = malloc(PK_DATA_MAX + 32);
    cJSON *json = NULL; pk_state_t *candidate = NULL;
    size_t client_len = 0, data_len = 0;
    const char *type = pk_string(response, "type");
    if (!s_pk_storage_ok || strcmp(origin, s_pk.origin) || !client || !bytes || !pk_unique(response, 0) ||
        !type || strcmp(type, "public-key") || !pk_decode(pk_string(response, "clientDataJSON"), client, PK_DATA_MAX, &client_len) || memchr(client, 0, client_len)) goto done;
    json = si_passkey_parse_json((const char *)client);
    if (!json) goto done;
    const char *challenge = pk_string(json, "challenge"); pk_challenge_t ticket = {0};
    if (!challenge || strlen(challenge) != 43) goto done;
    uint32_t generation = si_auth_credential_generation();
    int64_t now = esp_timer_get_time();
    int spent = -1;
    if (registration) {
        /* Consume before validation: failed responses cannot retry a challenge. */
        for (unsigned i = 0; i < PK_CHALLENGES; ++i) {
            pk_challenge_t *p = &s_pk_pending[i];
            if (pk_equal(challenge, p->challenge, 43)) { ticket = *p; memset(p, 0, sizeof(*p)); break; }
        }
    } else if ((spent = pk_login_ticket(challenge, generation, now, &ticket)) < 0) goto done;
    if (!ticket.expires || ticket.expires <= now || ticket.registration != registration || ticket.generation != generation ||
        (registration && (!session_id || strcmp(session_id, ticket.session_id)))) goto done;
    const char *client_type = pk_string(json, "type"), *client_origin = pk_string(json, "origin");
    const cJSON *cross = cJSON_GetObjectItemCaseSensitive(json, "crossOrigin");
    if (!client_type || strcmp(client_type, registration ? "webauthn.create" : "webauthn.get") || !client_origin || strcmp(client_origin, s_pk.origin) ||
        (cross && !cJSON_IsFalse(cross)) || cJSON_GetObjectItemCaseSensitive(json, "topOrigin")) goto done;
    pk_credential_t key = {0};
    if (!pk_decode(pk_string(response, "rawId"), key.id, sizeof(key.id), &key.id_len)) goto done;
    int index = -1;
    for (unsigned i = 0; i < s_pk.count; ++i)
        if (key.id_len == s_pk.keys[i].id_len && pk_equal(key.id, s_pk.keys[i].id, key.id_len)) index = (int)i;
    if (registration) {
        const uint8_t *auth_data = NULL; size_t n = 0;
        if (index >= 0 || s_pk.count >= SI_PASSKEY_LIMIT || !pk_decode(pk_string(response, "attestationObject"), bytes, PK_DATA_MAX, &data_len) ||
            !pk_attestation(bytes, data_len, &auth_data, &n) || !pk_auth_data(auth_data, n, true, &key)) goto done;
        strcpy(key.label, ticket.label);
    } else {
        uint8_t user[32], signature[80], hash[32]; size_t user_len, signature_len;
        /* Discoverable login always requires the stored user handle. */
        if (index < 0 || !pk_decode(pk_string(response, "userHandle"), user, 32, &user_len) || user_len != 32 || !pk_equal(user, s_pk.user, 32)) goto done;
        key = s_pk.keys[index];
        if (!pk_decode(pk_string(response, "authenticatorData"), bytes, PK_DATA_MAX, &data_len) || !pk_auth_data(bytes, data_len, false, &key) ||
            !pk_decode(pk_string(response, "signature"), signature, sizeof(signature), &signature_len) ||
            mbedtls_sha256(client, client_len, bytes + data_len, 0) ||
            mbedtls_sha256(bytes, data_len + 32, hash, 0)) goto done;
        mbedtls_pk_context parsed; mbedtls_pk_init(&parsed);
        bool valid = pk_key_parse(key.public_key, &parsed) && mbedtls_pk_verify(&parsed, MBEDTLS_MD_SHA256, hash, 32, signature, signature_len) == 0;
        mbedtls_pk_free(&parsed); if (!valid) goto done;
    }
    if (generation != si_auth_credential_generation()) goto done;
    if (registration || key.counter != s_pk.keys[index].counter) {
        candidate = malloc(sizeof(*candidate)); if (!candidate) { ret = ESP_ERR_NO_MEM; goto done; }
        *candidate = s_pk;
        if (registration) candidate->keys[candidate->count++] = key;
        else candidate->keys[index] = key;
        ret = registration ? si_auth_security_commit(generation, pk_commit, candidate) : pk_save(candidate);
        if (ret != ESP_OK) goto done;
        if (registration) memset(s_pk_pending, 0, sizeof(s_pk_pending));
    }
    /* A verified login challenge is single-use until it would have expired. */
    if (spent >= 0) s_pk_spent[spent].expires = ticket.expires;
    *accepted_generation = generation; ret = ESP_OK;
done:
    free(candidate); cJSON_Delete(json); free(client); free(bytes);
    xSemaphoreGive(s_pk_lock); return ret;
}
