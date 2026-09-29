#include "agent_request_broker.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#ifndef SI_AGENT_REQUEST_BROKER_HOST_TEST
#include "esp_attr.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "time_utils.h"
#define SI_AGENT_REQUEST_EXT_RAM EXT_RAM_BSS_ATTR
#else
#define SI_AGENT_REQUEST_EXT_RAM
#endif

#define SI_AGENT_REQUEST_DEFAULT_TTL_MS 120000U
#define SI_AGENT_REQUEST_MIN_TTL_MS 5000U
#define SI_AGENT_REQUEST_MAX_TTL_MS 600000U

static SI_AGENT_REQUEST_EXT_RAM si_agent_request_status_t
    s_requests[SI_AGENT_REQUEST_QUEUE_CAPACITY];
static uint64_t s_next_sequence = 1U;
static si_agent_request_now_fn_t s_now_fn;
static void *s_now_context;

#ifndef SI_AGENT_REQUEST_BROKER_HOST_TEST
static SemaphoreHandle_t s_lock;
#else
static bool s_started;
static bool s_host_lock_unavailable;
static uint32_t s_host_random = 0x6d2b79f5U;
#endif

static void copy_text(char *destination, size_t destination_size,
                      const char *source)
{
    if (!destination || destination_size == 0U) {
        return;
    }
    const char *text = source ? source : "";
    size_t length = strlen(text);
    if (length >= destination_size) {
        length = destination_size - 1U;
    }
    if (length > 0U) {
        memcpy(destination, text, length);
    }
    destination[length] = '\0';
}

static bool value_valid(const char *value, size_t max_len, bool required)
{
    if (!value) {
        return !required;
    }
    size_t length = strlen(value);
    return (!required || length > 0U) && length <= max_len;
}

static bool fixed_value_valid(const char *value, size_t capacity,
                              bool required)
{
    if (!value || capacity == 0U) {
        return false;
    }
    const char *end = memchr(value, '\0', capacity);
    if (!end) {
        return false;
    }
    return !required || end != value;
}

static bool kind_valid(si_agent_request_kind_t kind)
{
    return kind == SI_AGENT_REQUEST_KIND_CONTEXT ||
           kind == SI_AGENT_REQUEST_KIND_ACTION;
}

static bool risk_valid(si_agent_request_risk_t risk)
{
    return risk >= SI_AGENT_REQUEST_RISK_LOW &&
           risk <= SI_AGENT_REQUEST_RISK_CRITICAL;
}

static bool scope_valid(si_agent_request_grant_scope_t scope)
{
    return scope >= SI_AGENT_REQUEST_GRANT_ONCE &&
           scope <= SI_AGENT_REQUEST_GRANT_THREAD;
}

static bool state_valid(si_agent_request_state_t state)
{
    return state >= SI_AGENT_REQUEST_REQUESTED &&
           state <= SI_AGENT_REQUEST_UNKNOWN;
}

static bool actor_valid(const si_agent_request_actor_t *actor,
                        bool require_known_principal)
{
    if (!actor ||
        !fixed_value_valid(actor->session_id, sizeof(actor->session_id), true) ||
        !fixed_value_valid(actor->instance_id, sizeof(actor->instance_id), true)) {
        return false;
    }
    return !require_known_principal ||
           (actor->principal > SI_AGENT_REQUEST_PRINCIPAL_UNSPECIFIED &&
            actor->principal <= SI_AGENT_REQUEST_PRINCIPAL_RUNTIME);
}

static bool actor_equal(const si_agent_request_actor_t *left,
                        const si_agent_request_actor_t *right)
{
    return left && right && left->principal == right->principal &&
           strcmp(left->session_id, right->session_id) == 0 &&
           strcmp(left->instance_id, right->instance_id) == 0;
}

static uint32_t clamp_ttl(uint32_t ttl_ms)
{
    if (ttl_ms == 0U) {
        return SI_AGENT_REQUEST_DEFAULT_TTL_MS;
    }
    if (ttl_ms < SI_AGENT_REQUEST_MIN_TTL_MS) {
        return SI_AGENT_REQUEST_MIN_TTL_MS;
    }
    if (ttl_ms > SI_AGENT_REQUEST_MAX_TTL_MS) {
        return SI_AGENT_REQUEST_MAX_TTL_MS;
    }
    return ttl_ms;
}

static uint32_t default_now_ms(void *context)
{
    (void)context;
#ifndef SI_AGENT_REQUEST_BROKER_HOST_TEST
    return si_monotonic_ms();
#else
    return 0U;
#endif
}

static uint32_t broker_now_ms(void)
{
    si_agent_request_now_fn_t now_fn = s_now_fn ? s_now_fn : default_now_ms;
    return now_fn(s_now_context);
}

static uint32_t broker_random(void)
{
#ifndef SI_AGENT_REQUEST_BROKER_HOST_TEST
    return esp_random();
#else
    s_host_random ^= s_host_random << 13U;
    s_host_random ^= s_host_random >> 17U;
    s_host_random ^= s_host_random << 5U;
    return s_host_random;
#endif
}

static esp_err_t sha256_hex(
    const char *value, char out[SI_AGENT_REQUEST_HASH_HEX_LEN + 1])
{
    if (!value || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t digest[32] = {0};
#ifndef SI_AGENT_REQUEST_BROKER_HOST_TEST
    if (mbedtls_sha256((const unsigned char *)value, strlen(value),
                       digest, 0) != 0) {
        return ESP_FAIL;
    }
#else
    /* Host-only deterministic stand-in; firmware always uses SHA-256. */
    uint32_t hash = 2166136261U;
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor; ++cursor) {
        hash = (hash ^ *cursor) * 16777619U;
    }
    for (size_t index = 0; index < sizeof(digest); ++index) {
        hash ^= hash << 13U;
        hash ^= hash >> 17U;
        hash ^= hash << 5U;
        digest[index] = (uint8_t)(hash >> ((index % 4U) * 8U));
    }
#endif
    for (size_t index = 0; index < sizeof(digest); ++index) {
        snprintf(out + index * 2U, 3U, "%02x", digest[index]);
    }
    out[SI_AGENT_REQUEST_HASH_HEX_LEN] = '\0';
    return ESP_OK;
}

static esp_err_t broker_lock(uint32_t timeout_ms)
{
#ifndef SI_AGENT_REQUEST_BROKER_HOST_TEST
    if (!s_lock ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
#else
    (void)timeout_ms;
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_host_lock_unavailable) {
        return ESP_ERR_TIMEOUT;
    }
#endif
    return ESP_OK;
}

static void broker_unlock(void)
{
#ifndef SI_AGENT_REQUEST_BROKER_HOST_TEST
    xSemaphoreGive(s_lock);
#endif
}

static bool deadline_reached(uint32_t now_ms, uint32_t expires_at_ms)
{
    return (int32_t)(now_ms - expires_at_ms) >= 0;
}

static uint32_t remaining_ttl(uint32_t now_ms, uint32_t expires_at_ms)
{
    return deadline_reached(now_ms, expires_at_ms) ?
           0U : expires_at_ms - now_ms;
}

