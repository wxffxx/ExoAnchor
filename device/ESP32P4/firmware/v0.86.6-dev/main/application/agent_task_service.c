#include "agent_task_service.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#ifndef SI_AGENT_TASK_SERVICE_HOST_TEST
#include "device_observation_service.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#define SI_AGENT_TASK_SERVICE_EXT_RAM EXT_RAM_BSS_ATTR
#else
#define SI_AGENT_TASK_SERVICE_EXT_RAM
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "time_utils.h"

#define SI_AGENT_TASK_RECORD_CAPACITY \
    (SI_AGENT_TASK_PENDING_CAPACITY + 1U)
#define SI_AGENT_TASK_DEDUPE_CAPACITY 32U
#define SI_AGENT_TASK_RECOVERY_ACTION_CAPACITY 4U
#define SI_AGENT_TASK_ACTION_LEDGER_CAPACITY 8U

typedef struct {
    bool used;
    si_agent_task_execution_t execution;
    uint64_t execution_started_ms;
    bool mutation_seen;
    bool completion_artifact_verified;
    si_agent_completion_artifact_t completion_artifact;
} task_record_t;

typedef struct {
    bool used;
    bool terminal;
    char key[SI_AGENT_TASK_IDEMPOTENCY_MAX_LEN + 1U];
    char thread_id[SI_AGENT_TASK_ID_MAX + 1U];
    char turn_id[SI_AGENT_TASK_ID_MAX + 1U];
    char run_id[SI_AGENT_TASK_ID_MAX + 1U];
} task_dedupe_t;

typedef struct {
    bool used;
    char thread_id[SI_AGENT_TASK_ID_MAX + 1U];
    char turn_id[SI_AGENT_TASK_ID_MAX + 1U];
    char run_id[SI_AGENT_TASK_ID_MAX + 1U];
    char unresolved_actions[SI_AGENT_TASK_RECOVERY_ACTION_CAPACITY]
                           [SI_AGENT_TASK_ID_MAX + 1U];
    size_t unresolved_action_count;
    bool unresolved_action_overflow;
} recovery_open_turn_t;

typedef struct {
    bool used;
    bool result_recorded;
    bool mutation;
    uint32_t plan_version;
    char run_id[SI_AGENT_TASK_ID_MAX + 1U];
    char step_id[SI_AGENT_TASK_ID_MAX + 1U];
    char action_id[SI_AGENT_TASK_ID_MAX + 1U];
} task_action_ledger_t;

static SemaphoreHandle_t s_task_lock;
static SI_AGENT_TASK_SERVICE_EXT_RAM si_agent_task_runtime_t s_runtime;
static SI_AGENT_TASK_SERVICE_EXT_RAM task_record_t
    s_records[SI_AGENT_TASK_RECORD_CAPACITY];
static SI_AGENT_TASK_SERVICE_EXT_RAM task_dedupe_t
    s_dedupe[SI_AGENT_TASK_DEDUPE_CAPACITY];
static SI_AGENT_TASK_SERVICE_EXT_RAM task_action_ledger_t
    s_action_ledger[SI_AGENT_TASK_ACTION_LEDGER_CAPACITY];
/*
 * Every public mutation is serialized by s_task_lock. Keep the large
 * transaction candidates in external BSS instead of copying them onto the
 * caller's stack (the normal caller is the 12 KiB shared HTTP task).
 */
static SI_AGENT_TASK_SERVICE_EXT_RAM si_agent_task_runtime_t
    s_runtime_candidate;
static SI_AGENT_TASK_SERVICE_EXT_RAM si_agent_task_execution_t
    s_execution_candidate;
static SI_AGENT_TASK_SERVICE_EXT_RAM char s_idempotency_scope[768];
static size_t s_dedupe_next;
static uint64_t s_latest_event_seq;
static uint64_t s_logical_time;
static uint32_t s_id_counter;
typedef enum {
    TASK_SERVICE_LIFECYCLE_COLD = 0,
    TASK_SERVICE_LIFECYCLE_RESERVED,
    TASK_SERVICE_LIFECYCLE_RUNNING,
    TASK_SERVICE_LIFECYCLE_READY,
    TASK_SERVICE_LIFECYCLE_FAILED,
} task_service_lifecycle_t;
/* Release-published lifecycle is the sole startup truth. s_start_result is
 * written before FAILED and read only after an acquire load observes FAILED. */
static uint32_t s_lifecycle = TASK_SERVICE_LIFECYCLE_COLD;
static esp_err_t s_start_result = ESP_ERR_INVALID_STATE;
static bool s_storage_healthy;
static si_agent_event_recovery_status_t s_recovery_status =
    SI_AGENT_EVENT_RECOVERY_IO_ERROR;
static char s_last_error[160];
#ifdef SI_AGENT_TASK_SERVICE_HOST_TEST
static uint32_t s_host_random = 0x6d2b79f5U;
#endif

static bool bounded(const char *value, size_t limit, bool required)
{
    if (!value) {
        return !required;
    }
    for (size_t index = 0; index <= limit; ++index) {
        if (value[index] == '\0') {
            return !required || index > 0U;
        }
    }
    return false;
}

