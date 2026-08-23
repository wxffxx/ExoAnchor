#include "agent_event_store.h"

#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "storage_layout.h"

#ifdef ESP_PLATFORM
#include "mbedtls/sha256.h"
#include "storage_manager.h"
#endif

#ifndef SI_AGENT_EVENT_STORE_DIR
#define SI_AGENT_EVENT_STORE_DIR SI_STORAGE_AGENT_DIR
#endif

#ifndef SI_AGENT_EVENT_STORE_FILE
#define SI_AGENT_EVENT_STORE_FILE SI_AGENT_EVENT_STORE_DIR "/TASKS.LOG"
#endif

#define SI_AGENT_EVENT_ERROR_MAX 192U
#define SI_AGENT_EVENT_JSON_FIELD_COUNT 16U
#define SI_AGENT_EVENT_JSON_SAFE_INTEGER UINT64_C(9007199254740991)
#define SI_AGENT_EVENT_PAYLOAD_MAX_DEPTH 16U
#define SI_AGENT_EVENT_LOCK_WAIT_MS 250U
#define SI_AGENT_EVENT_ZERO_HASH                                                \
    "0000000000000000000000000000000000000000000000000000000000000000"

static SemaphoreHandle_t s_event_store_lock;
static bool s_event_store_ready;
static uint64_t s_event_store_latest_seq;
static uint64_t s_event_store_file_bytes;
static char s_event_store_latest_hash[SI_AGENT_EVENT_HASH_HEX_LEN + 1U] =
    SI_AGENT_EVENT_ZERO_HASH;
static char s_event_store_last_error[SI_AGENT_EVENT_ERROR_MAX];

/*
 * ESP builds use the project's mbedTLS dependency.  The small host-only
 * implementation keeps POSIX contract tests independent of platform crypto
 * packages while producing the same SHA-256 bytes.
 */
#ifndef ESP_PLATFORM
typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_used;
} si_host_sha256_t;

static uint32_t sha_rotr(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32U - count));
}