static bool transition_allowed(si_agent_request_state_t from,
                               si_agent_request_state_t to)
{
    switch (from) {
    case SI_AGENT_REQUEST_REQUESTED:
        return to == SI_AGENT_REQUEST_EVALUATING ||
               to == SI_AGENT_REQUEST_CANCELLED ||
               to == SI_AGENT_REQUEST_EXPIRED;
    case SI_AGENT_REQUEST_EVALUATING:
        return to == SI_AGENT_REQUEST_REVIEWING ||
               to == SI_AGENT_REQUEST_WAITING_USER ||
               to == SI_AGENT_REQUEST_GRANTED ||
               to == SI_AGENT_REQUEST_DENIED ||
               to == SI_AGENT_REQUEST_CANCELLED ||
               to == SI_AGENT_REQUEST_EXPIRED;
    case SI_AGENT_REQUEST_REVIEWING:
        return to == SI_AGENT_REQUEST_WAITING_USER ||
               to == SI_AGENT_REQUEST_GRANTED ||
               to == SI_AGENT_REQUEST_DENIED ||
               to == SI_AGENT_REQUEST_CANCELLED ||
               to == SI_AGENT_REQUEST_EXPIRED;
    case SI_AGENT_REQUEST_WAITING_USER:
        return to == SI_AGENT_REQUEST_GRANTED ||
               to == SI_AGENT_REQUEST_DENIED ||
               to == SI_AGENT_REQUEST_CANCELLED ||
               to == SI_AGENT_REQUEST_EXPIRED;
    case SI_AGENT_REQUEST_GRANTED:
        return to == SI_AGENT_REQUEST_EXECUTING ||
               to == SI_AGENT_REQUEST_CANCELLED ||
               to == SI_AGENT_REQUEST_EXPIRED;
    case SI_AGENT_REQUEST_EXECUTING:
        return to == SI_AGENT_REQUEST_VERIFYING ||
               to == SI_AGENT_REQUEST_FAILED ||
               to == SI_AGENT_REQUEST_UNKNOWN;
    case SI_AGENT_REQUEST_VERIFYING:
        return to == SI_AGENT_REQUEST_COMPLETED ||
               to == SI_AGENT_REQUEST_FAILED ||
               to == SI_AGENT_REQUEST_UNKNOWN;
    case SI_AGENT_REQUEST_EMPTY:
    case SI_AGENT_REQUEST_COMPLETED:
    case SI_AGENT_REQUEST_FAILED:
    case SI_AGENT_REQUEST_DENIED:
    case SI_AGENT_REQUEST_EXPIRED:
    case SI_AGENT_REQUEST_CANCELLED:
    case SI_AGENT_REQUEST_UNKNOWN:
    default:
        return false;
    }
}

static bool set_state_locked(si_agent_request_status_t *request,
                             si_agent_request_state_t next,
                             uint32_t now_ms)
{
    if (!request || !transition_allowed(request->state, next)) {
        return false;
    }
    request->state = next;
    switch (next) {
    case SI_AGENT_REQUEST_GRANTED:
    case SI_AGENT_REQUEST_DENIED:
    case SI_AGENT_REQUEST_EXPIRED:
    case SI_AGENT_REQUEST_CANCELLED:
        request->decided_ms = now_ms;
        break;
    case SI_AGENT_REQUEST_EXECUTING:
        request->consumed_ms = now_ms;
        request->grant_consumed = true;
        break;
    case SI_AGENT_REQUEST_VERIFYING:
        request->verifying_ms = now_ms;
        break;
    case SI_AGENT_REQUEST_COMPLETED:
    case SI_AGENT_REQUEST_FAILED:
    case SI_AGENT_REQUEST_UNKNOWN:
        request->completed_ms = now_ms;
        break;
    default:
        break;
    }
    return true;
}

static bool state_expires(si_agent_request_state_t state)
{
    return state == SI_AGENT_REQUEST_REQUESTED ||
           state == SI_AGENT_REQUEST_EVALUATING ||
           state == SI_AGENT_REQUEST_REVIEWING ||
           state == SI_AGENT_REQUEST_WAITING_USER ||
           state == SI_AGENT_REQUEST_GRANTED;
}

static bool state_terminal(si_agent_request_state_t state)
{
    return state == SI_AGENT_REQUEST_COMPLETED ||
           state == SI_AGENT_REQUEST_FAILED ||
           state == SI_AGENT_REQUEST_DENIED ||
           state == SI_AGENT_REQUEST_EXPIRED ||
           state == SI_AGENT_REQUEST_CANCELLED ||
           state == SI_AGENT_REQUEST_UNKNOWN;
}

static void expire_all_locked(uint32_t now_ms)
{
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        si_agent_request_status_t *request = &s_requests[index];
        if (state_expires(request->state) &&
            deadline_reached(now_ms, request->expires_at_ms)) {
            (void)set_state_locked(request, SI_AGENT_REQUEST_EXPIRED, now_ms);
            copy_text(request->decision_source,
                      sizeof(request->decision_source), "ttl");
            copy_text(request->error, sizeof(request->error),
                      "request or grant expired");
        }
    }
}

static si_agent_request_status_t *find_by_id_locked(const char *request_id)
{
    if (!request_id || !request_id[0]) {
        return NULL;
    }
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        if (strcmp(s_requests[index].request_id, request_id) == 0) {
            return &s_requests[index];
        }
    }
    return NULL;
}

static si_agent_request_status_t *find_idempotency_locked(
    const char *idempotency_key)
{
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        if (s_requests[index].state != SI_AGENT_REQUEST_EMPTY &&
            strcmp(s_requests[index].idempotency_key,
                   idempotency_key) == 0) {
            return &s_requests[index];
        }
    }
    return NULL;
}

static si_agent_request_status_t *allocate_slot_locked(void)
{
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        si_agent_request_status_t *request = &s_requests[index];
        if (request->state == SI_AGENT_REQUEST_EMPTY) {
            return request;
        }
    }
    /*
     * Never silently evict a terminal record: doing so would lose the local
     * idempotency tombstone and could permit a replayed effect. The Task
     * Service may compact only after it durably records the owning Task as
     * terminal, using si_agent_request_broker_retire_run().
     */
    return NULL;
}

static bool resource_auto_approved(const si_agent_request_v2_input_t *input)
{
    if (!input || input->risk != SI_AGENT_REQUEST_RISK_LOW ||
        input->lease_required) {
        return false;
    }
    static const char *const resources[] = {
        "capability.catalog", "device.status", "observe_status",
        "observe_hid_status", "observe_video_status", "uart_status",
        "uart_read", "check_service", "verify_port", "memory_search",
        "history_search", "wait", "web_search", "host_display.observe",
        "host_display.status", "host_display.plan",
        "boot_key_sequence.status", "boot_key_sequence.plan",
    };
    for (size_t index = 0; index < sizeof(resources) / sizeof(resources[0]);
         ++index) {
        if (strcmp(input->resource, resources[index]) == 0) {
            return true;
        }
    }
    return false;
}

static bool binding_matches_status(
    const si_agent_request_binding_input_t *binding,
    const si_agent_request_status_t *status,
    const char *normalized_args_hash)
{
    return binding && status && normalized_args_hash &&
           strcmp(binding->device_id, status->device_id) == 0 &&
           strcmp(binding->thread_id, status->thread_id) == 0 &&
           strcmp(binding->turn_id, status->turn_id) == 0 &&
           strcmp(binding->run_id, status->run_id) == 0 &&
           strcmp(binding->step_id, status->step_id) == 0 &&
           binding->plan_version == status->plan_version &&
           strcmp(binding->idempotency_key, status->idempotency_key) == 0 &&
           strcmp(normalized_args_hash, status->normalized_args_hash) == 0;
}