static bool sha256_hex_valid(const char *value)
{
    if (!value || strnlen(value, 65U) != 64U) {
        return false;
    }
    for (size_t index = 0; index < 64U; ++index) {
        char ch = value[index];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool origin_valid(si_agent_task_origin_t origin)
{
    return origin >= SI_AGENT_TASK_ORIGIN_WEB &&
           origin <= SI_AGENT_TASK_ORIGIN_SYSTEM;
}

static bool source_auth_valid(const si_agent_task_source_t *source)
{
    if (!source ||
        !bounded(source->auth_id, SI_AGENT_TASK_SOURCE_ID_MAX_LEN, true) ||
        source->auth_generation == 0U ||
        source->principal <= SI_PRINCIPAL_ANONYMOUS ||
        source->principal > SI_PRINCIPAL_SYSTEM) {
        return false;
    }
    switch (source->auth_kind) {
    case SI_AGENT_TASK_AUTH_DEVICE_SESSION:
        return ((source->origin == SI_AGENT_TASK_ORIGIN_WEB &&
                 source->principal == SI_PRINCIPAL_BROWSER) ||
                (source->origin == SI_AGENT_TASK_ORIGIN_MCP &&
                 source->principal == SI_PRINCIPAL_MCP));
    case SI_AGENT_TASK_AUTH_ADAPTER_DELEGATION:
        return source->origin == SI_AGENT_TASK_ORIGIN_QQ;
    case SI_AGENT_TASK_AUTH_SYSTEM:
        return source->origin == SI_AGENT_TASK_ORIGIN_SYSTEM &&
               source->principal == SI_PRINCIPAL_SYSTEM;
    default:
        return false;
    }
}

static const char *source_principal_name(si_principal_kind_t principal)
{
    switch (principal) {
    case SI_PRINCIPAL_BROWSER: return "browser";
    case SI_PRINCIPAL_MCP: return "mcp";
    case SI_PRINCIPAL_AGENT: return "agent";
    case SI_PRINCIPAL_SYSTEM: return "system";
    case SI_PRINCIPAL_ANONYMOUS:
    default: return "anonymous";
    }
}

static bool history_policy_valid(si_agent_task_history_policy_t policy)
{
    return policy >= SI_AGENT_TASK_HISTORY_NONE &&
           policy <= SI_AGENT_TASK_HISTORY_THREAD;
}

static bool completion_criteria_kind_valid(
    si_agent_task_completion_criteria_kind_t kind)
{
    return kind == SI_AGENT_TASK_CRITERIA_OBSERVATION ||
           kind == SI_AGENT_TASK_CRITERIA_DELIVERABLE;
}

static const char *completion_criteria_kind_name(
    si_agent_task_completion_criteria_kind_t kind)
{
    return kind == SI_AGENT_TASK_CRITERIA_DELIVERABLE ?
        "deliverable" : "observation";
}

static void set_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(s_last_error, sizeof(s_last_error), format, args);
    va_end(args);
}

static uint64_t next_logical_time(void)
{
    if (s_logical_time == UINT64_MAX) {
        return s_logical_time;
    }
    return ++s_logical_time;
}

static int64_t server_time_ms(void)
{
    time_t now = time(NULL);
    /* Before SNTP, do not present an untrusted epoch as trusted server time. */
    return now >= 1700000000 ? (int64_t)now * 1000LL : 0LL;
}

static void digest_hex(const char *value, char out[65])
{
#ifndef SI_AGENT_TASK_SERVICE_HOST_TEST
    unsigned char digest[32] = {0};
    const char *text = value ? value : "";
    if (mbedtls_sha256((const unsigned char *)text, strlen(text),
                       digest, 0) != 0) {
        snprintf(out, 65, "unavailable");
        return;
    }
    for (size_t index = 0; index < sizeof(digest); ++index) {
        snprintf(out + index * 2U, 3U, "%02x", digest[index]);
    }
#else
    /* Host tests exercise event semantics, not the platform SHA provider.
     * Keep a deterministic 64-hex projection so the production call sites
     * and payload shape are compiled without weakening the firmware build. */
    const unsigned char *cursor =
        (const unsigned char *)(value ? value : "");
    uint64_t state[4] = {
        UINT64_C(0xcbf29ce484222325), UINT64_C(0x9e3779b97f4a7c15),
        UINT64_C(0x243f6a8885a308d3), UINT64_C(0x13198a2e03707344),
    };
    for (size_t index = 0; cursor[index] != '\0'; ++index) {
        for (size_t lane = 0; lane < 4U; ++lane) {
            state[lane] ^= (uint64_t)cursor[index] + lane;
            state[lane] *= UINT64_C(0x100000001b3);
            state[lane] ^= state[lane] >> (13U + lane);
        }
    }
    snprintf(out, 65, "%016" PRIx64 "%016" PRIx64
             "%016" PRIx64 "%016" PRIx64,
             state[0], state[1], state[2], state[3]);
#endif
}

static bool current_target_identity(
    char out[SI_AGENT_VERIFY_TARGET_MAX_LEN + 1U])
{
    if (!out) {
        return false;
    }
    out[0] = '\0';
#ifdef SI_AGENT_TASK_SERVICE_HOST_TEST
    extern bool si_agent_task_service_host_target_identity(
        char *target_out, size_t target_out_size);
    return si_agent_task_service_host_target_identity(
        out, SI_AGENT_VERIFY_TARGET_MAX_LEN + 1U);
#else
    si_device_network_observation_t network = {0};
    si_device_observation_get_network(&network);
    size_t length = strnlen(network.device_id, sizeof(network.device_id));
    if (length == 0U || length >= sizeof(network.device_id) ||
        length > SI_AGENT_VERIFY_TARGET_MAX_LEN) {
        return false;
    }
    memcpy(out, network.device_id, length);
    out[length] = '\0';
    return true;
#endif
}

static void copy_text(char *destination, size_t capacity, const char *value)
{
    if (!destination || capacity == 0U) {
        return;
    }
    snprintf(destination, capacity, "%s", value ? value : "");
}

static void new_id(const char *prefix, char out[SI_AGENT_TASK_ID_MAX + 1U])
{
#ifndef SI_AGENT_TASK_SERVICE_HOST_TEST
    uint32_t random = esp_random();
#else
    s_host_random ^= s_host_random << 13U;
    s_host_random ^= s_host_random >> 17U;
    s_host_random ^= s_host_random << 5U;
    uint32_t random = s_host_random;
#endif
    uint32_t counter = ++s_id_counter;
    snprintf(out, SI_AGENT_TASK_ID_MAX + 1U,
             "%s-%08" PRIx32 "-%08" PRIx32,
             prefix, random, counter);
}

static task_record_t *record_by_run(const char *run_id)
{
    if (!run_id || !run_id[0]) {
        return NULL;
    }
    for (size_t index = 0; index < SI_AGENT_TASK_RECORD_CAPACITY; ++index) {
        if (s_records[index].used &&
            strcmp(s_records[index].execution.run_id, run_id) == 0) {
            return &s_records[index];
        }
    }
    return NULL;
}

static task_record_t *record_free(void)
{
    for (size_t index = 0; index < SI_AGENT_TASK_RECORD_CAPACITY; ++index) {
        if (!s_records[index].used) {
            return &s_records[index];
        }
    }
    return NULL;
}

static task_action_ledger_t *action_ledger_find(const char *run_id,
                                                const char *action_id)
{
    for (size_t index = 0; index < SI_AGENT_TASK_ACTION_LEDGER_CAPACITY;
         ++index) {
        task_action_ledger_t *item = &s_action_ledger[index];
        if (item->used && run_id && action_id &&
            strcmp(item->run_id, run_id) == 0 &&
            strcmp(item->action_id, action_id) == 0) {
            return item;
        }
    }
    return NULL;
}

static task_action_ledger_t *action_ledger_free(void)
{
    for (size_t index = 0; index < SI_AGENT_TASK_ACTION_LEDGER_CAPACITY;
         ++index) {
        if (!s_action_ledger[index].used) {
            return &s_action_ledger[index];
        }
    }
    return NULL;
}

static void action_ledger_clear_run(const char *run_id)
{
    if (!run_id || !run_id[0]) {
        return;
    }
    for (size_t index = 0; index < SI_AGENT_TASK_ACTION_LEDGER_CAPACITY;
         ++index) {
        if (s_action_ledger[index].used &&
            strcmp(s_action_ledger[index].run_id, run_id) == 0) {
            memset(&s_action_ledger[index], 0,
                   sizeof(s_action_ledger[index]));
        }
    }
}

static task_dedupe_t *dedupe_find(const char *key)
{
    if (!key || !key[0]) {
        return NULL;
    }
    for (size_t index = 0; index < SI_AGENT_TASK_DEDUPE_CAPACITY; ++index) {
        if (s_dedupe[index].used && strcmp(s_dedupe[index].key, key) == 0) {
            return &s_dedupe[index];
        }
    }
    return NULL;
}

static task_dedupe_t *dedupe_remember(const char *key,
                                      const char *thread_id,
                                      const char *turn_id,
                                      const char *run_id)
{
    task_dedupe_t *item = dedupe_find(key);
    if (!item) {
        item = &s_dedupe[s_dedupe_next++ % SI_AGENT_TASK_DEDUPE_CAPACITY];
        memset(item, 0, sizeof(*item));
    }
    item->used = true;
    copy_text(item->key, sizeof(item->key), key);
    copy_text(item->thread_id, sizeof(item->thread_id), thread_id);
    copy_text(item->turn_id, sizeof(item->turn_id), turn_id);
    copy_text(item->run_id, sizeof(item->run_id), run_id);
    return item;
}

static const si_agent_task_turn_t *active_for_run(const char *run_id)
{
    const si_agent_task_turn_t *turn = si_agent_task_runtime_active(&s_runtime);
    return turn && run_id && strcmp(turn->run_attempt.run_attempt_id, run_id) == 0
               ? turn
               : NULL;
}

static bool active_step_bound(const si_agent_task_turn_t *turn,
                              const char *step_id)
{
    if (!turn || !turn->has_plan || !step_id || !step_id[0]) {
        return false;
    }
    for (size_t index = 0; index < turn->plan.step_count; ++index) {
        if (strcmp(turn->plan.steps[index].step_id, step_id) == 0 &&
            turn->plan.steps[index].status == SI_AGENT_STEP_IN_PROGRESS) {
            return true;
        }
    }
    return false;
}

static esp_err_t task_result_to_esp(si_agent_task_result_t result)
{
    switch (result) {
    case SI_AGENT_TASK_OK:
        return ESP_OK;
    case SI_AGENT_TASK_ERR_INVALID_ARGUMENT:
    case SI_AGENT_TASK_ERR_INVALID_ID:
        return ESP_ERR_INVALID_ARG;
    case SI_AGENT_TASK_ERR_QUEUE_FULL:
    case SI_AGENT_TASK_ERR_CAPACITY:
        return ESP_ERR_NO_MEM;
    case SI_AGENT_TASK_ERR_NOT_FOUND:
    case SI_AGENT_TASK_ERR_NO_ACTIVE_TURN:
        return ESP_ERR_NOT_FOUND;
    case SI_AGENT_TASK_ERR_CONFLICT:
    case SI_AGENT_TASK_ERR_INVALID_TRANSITION:
    case SI_AGENT_TASK_ERR_VERIFICATION_REQUIRED:
    case SI_AGENT_TASK_ERR_TIME_REGRESSION:
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

static esp_err_t append_event_stored(const char *event_type,
                                     const char *thread_id,
                                     const char *turn_id,
                                     const char *run_id,
                                     const char *step_id,
                                     const char *action_id,
                                     const char *artifact_id,
                                     const cJSON *payload,
                                     si_agent_event_record_t *stored_out)
{
    si_agent_event_append_t event = {
        .thread_id = thread_id,
        .turn_id = turn_id,
        .run_id = run_id,
        .step_id = step_id,
        .action_id = action_id,
        .artifact_id = artifact_id,
        .event_type = event_type,
        .server_time_ms = server_time_ms(),
        .monotonic_time_ms = (int64_t)si_monotonic_ms(),
        .payload = payload,
    };
    si_agent_event_record_t stored = {0};
    esp_err_t result = si_agent_event_store_append(&event, &stored);
    if (result != ESP_OK) {
        s_storage_healthy = false;
        set_error("append %s failed: %s", event_type,
                  si_agent_event_store_last_error());
        return result;
    }
    s_storage_healthy = true;
    s_latest_event_seq = stored.seq;
    if (stored_out) {
        *stored_out = stored;
    }
    return ESP_OK;
}

static esp_err_t append_event(const char *event_type,
                              const char *thread_id,
                              const char *turn_id,
                              const char *run_id,
                              const char *step_id,
                              const char *action_id,
                              const char *artifact_id,
                              const cJSON *payload)
{
    return append_event_stored(event_type, thread_id, turn_id, run_id,
                               step_id, action_id, artifact_id, payload,
                               NULL);
}

static cJSON *payload_status(const char *status)
{
    cJSON *payload = cJSON_CreateObject();
    if (payload) {
        cJSON_AddStringToObject(payload, "status", status ? status : "");
    }
    return payload;
}

static bool event_is_terminal(const char *event_type)
{
    return event_type &&
           (strcmp(event_type, "turn.completed") == 0 ||
            strcmp(event_type, "turn.failed") == 0 ||
            strcmp(event_type, "turn.interrupted") == 0 ||
            strcmp(event_type, "turn.cancelled") == 0 ||
            strcmp(event_type, "turn.outcome_unknown") == 0);
}

static bool candidate_is_outcome_unknown(
    const si_agent_task_runtime_t *candidate)
{
    return candidate && candidate->has_active_turn &&
           candidate->active_turn.status == SI_AGENT_TURN_OUTCOME_UNKNOWN;
}

static bool payload_add_outcome_unknown(cJSON *payload,
                                        const char *reason,
                                        const char *terminal_source)
{
    return payload && reason && reason[0] && terminal_source &&
           terminal_source[0] &&
           cJSON_AddStringToObject(payload, "outcome", "unknown") != NULL &&
           cJSON_AddStringToObject(payload, "reason", reason) != NULL &&
           cJSON_AddStringToObject(payload, "terminal_source",
                                   terminal_source) != NULL &&
           cJSON_AddBoolToObject(payload, "blind_retry_forbidden", true) !=
               NULL;
}

static void commit_candidate(const si_agent_task_runtime_t *candidate,
                             const task_record_t *record)
{
    if (!candidate) {
        return;
    }
    s_runtime = *candidate;
    if (record &&
        si_agent_turn_status_is_terminal(candidate->active_turn.status)) {
        action_ledger_clear_run(record->execution.run_id);
        task_dedupe_t *dedupe = dedupe_find(
            record->execution.idempotency_key);
        if (dedupe) {
            dedupe->terminal = true;
        }
    }
}

static recovery_open_turn_t *recovery_open_find(
    recovery_open_turn_t open[SI_AGENT_TASK_RECORD_CAPACITY],
    const char *run_id)
{
    for (size_t index = 0; index < SI_AGENT_TASK_RECORD_CAPACITY; ++index) {
        if (open[index].used && run_id &&
            strcmp(open[index].run_id, run_id) == 0) {
            return &open[index];
        }
    }
    return NULL;
}

typedef struct {
    recovery_open_turn_t open[SI_AGENT_TASK_RECORD_CAPACITY];
} recovery_context_t;

/* Boot recovery runs under s_task_lock and can be several KiB as well. */
static SI_AGENT_TASK_SERVICE_EXT_RAM recovery_context_t s_recovery_context;

static void recovery_action_add(recovery_open_turn_t *turn,
                                const char *action_id)
{
    if (!turn || !action_id || !action_id[0]) {
        return;
    }
    for (size_t index = 0; index < turn->unresolved_action_count; ++index) {
        if (strcmp(turn->unresolved_actions[index], action_id) == 0) {
            return;
        }
    }
    if (turn->unresolved_action_count <
        SI_AGENT_TASK_RECOVERY_ACTION_CAPACITY) {
        copy_text(turn->unresolved_actions[turn->unresolved_action_count++],
                  SI_AGENT_TASK_ID_MAX + 1U, action_id);
    } else {
        /* Losing track of an action must never turn a reboot into a blind
         * retry. Preserve a fail-closed overflow tombstone. */
        turn->unresolved_action_overflow = true;
    }
}

static void recovery_action_remove(recovery_open_turn_t *turn,
                                   const char *action_id)
{
    if (!turn || !action_id || !action_id[0]) {
        return;
    }
    for (size_t index = 0; index < turn->unresolved_action_count; ++index) {
        if (strcmp(turn->unresolved_actions[index], action_id) == 0) {
            for (size_t next = index + 1U;
                 next < turn->unresolved_action_count; ++next) {
                copy_text(turn->unresolved_actions[next - 1U],
                          SI_AGENT_TASK_ID_MAX + 1U,
                          turn->unresolved_actions[next]);
            }
            turn->unresolved_action_count--;
            turn->unresolved_actions[turn->unresolved_action_count][0] = '\0';
            return;
        }
    }
}

static esp_err_t recovery_event(const si_agent_event_record_t *record,
                                void *user_ctx)
{
    recovery_context_t *context = (recovery_context_t *)user_ctx;
    if (!record || !context) {
        return ESP_ERR_INVALID_ARG;
    }
    if (record->seq > s_latest_event_seq) {
        s_latest_event_seq = record->seq;
    }
    if (strcmp(record->event_type, "turn.submitted") == 0) {
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(
            record->payload, "idempotency_key_hash");
        if (cJSON_IsString(key) && key->valuestring[0]) {
            (void)dedupe_remember(key->valuestring, record->thread_id,
                                  record->turn_id, record->run_id);
        }
        recovery_open_turn_t *slot = NULL;
        for (size_t index = 0; index < SI_AGENT_TASK_RECORD_CAPACITY; ++index) {
            if (!context->open[index].used) {
                slot = &context->open[index];
                break;
            }
        }
        if (slot) {
            memset(slot, 0, sizeof(*slot));
            slot->used = true;
            copy_text(slot->thread_id, sizeof(slot->thread_id),
                      record->thread_id);
            copy_text(slot->turn_id, sizeof(slot->turn_id), record->turn_id);
            copy_text(slot->run_id, sizeof(slot->run_id), record->run_id);
        }
    } else if (event_is_terminal(record->event_type)) {
        recovery_open_turn_t *open = recovery_open_find(context->open,
                                                        record->run_id);
        if (open) {
            memset(open, 0, sizeof(*open));
        }
        for (size_t index = 0; index < SI_AGENT_TASK_DEDUPE_CAPACITY; ++index) {
            if (s_dedupe[index].used &&
                strcmp(s_dedupe[index].run_id, record->run_id) == 0) {
                s_dedupe[index].terminal = true;
            }
        }
    } else if (strcmp(record->event_type, "action.started") == 0) {
        recovery_action_add(recovery_open_find(context->open, record->run_id),
                            record->action_id);
    } else if (strcmp(record->event_type, "action.verified") == 0 ||
               strcmp(record->event_type,
                      "action.cancelled_before_effect") == 0) {
        /* A tool return is not verification. Keep action.started unresolved
         * until authoritative evidence or an explicit pre-effect cancel is
         * durable. */
        recovery_action_remove(
            recovery_open_find(context->open, record->run_id),
            record->action_id);
    }
    return ESP_OK;
}

static esp_err_t recover_open_turns(recovery_context_t *context)
{
    for (size_t index = 0; index < SI_AGENT_TASK_RECORD_CAPACITY; ++index) {
        recovery_open_turn_t *open = &context->open[index];
        if (!open->used) {
            continue;
        }
        bool unknown = open->unresolved_action_count > 0U ||
                       open->unresolved_action_overflow;
        const char *unresolved_action_id =
            open->unresolved_action_count > 0U ?
            open->unresolved_actions[0] : NULL;
        cJSON *payload = cJSON_CreateObject();
        if (!payload) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(payload, "reason", "device_reboot");
        cJSON_AddBoolToObject(payload, "blind_retry_forbidden", unknown);
        if (unknown) {
            cJSON_AddStringToObject(payload, "outcome", "unknown");
            if (unresolved_action_id) {
                cJSON_AddStringToObject(payload, "unresolved_action_id",
                                        unresolved_action_id);
            }
            cJSON_AddBoolToObject(payload,
                                  "unresolved_action_tracking_overflow",
                                  open->unresolved_action_overflow);
        } else {
            cJSON_AddStringToObject(payload, "outcome", "interrupted");
        }
        esp_err_t result = append_event(
            unknown ? "turn.outcome_unknown" : "turn.interrupted",
            open->thread_id, open->turn_id, open->run_id, NULL,
            unresolved_action_id, NULL, payload);
        cJSON_Delete(payload);
        if (result != ESP_OK) {
            return result;
        }
        for (size_t dedupe = 0; dedupe < SI_AGENT_TASK_DEDUPE_CAPACITY;
             ++dedupe) {
            if (s_dedupe[dedupe].used &&
                strcmp(s_dedupe[dedupe].run_id, open->run_id) == 0) {
                s_dedupe[dedupe].terminal = true;
            }
        }
    }
    return ESP_OK;
}

esp_err_t si_agent_task_service_prepare(void)
{
    if (__atomic_load_n(&s_task_lock, __ATOMIC_ACQUIRE)) {
        return ESP_OK;
    }
    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (!candidate) {
        set_error("create task service mutex failed");
        return ESP_ERR_NO_MEM;
    }
    SemaphoreHandle_t expected = NULL;
    if (!__atomic_compare_exchange_n(
            &s_task_lock, &expected, candidate, false,
            __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        vSemaphoreDelete(candidate);
    }
    return ESP_OK;
}

static esp_err_t task_service_start_internal(void)
{
    esp_err_t prepare_result = si_agent_task_service_prepare();
    if (prepare_result != ESP_OK) {
        return prepare_result;
    }
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_start_result = ESP_FAIL;

    memset(&s_runtime, 0, sizeof(s_runtime));
    memset(s_records, 0, sizeof(s_records));
    memset(s_dedupe, 0, sizeof(s_dedupe));
    memset(s_action_ledger, 0, sizeof(s_action_ledger));
    s_latest_event_seq = 0U;
    s_logical_time = 0U;
    s_dedupe_next = 0U;
    si_agent_task_result_t init_result =
        si_agent_task_runtime_init(&s_runtime, "device-agent-runtime-v2",
                                   next_logical_time());
    if (init_result != SI_AGENT_TASK_OK) {
        set_error("runtime init failed: %s",
                  si_agent_task_result_name(init_result));
        s_start_result = task_result_to_esp(init_result);
        xSemaphoreGive(s_task_lock);
        return s_start_result;
    }

    si_agent_event_recovery_t recovery = {0};
    esp_err_t result = si_agent_event_store_init(&recovery);
    s_recovery_status = recovery.status;
    s_storage_healthy = result == ESP_OK;
    if (result != ESP_OK) {
        set_error("event store init failed: %s",
                  si_agent_event_store_last_error());
        s_start_result = result;
        xSemaphoreGive(s_task_lock);
        return s_start_result;
    }
    memset(&s_recovery_context, 0, sizeof(s_recovery_context));
    result = si_agent_event_store_recover(recovery_event,
                                          &s_recovery_context, &recovery);
    s_recovery_status = recovery.status;
    if (result == ESP_OK) {
        result = recover_open_turns(&s_recovery_context);
    }
    memset(&s_recovery_context, 0, sizeof(s_recovery_context));
    if (result != ESP_OK) {
        s_storage_healthy = false;
        set_error("task recovery failed: %s",
                  si_agent_event_store_last_error());
        s_start_result = result;
        xSemaphoreGive(s_task_lock);
        return s_start_result;
    }
    s_start_result = ESP_OK;
    s_storage_healthy = true;
    s_last_error[0] = '\0';
    xSemaphoreGive(s_task_lock);
    return ESP_OK;
}

esp_err_t si_agent_task_service_reserve_recovery(void)
{
    esp_err_t result = si_agent_task_service_prepare();
    if (result != ESP_OK) {
        return result;
    }
    for (;;) {
        uint32_t lifecycle =
            __atomic_load_n(&s_lifecycle, __ATOMIC_ACQUIRE);
        if (lifecycle != TASK_SERVICE_LIFECYCLE_COLD) {
            return lifecycle == TASK_SERVICE_LIFECYCLE_FAILED ?
                s_start_result : ESP_OK;
        }
        uint32_t expected = TASK_SERVICE_LIFECYCLE_COLD;
        if (__atomic_compare_exchange_n(
                &s_lifecycle, &expected,
                TASK_SERVICE_LIFECYCLE_RESERVED, false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return ESP_OK;
        }
    }
}

esp_err_t si_agent_task_service_recover_reserved(void)
{
    uint32_t expected = TASK_SERVICE_LIFECYCLE_RESERVED;
    if (!__atomic_compare_exchange_n(
            &s_lifecycle, &expected, TASK_SERVICE_LIFECYCLE_RUNNING,
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return expected == TASK_SERVICE_LIFECYCLE_READY ? ESP_OK :
               expected == TASK_SERVICE_LIFECYCLE_FAILED ? s_start_result :
               expected == TASK_SERVICE_LIFECYCLE_RUNNING ? ESP_ERR_TIMEOUT :
               ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = task_service_start_internal();
    s_start_result = result;
    __atomic_store_n(
        &s_lifecycle,
        result == ESP_OK ? TASK_SERVICE_LIFECYCLE_READY :
                           TASK_SERVICE_LIFECYCLE_FAILED,
        __ATOMIC_RELEASE);
    return result;
}

esp_err_t si_agent_task_service_start(void)
{
    for (;;) {
        uint32_t lifecycle =
            __atomic_load_n(&s_lifecycle, __ATOMIC_ACQUIRE);
        if (lifecycle == TASK_SERVICE_LIFECYCLE_READY) {
            return ESP_OK;
        }
        if (lifecycle == TASK_SERVICE_LIFECYCLE_FAILED) {
            return s_start_result;
        }
        if (lifecycle == TASK_SERVICE_LIFECYCLE_RESERVED) {
            return ESP_ERR_INVALID_STATE;
        }
        if (lifecycle == TASK_SERVICE_LIFECYCLE_RUNNING) {
            return ESP_ERR_TIMEOUT;
        }
        uint32_t expected = TASK_SERVICE_LIFECYCLE_COLD;
        if (!__atomic_compare_exchange_n(
                &s_lifecycle, &expected,
                TASK_SERVICE_LIFECYCLE_RUNNING, false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            continue;
        }
        esp_err_t result = task_service_start_internal();
        s_start_result = result;
        __atomic_store_n(
            &s_lifecycle,
            result == ESP_OK ? TASK_SERVICE_LIFECYCLE_READY :
                               TASK_SERVICE_LIFECYCLE_FAILED,
            __ATOMIC_RELEASE);
        return result;
    }
}

static bool submit_valid(const si_agent_task_submit_t *input)
{
    return input &&
           bounded(input->thread_id, SI_AGENT_TASK_ID_MAX, false) &&
           bounded(input->turn_id, SI_AGENT_TASK_ID_MAX, false) &&
           bounded(input->run_id, SI_AGENT_TASK_ID_MAX, false) &&
           ((input->turn_id && input->turn_id[0]) ==
            (input->run_id && input->run_id[0])) &&
           bounded(input->goal, SI_AGENT_TASK_GOAL_MAX_LEN, true) &&
           bounded(input->completion_criteria,
                   SI_AGENT_TASK_CRITERIA_MAX_LEN, true) &&
           bounded(input->idempotency_key,
                   SI_AGENT_TASK_IDEMPOTENCY_MAX_LEN, true) &&
           bounded(input->profile, SI_AGENT_TASK_PROFILE_MAX_LEN, false) &&
           bounded(input->model, SI_AGENT_TASK_MODEL_MAX_LEN, false) &&
           bounded(input->page_context_json,
                   SI_AGENT_TASK_PAGE_CONTEXT_MAX_LEN, false) &&
           bounded(input->source.source_account_id,
                   SI_AGENT_TASK_SOURCE_ID_MAX_LEN, false) &&
           bounded(input->source.source_conversation_id,
                   SI_AGENT_TASK_SOURCE_ID_MAX_LEN, false) &&
           bounded(input->source.source_actor_id,
                   SI_AGENT_TASK_SOURCE_ID_MAX_LEN, false) &&
           bounded(input->source.external_event_id,
                   SI_AGENT_TASK_SOURCE_ID_MAX_LEN, false) &&
           origin_valid(input->source.origin) &&
           source_auth_valid(&input->source) &&
           history_policy_valid(input->source.history_policy) &&
           completion_criteria_kind_valid(input->completion_criteria_kind) &&
           si_capability_set_has(input->source.authority_ceiling,
                                 SI_CAPABILITY_AGENT_RUN) &&
           (input->source.origin != SI_AGENT_TASK_ORIGIN_QQ ||
            (input->source.authority_ceiling &
             ~SI_AGENT_TASK_QQ_AUTHORITY_CEILING) == 0U);
}

static bool scoped_idempotency_hash(const si_agent_task_submit_t *input,
                                    char out[65])
{
    memset(s_idempotency_scope, 0, sizeof(s_idempotency_scope));
    int written = snprintf(
        s_idempotency_scope, sizeof(s_idempotency_scope),
        "%u|%u|%u|%u|%s|%" PRIu32
        "|%s|%s|%s|%s|%s|%s",
        (unsigned)input->source.origin,
        (unsigned)input->source.auth_kind,
        (unsigned)input->source.principal,
        (unsigned)input->completion_criteria_kind,
        input->source.auth_id,
        input->source.auth_generation,
        input->source.source_account_id ? input->source.source_account_id : "",
        input->source.source_conversation_id ?
            input->source.source_conversation_id : "",
        input->source.source_actor_id ? input->source.source_actor_id : "",
        input->source.external_event_id ? input->source.external_event_id : "",
        input->thread_id ? input->thread_id : "",
        input->idempotency_key);
    if (written <= 0 ||
        (size_t)written >= sizeof(s_idempotency_scope)) {
        memset(s_idempotency_scope, 0, sizeof(s_idempotency_scope));
        return false;
    }
    digest_hex(s_idempotency_scope, out);
    memset(s_idempotency_scope, 0, sizeof(s_idempotency_scope));
    return sha256_hex_valid(out);
}

esp_err_t si_agent_task_service_submit(const si_agent_task_submit_t *input,
                                       si_agent_task_receipt_t *receipt)
{
    if (!submit_valid(input) || !receipt) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) {
        return start_result;
    }
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memset(receipt, 0, sizeof(*receipt));
    char idempotency_hash[65] = {0};
    if (!scoped_idempotency_hash(input, idempotency_hash)) {
        xSemaphoreGive(s_task_lock);
        return ESP_FAIL;
    }
    task_dedupe_t *duplicate = dedupe_find(idempotency_hash);
    if (duplicate) {
        receipt->accepted = true;
        receipt->deduplicated = true;
        copy_text(receipt->thread_id, sizeof(receipt->thread_id),
                  duplicate->thread_id);
        copy_text(receipt->turn_id, sizeof(receipt->turn_id),
                  duplicate->turn_id);
        copy_text(receipt->run_id, sizeof(receipt->run_id),
                  duplicate->run_id);
        xSemaphoreGive(s_task_lock);
        return ESP_OK;
    }

    task_record_t *slot = record_free();
    if (!slot) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NO_MEM;
    }
    memset(&s_execution_candidate, 0, sizeof(s_execution_candidate));
    /* A full-size compound literal would silently recreate this several-KiB
     * execution object on the shared HTTP caller's stack. */
    s_execution_candidate.used = true;
    s_execution_candidate.origin = input->source.origin;
    s_execution_candidate.auth_kind = input->source.auth_kind;
    s_execution_candidate.source_principal = input->source.principal;
    s_execution_candidate.source_auth_generation =
        input->source.auth_generation;
    s_execution_candidate.authority_ceiling =
        input->source.authority_ceiling;
    s_execution_candidate.history_policy = input->source.history_policy;
    s_execution_candidate.dry_run = input->dry_run;
    s_execution_candidate.include_screenshot = input->include_screenshot;
    s_execution_candidate.allow_web_search = input->allow_web_search;
    s_execution_candidate.completion_criteria_kind =
        input->completion_criteria_kind;
    if (input->thread_id && input->thread_id[0]) {
        copy_text(s_execution_candidate.thread_id,
                  sizeof(s_execution_candidate.thread_id),
                  input->thread_id);
    } else {
        new_id("thread", s_execution_candidate.thread_id);
    }
    if (input->turn_id && input->turn_id[0]) {
        copy_text(s_execution_candidate.turn_id,
                  sizeof(s_execution_candidate.turn_id), input->turn_id);
        copy_text(s_execution_candidate.run_id,
                  sizeof(s_execution_candidate.run_id), input->run_id);
    } else {
        new_id("turn", s_execution_candidate.turn_id);
        new_id("run", s_execution_candidate.run_id);
    }
    copy_text(s_execution_candidate.goal,
              sizeof(s_execution_candidate.goal), input->goal);
    copy_text(s_execution_candidate.completion_criteria,
              sizeof(s_execution_candidate.completion_criteria),
              input->completion_criteria);
    copy_text(s_execution_candidate.idempotency_key,
              sizeof(s_execution_candidate.idempotency_key),
              idempotency_hash);
    copy_text(s_execution_candidate.profile,
              sizeof(s_execution_candidate.profile), input->profile);
    copy_text(s_execution_candidate.model,
              sizeof(s_execution_candidate.model), input->model);
    copy_text(s_execution_candidate.page_context_json,
              sizeof(s_execution_candidate.page_context_json),
              input->page_context_json);
    copy_text(s_execution_candidate.source_auth_id,
              sizeof(s_execution_candidate.source_auth_id),
              input->source.auth_id);

    s_runtime_candidate = s_runtime;
    bool started = false;
    si_agent_task_result_t submit_result = si_agent_task_runtime_submit(
        &s_runtime_candidate, s_execution_candidate.thread_id,
        s_execution_candidate.turn_id, s_execution_candidate.run_id,
        next_logical_time(), &started);
    if (submit_result != SI_AGENT_TASK_OK) {
        set_error("task submit rejected: %s",
                  si_agent_task_result_name(submit_result));
        memset(&s_execution_candidate, 0, sizeof(s_execution_candidate));
        xSemaphoreGive(s_task_lock);
        return task_result_to_esp(submit_result);
    }
    if (input->run_id && input->run_id[0] && !started) {
        /* Correlated IDs belong to the single legacy-primary executor. They
         * must describe the active run immediately, never a hidden V2 queue
         * entry which could become authoritative after the legacy run ends. */
        set_error("correlated shadow submit requires an idle active slot");
        memset(&s_execution_candidate, 0, sizeof(s_execution_candidate));
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_INVALID_STATE;
    }

    char goal_hash[65] = {0};
    char criteria_hash[65] = {0};
    char source_auth_id_hash[65] = {0};
    digest_hex(input->goal, goal_hash);
    digest_hex(input->completion_criteria, criteria_hash);
    digest_hex(input->source.auth_id, source_auth_id_hash);
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        memset(&s_execution_candidate, 0, sizeof(s_execution_candidate));
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(payload, "status", started ? "understanding" :
                                                        "queued");
    cJSON_AddStringToObject(payload, "origin",
                            si_agent_task_origin_name(input->source.origin));
    cJSON_AddStringToObject(payload, "source_auth_kind",
                            si_agent_task_auth_kind_name(
                                input->source.auth_kind));
    cJSON_AddStringToObject(payload, "source_principal",
                            source_principal_name(input->source.principal));
    cJSON_AddStringToObject(payload, "source_auth_id_hash",
                            source_auth_id_hash);
    cJSON_AddNumberToObject(payload, "source_auth_generation",
                            input->source.auth_generation);
    cJSON_AddNumberToObject(payload, "authority_ceiling",
                            input->source.authority_ceiling);
    cJSON_AddStringToObject(payload, "history_policy",
                            si_agent_task_history_policy_name(
                                input->source.history_policy));
    cJSON_AddStringToObject(payload, "idempotency_key_hash",
                            idempotency_hash);
    cJSON_AddStringToObject(payload, "goal_hash", goal_hash);
    cJSON_AddNumberToObject(payload, "goal_bytes", strlen(input->goal));
    cJSON_AddStringToObject(payload, "completion_criteria_hash",
                            criteria_hash);
    cJSON_AddStringToObject(
        payload, "completion_criteria_kind",
        completion_criteria_kind_name(input->completion_criteria_kind));
    cJSON_AddBoolToObject(payload, "raw_goal_persisted", false);
    cJSON_AddBoolToObject(payload, "dry_run", input->dry_run);
    cJSON_AddBoolToObject(payload, "include_screenshot",
                          input->include_screenshot);
    cJSON_AddBoolToObject(payload, "allow_web_search",
                          input->allow_web_search);
    cJSON_AddBoolToObject(payload, "page_context_attached",
                          input->page_context_json &&
                          input->page_context_json[0]);
    if (input->profile && input->profile[0]) {
        cJSON_AddStringToObject(payload, "profile", input->profile);
    }
    if (input->model && input->model[0]) {
        cJSON_AddStringToObject(payload, "model", input->model);
    }
    esp_err_t append_result = append_event(
        "turn.submitted", s_execution_candidate.thread_id,
        s_execution_candidate.turn_id, s_execution_candidate.run_id,
        NULL, NULL, NULL, payload);
    cJSON_Delete(payload);
    if (append_result != ESP_OK) {
        memset(&s_execution_candidate, 0, sizeof(s_execution_candidate));
        xSemaphoreGive(s_task_lock);
        return append_result;
    }

    s_runtime = s_runtime_candidate;
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->execution = s_execution_candidate;
    (void)dedupe_remember(s_execution_candidate.idempotency_key,
                          s_execution_candidate.thread_id,
                          s_execution_candidate.turn_id,
                          s_execution_candidate.run_id);
    receipt->accepted = true;
    receipt->started = started;
    receipt->queued = !started;
    receipt->queue_position = started ? 0U :
        si_agent_task_runtime_pending_count(&s_runtime);
    copy_text(receipt->thread_id, sizeof(receipt->thread_id),
              s_execution_candidate.thread_id);
    copy_text(receipt->turn_id, sizeof(receipt->turn_id),
              s_execution_candidate.turn_id);
    copy_text(receipt->run_id, sizeof(receipt->run_id),
              s_execution_candidate.run_id);
    memset(&s_execution_candidate, 0, sizeof(s_execution_candidate));
    xSemaphoreGive(s_task_lock);
    return ESP_OK;
}

esp_err_t si_agent_task_service_snapshot(
    si_agent_task_service_snapshot_t *snapshot)
{
    if (!snapshot) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->recovery_status = s_recovery_status;
        return start_result;
    }
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    /* s_latest_event_seq is filled by background recovery and every successful
     * append. Snapshot is a RAM projection and must never touch TASKS.LOG;
     * Web/status callers therefore stay bounded even while TF is slow. */
    snapshot->available = true;
    snapshot->storage_healthy = s_storage_healthy;
    snapshot->recovery_status = s_recovery_status;
    snapshot->latest_event_seq = s_latest_event_seq;
    snapshot->queue_depth =
        si_agent_task_runtime_pending_count(&s_runtime);
    snapshot->runtime = s_runtime;
    const si_agent_task_turn_t *active =
        si_agent_task_runtime_active(&s_runtime);
    task_record_t *record = active ?
        record_by_run(active->run_attempt.run_attempt_id) : NULL;
    if (record) {
        snapshot->active = record->execution;
    }
    xSemaphoreGive(s_task_lock);
    return ESP_OK;
}

esp_err_t si_agent_task_service_get_execution(
    const char *run_id, si_agent_task_execution_t *execution)
{
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true) || !execution) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) {
        return start_result;
    }
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    task_record_t *record = record_by_run(run_id);
    if (!record) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *execution = record->execution;
    xSemaphoreGive(s_task_lock);
    return ESP_OK;
}

typedef si_agent_task_result_t (*runtime_mutation_t)(
    si_agent_task_runtime_t *runtime, uint64_t now);

static esp_err_t mutate_simple(const char *run_id, const char *event_type,
                               runtime_mutation_t mutation,
                               const char *reason)
{
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true) || !mutation) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) {
        return start_result;
    }
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const si_agent_task_turn_t *active = active_for_run(run_id);
    task_record_t *record = record_by_run(run_id);
    if (!active || !record) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_runtime_candidate = s_runtime;
    si_agent_task_result_t mutation_result =
        mutation(&s_runtime_candidate, next_logical_time());
    if (mutation_result != SI_AGENT_TASK_OK) {
        set_error("%s rejected: %s", event_type,
                  si_agent_task_result_name(mutation_result));
        xSemaphoreGive(s_task_lock);
        return task_result_to_esp(mutation_result);
    }
    cJSON *payload = payload_status(
        si_agent_turn_status_name(s_runtime_candidate.active_turn.status));
    if (!payload) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NO_MEM;
    }
    const bool outcome_unknown =
        candidate_is_outcome_unknown(&s_runtime_candidate);
    const char *canonical_event_type = outcome_unknown ?
        "turn.outcome_unknown" : event_type;
    if (outcome_unknown) {
        if (!payload_add_outcome_unknown(
                payload,
                reason && reason[0] ? reason : "outcome_unknown",
                event_type)) {
            cJSON_Delete(payload);
            xSemaphoreGive(s_task_lock);
            return ESP_ERR_NO_MEM;
        }
    } else if (reason && reason[0]) {
        cJSON_AddStringToObject(payload, "reason", reason);
    }
    esp_err_t append_result = append_event(
        canonical_event_type, record->execution.thread_id,
        record->execution.turn_id,
        record->execution.run_id, NULL, NULL, NULL, payload);
    cJSON_Delete(payload);
    if (append_result == ESP_OK) {
        commit_candidate(&s_runtime_candidate, record);
    }
    xSemaphoreGive(s_task_lock);
    return append_result;
}