static void sha_transform(si_host_sha256_t *ctx, const uint8_t block[64])
{
    static const uint32_t k[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };
    uint32_t words[64];
    for (size_t i = 0; i < 16U; ++i) {
        const size_t offset = i * 4U;
        words[i] = ((uint32_t)block[offset] << 24U) |
                   ((uint32_t)block[offset + 1U] << 16U) |
                   ((uint32_t)block[offset + 2U] << 8U) |
                   (uint32_t)block[offset + 3U];
    }
    for (size_t i = 16U; i < 64U; ++i) {
        const uint32_t s0 = sha_rotr(words[i - 15U], 7U) ^
                            sha_rotr(words[i - 15U], 18U) ^
                            (words[i - 15U] >> 3U);
        const uint32_t s1 = sha_rotr(words[i - 2U], 17U) ^
                            sha_rotr(words[i - 2U], 19U) ^
                            (words[i - 2U] >> 10U);
        words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];
    for (size_t i = 0; i < 64U; ++i) {
        const uint32_t sum1 = sha_rotr(e, 6U) ^ sha_rotr(e, 11U) ^
                              sha_rotr(e, 25U);
        const uint32_t choose = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + sum1 + choose + k[i] + words[i];
        const uint32_t sum0 = sha_rotr(a, 2U) ^ sha_rotr(a, 13U) ^
                              sha_rotr(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha_init(si_host_sha256_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
}

static void sha_update(si_host_sha256_t *ctx, const uint8_t *data, size_t len)
{
    while (len > 0U) {
        size_t amount = sizeof(ctx->block) - ctx->block_used;
        if (amount > len) {
            amount = len;
        }
        memcpy(ctx->block + ctx->block_used, data, amount);
        ctx->block_used += amount;
        data += amount;
        len -= amount;
        if (ctx->block_used == sizeof(ctx->block)) {
            sha_transform(ctx, ctx->block);
            ctx->bit_count += UINT64_C(512);
            ctx->block_used = 0U;
        }
    }
}

static void sha_finish(si_host_sha256_t *ctx, uint8_t digest[32])
{
    ctx->bit_count += (uint64_t)ctx->block_used * 8U;
    ctx->block[ctx->block_used++] = 0x80U;
    if (ctx->block_used > 56U) {
        memset(ctx->block + ctx->block_used, 0,
               sizeof(ctx->block) - ctx->block_used);
        sha_transform(ctx, ctx->block);
        ctx->block_used = 0U;
    }
    memset(ctx->block + ctx->block_used, 0, 56U - ctx->block_used);
    for (size_t i = 0; i < 8U; ++i) {
        ctx->block[63U - i] = (uint8_t)(ctx->bit_count >> (i * 8U));
    }
    sha_transform(ctx, ctx->block);
    for (size_t i = 0; i < 8U; ++i) {
        digest[i * 4U] = (uint8_t)(ctx->state[i] >> 24U);
        digest[i * 4U + 1U] = (uint8_t)(ctx->state[i] >> 16U);
        digest[i * 4U + 2U] = (uint8_t)(ctx->state[i] >> 8U);
        digest[i * 4U + 3U] = (uint8_t)ctx->state[i];
    }
}
#endif

static void event_store_set_error(const char *fmt, ...)
{
    if (!fmt) {
        s_event_store_last_error[0] = '\0';
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_event_store_last_error, sizeof(s_event_store_last_error),
              fmt, args);
    va_end(args);
}

static void copy_text(char *out, size_t out_size, const char *value)
{
    if (!out || out_size == 0U) {
        return;
    }
    const char *source = value ? value : "";
    const size_t length = strlen(source);
    const size_t copied = length < out_size - 1U ? length : out_size - 1U;
    memcpy(out, source, copied);
    out[copied] = '\0';
}

static bool ascii_alnum(unsigned char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z');
}

static bool identifier_valid(const char *value, bool required)
{
    if (!value || value[0] == '\0') {
        return !required;
    }
    const size_t length = strlen(value);
    if (length > SI_AGENT_EVENT_ID_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = (unsigned char)value[i];
        if (!ascii_alnum(ch) && ch != '-' && ch != '_' && ch != '.') {
            return false;
        }
    }
    return true;
}

static bool event_type_valid(const char *value)
{
    if (!value || value[0] == '\0') {
        return false;
    }
    const size_t length = strlen(value);
    if (length > SI_AGENT_EVENT_TYPE_MAX_LEN ||
        !ascii_alnum((unsigned char)value[0])) {
        return false;
    }
    for (size_t i = 1U; i < length; ++i) {
        const unsigned char ch = (unsigned char)value[i];
        if (!ascii_alnum(ch) && ch != '-' && ch != '_' && ch != '.') {
            return false;
        }
    }
    return true;
}

static bool hash_valid(const char *value)
{
    if (!value || strlen(value) != SI_AGENT_EVENT_HASH_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < SI_AGENT_EVENT_HASH_HEX_LEN; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool hash_equal(const char *left, const char *right)
{
    if (!left || !right) {
        return false;
    }
    uint8_t difference = 0U;
    for (size_t i = 0; i < SI_AGENT_EVENT_HASH_HEX_LEN; ++i) {
        difference |= (uint8_t)(left[i] ^ right[i]);
    }
    return difference == 0U;
}

static bool payload_key_forbidden(const char *key)
{
    static const char *const forbidden[] = {
        "password", "passwd", "secret", "appsecret", "token",
        "access_token", "refresh_token", "api_key", "private_key",
        "ssh_key", "authorization", "cookie", "session_cookie",
        "credential", "credentials", "chain_of_thought", "reasoning",
        "raw_reasoning", "private_reasoning",
    };
    if (!key) {
        return false;
    }
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        if (strcasecmp(key, forbidden[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool payload_tree_safe(const cJSON *node, size_t depth)
{
    if (!node || depth > SI_AGENT_EVENT_PAYLOAD_MAX_DEPTH) {
        return false;
    }
    if (cJSON_IsInvalid(node) || cJSON_IsRaw(node) ||
        (cJSON_IsNumber(node) &&
         (node->valuedouble != node->valuedouble ||
          node->valuedouble > DBL_MAX || node->valuedouble < -DBL_MAX))) {
        return false;
    }
    if (cJSON_IsObject(node)) {
        for (const cJSON *child = node->child; child; child = child->next) {
            if (payload_key_forbidden(child->string) ||
                !payload_tree_safe(child, depth + 1U)) {
                return false;
            }
        }
    } else if (cJSON_IsArray(node)) {
        for (const cJSON *child = node->child; child; child = child->next) {
            if (!payload_tree_safe(child, depth + 1U)) {
                return false;
            }
        }
    }
    return true;
}

static esp_err_t sha256_hex(const uint8_t *data, size_t length,
                            char out[SI_AGENT_EVENT_HASH_HEX_LEN + 1U])
{
    if ((!data && length > 0U) || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t digest[32];
#ifdef ESP_PLATFORM
    if (mbedtls_sha256(data, length, digest, 0) != 0) {
        return ESP_FAIL;
    }
#else
    si_host_sha256_t ctx;
    sha_init(&ctx);
    sha_update(&ctx, data, length);
    sha_finish(&ctx, digest);
#endif
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        out[i * 2U] = hex[digest[i] >> 4U];
        out[i * 2U + 1U] = hex[digest[i] & 0x0fU];
    }
    out[SI_AGENT_EVENT_HASH_HEX_LEN] = '\0';
    return ESP_OK;
}

static esp_err_t event_hash_compute(const si_agent_event_record_t *record,
                                    char out[SI_AGENT_EVENT_HASH_HEX_LEN + 1U])
{
    if (!record || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    char material[1024];
    const int written = snprintf(
        material, sizeof(material),
        "%s\n%" PRIu64 "\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%" PRId64
        "\n%" PRId64 "\n%s\n%s",
        SI_AGENT_EVENT_SCHEMA, record->seq, record->thread_id,
        record->turn_id, record->run_id, record->step_id, record->request_id,
        record->action_id, record->artifact_id, record->event_type,
        record->server_time_ms, record->monotonic_time_ms,
        record->payload_hash, record->previous_hash);
    if (written < 0 || (size_t)written >= sizeof(material)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return sha256_hex((const uint8_t *)material, (size_t)written, out);
}

static esp_err_t payload_hash_compute(const cJSON *payload,
                                      char out[SI_AGENT_EVENT_HASH_HEX_LEN + 1U],
                                      size_t *json_len_out)
{
    if (!cJSON_IsObject(payload) || !payload_tree_safe(payload, 0U)) {
        event_store_set_error("payload must be a safe bounded object");
        return ESP_ERR_INVALID_ARG;
    }
    char *json = cJSON_PrintUnformatted(payload);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    const size_t length = strlen(json);
    if (json_len_out) {
        *json_len_out = length;
    }
    esp_err_t ret = ESP_OK;
    if (length == 0U || length > SI_AGENT_EVENT_PAYLOAD_MAX_BYTES) {
        event_store_set_error("payload size %u exceeds limit",
                              (unsigned)length);
        ret = ESP_ERR_INVALID_SIZE;
    } else {
        ret = sha256_hex((const uint8_t *)json, length, out);
    }
    cJSON_free(json);
    return ret;
}

static size_t json_field_count(const cJSON *object)
{
    size_t count = 0U;
    for (const cJSON *child = object ? object->child : NULL;
         child; child = child->next) {
        ++count;
    }
    return count;
}

static const char *json_required_string(const cJSON *root, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static bool json_safe_uint(const cJSON *item, uint64_t *value_out)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > (double)SI_AGENT_EVENT_JSON_SAFE_INTEGER) {
        return false;
    }
    const uint64_t value = (uint64_t)item->valuedouble;
    if ((double)value != item->valuedouble) {
        return false;
    }
    if (value_out) {
        *value_out = value;
    }
    return true;
}

static esp_err_t parse_and_verify_record(
    const char *json, size_t json_len, uint64_t expected_seq,
    const char expected_previous_hash[SI_AGENT_EVENT_HASH_HEX_LEN + 1U],
    si_agent_event_record_t *record_out, cJSON **root_out)
{
    if (!json || json_len == 0U || !record_out || !root_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *root_out = NULL;
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithOpts(json, &parse_end, true);
    if (!root || !cJSON_IsObject(root) || !parse_end ||
        (size_t)(parse_end - json) != json_len ||
        json_field_count(root) != SI_AGENT_EVENT_JSON_FIELD_COUNT) {
        cJSON_Delete(root);
        event_store_set_error("invalid event envelope JSON");
        return ESP_ERR_INVALID_STATE;
    }

    const char *schema = json_required_string(root, "schema");
    const char *thread_id = json_required_string(root, "thread_id");
    const char *turn_id = json_required_string(root, "turn_id");
    const char *run_id = json_required_string(root, "run_id");
    const char *step_id = json_required_string(root, "step_id");
    const char *request_id = json_required_string(root, "request_id");
    const char *action_id = json_required_string(root, "action_id");
    const char *artifact_id = json_required_string(root, "artifact_id");
    const char *event_type = json_required_string(root, "event_type");
    const char *payload_hash = json_required_string(root, "payload_hash");
    const char *previous_hash = json_required_string(root, "previous_hash");
    const char *event_hash = json_required_string(root, "event_hash");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    uint64_t seq = 0U;
    uint64_t server_time = 0U;
    uint64_t monotonic_time = 0U;
    const bool valid =
        schema && strcmp(schema, SI_AGENT_EVENT_SCHEMA) == 0 &&
        identifier_valid(thread_id, true) && identifier_valid(turn_id, true) &&
        identifier_valid(run_id, false) && identifier_valid(step_id, false) &&
        identifier_valid(request_id, false) && identifier_valid(action_id, false) &&
        identifier_valid(artifact_id, false) && event_type_valid(event_type) &&
        hash_valid(payload_hash) && hash_valid(previous_hash) &&
        hash_valid(event_hash) &&
        json_safe_uint(cJSON_GetObjectItemCaseSensitive(root, "seq"), &seq) &&
        json_safe_uint(cJSON_GetObjectItemCaseSensitive(root, "server_time_ms"),
                       &server_time) &&
        json_safe_uint(cJSON_GetObjectItemCaseSensitive(root,
                                                        "monotonic_time_ms"),
                       &monotonic_time) &&
        cJSON_IsObject(payload);
    if (!valid || seq != expected_seq ||
        !hash_equal(previous_hash, expected_previous_hash)) {
        cJSON_Delete(root);
        event_store_set_error("invalid event fields, sequence, or hash link");
        return ESP_ERR_INVALID_STATE;
    }

    memset(record_out, 0, sizeof(*record_out));
    record_out->seq = seq;
    copy_text(record_out->thread_id, sizeof(record_out->thread_id), thread_id);
    copy_text(record_out->turn_id, sizeof(record_out->turn_id), turn_id);
    copy_text(record_out->run_id, sizeof(record_out->run_id), run_id);
    copy_text(record_out->step_id, sizeof(record_out->step_id), step_id);
    copy_text(record_out->request_id, sizeof(record_out->request_id), request_id);
    copy_text(record_out->action_id, sizeof(record_out->action_id), action_id);
    copy_text(record_out->artifact_id, sizeof(record_out->artifact_id), artifact_id);
    copy_text(record_out->event_type, sizeof(record_out->event_type), event_type);
    record_out->server_time_ms = (int64_t)server_time;
    record_out->monotonic_time_ms = (int64_t)monotonic_time;
    copy_text(record_out->payload_hash, sizeof(record_out->payload_hash), payload_hash);
    copy_text(record_out->previous_hash, sizeof(record_out->previous_hash), previous_hash);
    copy_text(record_out->event_hash, sizeof(record_out->event_hash), event_hash);
    record_out->payload = payload;

    char calculated_payload_hash[SI_AGENT_EVENT_HASH_HEX_LEN + 1U];
    char calculated_event_hash[SI_AGENT_EVENT_HASH_HEX_LEN + 1U];
    if (payload_hash_compute(payload, calculated_payload_hash, NULL) != ESP_OK ||
        event_hash_compute(record_out, calculated_event_hash) != ESP_OK ||
        !hash_equal(payload_hash, calculated_payload_hash) ||
        !hash_equal(event_hash, calculated_event_hash)) {
        cJSON_Delete(root);
        event_store_set_error("event payload or envelope hash mismatch");
        return ESP_ERR_INVALID_STATE;
    }

    *root_out = root;
    return ESP_OK;
}

static esp_err_t ensure_store_ready_locked(void)
{
#ifdef ESP_PLATFORM
    si_storage_tf_status_t status;
    si_storage_get_tf_status(&status);
    if (!status.mounted) {
        event_store_set_error("TF card is not mounted");
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t layout_ret = si_storage_ensure_layout();
    if (layout_ret != ESP_OK) {
        event_store_set_error("Agent journal directory is unavailable");
        return layout_ret;
    }
#else
    struct stat dir_stat;
    if (stat(SI_AGENT_EVENT_STORE_DIR, &dir_stat) != 0) {
        if (mkdir(SI_AGENT_EVENT_STORE_DIR, 0775) != 0 && errno != EEXIST) {
            event_store_set_error("create Agent journal directory failed errno=%d",
                                  errno);
            return ESP_FAIL;
        }
    } else if (!S_ISDIR(dir_stat.st_mode)) {
        event_store_set_error("Agent journal path is not a directory");
        return ESP_FAIL;
    }
#endif

    errno = 0;
    FILE *file = fopen(SI_AGENT_EVENT_STORE_FILE, "ab");
    if (!file) {
        event_store_set_error("open TASKS.LOG failed errno=%d", errno);
        return ESP_FAIL;
    }
    if (fclose(file) != 0) {
        event_store_set_error("close TASKS.LOG failed errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t event_store_lock(void)
{
    if (!s_event_store_lock) {
        event_store_set_error("event store is not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_event_store_lock,
                       pdMS_TO_TICKS(SI_AGENT_EVENT_LOCK_WAIT_MS)) != pdTRUE) {
        event_store_set_error("event store mutex timeout after %u ms",
                              (unsigned)SI_AGENT_EVENT_LOCK_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void recovery_reset(si_agent_event_recovery_t *recovery)
{
    if (!recovery) {
        return;
    }
    memset(recovery, 0, sizeof(*recovery));
    recovery->status = SI_AGENT_EVENT_RECOVERY_EMPTY;
    copy_text(recovery->latest_hash, sizeof(recovery->latest_hash),
              SI_AGENT_EVENT_ZERO_HASH);
}

static esp_err_t scan_locked(uint64_t after_seq,
                             si_agent_event_replay_cb_t callback,
                             void *user_ctx,
                             si_agent_event_recovery_t *recovery)
{
    recovery_reset(recovery);
    struct stat file_stat;
    errno = 0;
    if (stat(SI_AGENT_EVENT_STORE_FILE, &file_stat) != 0) {
        if (recovery) {
            recovery->status = SI_AGENT_EVENT_RECOVERY_IO_ERROR;
        }
        event_store_set_error("stat TASKS.LOG failed errno=%d", errno);
        return ESP_FAIL;
    }
    if (file_stat.st_size < 0 ||
        (uint64_t)file_stat.st_size > SI_AGENT_EVENT_JOURNAL_MAX_BYTES) {
        if (recovery) {
            recovery->status = SI_AGENT_EVENT_RECOVERY_CORRUPT;
            recovery->file_bytes = file_stat.st_size > 0
                                       ? (uint64_t)file_stat.st_size
                                       : 0U;
        }
        event_store_set_error("TASKS.LOG size is outside the journal limit");
        return ESP_ERR_INVALID_STATE;
    }
    if (recovery) {
        recovery->file_bytes = (uint64_t)file_stat.st_size;
    }

    errno = 0;
    FILE *file = fopen(SI_AGENT_EVENT_STORE_FILE, "rb");
    if (!file) {
        if (recovery) {
            recovery->status = SI_AGENT_EVENT_RECOVERY_IO_ERROR;
        }
        event_store_set_error("read TASKS.LOG failed errno=%d", errno);
        return ESP_FAIL;
    }
    char *line = malloc(SI_AGENT_EVENT_RECORD_MAX_BYTES + 1U);
    if (!line) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    uint64_t offset = 0U;
    uint64_t latest_seq = 0U;
    char latest_hash[SI_AGENT_EVENT_HASH_HEX_LEN + 1U];
    copy_text(latest_hash, sizeof(latest_hash), SI_AGENT_EVENT_ZERO_HASH);
    esp_err_t ret = ESP_OK;
    for (;;) {
        const uint64_t record_offset = offset;
        size_t line_len = 0U;
        bool oversized = false;
        bool terminated = false;
        int ch;
        while ((ch = fgetc(file)) != EOF) {
            ++offset;
            if (ch == '\n') {
                terminated = true;
                break;
            }
            if (line_len < SI_AGENT_EVENT_RECORD_MAX_BYTES) {
                line[line_len++] = (char)ch;
            } else {
                oversized = true;
            }
        }
        if (!terminated) {
            if (ferror(file)) {
                if (recovery) {
                    recovery->status = SI_AGENT_EVENT_RECOVERY_IO_ERROR;
                    recovery->fault_offset = record_offset;
                }
                event_store_set_error("read TASKS.LOG failed at offset %" PRIu64,
                                      record_offset);
                ret = ESP_FAIL;
            } else if (line_len > 0U || oversized) {
                if (recovery) {
                    recovery->status = SI_AGENT_EVENT_RECOVERY_TAIL_TRUNCATED;
                    recovery->fault_offset = record_offset;
                }
                event_store_set_error("TASKS.LOG has an unterminated tail at offset %" PRIu64,
                                      record_offset);
                ret = ESP_ERR_INVALID_STATE;
            }
            break;
        }
        if (oversized || line_len == 0U) {
            if (recovery) {
                recovery->status = SI_AGENT_EVENT_RECOVERY_CORRUPT;
                recovery->fault_offset = record_offset;
            }
            event_store_set_error("TASKS.LOG has an invalid record size at offset %" PRIu64,
                                  record_offset);
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        if (line[line_len - 1U] == '\r') {
            --line_len;
        }
        if (line_len == 0U || memchr(line, '\0', line_len) != NULL) {
            if (recovery) {
                recovery->status = SI_AGENT_EVENT_RECOVERY_CORRUPT;
                recovery->fault_offset = record_offset;
            }
            event_store_set_error("TASKS.LOG contains an invalid record at offset %" PRIu64,
                                  record_offset);
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        line[line_len] = '\0';

        si_agent_event_record_t record;
        cJSON *root = NULL;
        ret = parse_and_verify_record(line, line_len, latest_seq + 1U,
                                      latest_hash, &record, &root);
        if (ret != ESP_OK) {
            if (recovery) {
                recovery->status = SI_AGENT_EVENT_RECOVERY_CORRUPT;
                recovery->fault_offset = record_offset;
            }
            break;
        }
        latest_seq = record.seq;
        copy_text(latest_hash, sizeof(latest_hash), record.event_hash);
        if (recovery) {
            recovery->latest_seq = latest_seq;
            copy_text(recovery->latest_hash, sizeof(recovery->latest_hash),
                      latest_hash);
            ++recovery->valid_record_count;
            recovery->valid_bytes = offset;
        }
        if (callback && record.seq > after_seq) {
            ret = callback(&record, user_ctx);
        }
        cJSON_Delete(root);
        if (ret != ESP_OK) {
            break;
        }
    }

    free(line);
    if (fclose(file) != 0 && ret == ESP_OK) {
        if (recovery) {
            recovery->status = SI_AGENT_EVENT_RECOVERY_IO_ERROR;
        }
        event_store_set_error("close TASKS.LOG failed errno=%d", errno);
        return ESP_FAIL;
    }
    if (ret == ESP_OK && recovery && offset != recovery->file_bytes) {
        recovery->status = SI_AGENT_EVENT_RECOVERY_IO_ERROR;
        recovery->fault_offset = offset;
        event_store_set_error("TASKS.LOG changed while being scanned");
        return ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK && recovery) {
        recovery->status = latest_seq == 0U ? SI_AGENT_EVENT_RECOVERY_EMPTY
                                            : SI_AGENT_EVENT_RECOVERY_CLEAN;
        recovery->latest_seq = latest_seq;
        copy_text(recovery->latest_hash, sizeof(recovery->latest_hash),
                  latest_hash);
        recovery->valid_bytes = offset;
    }
    return ret;
}

static bool append_input_valid(const si_agent_event_append_t *event)
{
    return event && identifier_valid(event->thread_id, true) &&
           identifier_valid(event->turn_id, true) &&
           identifier_valid(event->run_id, false) &&
           identifier_valid(event->step_id, false) &&
           identifier_valid(event->request_id, false) &&
           identifier_valid(event->action_id, false) &&
           identifier_valid(event->artifact_id, false) &&
           event_type_valid(event->event_type) &&
           event->server_time_ms >= 0 && event->monotonic_time_ms >= 0 &&
           (uint64_t)event->server_time_ms <= SI_AGENT_EVENT_JSON_SAFE_INTEGER &&
           (uint64_t)event->monotonic_time_ms <= SI_AGENT_EVENT_JSON_SAFE_INTEGER &&
           cJSON_IsObject(event->payload);
}

static bool add_envelope_fields(cJSON *root,
                                const si_agent_event_record_t *record,
                                const cJSON *payload)
{
    cJSON *payload_copy = NULL;
    if (!cJSON_AddStringToObject(root, "schema", SI_AGENT_EVENT_SCHEMA) ||
        !cJSON_AddNumberToObject(root, "seq", (double)record->seq) ||
        !cJSON_AddStringToObject(root, "thread_id", record->thread_id) ||
        !cJSON_AddStringToObject(root, "turn_id", record->turn_id) ||
        !cJSON_AddStringToObject(root, "run_id", record->run_id) ||
        !cJSON_AddStringToObject(root, "step_id", record->step_id) ||
        !cJSON_AddStringToObject(root, "request_id", record->request_id) ||
        !cJSON_AddStringToObject(root, "action_id", record->action_id) ||
        !cJSON_AddStringToObject(root, "artifact_id", record->artifact_id) ||
        !cJSON_AddStringToObject(root, "event_type", record->event_type) ||
        !cJSON_AddNumberToObject(root, "server_time_ms",
                                (double)record->server_time_ms) ||
        !cJSON_AddNumberToObject(root, "monotonic_time_ms",
                                (double)record->monotonic_time_ms) ||
        !cJSON_AddStringToObject(root, "payload_hash", record->payload_hash) ||
        !cJSON_AddStringToObject(root, "previous_hash", record->previous_hash) ||
        !cJSON_AddStringToObject(root, "event_hash", record->event_hash)) {
        return false;
    }
    payload_copy = cJSON_Duplicate(payload, true);
    if (!payload_copy || !cJSON_AddItemToObject(root, "payload", payload_copy)) {
        cJSON_Delete(payload_copy);
        return false;
    }
    return true;
}

esp_err_t si_agent_event_store_init(si_agent_event_recovery_t *recovery_out)
{
    if (!s_event_store_lock) {
        s_event_store_lock = xSemaphoreCreateMutex();
        if (!s_event_store_lock) {
            event_store_set_error("create event store mutex failed");
            return ESP_ERR_NO_MEM;
        }
    }
    return si_agent_event_store_recover(NULL, NULL, recovery_out);
}

esp_err_t si_agent_event_store_recover(si_agent_event_replay_cb_t callback,
                                       void *user_ctx,
                                       si_agent_event_recovery_t *recovery_out)
{
    if (!s_event_store_lock) {
        event_store_set_error("event store init is required before recovery");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = event_store_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    s_event_store_ready = false;
    ret = ensure_store_ready_locked();
    si_agent_event_recovery_t local_recovery;
    si_agent_event_recovery_t *result = recovery_out ? recovery_out
                                                      : &local_recovery;
    recovery_reset(result);
    if (ret == ESP_OK) {
        ret = scan_locked(0U, callback, user_ctx, result);
    } else {
        result->status = SI_AGENT_EVENT_RECOVERY_IO_ERROR;
    }
    if (ret == ESP_OK) {
        s_event_store_latest_seq = result->latest_seq;
        s_event_store_file_bytes = result->file_bytes;
        copy_text(s_event_store_latest_hash,
                  sizeof(s_event_store_latest_hash), result->latest_hash);
        s_event_store_ready = true;
        event_store_set_error(NULL);
    }
    xSemaphoreGive(s_event_store_lock);
    return ret;
}

esp_err_t si_agent_event_store_append(const si_agent_event_append_t *event,
                                      si_agent_event_record_t *stored_out)
{
    if (!append_input_valid(event)) {
        event_store_set_error("invalid event envelope input");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = event_store_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (!s_event_store_ready) {
        xSemaphoreGive(s_event_store_lock);
        event_store_set_error("event journal has not recovered cleanly");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_event_store_latest_seq >= SI_AGENT_EVENT_JSON_SAFE_INTEGER) {
        xSemaphoreGive(s_event_store_lock);
        event_store_set_error("event sequence exhausted");
        return ESP_ERR_INVALID_SIZE;
    }

    struct stat file_stat;
    errno = 0;
    if (stat(SI_AGENT_EVENT_STORE_FILE, &file_stat) != 0 ||
        file_stat.st_size < 0 ||
        (uint64_t)file_stat.st_size != s_event_store_file_bytes) {
        s_event_store_ready = false;
        xSemaphoreGive(s_event_store_lock);
        event_store_set_error("TASKS.LOG changed after recovery");
        return ESP_ERR_INVALID_STATE;
    }

    si_agent_event_record_t record;
    memset(&record, 0, sizeof(record));
    record.seq = s_event_store_latest_seq + 1U;
    copy_text(record.thread_id, sizeof(record.thread_id), event->thread_id);
    copy_text(record.turn_id, sizeof(record.turn_id), event->turn_id);
    copy_text(record.run_id, sizeof(record.run_id), event->run_id);
    copy_text(record.step_id, sizeof(record.step_id), event->step_id);
    copy_text(record.request_id, sizeof(record.request_id), event->request_id);
    copy_text(record.action_id, sizeof(record.action_id), event->action_id);
    copy_text(record.artifact_id, sizeof(record.artifact_id), event->artifact_id);
    copy_text(record.event_type, sizeof(record.event_type), event->event_type);
    record.server_time_ms = event->server_time_ms;
    record.monotonic_time_ms = event->monotonic_time_ms;
    copy_text(record.previous_hash, sizeof(record.previous_hash),
              s_event_store_latest_hash);
    ret = payload_hash_compute(event->payload, record.payload_hash, NULL);
    if (ret == ESP_OK) {
        ret = event_hash_compute(&record, record.event_hash);
    }
    if (ret != ESP_OK) {
        xSemaphoreGive(s_event_store_lock);
        return ret;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root || !add_envelope_fields(root, &record, event->payload)) {
        cJSON_Delete(root);
        xSemaphoreGive(s_event_store_lock);
        return ESP_ERR_NO_MEM;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        xSemaphoreGive(s_event_store_lock);
        return ESP_ERR_NO_MEM;
    }
    const size_t json_len = strlen(json);
    if (json_len == 0U || json_len > SI_AGENT_EVENT_RECORD_MAX_BYTES ||
        s_event_store_file_bytes + json_len + 1U >
            SI_AGENT_EVENT_JOURNAL_MAX_BYTES) {
        cJSON_free(json);
        xSemaphoreGive(s_event_store_lock);
        event_store_set_error("event record or journal size limit exceeded");
        return ESP_ERR_INVALID_SIZE;
    }

    errno = 0;
    FILE *file = fopen(SI_AGENT_EVENT_STORE_FILE, "ab");
    if (!file) {
        cJSON_free(json);
        s_event_store_ready = false;
        xSemaphoreGive(s_event_store_lock);
        event_store_set_error("append TASKS.LOG failed errno=%d", errno);
        return ESP_FAIL;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || ftell(file) < 0 ||
        (uint64_t)ftell(file) != s_event_store_file_bytes) {
        (void)fclose(file);
        cJSON_free(json);
        s_event_store_ready = false;
        xSemaphoreGive(s_event_store_lock);
        event_store_set_error("TASKS.LOG size changed before append");
        return ESP_ERR_INVALID_STATE;
    }
    const bool wrote = fwrite(json, 1U, json_len, file) == json_len &&
                       fwrite("\n", 1U, 1U, file) == 1U;
    cJSON_free(json);
    const bool flushed = wrote && fflush(file) == 0;
    const int descriptor = flushed ? fileno(file) : -1;
    /* The cursor is committed only after the VFS acknowledges media sync. */
    const bool synced = descriptor >= 0 && fsync(descriptor) == 0;
    const bool closed = fclose(file) == 0;
    if (!wrote || !flushed || !synced || !closed) {
        s_event_store_ready = false;
        xSemaphoreGive(s_event_store_lock);
        event_store_set_error("durable TASKS.LOG append failed errno=%d", errno);
        return ESP_FAIL;
    }

    s_event_store_latest_seq = record.seq;
    s_event_store_file_bytes += json_len + 1U;
    copy_text(s_event_store_latest_hash, sizeof(s_event_store_latest_hash),
              record.event_hash);
    record.payload = event->payload;
    if (stored_out) {
        *stored_out = record;
    }
    event_store_set_error(NULL);
    xSemaphoreGive(s_event_store_lock);
    return ESP_OK;
}

esp_err_t si_agent_event_store_replay(uint64_t after_seq,
                                      si_agent_event_replay_cb_t callback,
                                      void *user_ctx,
                                      si_agent_event_recovery_t *recovery_out)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = event_store_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (!s_event_store_ready) {
        xSemaphoreGive(s_event_store_lock);
        event_store_set_error("event journal has not recovered cleanly");
        return ESP_ERR_INVALID_STATE;
    }
    si_agent_event_recovery_t local_recovery;
    si_agent_event_recovery_t *result = recovery_out ? recovery_out
                                                      : &local_recovery;
    ret = scan_locked(after_seq, callback, user_ctx, result);
    if (ret == ESP_OK &&
        (result->latest_seq != s_event_store_latest_seq ||
         result->file_bytes != s_event_store_file_bytes ||
         !hash_equal(result->latest_hash, s_event_store_latest_hash))) {
        event_store_set_error("TASKS.LOG cursor changed during replay");
        ret = ESP_ERR_INVALID_STATE;
    }
    if (result->status == SI_AGENT_EVENT_RECOVERY_TAIL_TRUNCATED ||
        result->status == SI_AGENT_EVENT_RECOVERY_CORRUPT ||
        result->status == SI_AGENT_EVENT_RECOVERY_IO_ERROR) {
        s_event_store_ready = false;
    }
    xSemaphoreGive(s_event_store_lock);
    return ret;
}

esp_err_t si_agent_event_store_latest(
    uint64_t *seq_out,
    char hash_out[SI_AGENT_EVENT_HASH_HEX_LEN + 1U])
{
    if (!seq_out || !hash_out) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = event_store_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (!s_event_store_ready) {
        xSemaphoreGive(s_event_store_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *seq_out = s_event_store_latest_seq;
    copy_text(hash_out, SI_AGENT_EVENT_HASH_HEX_LEN + 1U,
              s_event_store_latest_hash);
    xSemaphoreGive(s_event_store_lock);
    return ESP_OK;
}

const char *si_agent_event_store_path(void)
{
    return SI_AGENT_EVENT_STORE_FILE;
}

const char *si_agent_event_store_last_error(void)
{
    return s_event_store_last_error;
}

#ifdef SI_AGENT_EVENT_STORE_HOST_TEST
bool si_agent_event_store_host_hold_lock(void)
{
    return event_store_lock() == ESP_OK;
}

void si_agent_event_store_host_release_lock(void)
{
    if (s_event_store_lock) {
        (void)xSemaphoreGive(s_event_store_lock);
    }
}
#endif

const char *si_agent_event_recovery_status_name(
    si_agent_event_recovery_status_t status)
{
    switch (status) {
    case SI_AGENT_EVENT_RECOVERY_CLEAN:
        return "clean";
    case SI_AGENT_EVENT_RECOVERY_EMPTY:
        return "empty";
    case SI_AGENT_EVENT_RECOVERY_TAIL_TRUNCATED:
        return "tail_truncated";
    case SI_AGENT_EVENT_RECOVERY_CORRUPT:
        return "corrupt";
    case SI_AGENT_EVENT_RECOVERY_IO_ERROR:
        return "io_error";
    default:
        return "unknown";
    }
}