static bool request_exact_match(const si_agent_request_v2_input_t *input,
                                const si_agent_request_status_t *status,
                                const char *normalized_args_hash)
{
    return input && status &&
           binding_matches_status(&input->binding, status,
                                  normalized_args_hash) &&
           input->kind == status->kind && input->risk == status->risk &&
           input->lease_required == status->lease_required &&
           input->reviewer_allowed == status->reviewer_allowed &&
           input->requested_grant_scope == status->requested_grant_scope &&
           strcmp(input->resource, status->resource) == 0 &&
           strcmp(input->reason, status->reason) == 0 &&
           strcmp(input->expected_effect ? input->expected_effect : "",
                  status->expected_effect) == 0 &&
           strcmp(input->verification ? input->verification : "",
                  status->verification) == 0 &&
           actor_equal(&input->requester, &status->requester);
}

static bool grant_scope_matches(const si_agent_request_status_t *grant,
                                const si_agent_request_v2_input_t *input,
                                const char *normalized_args_hash,
                                uint32_t now_ms)
{
    if (!grant || !input || grant->state != SI_AGENT_REQUEST_COMPLETED ||
        grant->grant_scope == SI_AGENT_REQUEST_GRANT_ONCE ||
        deadline_reached(now_ms, grant->expires_at_ms) ||
        grant->kind != input->kind || grant->risk != input->risk ||
        grant->lease_required != input->lease_required ||
        grant->plan_version != input->binding.plan_version ||
        strcmp(grant->device_id, input->binding.device_id) != 0 ||
        strcmp(grant->resource, input->resource) != 0 ||
        strcmp(grant->expected_effect,
               input->expected_effect ? input->expected_effect : "") != 0 ||
        strcmp(grant->verification,
               input->verification ? input->verification : "") != 0 ||
        strcmp(grant->normalized_args_hash, normalized_args_hash) != 0 ||
        !actor_equal(&grant->requester, &input->requester)) {
        return false;
    }
    switch (grant->grant_scope) {
    case SI_AGENT_REQUEST_GRANT_STEP:
        return strcmp(grant->thread_id, input->binding.thread_id) == 0 &&
               strcmp(grant->turn_id, input->binding.turn_id) == 0 &&
               strcmp(grant->run_id, input->binding.run_id) == 0 &&
               strcmp(grant->step_id, input->binding.step_id) == 0;
    case SI_AGENT_REQUEST_GRANT_RUN:
        return strcmp(grant->thread_id, input->binding.thread_id) == 0 &&
               strcmp(grant->turn_id, input->binding.turn_id) == 0 &&
               strcmp(grant->run_id, input->binding.run_id) == 0;
    case SI_AGENT_REQUEST_GRANT_THREAD:
        return grant->kind == SI_AGENT_REQUEST_KIND_CONTEXT &&
               grant->risk == SI_AGENT_REQUEST_RISK_LOW &&
               strcmp(grant->thread_id, input->binding.thread_id) == 0;
    case SI_AGENT_REQUEST_GRANT_ONCE:
    default:
        return false;
    }
}

static bool v2_input_valid(const si_agent_request_v2_input_t *input)
{
    if (!input || !kind_valid(input->kind) || !risk_valid(input->risk) ||
        (input->delegated_auto_approval &&
         !risk_valid(input->auto_approve_through)) ||
        !scope_valid(input->requested_grant_scope) ||
        input->requester.principal != SI_AGENT_REQUEST_PRINCIPAL_AGENT ||
        !actor_valid(&input->requester, true) ||
        !value_valid(input->binding.device_id,
                     SI_AGENT_REQUEST_DEVICE_ID_MAX_LEN, true) ||
        !value_valid(input->binding.thread_id,
                     SI_AGENT_REQUEST_THREAD_ID_MAX_LEN, true) ||
        !value_valid(input->binding.turn_id,
                     SI_AGENT_REQUEST_TURN_ID_MAX_LEN, true) ||
        !value_valid(input->binding.run_id,
                     SI_AGENT_REQUEST_RUN_ID_MAX_LEN, true) ||
        !value_valid(input->binding.step_id,
                     SI_AGENT_REQUEST_STEP_ID_MAX_LEN, true) ||
        input->binding.plan_version == 0U ||
        !value_valid(input->binding.normalized_args_json,
                     SI_AGENT_REQUEST_PARAMS_MAX_LEN, true) ||
        !value_valid(input->binding.idempotency_key,
                     SI_AGENT_REQUEST_IDEMPOTENCY_KEY_MAX_LEN, true) ||
        !value_valid(input->resource,
                     SI_AGENT_REQUEST_RESOURCE_MAX_LEN, true) ||
        !value_valid(input->reason,
                     SI_AGENT_REQUEST_REASON_MAX_LEN, true) ||
        !value_valid(input->expected_effect,
                     SI_AGENT_REQUEST_EFFECT_MAX_LEN,
                     input->kind == SI_AGENT_REQUEST_KIND_ACTION) ||
        !value_valid(input->verification,
                     SI_AGENT_REQUEST_VERIFY_MAX_LEN,
                     input->kind == SI_AGENT_REQUEST_KIND_ACTION)) {
        return false;
    }
    return input->requested_grant_scope != SI_AGENT_REQUEST_GRANT_THREAD ||
           (input->kind == SI_AGENT_REQUEST_KIND_CONTEXT &&
            input->risk == SI_AGENT_REQUEST_RISK_LOW &&
            !input->lease_required);
}

static void set_actor(si_agent_request_actor_t *destination,
                      si_agent_request_principal_t principal,
                      const char *session_id, const char *instance_id)
{
    if (!destination) {
        return;
    }
    memset(destination, 0, sizeof(*destination));
    destination->principal = principal;
    copy_text(destination->session_id, sizeof(destination->session_id),
              session_id);
    copy_text(destination->instance_id, sizeof(destination->instance_id),
              instance_id);
}

esp_err_t si_agent_request_broker_start(void)
{
#ifndef SI_AGENT_REQUEST_BROKER_HOST_TEST
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
#else
    if (s_started) {
        return ESP_OK;
    }
    s_started = true;
#endif
    memset(s_requests, 0, sizeof(s_requests));
    s_next_sequence = 1U;
    return ESP_OK;
}

esp_err_t si_agent_request_broker_set_now_provider(
    si_agent_request_now_fn_t now_fn, void *context)
{
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    s_now_fn = now_fn;
    s_now_context = context;
    broker_unlock();
    return ESP_OK;
}