static si_agent_task_result_t mutate_pause(si_agent_task_runtime_t *runtime,
                                           uint64_t now)
{
    return si_agent_task_runtime_pause(runtime, now);
}

static si_agent_task_result_t mutate_resume(si_agent_task_runtime_t *runtime,
                                            uint64_t now)
{
    return si_agent_task_runtime_resume(runtime, now);
}

static si_agent_task_result_t mutate_interrupt(
    si_agent_task_runtime_t *runtime, uint64_t now)
{
    return si_agent_task_runtime_interrupt(runtime, now);
}

static si_agent_task_result_t mutate_unknown(si_agent_task_runtime_t *runtime,
                                             uint64_t now)
{
    return si_agent_task_runtime_mark_outcome_unknown(runtime, now);
}

static si_agent_task_result_t mutate_fail(si_agent_task_runtime_t *runtime,
                                          uint64_t now)
{
    return si_agent_task_runtime_fail(runtime, now);
}

static si_agent_task_result_t mutate_complete(si_agent_task_runtime_t *runtime,
                                              uint64_t now)
{
    return si_agent_task_runtime_complete(runtime, now);
}

esp_err_t si_agent_task_service_set_phase(const char *run_id,
                                          si_agent_turn_status_t phase)
{
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) {
        return start_result;
    }
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    task_record_t *record = record_by_run(run_id);
    if (!active_for_run(run_id) || !record) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_runtime_candidate = s_runtime;
    si_agent_task_result_t mutation_result = si_agent_task_runtime_set_phase(
        &s_runtime_candidate, phase, next_logical_time());
    if (mutation_result != SI_AGENT_TASK_OK) {
        xSemaphoreGive(s_task_lock);
        return task_result_to_esp(mutation_result);
    }
    cJSON *payload = payload_status(si_agent_turn_status_name(phase));
    esp_err_t result = payload ? append_event(
        "turn.phase_changed", record->execution.thread_id,
        record->execution.turn_id, record->execution.run_id, NULL, NULL,
        NULL, payload) : ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK) {
        s_runtime = s_runtime_candidate;
        if (phase == SI_AGENT_TURN_EXECUTING &&
            record->execution_started_ms == 0U) {
            record->execution_started_ms = si_monotonic_ms();
            if (record->execution_started_ms == 0U) {
                record->execution_started_ms = 1U;
            }
        }
    }
    xSemaphoreGive(s_task_lock);
    return result;
}

esp_err_t si_agent_task_service_begin_plan(const char *run_id,
                                           const char *plan_id)
{
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(plan_id, SI_AGENT_TASK_ID_MAX, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) return start_result;
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    task_record_t *record = record_by_run(run_id);
    if (!active_for_run(run_id) || !record) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_runtime_candidate = s_runtime;
    si_agent_task_result_t mutation = si_agent_task_runtime_begin_plan(
        &s_runtime_candidate, plan_id, next_logical_time());
    if (mutation != SI_AGENT_TASK_OK) {
        xSemaphoreGive(s_task_lock);
        return task_result_to_esp(mutation);
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload) {
        cJSON_AddStringToObject(payload, "plan_id", plan_id);
        cJSON_AddNumberToObject(payload, "version",
                                s_runtime_candidate.active_turn.plan.version);
    }
    esp_err_t result = payload ? append_event(
        "plan.started", record->execution.thread_id,
        record->execution.turn_id, record->execution.run_id, NULL, NULL,
        NULL, payload) : ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK) s_runtime = s_runtime_candidate;
    xSemaphoreGive(s_task_lock);
    return result;
}

esp_err_t si_agent_task_service_add_step(const char *run_id,
                                         const char *step_id)
{
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(step_id, SI_AGENT_TASK_ID_MAX, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) return start_result;
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    task_record_t *record = record_by_run(run_id);
    if (!active_for_run(run_id) || !record) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_runtime_candidate = s_runtime;
    si_agent_task_result_t mutation = si_agent_task_runtime_add_step(
        &s_runtime_candidate, step_id, next_logical_time());
    if (mutation != SI_AGENT_TASK_OK) {
        xSemaphoreGive(s_task_lock);
        return task_result_to_esp(mutation);
    }
    cJSON *payload = payload_status("pending");
    esp_err_t result = payload ? append_event(
        "step.added", record->execution.thread_id,
        record->execution.turn_id, record->execution.run_id, step_id, NULL,
        NULL, payload) : ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK) s_runtime = s_runtime_candidate;
    xSemaphoreGive(s_task_lock);
    return result;
}