esp_err_t si_agent_request_broker_create_v2(
    const si_agent_request_v2_input_t *input,
    si_agent_request_status_t *out)
{
    if (!v2_input_valid(input)) {
        return ESP_ERR_INVALID_ARG;
    }
    char args_hash[SI_AGENT_REQUEST_HASH_HEX_LEN + 1] = {0};
    esp_err_t ret = sha256_hex(input->binding.normalized_args_json, args_hash);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t now_ms = broker_now_ms();
    expire_all_locked(now_ms);

    si_agent_request_status_t *duplicate = find_idempotency_locked(
        input->binding.idempotency_key);
    if (duplicate) {
        ret = request_exact_match(input, duplicate, args_hash) ?
              ESP_OK : ESP_ERR_INVALID_STATE;
        if (out) {
            *out = *duplicate;
        }
        broker_unlock();
        return ret;
    }

    si_agent_request_status_t reusable_copy = {0};
    bool has_reusable_grant = false;
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        if (grant_scope_matches(&s_requests[index], input, args_hash, now_ms)) {
            reusable_copy = s_requests[index];
            has_reusable_grant = true;
            break;
        }
    }

    si_agent_request_status_t *request = allocate_slot_locked();
    if (!request) {
        broker_unlock();
        return ESP_ERR_NO_MEM;
    }
    memset(request, 0, sizeof(*request));
    request->state = SI_AGENT_REQUEST_REQUESTED;
    request->sequence = s_next_sequence++;
    request->created_ms = now_ms;
    request->expires_at_ms = now_ms + clamp_ttl(input->ttl_ms);
    request->kind = input->kind;
    request->risk = input->risk;
    request->lease_required = input->lease_required;
    request->reviewer_allowed = input->reviewer_allowed;
    request->requested_grant_scope = input->requested_grant_scope;
    request->grant_scope = SI_AGENT_REQUEST_GRANT_ONCE;
    request->plan_version = input->binding.plan_version;
    request->requester = input->requester;
    snprintf(request->request_id, sizeof(request->request_id),
             "req-%08" PRIx32 "-%08" PRIx32,
             broker_random(), (uint32_t)request->sequence);
    copy_text(request->device_id, sizeof(request->device_id),
              input->binding.device_id);
    copy_text(request->thread_id, sizeof(request->thread_id),
              input->binding.thread_id);
    copy_text(request->turn_id, sizeof(request->turn_id),
              input->binding.turn_id);
    copy_text(request->run_id, sizeof(request->run_id),
              input->binding.run_id);
    copy_text(request->step_id, sizeof(request->step_id),
              input->binding.step_id);
    copy_text(request->resource, sizeof(request->resource), input->resource);
    copy_text(request->reason, sizeof(request->reason), input->reason);
    copy_text(request->params_json, sizeof(request->params_json),
              input->binding.normalized_args_json);
    copy_text(request->params_hash, sizeof(request->params_hash), args_hash);
    copy_text(request->normalized_args_hash,
              sizeof(request->normalized_args_hash), args_hash);
    copy_text(request->idempotency_key, sizeof(request->idempotency_key),
              input->binding.idempotency_key);
    copy_text(request->expected_effect, sizeof(request->expected_effect),
              input->expected_effect);
    copy_text(request->verification, sizeof(request->verification),
              input->verification);

    (void)set_state_locked(request, SI_AGENT_REQUEST_EVALUATING, now_ms);
    if (has_reusable_grant) {
        request->grant_scope =
            reusable_copy.grant_scope < input->requested_grant_scope ?
            reusable_copy.grant_scope : input->requested_grant_scope;
        if (remaining_ttl(now_ms, reusable_copy.expires_at_ms) <
            remaining_ttl(now_ms, request->expires_at_ms)) {
            request->expires_at_ms = reusable_copy.expires_at_ms;
        }
        copy_text(request->parent_grant_request_id,
                  sizeof(request->parent_grant_request_id),
                  reusable_copy.request_id);
        copy_text(request->decision_source,
                  sizeof(request->decision_source), "grant_reuse");
        request->decider = reusable_copy.decider;
        request->reviewer = reusable_copy.reviewer;
        copy_text(request->decided_by_session,
                  sizeof(request->decided_by_session),
                  reusable_copy.decided_by_session);
        (void)set_state_locked(request, SI_AGENT_REQUEST_GRANTED, now_ms);
    } else if (input->delegated_auto_approval &&
               input->risk <= input->auto_approve_through) {
        request->grant_scope = SI_AGENT_REQUEST_GRANT_ONCE;
        set_actor(&request->decider, SI_AGENT_REQUEST_PRINCIPAL_POLICY,
                  "access-mode", "delegated-policy-v1");
        copy_text(request->decision_source,
                  sizeof(request->decision_source), "delegated_policy");
        copy_text(request->decided_by_session,
                  sizeof(request->decided_by_session), "access-mode");
        (void)set_state_locked(request, SI_AGENT_REQUEST_GRANTED, now_ms);
    } else if (resource_auto_approved(input)) {
        request->grant_scope = SI_AGENT_REQUEST_GRANT_ONCE;
        set_actor(&request->decider, SI_AGENT_REQUEST_PRINCIPAL_POLICY,
                  "broker", "deterministic-policy-v1");
        copy_text(request->decision_source,
                  sizeof(request->decision_source), "policy");
        copy_text(request->decided_by_session,
                  sizeof(request->decided_by_session), "broker");
        (void)set_state_locked(request, SI_AGENT_REQUEST_GRANTED, now_ms);
    } else if (input->reviewer_allowed &&
               input->risk <= SI_AGENT_REQUEST_RISK_MEDIUM) {
        (void)set_state_locked(request, SI_AGENT_REQUEST_REVIEWING, now_ms);
    } else {
        (void)set_state_locked(request, SI_AGENT_REQUEST_WAITING_USER, now_ms);
    }
    if (out) {
        *out = *request;
    }
    broker_unlock();
    return ESP_OK;
}