static const char *step_status_name(si_agent_step_status_t status)
{
    switch (status) {
    case SI_AGENT_STEP_PENDING: return "pending";
    case SI_AGENT_STEP_IN_PROGRESS: return "in_progress";
    case SI_AGENT_STEP_COMPLETED: return "completed";
    case SI_AGENT_STEP_BLOCKED: return "blocked";
    case SI_AGENT_STEP_SKIPPED: return "skipped";
    case SI_AGENT_STEP_FAILED: return "failed";
    case SI_AGENT_STEP_OUTCOME_UNKNOWN: return "outcome_unknown";
    default: return "invalid";
    }
}

esp_err_t si_agent_task_service_set_step_status(
    const char *run_id, const char *step_id, si_agent_step_status_t status)
{
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(step_id, SI_AGENT_TASK_ID_MAX, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) return start_result;
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    task_record_t *record = record_by_run(run_id);
    if (!active_for_run(run_id) || !record) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_runtime_candidate = s_runtime;
    si_agent_task_result_t mutation = si_agent_task_runtime_set_step_status(
        &s_runtime_candidate, step_id, status, next_logical_time());
    if (mutation != SI_AGENT_TASK_OK) {
        xSemaphoreGive(s_task_lock);
        return task_result_to_esp(mutation);
    }
    const bool outcome_unknown =
        candidate_is_outcome_unknown(&s_runtime_candidate);
    const char *event_type = outcome_unknown ? "turn.outcome_unknown" :
                                               "step.status_changed";
    cJSON *payload = payload_status(step_status_name(status));
    if (outcome_unknown &&
        !payload_add_outcome_unknown(payload,
                                     "step_status_outcome_unknown",
                                     "step.status_changed")) {
        cJSON_Delete(payload);
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = payload ? append_event(
        event_type, record->execution.thread_id,
        record->execution.turn_id, record->execution.run_id, step_id, NULL,
        NULL, payload) : ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK) commit_candidate(&s_runtime_candidate, record);
    xSemaphoreGive(s_task_lock);
    return result;
}

static const char *verification_name(si_agent_verification_status_t status)
{
    switch (status) {
    case SI_AGENT_VERIFICATION_UNRECORDED: return "unrecorded";
    case SI_AGENT_VERIFICATION_PASSED: return "passed";
    case SI_AGENT_VERIFICATION_FAILED: return "failed";
    case SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN: return "outcome_unknown";
    default: return "invalid";
    }
}

static esp_err_t record_step_verification(
    const char *run_id, const char *step_id,
    si_agent_verification_status_t verification,
    const char *evidence_id, bool verifier_issued)
{
    if (verification == SI_AGENT_VERIFICATION_PASSED && !verifier_issued) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(step_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(evidence_id, SI_AGENT_TASK_ID_MAX,
                 verification == SI_AGENT_VERIFICATION_PASSED)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) return start_result;
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    task_record_t *record = record_by_run(run_id);
    if (!active_for_run(run_id) || !record) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (verification == SI_AGENT_VERIFICATION_PASSED &&
        (!record->completion_artifact_verified ||
         strcmp(record->completion_artifact.artifact_id,
                evidence_id ? evidence_id : "") != 0)) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_ALLOWED;
    }
    s_runtime_candidate = s_runtime;
    si_agent_task_result_t mutation =
        si_agent_task_runtime_record_step_verification(
            &s_runtime_candidate, step_id, verification,
            next_logical_time());
    if (mutation != SI_AGENT_TASK_OK) {
        xSemaphoreGive(s_task_lock);
        return task_result_to_esp(mutation);
    }
    const bool outcome_unknown =
        candidate_is_outcome_unknown(&s_runtime_candidate);
    const char *event_type = outcome_unknown ? "turn.outcome_unknown" :
                                               "step.verification_recorded";
    cJSON *payload = payload_status(verification_name(verification));
    bool payload_ok = payload != NULL;
    if (payload_ok && evidence_id && evidence_id[0]) {
        payload_ok = cJSON_AddStringToObject(
            payload, "evidence_id", evidence_id) != NULL;
    }
    if (payload_ok && outcome_unknown) {
        payload_ok = payload_add_outcome_unknown(
            payload, "step_verification_outcome_unknown",
            "step.verification_recorded");
    }
    esp_err_t result = payload_ok ? append_event(
        event_type, record->execution.thread_id,
        record->execution.turn_id, record->execution.run_id, step_id, NULL,
        evidence_id && evidence_id[0] ? evidence_id : NULL,
        payload) : ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK) commit_candidate(&s_runtime_candidate, record);
    xSemaphoreGive(s_task_lock);
    return result;
}

esp_err_t si_agent_task_service_record_step_verification(
    const char *run_id, const char *step_id,
    si_agent_verification_status_t verification,
    const char *evidence_id)
{
    /* PASSED is intentionally unavailable through this legacy scalar API.
     * Only complete_readonly() may forward an internally re-verified durable
     * artifact into the private verifier-issued path. */
    return record_step_verification(run_id, step_id, verification,
                                    evidence_id, false);
}

static esp_err_t record_turn_verification(
    const char *run_id, si_agent_verification_status_t verification,
    const char *evidence_id, bool verifier_issued)
{
    if (verification == SI_AGENT_VERIFICATION_PASSED && !verifier_issued) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(evidence_id, SI_AGENT_TASK_ID_MAX,
                 verification == SI_AGENT_VERIFICATION_PASSED)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) return start_result;
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    task_record_t *record = record_by_run(run_id);
    if (!active_for_run(run_id) || !record) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (verification == SI_AGENT_VERIFICATION_PASSED &&
        (!record->completion_artifact_verified ||
         strcmp(record->completion_artifact.artifact_id,
                evidence_id ? evidence_id : "") != 0)) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_ALLOWED;
    }
    s_runtime_candidate = s_runtime;
    si_agent_task_result_t mutation =
        si_agent_task_runtime_record_turn_verification(
            &s_runtime_candidate, verification, next_logical_time());
    if (mutation != SI_AGENT_TASK_OK) {
        xSemaphoreGive(s_task_lock);
        return task_result_to_esp(mutation);
    }
    const bool outcome_unknown =
        candidate_is_outcome_unknown(&s_runtime_candidate);
    const char *event_type = outcome_unknown ? "turn.outcome_unknown" :
                                               "turn.verification_recorded";
    cJSON *payload = payload_status(verification_name(verification));
    bool payload_ok = payload != NULL;
    if (payload_ok && evidence_id && evidence_id[0]) {
        payload_ok = cJSON_AddStringToObject(
            payload, "evidence_id", evidence_id) != NULL;
    }
    if (payload_ok && outcome_unknown) {
        payload_ok = payload_add_outcome_unknown(
            payload, "turn_verification_outcome_unknown",
            "turn.verification_recorded");
    }
    esp_err_t result = payload_ok ? append_event(
        event_type, record->execution.thread_id,
        record->execution.turn_id, record->execution.run_id, NULL, NULL,
        evidence_id && evidence_id[0] ? evidence_id : NULL,
        payload) : ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK) commit_candidate(&s_runtime_candidate, record);
    xSemaphoreGive(s_task_lock);
    return result;
}

esp_err_t si_agent_task_service_record_turn_verification(
    const char *run_id, si_agent_verification_status_t verification,
    const char *evidence_id)
{
    return record_turn_verification(run_id, verification, evidence_id, false);
}

static bool completion_artifact_matches(
    const si_agent_completion_artifact_t *expected,
    const si_agent_completion_artifact_t *actual)
{
    return expected && actual && expected->event_seq == actual->event_seq &&
           expected->captured_ms == actual->captured_ms &&
           expected->generation == actual->generation &&
           expected->artifact_bytes == actual->artifact_bytes &&
           expected->kind == actual->kind &&
           expected->issuer == actual->issuer &&
           strcmp(expected->artifact_id, actual->artifact_id) == 0 &&
           strcmp(expected->artifact_hash, actual->artifact_hash) == 0 &&
           strcmp(expected->action_id, actual->action_id) == 0 &&
           strcmp(expected->target_identity, actual->target_identity) == 0 &&
           strcmp(expected->completion_criteria_hash,
                  actual->completion_criteria_hash) == 0 &&
           strcmp(expected->event_hash, actual->event_hash) == 0;
}

esp_err_t si_agent_task_service_record_readonly_completion_artifact(
    const char *run_id, const char *step_id, const char *artifact_hash,
    uint32_t artifact_bytes, si_agent_completion_artifact_t *artifact_out)
{
    if (artifact_out) {
        memset(artifact_out, 0, sizeof(*artifact_out));
    }
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(step_id, SI_AGENT_TASK_ID_MAX, true) ||
        !sha256_hex_valid(artifact_hash) || artifact_bytes == 0U ||
        artifact_bytes > SI_AGENT_TASK_COMPLETION_ARTIFACT_MAX_BYTES ||
        !artifact_out) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) {
        return start_result;
    }
    char target_identity[SI_AGENT_VERIFY_TARGET_MAX_LEN + 1U] = {0};
    if (!current_target_identity(target_identity)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const si_agent_task_turn_t *turn = active_for_run(run_id);
    task_record_t *record = record_by_run(run_id);
    if (!turn || !record || !active_step_bound(turn, step_id) ||
        record->mutation_seen || record->execution_started_ms == 0U ||
        record->completion_artifact.event_seq != 0U) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t now_ms = si_monotonic_ms();
    if (now_ms <= record->execution_started_ms) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_INVALID_STATE;
    }
    char criteria_hash[SI_AGENT_VERIFY_HASH_MAX_LEN + 1U] = {0};
    digest_hex(record->execution.completion_criteria, criteria_hash);
    if (!sha256_hex_valid(criteria_hash)) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_INVALID_STATE;
    }
    char artifact_id[SI_AGENT_VERIFY_ID_MAX_LEN + 1U] = {0};
    snprintf(artifact_id, sizeof(artifact_id), "final-%016" PRIx64,
             s_latest_event_seq + 1U);
    cJSON *payload = cJSON_CreateObject();
    bool payload_ok = payload &&
        cJSON_AddStringToObject(payload, "status", "recorded") != NULL &&
        cJSON_AddStringToObject(payload, "kind", "final_response") != NULL &&
        cJSON_AddStringToObject(payload, "issuer", "artifact_store") != NULL &&
        cJSON_AddStringToObject(payload, "artifact_hash", artifact_hash) != NULL &&
        cJSON_AddNumberToObject(payload, "artifact_bytes", artifact_bytes) != NULL &&
        cJSON_AddStringToObject(payload, "target_identity",
                               target_identity) != NULL &&
        cJSON_AddStringToObject(payload, "completion_criteria_hash",
                               criteria_hash) != NULL &&
        cJSON_AddStringToObject(
            payload, "completion_criteria_kind",
            completion_criteria_kind_name(
                record->execution.completion_criteria_kind)) != NULL &&
        cJSON_AddBoolToObject(payload, "mutation", false) != NULL &&
        cJSON_AddBoolToObject(payload, "raw_response_persisted", false) != NULL;
    si_agent_event_record_t stored = {0};
    esp_err_t result = payload_ok ? append_event_stored(
        "completion.artifact", record->execution.thread_id,
        record->execution.turn_id, record->execution.run_id, step_id, run_id,
        artifact_id, payload, &stored) : ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK && stored.seq > 0U && stored.monotonic_time_ms > 0 &&
        (uint64_t)stored.monotonic_time_ms > record->execution_started_ms &&
        sha256_hex_valid(stored.event_hash)) {
        si_agent_completion_artifact_t artifact = {
            .event_seq = stored.seq,
            .captured_ms = (uint64_t)stored.monotonic_time_ms,
            .generation = stored.seq,
            .artifact_bytes = artifact_bytes,
            .kind = SI_AGENT_EVIDENCE_ARTIFACT,
            .issuer = SI_AGENT_EVIDENCE_ISSUER_ARTIFACT_STORE,
        };
        copy_text(artifact.artifact_id, sizeof(artifact.artifact_id),
                  artifact_id);
        copy_text(artifact.artifact_hash, sizeof(artifact.artifact_hash),
                  artifact_hash);
        copy_text(artifact.action_id, sizeof(artifact.action_id), run_id);
        copy_text(artifact.target_identity, sizeof(artifact.target_identity),
                  target_identity);
        copy_text(artifact.completion_criteria_hash,
                  sizeof(artifact.completion_criteria_hash), criteria_hash);
        copy_text(artifact.event_hash, sizeof(artifact.event_hash),
                  stored.event_hash);
        record->completion_artifact = artifact;
        *artifact_out = artifact;
    } else if (result == ESP_OK) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    xSemaphoreGive(s_task_lock);
    return result;
}