esp_err_t si_agent_request_broker_get_by_id(
    const char *request_id, si_agent_request_status_t *out)
{
    if (!value_valid(request_id, SI_AGENT_REQUEST_ID_MAX_LEN, true) || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    expire_all_locked(broker_now_ms());
    si_agent_request_status_t *request = find_by_id_locked(request_id);
    if (request) {
        *out = *request;
        ret = ESP_OK;
    } else {
        memset(out, 0, sizeof(*out));
        ret = ESP_ERR_NOT_FOUND;
    }
    broker_unlock();
    return ret;
}

esp_err_t si_agent_request_broker_enumerate(
    const char *run_id, si_agent_request_status_t *out,
    size_t capacity, size_t *out_count)
{
    if (!out_count ||
        (run_id && !value_valid(run_id, SI_AGENT_REQUEST_RUN_ID_MAX_LEN, false)) ||
        (capacity > 0U && !out)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    expire_all_locked(broker_now_ms());
    size_t indices[SI_AGENT_REQUEST_QUEUE_CAPACITY] = {0};
    size_t count = 0U;
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        if (s_requests[index].state == SI_AGENT_REQUEST_EMPTY ||
            (run_id && run_id[0] &&
             strcmp(run_id, s_requests[index].run_id) != 0)) {
            continue;
        }
        size_t insert_at = count;
        while (insert_at > 0U &&
               s_requests[indices[insert_at - 1U]].sequence >
               s_requests[index].sequence) {
            indices[insert_at] = indices[insert_at - 1U];
            --insert_at;
        }
        indices[insert_at] = index;
        ++count;
    }
    size_t copy_count = count < capacity ? count : capacity;
    for (size_t index = 0; index < copy_count; ++index) {
        out[index] = s_requests[indices[index]];
    }
    *out_count = count;
    broker_unlock();
    return count > capacity ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t si_agent_request_broker_decide_v2(
    const si_agent_request_decision_t *decision,
    si_agent_request_status_t *out)
{
    if (!decision ||
        !value_valid(decision->request_id,
                     SI_AGENT_REQUEST_ID_MAX_LEN, true) ||
        !value_valid(decision->response,
                     SI_AGENT_REQUEST_RESPONSE_MAX_LEN, false) ||
        !actor_valid(&decision->decider, true) ||
        !scope_valid(decision->grant_scope)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t now_ms = broker_now_ms();
    expire_all_locked(now_ms);
    si_agent_request_status_t *request = find_by_id_locked(decision->request_id);
    if (!request) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (actor_equal(&decision->decider, &request->requester)) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (request->state != SI_AGENT_REQUEST_WAITING_USER &&
               request->state != SI_AGENT_REQUEST_REVIEWING) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        bool browser_decision =
            decision->decider.principal == SI_AGENT_REQUEST_PRINCIPAL_BROWSER;
        bool reviewer_decision =
            decision->decider.principal == SI_AGENT_REQUEST_PRINCIPAL_REVIEWER &&
            request->reviewer_allowed &&
            request->risk <= SI_AGENT_REQUEST_RISK_MEDIUM;
        if (!browser_decision && !reviewer_decision) {
            ret = ESP_ERR_INVALID_STATE;
        } else if (decision->approved &&
                   (decision->grant_scope > request->requested_grant_scope ||
                    (decision->grant_scope == SI_AGENT_REQUEST_GRANT_THREAD &&
                     (request->kind != SI_AGENT_REQUEST_KIND_CONTEXT ||
                      request->risk != SI_AGENT_REQUEST_RISK_LOW ||
                      request->lease_required)))) {
            ret = ESP_ERR_INVALID_ARG;
        } else {
            request->decider = decision->decider;
            if (reviewer_decision) {
                request->reviewer = decision->decider;
                copy_text(request->decision_source,
                          sizeof(request->decision_source), "reviewer");
            } else {
                copy_text(request->decision_source,
                          sizeof(request->decision_source), "browser");
            }
            copy_text(request->decided_by_session,
                      sizeof(request->decided_by_session),
                      decision->decider.session_id);
            if (decision->approved) {
                request->grant_scope = decision->grant_scope;
                copy_text(request->response, sizeof(request->response),
                          decision->response);
                (void)set_state_locked(request, SI_AGENT_REQUEST_GRANTED, now_ms);
            } else {
                copy_text(request->error, sizeof(request->error),
                          "request denied");
                (void)set_state_locked(request, SI_AGENT_REQUEST_DENIED, now_ms);
            }
            ret = ESP_OK;
        }
    }
    if (out && request) {
        *out = *request;
    }
    broker_unlock();
    return ret;
}

esp_err_t si_agent_request_broker_consume(
    const si_agent_request_consume_t *consume,
    si_agent_request_status_t *out)
{
    if (!consume ||
        !value_valid(consume->request_id,
                     SI_AGENT_REQUEST_ID_MAX_LEN, true) ||
        !value_valid(consume->binding.device_id,
                     SI_AGENT_REQUEST_DEVICE_ID_MAX_LEN, true) ||
        !value_valid(consume->binding.thread_id,
                     SI_AGENT_REQUEST_THREAD_ID_MAX_LEN, true) ||
        !value_valid(consume->binding.turn_id,
                     SI_AGENT_REQUEST_TURN_ID_MAX_LEN, true) ||
        !value_valid(consume->binding.run_id,
                     SI_AGENT_REQUEST_RUN_ID_MAX_LEN, true) ||
        !value_valid(consume->binding.step_id,
                     SI_AGENT_REQUEST_STEP_ID_MAX_LEN, true) ||
        consume->binding.plan_version == 0U ||
        !value_valid(consume->binding.normalized_args_json,
                     SI_AGENT_REQUEST_PARAMS_MAX_LEN, true) ||
        !value_valid(consume->binding.idempotency_key,
                     SI_AGENT_REQUEST_IDEMPOTENCY_KEY_MAX_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    char args_hash[SI_AGENT_REQUEST_HASH_HEX_LEN + 1] = {0};
    esp_err_t ret = sha256_hex(consume->binding.normalized_args_json, args_hash);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t now_ms = broker_now_ms();
    expire_all_locked(now_ms);
    si_agent_request_status_t *request = find_by_id_locked(consume->request_id);
    if (!request) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (request->state != SI_AGENT_REQUEST_GRANTED) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (!binding_matches_status(&consume->binding, request, args_hash)) {
        (void)set_state_locked(request, SI_AGENT_REQUEST_EXPIRED, now_ms);
        copy_text(request->decision_source,
                  sizeof(request->decision_source), "scope_changed");
        copy_text(request->error, sizeof(request->error),
                  "binding, plan, arguments, or idempotency changed");
        ret = ESP_ERR_INVALID_STATE;
    } else if (!set_state_locked(request, SI_AGENT_REQUEST_EXECUTING, now_ms)) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        ret = ESP_OK;
    }
    if (out && request) {
        *out = *request;
    }
    broker_unlock();
    return ret;
}

esp_err_t si_agent_request_broker_mark_verifying(
    const char *request_id, si_agent_request_status_t *out)
{
    if (!value_valid(request_id, SI_AGENT_REQUEST_ID_MAX_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    si_agent_request_status_t *request = find_by_id_locked(request_id);
    if (!request) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (!set_state_locked(request, SI_AGENT_REQUEST_VERIFYING,
                                 broker_now_ms())) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        ret = ESP_OK;
    }
    if (out && request) {
        *out = *request;
    }
    broker_unlock();
    return ret;
}

esp_err_t si_agent_request_broker_complete_verified(
    const char *request_id, const char *verification_evidence,
    si_agent_request_status_t *out)
{
    if (!value_valid(request_id, SI_AGENT_REQUEST_ID_MAX_LEN, true) ||
        !value_valid(verification_evidence,
                     SI_AGENT_REQUEST_RESPONSE_MAX_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    si_agent_request_status_t *request = find_by_id_locked(request_id);
    if (!request) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (!set_state_locked(request, SI_AGENT_REQUEST_COMPLETED,
                                 broker_now_ms())) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        copy_text(request->verification_evidence,
                  sizeof(request->verification_evidence),
                  verification_evidence);
        request->error[0] = '\0';
        ret = ESP_OK;
    }
    if (out && request) {
        *out = *request;
    }
    broker_unlock();
    return ret;
}

esp_err_t si_agent_request_broker_mark_unknown(
    const char *request_id, const char *error,
    si_agent_request_status_t *out)
{
    if (!value_valid(request_id, SI_AGENT_REQUEST_ID_MAX_LEN, true) ||
        !value_valid(error, SI_AGENT_REQUEST_ERROR_MAX_LEN, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    si_agent_request_status_t *request = find_by_id_locked(request_id);
    if (!request) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (!set_state_locked(request, SI_AGENT_REQUEST_UNKNOWN,
                                 broker_now_ms())) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        copy_text(request->error, sizeof(request->error),
                  error && error[0] ? error : "execution outcome unknown");
        ret = ESP_OK;
    }
    if (out && request) {
        *out = *request;
    }
    broker_unlock();
    return ret;
}

static bool snapshot_status_valid(const si_agent_request_status_t *status)
{
    if (!status || !state_valid(status->state) || !kind_valid(status->kind) ||
        !risk_valid(status->risk) ||
        !scope_valid(status->requested_grant_scope) ||
        !scope_valid(status->grant_scope) || status->plan_version == 0U ||
        status->sequence == 0U ||
        status->requester.principal != SI_AGENT_REQUEST_PRINCIPAL_AGENT ||
        !actor_valid(&status->requester, true) ||
        !fixed_value_valid(status->request_id, sizeof(status->request_id), true) ||
        !fixed_value_valid(status->parent_grant_request_id,
                           sizeof(status->parent_grant_request_id), false) ||
        !fixed_value_valid(status->device_id, sizeof(status->device_id), true) ||
        !fixed_value_valid(status->thread_id, sizeof(status->thread_id), true) ||
        !fixed_value_valid(status->turn_id, sizeof(status->turn_id), true) ||
        !fixed_value_valid(status->run_id, sizeof(status->run_id), true) ||
        !fixed_value_valid(status->step_id, sizeof(status->step_id), true) ||
        !fixed_value_valid(status->resource, sizeof(status->resource), true) ||
        !fixed_value_valid(status->reason, sizeof(status->reason), true) ||
        !fixed_value_valid(status->params_json, sizeof(status->params_json), true) ||
        !fixed_value_valid(status->normalized_args_hash,
                           sizeof(status->normalized_args_hash), true) ||
        !fixed_value_valid(status->params_hash,
                           sizeof(status->params_hash), true) ||
        !fixed_value_valid(status->idempotency_key,
                           sizeof(status->idempotency_key), true) ||
        !fixed_value_valid(status->expected_effect,
                           sizeof(status->expected_effect), false) ||
        !fixed_value_valid(status->verification,
                           sizeof(status->verification), false) ||
        !fixed_value_valid(status->decision_source,
                           sizeof(status->decision_source), false) ||
        !fixed_value_valid(status->decided_by_session,
                           sizeof(status->decided_by_session), false) ||
        !fixed_value_valid(status->response, sizeof(status->response), false) ||
        !fixed_value_valid(status->verification_evidence,
                           sizeof(status->verification_evidence), false) ||
        !fixed_value_valid(status->error, sizeof(status->error), false) ||
        (status->reviewer.principal != SI_AGENT_REQUEST_PRINCIPAL_UNSPECIFIED &&
         (status->reviewer.principal != SI_AGENT_REQUEST_PRINCIPAL_REVIEWER ||
          !actor_valid(&status->reviewer, true))) ||
        (status->decider.principal != SI_AGENT_REQUEST_PRINCIPAL_UNSPECIFIED &&
         !actor_valid(&status->decider, true)) ||
        (status->grant_scope == SI_AGENT_REQUEST_GRANT_THREAD &&
         (status->kind != SI_AGENT_REQUEST_KIND_CONTEXT ||
          status->risk != SI_AGENT_REQUEST_RISK_LOW ||
          status->lease_required))) {
        return false;
    }
    bool effect_started =
        status->state == SI_AGENT_REQUEST_EXECUTING ||
        status->state == SI_AGENT_REQUEST_VERIFYING ||
        status->state == SI_AGENT_REQUEST_COMPLETED ||
        status->state == SI_AGENT_REQUEST_FAILED ||
        status->state == SI_AGENT_REQUEST_UNKNOWN;
    if (status->grant_consumed != effect_started) {
        return false;
    }
    char hash[SI_AGENT_REQUEST_HASH_HEX_LEN + 1] = {0};
    return sha256_hex(status->params_json, hash) == ESP_OK &&
           strcmp(hash, status->normalized_args_hash) == 0 &&
           strcmp(hash, status->params_hash) == 0;
}

esp_err_t si_agent_request_broker_export_snapshot(
    si_agent_request_snapshot_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t now_ms = broker_now_ms();
    expire_all_locked(now_ms);
    memset(out, 0, sizeof(*out));
    out->schema_version = SI_AGENT_REQUEST_SNAPSHOT_SCHEMA_VERSION;
    out->next_sequence = s_next_sequence;
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        if (s_requests[index].state == SI_AGENT_REQUEST_EMPTY) {
            continue;
        }
        si_agent_request_snapshot_entry_t *entry = &out->entries[out->count++];
        entry->status = s_requests[index];
        entry->remaining_ttl_ms = remaining_ttl(
            now_ms, s_requests[index].expires_at_ms);
    }
    broker_unlock();
    return ESP_OK;
}

esp_err_t si_agent_request_broker_import_snapshot(
    const si_agent_request_snapshot_t *snapshot)
{
    if (!snapshot ||
        snapshot->schema_version != SI_AGENT_REQUEST_SNAPSHOT_SCHEMA_VERSION ||
        snapshot->count > SI_AGENT_REQUEST_QUEUE_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t max_sequence = 0U;
    for (size_t index = 0; index < snapshot->count; ++index) {
        const si_agent_request_status_t *status =
            &snapshot->entries[index].status;
        if (!snapshot_status_valid(status)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (status->sequence > max_sequence) {
            max_sequence = status->sequence;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const si_agent_request_status_t *other =
                &snapshot->entries[previous].status;
            if (strcmp(status->request_id, other->request_id) == 0 ||
                strcmp(status->idempotency_key, other->idempotency_key) == 0 ||
                status->sequence == other->sequence) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t now_ms = broker_now_ms();
    memset(s_requests, 0, sizeof(s_requests));
    for (size_t index = 0; index < snapshot->count; ++index) {
        s_requests[index] = snapshot->entries[index].status;
        si_agent_request_status_t *status = &s_requests[index];
        uint32_t remaining = snapshot->entries[index].remaining_ttl_ms;
        if (status->state == SI_AGENT_REQUEST_EXECUTING ||
            status->state == SI_AGENT_REQUEST_VERIFYING) {
            status->state = SI_AGENT_REQUEST_UNKNOWN;
            status->completed_ms = now_ms;
            copy_text(status->decision_source,
                      sizeof(status->decision_source), "restore");
            copy_text(status->error, sizeof(status->error),
                      "device restarted after effect began; outcome unknown");
        } else if (status->state == SI_AGENT_REQUEST_GRANTED) {
            /* Monotonic TTL cannot account for powered-off time. Fail closed. */
            status->state = SI_AGENT_REQUEST_EXPIRED;
            status->decided_ms = now_ms;
            copy_text(status->decision_source,
                      sizeof(status->decision_source), "restore");
            copy_text(status->error, sizeof(status->error),
                      "grant invalidated by restart");
        } else if (state_expires(status->state)) {
            status->expires_at_ms = now_ms + remaining;
            if (remaining == 0U) {
                status->state = SI_AGENT_REQUEST_EXPIRED;
                status->decided_ms = now_ms;
                copy_text(status->decision_source,
                          sizeof(status->decision_source), "restore");
                copy_text(status->error, sizeof(status->error),
                          "request expired before restore");
            }
        } else {
            /* Completed grants are audit records, not reboot-spanning grants. */
            status->expires_at_ms = now_ms;
        }
    }
    uint64_t candidate = snapshot->next_sequence;
    if (candidate <= max_sequence) {
        candidate = max_sequence + 1U;
    }
    s_next_sequence = candidate > 0U ? candidate : 1U;
    broker_unlock();
    return ESP_OK;
}

esp_err_t si_agent_request_broker_retire_run(const char *run_id)
{
    if (!value_valid(run_id, SI_AGENT_REQUEST_RUN_ID_MAX_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    expire_all_locked(broker_now_ms());
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        const si_agent_request_status_t *request = &s_requests[index];
        if (request->state != SI_AGENT_REQUEST_EMPTY &&
            strcmp(request->run_id, run_id) == 0 &&
            !state_terminal(request->state)) {
            broker_unlock();
            return ESP_ERR_INVALID_STATE;
        }
    }
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        si_agent_request_status_t *request = &s_requests[index];
        if (request->state != SI_AGENT_REQUEST_EMPTY &&
            strcmp(request->run_id, run_id) == 0) {
            memset(request, 0, sizeof(*request));
        }
    }
    broker_unlock();
    return ESP_OK;
}

esp_err_t si_agent_request_broker_terminalize_and_retire_run(
    const char *run_id)
{
    if (!value_valid(run_id, SI_AGENT_REQUEST_RUN_ID_MAX_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t now_ms = broker_now_ms();

    /* Validate the complete set before changing any record. */
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        const si_agent_request_status_t *request = &s_requests[index];
        if (request->state == SI_AGENT_REQUEST_EMPTY ||
            strcmp(request->run_id, run_id) != 0 ||
            state_terminal(request->state)) {
            continue;
        }
        if (!state_expires(request->state) &&
            request->state != SI_AGENT_REQUEST_EXECUTING &&
            request->state != SI_AGENT_REQUEST_VERIFYING) {
            broker_unlock();
            return ESP_ERR_INVALID_STATE;
        }
    }

    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        si_agent_request_status_t *request = &s_requests[index];
        if (request->state == SI_AGENT_REQUEST_EMPTY ||
            strcmp(request->run_id, run_id) != 0 ||
            state_terminal(request->state)) {
            continue;
        }
        if (request->state == SI_AGENT_REQUEST_EXECUTING ||
            request->state == SI_AGENT_REQUEST_VERIFYING) {
            (void)set_state_locked(request, SI_AGENT_REQUEST_UNKNOWN, now_ms);
            copy_text(request->error, sizeof(request->error),
                      "run retired after effect began; outcome unknown");
        } else {
            (void)set_state_locked(request, SI_AGENT_REQUEST_CANCELLED,
                                   now_ms);
            copy_text(request->error, sizeof(request->error),
                      "run retired before effect began");
        }
        copy_text(request->decision_source,
                  sizeof(request->decision_source), "runtime");
    }
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        si_agent_request_status_t *request = &s_requests[index];
        if (request->state != SI_AGENT_REQUEST_EMPTY &&
            strcmp(request->run_id, run_id) == 0) {
            memset(request, 0, sizeof(*request));
        }
    }
    broker_unlock();
    return ESP_OK;
}

esp_err_t si_agent_request_broker_create(
    const si_agent_request_input_t *input, si_agent_request_status_t *out)
{
    if (!input ||
        !value_valid(input->run_id, SI_AGENT_REQUEST_RUN_ID_MAX_LEN, true) ||
        !value_valid(input->resource,
                     SI_AGENT_REQUEST_RESOURCE_MAX_LEN, true) ||
        !value_valid(input->reason,
                     SI_AGENT_REQUEST_REASON_MAX_LEN, true) ||
        !value_valid(input->params_json,
                     SI_AGENT_REQUEST_PARAMS_MAX_LEN, true) ||
        !value_valid(input->expected_effect,
                     SI_AGENT_REQUEST_EFFECT_MAX_LEN, false) ||
        !value_valid(input->verification,
                     SI_AGENT_REQUEST_VERIFY_MAX_LEN, false) ||
        !kind_valid(input->kind) || !risk_valid(input->risk)) {
        return ESP_ERR_INVALID_ARG;
    }
    char idempotency_key[SI_AGENT_REQUEST_IDEMPOTENCY_KEY_MAX_LEN + 1] = {0};
    snprintf(idempotency_key, sizeof(idempotency_key),
             "legacy-%08" PRIx32 "-%08" PRIx32,
             broker_random(), broker_now_ms());
    si_agent_request_v2_input_t v2 = {
        .binding = {
            .device_id = "legacy-device",
            .thread_id = input->run_id,
            .turn_id = input->run_id,
            .run_id = input->run_id,
            .step_id = "legacy-step",
            .plan_version = 1U,
            .normalized_args_json = input->params_json,
            .idempotency_key = idempotency_key,
        },
        .kind = input->kind,
        .resource = input->resource,
        .reason = input->reason,
        .expected_effect = input->expected_effect,
        .verification = input->verification,
        .risk = input->risk,
        .lease_required = input->lease_required,
        .reviewer_allowed = false,
        .requested_grant_scope = SI_AGENT_REQUEST_GRANT_ONCE,
        .ttl_ms = input->ttl_ms,
    };
    set_actor(&v2.requester, SI_AGENT_REQUEST_PRINCIPAL_AGENT,
              input->run_id, "legacy-embedded-agent");
    return si_agent_request_broker_create_v2(&v2, out);
}

esp_err_t si_agent_request_broker_get(
    const char *run_id, si_agent_request_status_t *out)
{
    if (!out ||
        (run_id && !value_valid(run_id, SI_AGENT_REQUEST_RUN_ID_MAX_LEN, false))) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    expire_all_locked(broker_now_ms());
    si_agent_request_status_t *newest = NULL;
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        si_agent_request_status_t *candidate = &s_requests[index];
        if (candidate->state == SI_AGENT_REQUEST_EMPTY ||
            (run_id && run_id[0] && strcmp(run_id, candidate->run_id) != 0)) {
            continue;
        }
        if (!newest || candidate->sequence > newest->sequence) {
            newest = candidate;
        }
    }
    if (newest) {
        *out = *newest;
        ret = ESP_OK;
    } else {
        memset(out, 0, sizeof(*out));
        ret = run_id && run_id[0] ? ESP_ERR_NOT_FOUND : ESP_OK;
    }
    broker_unlock();
    return ret;
}

esp_err_t si_agent_request_broker_decide(
    const char *request_id, bool approved, const char *response,
    const char *session_id, si_agent_request_status_t *out)
{
    if (!value_valid(session_id, SI_AGENT_REQUEST_SESSION_ID_MAX_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    si_agent_request_decision_t decision = {
        .request_id = request_id,
        .approved = approved,
        .response = response,
        .grant_scope = SI_AGENT_REQUEST_GRANT_ONCE,
    };
    set_actor(&decision.decider, SI_AGENT_REQUEST_PRINCIPAL_BROWSER,
              session_id, "legacy-browser");
    return si_agent_request_broker_decide_v2(&decision, out);
}

esp_err_t si_agent_request_broker_cancel(
    const char *request_id, const char *session_id,
    si_agent_request_status_t *out)
{
    if (!value_valid(request_id, SI_AGENT_REQUEST_ID_MAX_LEN, true) ||
        !value_valid(session_id, SI_AGENT_REQUEST_SESSION_ID_MAX_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t now_ms = broker_now_ms();
    expire_all_locked(now_ms);
    si_agent_request_status_t *request = find_by_id_locked(request_id);
    if (!request) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (request->state == SI_AGENT_REQUEST_EXECUTING ||
               request->state == SI_AGENT_REQUEST_VERIFYING) {
        (void)set_state_locked(request, SI_AGENT_REQUEST_UNKNOWN, now_ms);
        copy_text(request->decision_source,
                  sizeof(request->decision_source), "browser");
        copy_text(request->decided_by_session,
                  sizeof(request->decided_by_session), session_id);
        copy_text(request->error, sizeof(request->error),
                  "cancelled after execution began; outcome unknown");
        ret = ESP_OK;
    } else if (request->state == SI_AGENT_REQUEST_REQUESTED ||
               request->state == SI_AGENT_REQUEST_EVALUATING ||
               request->state == SI_AGENT_REQUEST_REVIEWING ||
               request->state == SI_AGENT_REQUEST_WAITING_USER ||
               request->state == SI_AGENT_REQUEST_GRANTED) {
        (void)set_state_locked(request, SI_AGENT_REQUEST_CANCELLED, now_ms);
        copy_text(request->decision_source,
                  sizeof(request->decision_source), "browser");
        copy_text(request->decided_by_session,
                  sizeof(request->decided_by_session), session_id);
        copy_text(request->error, sizeof(request->error), "request cancelled");
        ret = ESP_OK;
    } else {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (out && request) {
        *out = *request;
    }
    broker_unlock();
    return ret;
}

esp_err_t si_agent_request_broker_finish(
    const char *request_id, bool ok, const char *error,
    si_agent_request_status_t *out)
{
    if (!value_valid(request_id, SI_AGENT_REQUEST_ID_MAX_LEN, true) ||
        !value_valid(error, SI_AGENT_REQUEST_ERROR_MAX_LEN, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_agent_request_broker_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = broker_lock(300U);
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t now_ms = broker_now_ms();
    expire_all_locked(now_ms);
    si_agent_request_status_t *request = find_by_id_locked(request_id);
    if (!request) {
        ret = ESP_ERR_NOT_FOUND;
    } else {
        /* Compatibility only: old callers did not have an explicit consume. */
        if (request->state == SI_AGENT_REQUEST_GRANTED) {
            (void)set_state_locked(request, SI_AGENT_REQUEST_EXECUTING, now_ms);
        }
        if (request->state != SI_AGENT_REQUEST_EXECUTING) {
            ret = ESP_ERR_INVALID_STATE;
        } else if (ok) {
            (void)set_state_locked(request, SI_AGENT_REQUEST_VERIFYING, now_ms);
            ret = ESP_OK;
        } else {
            (void)set_state_locked(request, SI_AGENT_REQUEST_FAILED, now_ms);
            copy_text(request->error, sizeof(request->error),
                      error && error[0] ? error : "request execution failed");
            ret = ESP_OK;
        }
    }
    if (out && request) {
        *out = *request;
    }
    broker_unlock();
    return ret;
}

void si_agent_request_broker_cancel_run(const char *run_id)
{
    if (!run_id || !run_id[0] ||
        si_agent_request_broker_start() != ESP_OK ||
        broker_lock(100U) != ESP_OK) {
        return;
    }
    uint32_t now_ms = broker_now_ms();
    expire_all_locked(now_ms);
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        si_agent_request_status_t *request = &s_requests[index];
        if (strcmp(run_id, request->run_id) != 0) {
            continue;
        }
        if (request->state == SI_AGENT_REQUEST_EXECUTING ||
            request->state == SI_AGENT_REQUEST_VERIFYING) {
            (void)set_state_locked(request, SI_AGENT_REQUEST_UNKNOWN, now_ms);
            copy_text(request->error, sizeof(request->error),
                      "run cancelled after execution began; outcome unknown");
        } else if (request->state == SI_AGENT_REQUEST_REQUESTED ||
                   request->state == SI_AGENT_REQUEST_EVALUATING ||
                   request->state == SI_AGENT_REQUEST_REVIEWING ||
                   request->state == SI_AGENT_REQUEST_WAITING_USER ||
                   request->state == SI_AGENT_REQUEST_GRANTED) {
            (void)set_state_locked(request, SI_AGENT_REQUEST_CANCELLED, now_ms);
            copy_text(request->error, sizeof(request->error), "run cancelled");
        } else {
            continue;
        }
        copy_text(request->decision_source,
                  sizeof(request->decision_source), "runtime");
    }
    broker_unlock();
}

const char *si_agent_request_kind_name(si_agent_request_kind_t kind)
{
    return kind == SI_AGENT_REQUEST_KIND_ACTION ? "action" : "context";
}

const char *si_agent_request_risk_name(si_agent_request_risk_t risk)
{
    switch (risk) {
    case SI_AGENT_REQUEST_RISK_MEDIUM:
        return "medium";
    case SI_AGENT_REQUEST_RISK_HIGH:
        return "high";
    case SI_AGENT_REQUEST_RISK_CRITICAL:
        return "critical";
    case SI_AGENT_REQUEST_RISK_LOW:
    default:
        return "low";
    }
}

const char *si_agent_request_state_name(si_agent_request_state_t state)
{
    switch (state) {
    case SI_AGENT_REQUEST_REQUESTED:
        return "requested";
    case SI_AGENT_REQUEST_EVALUATING:
        return "evaluating";
    case SI_AGENT_REQUEST_REVIEWING:
        return "reviewing";
    case SI_AGENT_REQUEST_WAITING_USER:
        return "waiting_user";
    case SI_AGENT_REQUEST_GRANTED:
        return "granted";
    case SI_AGENT_REQUEST_EXECUTING:
        return "executing";
    case SI_AGENT_REQUEST_VERIFYING:
        return "verifying";
    case SI_AGENT_REQUEST_COMPLETED:
        return "completed";
    case SI_AGENT_REQUEST_FAILED:
        return "failed";
    case SI_AGENT_REQUEST_DENIED:
        return "denied";
    case SI_AGENT_REQUEST_EXPIRED:
        return "expired";
    case SI_AGENT_REQUEST_CANCELLED:
        return "cancelled";
    case SI_AGENT_REQUEST_UNKNOWN:
        return "unknown";
    case SI_AGENT_REQUEST_EMPTY:
    default:
        return "empty";
    }
}

const char *si_agent_request_grant_scope_name(
    si_agent_request_grant_scope_t scope)
{
    switch (scope) {
    case SI_AGENT_REQUEST_GRANT_STEP:
        return "step";
    case SI_AGENT_REQUEST_GRANT_RUN:
        return "run";
    case SI_AGENT_REQUEST_GRANT_THREAD:
        return "thread";
    case SI_AGENT_REQUEST_GRANT_ONCE:
    default:
        return "once";
    }
}

const char *si_agent_request_principal_name(
    si_agent_request_principal_t principal)
{
    switch (principal) {
    case SI_AGENT_REQUEST_PRINCIPAL_AGENT:
        return "agent";
    case SI_AGENT_REQUEST_PRINCIPAL_BROWSER:
        return "browser";
    case SI_AGENT_REQUEST_PRINCIPAL_REVIEWER:
        return "reviewer";
    case SI_AGENT_REQUEST_PRINCIPAL_POLICY:
        return "policy";
    case SI_AGENT_REQUEST_PRINCIPAL_RUNTIME:
        return "runtime";
    case SI_AGENT_REQUEST_PRINCIPAL_UNSPECIFIED:
    default:
        return "unspecified";
    }
}