esp_err_t si_agent_task_service_complete_readonly(
    const char *run_id, const char *step_id,
    const si_agent_completion_artifact_t *artifact,
    si_agent_verification_result_t *verification_out)
{
    if (verification_out) {
        memset(verification_out, 0, sizeof(*verification_out));
        verification_out->status = SI_AGENT_VERIFY_INVALID;
        verification_out->accepted_index = SIZE_MAX;
    }
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(step_id, SI_AGENT_TASK_ID_MAX, true) || !artifact ||
        !verification_out) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) {
        return start_result;
    }
    char current_target[SI_AGENT_VERIFY_TARGET_MAX_LEN + 1U] = {0};
    if (!current_target_identity(current_target)) {
        snprintf(verification_out->reason, sizeof(verification_out->reason),
                 "stable target identity unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const si_agent_task_turn_t *turn = active_for_run(run_id);
    task_record_t *record = record_by_run(run_id);
    char criteria_hash[SI_AGENT_VERIFY_HASH_MAX_LEN + 1U] = {0};
    if (record) {
        digest_hex(record->execution.completion_criteria, criteria_hash);
    }
    if (!turn || !record || !active_step_bound(turn, step_id) ||
        record->mutation_seen || record->execution_started_ms == 0U ||
        record->execution.completion_criteria_kind !=
            SI_AGENT_TASK_CRITERIA_DELIVERABLE ||
        !completion_artifact_matches(&record->completion_artifact, artifact) ||
        strcmp(record->completion_artifact.target_identity,
               current_target) != 0 ||
        strcmp(record->completion_artifact.completion_criteria_hash,
               criteria_hash) != 0) {
        snprintf(verification_out->reason, sizeof(verification_out->reason),
                 record && record->execution.completion_criteria_kind !=
                                   SI_AGENT_TASK_CRITERIA_DELIVERABLE ?
                     "artifact delivery cannot verify observation criteria" :
                     "durable completion artifact binding mismatch");
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_ALLOWED;
    }
    const si_agent_verification_spec_t spec = {
        .mutation = false,
        .require_readback = false,
        .action_started_ms = record->execution_started_ms,
        .action_id = run_id,
        .target_identity = current_target,
        .completion_criteria_hash = criteria_hash,
        .minimum_generation = artifact->event_seq,
        .required_issuer = SI_AGENT_EVIDENCE_ISSUER_ARTIFACT_STORE,
    };
    const si_agent_evidence_t evidence = {
        .kind = record->completion_artifact.kind,
        .outcome = SI_AGENT_EVIDENCE_OUTCOME_MATCH,
        .captured_ms = record->completion_artifact.captured_ms,
        .evidence_id = record->completion_artifact.artifact_id,
        .artifact_hash = record->completion_artifact.artifact_hash,
        .action_id = record->completion_artifact.action_id,
        .target_identity = record->completion_artifact.target_identity,
        .completion_criteria_hash =
            record->completion_artifact.completion_criteria_hash,
        .generation = record->completion_artifact.generation,
        .issuer = record->completion_artifact.issuer,
    };
    *verification_out = si_agent_verify(&spec, &evidence, 1U);
    if (verification_out->status != SI_AGENT_VERIFY_PASSED) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_ALLOWED;
    }
    snprintf(verification_out->reason, sizeof(verification_out->reason),
             "durable artifact satisfies output-only deliverable criterion");
    record->completion_artifact_verified = true;
    char evidence_id[SI_AGENT_VERIFY_ID_MAX_LEN + 1U] = {0};
    copy_text(evidence_id, sizeof(evidence_id),
              record->completion_artifact.artifact_id);
    xSemaphoreGive(s_task_lock);

    esp_err_t result = record_step_verification(
        run_id, step_id, SI_AGENT_VERIFICATION_PASSED, evidence_id, true);
    if (result == ESP_OK) {
        result = si_agent_task_service_set_step_status(
            run_id, step_id, SI_AGENT_STEP_COMPLETED);
    }
    if (result == ESP_OK) {
        result = si_agent_task_service_set_phase(run_id,
                                                 SI_AGENT_TURN_VERIFYING);
    }
    if (result == ESP_OK) {
        result = record_turn_verification(
            run_id, SI_AGENT_VERIFICATION_PASSED, evidence_id, true);
    }
    if (result == ESP_OK) {
        result = si_agent_task_service_complete(run_id);
    }
    if (result != ESP_OK) {
        verification_out->status = SI_AGENT_VERIFY_INVALID;
        snprintf(verification_out->reason, sizeof(verification_out->reason),
                 "verified completion could not be durably committed");
    }
    return result;
}

esp_err_t si_agent_task_service_record_action_started(
    const char *run_id, const char *step_id, const char *action_id,
    const char *tool_name, const char *normalized_args_sha256,
    const char *idempotency_key, bool mutation)
{
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(step_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(action_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(tool_name, SI_AGENT_TASK_TOOL_MAX_LEN, true) ||
        !sha256_hex_valid(normalized_args_sha256) ||
        !bounded(idempotency_key, SI_AGENT_TASK_IDEMPOTENCY_MAX_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) return start_result;
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    const si_agent_task_turn_t *turn = active_for_run(run_id);
    task_record_t *record = record_by_run(run_id);
    if (!turn || !record || !active_step_bound(turn, step_id) ||
        si_agent_turn_status_is_terminal(turn->status)) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (action_ledger_find(run_id, action_id)) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_INVALID_STATE;
    }
    task_action_ledger_t *ledger = action_ledger_free();
    if (!ledger) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NO_MEM;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload) {
        cJSON_AddStringToObject(payload, "status", "executing");
        cJSON_AddStringToObject(payload, "tool_name", tool_name);
        cJSON_AddStringToObject(payload, "normalized_args_sha256",
                                normalized_args_sha256);
        cJSON_AddBoolToObject(payload, "mutation", mutation);
        char idempotency_hash[65] = {0};
        digest_hex(idempotency_key, idempotency_hash);
        cJSON_AddStringToObject(payload, "idempotency_key_hash",
                                idempotency_hash);
        cJSON_AddNumberToObject(payload, "plan_version",
                                turn->plan_version);
        cJSON_AddBoolToObject(payload, "raw_args_persisted", false);
    }
    esp_err_t result = payload ? append_event(
        "action.started", record->execution.thread_id,
        record->execution.turn_id, record->execution.run_id, step_id,
        action_id, NULL, payload) : ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK) {
        memset(ledger, 0, sizeof(*ledger));
        ledger->used = true;
        ledger->mutation = mutation;
        ledger->plan_version = turn->plan_version;
        copy_text(ledger->run_id, sizeof(ledger->run_id), run_id);
        copy_text(ledger->step_id, sizeof(ledger->step_id), step_id);
        copy_text(ledger->action_id, sizeof(ledger->action_id), action_id);
        record->mutation_seen = record->mutation_seen || mutation;
    }
    xSemaphoreGive(s_task_lock);
    return result;
}

esp_err_t si_agent_task_service_record_action_result(
    const char *run_id, const char *step_id, const char *action_id,
    bool succeeded, const char *artifact_id, const char *reason_code)
{
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(step_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(action_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(artifact_id, SI_AGENT_TASK_ID_MAX, false) ||
        !bounded(reason_code, SI_AGENT_TASK_REASON_CODE_MAX_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) return start_result;
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    const si_agent_task_turn_t *turn = active_for_run(run_id);
    task_record_t *record = record_by_run(run_id);
    task_action_ledger_t *ledger = action_ledger_find(run_id, action_id);
    if (!turn || !record || !ledger || ledger->result_recorded ||
        si_agent_turn_status_is_terminal(turn->status) ||
        !active_step_bound(turn, step_id) ||
        strcmp(ledger->step_id, step_id) != 0 ||
        ledger->plan_version != turn->plan_version) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload) {
        cJSON_AddStringToObject(payload, "status",
                                succeeded ? "executed" : "failed");
        cJSON_AddStringToObject(payload, "reason_code", reason_code);
        cJSON_AddBoolToObject(payload, "verified", false);
    }
    esp_err_t result = payload ? append_event(
        succeeded ? "action.result" : "action.failed",
        record->execution.thread_id, record->execution.turn_id,
        record->execution.run_id, step_id, action_id,
        succeeded && artifact_id && artifact_id[0] ? artifact_id : NULL,
        payload) : ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK) {
        ledger->result_recorded = true;
    }
    xSemaphoreGive(s_task_lock);
    return result;
}

esp_err_t si_agent_task_service_steer(const char *run_id,
                                      const char *message)
{
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true) ||
        !bounded(message, SI_AGENT_TASK_STEER_MAX_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) return start_result;
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    task_record_t *record = record_by_run(run_id);
    if (!active_for_run(run_id) || !record) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_runtime_candidate = s_runtime;
    si_agent_task_result_t mutation = si_agent_task_runtime_steer(
        &s_runtime_candidate, next_logical_time());
    if (mutation != SI_AGENT_TASK_OK) {
        xSemaphoreGive(s_task_lock);
        return task_result_to_esp(mutation);
    }
    char message_hash[65] = {0};
    digest_hex(message, message_hash);
    cJSON *payload = cJSON_CreateObject();
    if (payload) {
        cJSON_AddStringToObject(payload, "message_hash", message_hash);
        cJSON_AddNumberToObject(payload, "message_bytes", strlen(message));
        cJSON_AddBoolToObject(payload, "raw_message_persisted", false);
        cJSON_AddNumberToObject(payload, "intent_revision",
                                s_runtime_candidate.active_turn.intent_revision);
        cJSON_AddNumberToObject(payload, "plan_version",
                                s_runtime_candidate.active_turn.plan_version);
    }
    esp_err_t result = payload ? append_event(
        "turn.steered", record->execution.thread_id,
        record->execution.turn_id, record->execution.run_id, NULL, NULL,
        NULL, payload) : ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK) s_runtime = s_runtime_candidate;
    xSemaphoreGive(s_task_lock);
    return result;
}

esp_err_t si_agent_task_service_pause(const char *run_id)
{
    return mutate_simple(run_id, "turn.paused", mutate_pause, NULL);
}

esp_err_t si_agent_task_service_resume(const char *run_id)
{
    return mutate_simple(run_id, "turn.resumed", mutate_resume, NULL);
}

esp_err_t si_agent_task_service_cancel(const char *run_id)
{
    if (!bounded(run_id, SI_AGENT_TASK_ID_MAX, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) {
        return start_result;
    }
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    task_record_t *record = record_by_run(run_id);
    if (!active_for_run(run_id) || !record) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_NOT_FOUND;
    }
    const task_action_ledger_t *started_action = NULL;
    for (size_t index = 0; index < SI_AGENT_TASK_ACTION_LEDGER_CAPACITY;
         ++index) {
        if (s_action_ledger[index].used &&
            strcmp(s_action_ledger[index].run_id, run_id) == 0) {
            started_action = &s_action_ledger[index];
            break;
        }
    }
    s_runtime_candidate = s_runtime;
    si_agent_task_result_t mutation = started_action ?
        si_agent_task_runtime_mark_outcome_unknown(
            &s_runtime_candidate, next_logical_time()) :
        si_agent_task_runtime_cancel(&s_runtime_candidate,
                                     next_logical_time());
    if (mutation != SI_AGENT_TASK_OK) {
        xSemaphoreGive(s_task_lock);
        return task_result_to_esp(mutation);
    }
    const char *event_type = started_action ? "turn.outcome_unknown" :
                                              "turn.cancelled";
    cJSON *payload = payload_status(
        si_agent_turn_status_name(s_runtime_candidate.active_turn.status));
    bool payload_ok = payload != NULL;
    if (payload_ok && started_action) {
        payload_ok = payload_add_outcome_unknown(
            payload, "cancel_after_action_started", "turn.cancelled");
    } else if (payload_ok) {
        payload_ok = cJSON_AddStringToObject(
            payload, "reason", "cancel_requested") != NULL;
    }
    esp_err_t result = payload_ok ? append_event(
        event_type, record->execution.thread_id, record->execution.turn_id,
        record->execution.run_id, NULL,
        started_action ? started_action->action_id : NULL, NULL, payload) :
        ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK) {
        commit_candidate(&s_runtime_candidate, record);
    }
    xSemaphoreGive(s_task_lock);
    return result;
}

esp_err_t si_agent_task_service_interrupt(const char *run_id,
                                          const char *reason)
{
    return mutate_simple(run_id, "turn.interrupted", mutate_interrupt,
                         reason);
}

esp_err_t si_agent_task_service_mark_outcome_unknown(
    const char *run_id, const char *action_id, const char *reason)
{
    if (!bounded(action_id, SI_AGENT_TASK_ID_MAX, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = mutate_simple(run_id, "turn.outcome_unknown",
                                     mutate_unknown, reason);
    if (result == ESP_OK) {
        /* The terminal event already preserves the run; action detail is
         * intentionally available through action.started in the journal. */
        (void)action_id;
    }
    return result;
}

esp_err_t si_agent_task_service_fail(const char *run_id,
                                     const char *reason)
{
    return mutate_simple(run_id, "turn.failed", mutate_fail, reason);
}

esp_err_t si_agent_task_service_complete(const char *run_id)
{
    return mutate_simple(run_id, "turn.completed", mutate_complete, NULL);
}

esp_err_t si_agent_task_service_revoke_page_context(void)
{
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) return start_result;
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    size_t cleared = 0U;
    const si_agent_task_turn_t *active =
        si_agent_task_runtime_active(&s_runtime);
    task_record_t *event_record = active ?
        record_by_run(active->run_attempt.run_attempt_id) : NULL;
    for (size_t index = 0; index < SI_AGENT_TASK_RECORD_CAPACITY; ++index) {
        if (s_records[index].used &&
            s_records[index].execution.page_context_json[0]) {
            if (!event_record) {
                event_record = &s_records[index];
            }
            cleared++;
        }
    }
    if (!event_record || cleared == 0U) {
        xSemaphoreGive(s_task_lock);
        return ESP_OK;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload) {
        cJSON_AddNumberToObject(payload, "cleared_task_count", cleared);
        cJSON_AddBoolToObject(payload, "raw_context_persisted", false);
    }
    esp_err_t result = payload ? append_event(
        "context.revoked", event_record->execution.thread_id,
        event_record->execution.turn_id, event_record->execution.run_id,
        NULL, NULL, NULL, payload) : ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK) {
        for (size_t index = 0; index < SI_AGENT_TASK_RECORD_CAPACITY;
             ++index) {
            if (s_records[index].used) {
                memset(s_records[index].execution.page_context_json, 0,
                       sizeof(s_records[index].execution.page_context_json));
            }
        }
    }
    xSemaphoreGive(s_task_lock);
    return result;
}

esp_err_t si_agent_task_service_advance(si_agent_task_execution_t *next,
                                        bool *started_out)
{
    if (!next || !started_out) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) return start_result;
    if (xSemaphoreTake(s_task_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    memset(next, 0, sizeof(*next));
    *started_out = false;
    const si_agent_task_turn_t *old_active =
        si_agent_task_runtime_active(&s_runtime);
    if (!old_active || !si_agent_turn_status_is_terminal(old_active->status)) {
        xSemaphoreGive(s_task_lock);
        return ESP_ERR_INVALID_STATE;
    }
    char old_run_id[SI_AGENT_TASK_ID_MAX + 1U] = {0};
    char old_thread_id[SI_AGENT_TASK_ID_MAX + 1U] = {0};
    char old_turn_id[SI_AGENT_TASK_ID_MAX + 1U] = {0};
    copy_text(old_run_id, sizeof(old_run_id),
              old_active->run_attempt.run_attempt_id);
    copy_text(old_thread_id, sizeof(old_thread_id), old_active->thread_id);
    copy_text(old_turn_id, sizeof(old_turn_id), old_active->turn_id);
    s_runtime_candidate = s_runtime;
    bool started = false;
    si_agent_task_result_t mutation = si_agent_task_runtime_advance_queue(
        &s_runtime_candidate, next_logical_time(), &started);
    if (mutation != SI_AGENT_TASK_OK) {
        xSemaphoreGive(s_task_lock);
        return task_result_to_esp(mutation);
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload) {
        cJSON_AddBoolToObject(payload, "next_started", started);
        cJSON_AddNumberToObject(payload, "queue_depth",
            si_agent_task_runtime_pending_count(&s_runtime_candidate));
        if (started) {
            cJSON_AddStringToObject(payload, "next_thread_id",
                                    s_runtime_candidate.active_turn.thread_id);
            cJSON_AddStringToObject(payload, "next_turn_id",
                                    s_runtime_candidate.active_turn.turn_id);
            cJSON_AddStringToObject(payload, "next_run_id",
                s_runtime_candidate.active_turn.run_attempt.run_attempt_id);
        }
    }
    esp_err_t result = payload ? append_event(
        "queue.advanced", old_thread_id, old_turn_id, old_run_id, NULL,
        NULL, NULL, payload) : ESP_ERR_NO_MEM;
    cJSON_Delete(payload);
    if (result == ESP_OK) {
        s_runtime = s_runtime_candidate;
        task_record_t *old_record = record_by_run(old_run_id);
        if (old_record) memset(old_record, 0, sizeof(*old_record));
        if (started) {
            task_record_t *new_record = record_by_run(
                s_runtime_candidate.active_turn.run_attempt.run_attempt_id);
            if (!new_record) {
                s_storage_healthy = false;
                set_error("promoted task record missing");
                result = ESP_ERR_INVALID_STATE;
            } else {
                *next = new_record->execution;
                *started_out = true;
            }
        }
    }
    xSemaphoreGive(s_task_lock);
    return result;
}

esp_err_t si_agent_task_service_replay(
    uint64_t after_seq, si_agent_event_replay_cb_t callback, void *user_ctx,
    si_agent_event_recovery_t *recovery_out)
{
    esp_err_t start_result = si_agent_task_service_start();
    if (start_result != ESP_OK) return start_result;
    return si_agent_event_store_replay(after_seq, callback, user_ctx,
                                       recovery_out);
}

const char *si_agent_task_origin_name(si_agent_task_origin_t origin)
{
    switch (origin) {
    case SI_AGENT_TASK_ORIGIN_WEB: return "web";
    case SI_AGENT_TASK_ORIGIN_QQ: return "qq";
    case SI_AGENT_TASK_ORIGIN_MCP: return "mcp";
    case SI_AGENT_TASK_ORIGIN_SYSTEM: return "system";
    default: return "invalid";
    }
}

const char *si_agent_task_auth_kind_name(si_agent_task_auth_kind_t kind)
{
    switch (kind) {
    case SI_AGENT_TASK_AUTH_DEVICE_SESSION: return "device_session";
    case SI_AGENT_TASK_AUTH_ADAPTER_DELEGATION:
        return "adapter_delegation";
    case SI_AGENT_TASK_AUTH_SYSTEM: return "system";
    default: return "invalid";
    }
}

const char *si_agent_task_history_policy_name(
    si_agent_task_history_policy_t policy)
{
    switch (policy) {
    case SI_AGENT_TASK_HISTORY_NONE: return "none";
    case SI_AGENT_TASK_HISTORY_SINGLE_TURN: return "single_turn";
    case SI_AGENT_TASK_HISTORY_THREAD: return "thread";
    default: return "invalid";
    }
}

const char *si_agent_task_service_last_error(void)
{
    return s_last_error;
}
