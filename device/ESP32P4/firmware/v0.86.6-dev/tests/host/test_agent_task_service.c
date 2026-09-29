#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SI_AGENT_TASK_SERVICE_HOST_TEST 1
#include "../../main/application/agent_task_service.c"

#define FAKE_EVENT_CAPACITY 512U

typedef struct {
    si_agent_event_record_t record;
    cJSON *owned_payload;
} fake_event_t;

static fake_event_t s_fake_events[FAKE_EVENT_CAPACITY];
static size_t s_fake_event_count;
static bool s_fake_fail_next_append;
static bool s_fake_fail_init;
static unsigned s_fake_init_calls;
static uint32_t s_fake_now_ms = 1000U;
static char s_fake_store_error[96];

uint32_t si_monotonic_ms(void)
{
    return ++s_fake_now_ms;
}

bool si_agent_task_service_host_target_identity(char *target_out,
                                                size_t target_out_size)
{
    if (!target_out || target_out_size <= strlen("device-host")) {
        return false;
    }
    snprintf(target_out, target_out_size, "device-host");
    return true;
}

static void fake_copy(char *out, size_t capacity, const char *value)
{
    assert(out != NULL);
    assert(capacity > 0U);
    snprintf(out, capacity, "%s", value ? value : "");
}

static void fake_recovery(si_agent_event_recovery_t *recovery)
{
    if (!recovery) {
        return;
    }
    memset(recovery, 0, sizeof(*recovery));
    recovery->status = s_fake_event_count == 0U ?
        SI_AGENT_EVENT_RECOVERY_EMPTY : SI_AGENT_EVENT_RECOVERY_CLEAN;
    recovery->latest_seq = s_fake_event_count;
    recovery->valid_record_count = s_fake_event_count;
    memset(recovery->latest_hash, 'a', SI_AGENT_EVENT_HASH_HEX_LEN);
    recovery->latest_hash[SI_AGENT_EVENT_HASH_HEX_LEN] = '\0';
}

esp_err_t si_agent_event_store_init(si_agent_event_recovery_t *recovery_out)
{
    s_fake_init_calls++;
    if (s_fake_fail_init) {
        if (recovery_out) {
            memset(recovery_out, 0, sizeof(*recovery_out));
            recovery_out->status = SI_AGENT_EVENT_RECOVERY_IO_ERROR;
        }
        snprintf(s_fake_store_error, sizeof(s_fake_store_error),
                 "injected init failure");
        return ESP_FAIL;
    }
    fake_recovery(recovery_out);
    return ESP_OK;
}

esp_err_t si_agent_event_store_recover(si_agent_event_replay_cb_t callback,
                                       void *user_ctx,
                                       si_agent_event_recovery_t *recovery_out)
{
    for (size_t index = 0; index < s_fake_event_count; ++index) {
        if (callback) {
            esp_err_t result = callback(&s_fake_events[index].record,
                                        user_ctx);
            if (result != ESP_OK) {
                return result;
            }
        }
    }
    fake_recovery(recovery_out);
    return ESP_OK;
}

esp_err_t si_agent_event_store_append(const si_agent_event_append_t *event,
                                      si_agent_event_record_t *stored_out)
{
    if (s_fake_fail_next_append) {
        s_fake_fail_next_append = false;
        snprintf(s_fake_store_error, sizeof(s_fake_store_error),
                 "injected append failure");
        return ESP_FAIL;
    }
    if (!event || !event->payload ||
        s_fake_event_count >= FAKE_EVENT_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    fake_event_t *slot = &s_fake_events[s_fake_event_count];
    memset(slot, 0, sizeof(*slot));
    slot->owned_payload = cJSON_Duplicate(event->payload, true);
    if (!slot->owned_payload) {
        return ESP_ERR_NO_MEM;
    }
    slot->record.seq = s_fake_event_count + 1U;
    fake_copy(slot->record.thread_id, sizeof(slot->record.thread_id),
              event->thread_id);
    fake_copy(slot->record.turn_id, sizeof(slot->record.turn_id),
              event->turn_id);
    fake_copy(slot->record.run_id, sizeof(slot->record.run_id),
              event->run_id);
    fake_copy(slot->record.step_id, sizeof(slot->record.step_id),
              event->step_id);
    fake_copy(slot->record.action_id, sizeof(slot->record.action_id),
              event->action_id);
    fake_copy(slot->record.artifact_id, sizeof(slot->record.artifact_id),
              event->artifact_id);
    fake_copy(slot->record.event_type, sizeof(slot->record.event_type),
              event->event_type);
    slot->record.server_time_ms = event->server_time_ms;
    slot->record.monotonic_time_ms = event->monotonic_time_ms;
    slot->record.payload = slot->owned_payload;
    memset(slot->record.event_hash, 'a', SI_AGENT_EVENT_HASH_HEX_LEN);
    slot->record.event_hash[SI_AGENT_EVENT_HASH_HEX_LEN] = '\0';
    s_fake_event_count++;
    if (stored_out) {
        *stored_out = slot->record;
    }
    s_fake_store_error[0] = '\0';
    return ESP_OK;
}

esp_err_t si_agent_event_store_replay(uint64_t after_seq,
                                      si_agent_event_replay_cb_t callback,
                                      void *user_ctx,
                                      si_agent_event_recovery_t *recovery_out)
{
    for (size_t index = 0; index < s_fake_event_count; ++index) {
        if (s_fake_events[index].record.seq > after_seq && callback) {
            esp_err_t result = callback(&s_fake_events[index].record,
                                        user_ctx);
            if (result != ESP_OK) {
                return result;
            }
        }
    }
    fake_recovery(recovery_out);
    return ESP_OK;
}

esp_err_t si_agent_event_store_latest(
    uint64_t *seq_out, char hash_out[SI_AGENT_EVENT_HASH_HEX_LEN + 1U])
{
    if (!seq_out || !hash_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *seq_out = s_fake_event_count;
    memset(hash_out, 'a', SI_AGENT_EVENT_HASH_HEX_LEN);
    hash_out[SI_AGENT_EVENT_HASH_HEX_LEN] = '\0';
    return ESP_OK;
}

const char *si_agent_event_store_path(void)
{
    return "/fake/TASKS.LOG";
}

const char *si_agent_event_store_last_error(void)
{
    return s_fake_store_error;
}

const char *si_agent_event_recovery_status_name(
    si_agent_event_recovery_status_t status)
{
    switch (status) {
    case SI_AGENT_EVENT_RECOVERY_CLEAN: return "clean";
    case SI_AGENT_EVENT_RECOVERY_EMPTY: return "empty";
    case SI_AGENT_EVENT_RECOVERY_TAIL_TRUNCATED: return "tail_truncated";
    case SI_AGENT_EVENT_RECOVERY_CORRUPT: return "corrupt";
    case SI_AGENT_EVENT_RECOVERY_IO_ERROR: return "io_error";
    default: return "invalid";
    }
}

static void fake_log_clear(void)
{
    for (size_t index = 0; index < s_fake_event_count; ++index) {
        cJSON_Delete(s_fake_events[index].owned_payload);
    }
    memset(s_fake_events, 0, sizeof(s_fake_events));
    s_fake_event_count = 0U;
    s_fake_fail_next_append = false;
    s_fake_store_error[0] = '\0';
}

static void reset_task_service(bool clear_log)
{
    if (s_task_lock) {
        vSemaphoreDelete(s_task_lock);
    }
    s_task_lock = NULL;
    memset(&s_runtime, 0, sizeof(s_runtime));
    memset(s_records, 0, sizeof(s_records));
    memset(s_dedupe, 0, sizeof(s_dedupe));
    memset(s_action_ledger, 0, sizeof(s_action_ledger));
    s_dedupe_next = 0U;
    s_latest_event_seq = 0U;
    s_logical_time = 0U;
    s_id_counter = 0U;
    s_lifecycle = TASK_SERVICE_LIFECYCLE_COLD;
    s_start_result = ESP_ERR_INVALID_STATE;
    s_storage_healthy = false;
    s_recovery_status = SI_AGENT_EVENT_RECOVERY_IO_ERROR;
    s_last_error[0] = '\0';
    s_host_random = 0x6d2b79f5U;
    s_fake_now_ms = 1000U;
    s_fake_fail_init = false;
    s_fake_init_calls = 0U;
    if (clear_log) {
        fake_log_clear();
    }
}

static si_agent_task_submit_t valid_system_submit(const char *idempotency_key)
{
    return (si_agent_task_submit_t) {
        .thread_id = "thread-host",
        .goal = "verify canonical terminal event",
        .completion_criteria = "terminal journal event is replay-safe",
        .idempotency_key = idempotency_key,
        .dry_run = true,
        .source = {
            .origin = SI_AGENT_TASK_ORIGIN_SYSTEM,
            .auth_kind = SI_AGENT_TASK_AUTH_SYSTEM,
            .principal = SI_PRINCIPAL_SYSTEM,
            .auth_id = "device-host-test",
            .auth_generation = 1U,
            .authority_ceiling =
                SI_CAPABILITY_OBSERVE | SI_CAPABILITY_AGENT_RUN,
            .history_policy = SI_AGENT_TASK_HISTORY_SINGLE_TURN,
        },
    };
}

static si_agent_task_receipt_t submit_task(unsigned sequence)
{
    char idempotency_key[32];
    snprintf(idempotency_key, sizeof(idempotency_key), "host-idem-%u",
             sequence);
    si_agent_task_submit_t input = valid_system_submit(idempotency_key);
    si_agent_task_receipt_t receipt = {0};
    assert(si_agent_task_service_submit(&input, &receipt) == ESP_OK);
    assert(receipt.accepted);
    assert(receipt.started);
    assert(!receipt.queued);
    const cJSON *idempotency_hash = cJSON_GetObjectItemCaseSensitive(
        s_fake_events[s_fake_event_count - 1U].record.payload,
        "idempotency_key_hash");
    assert(cJSON_IsString(idempotency_hash));
    assert(strlen(idempotency_hash->valuestring) == 64U);
    assert(strstr(idempotency_hash->valuestring, "host-idem-") == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(
               s_fake_events[s_fake_event_count - 1U].record.payload,
               "idempotency_key") == NULL);
    const cJSON *source_auth_kind = cJSON_GetObjectItemCaseSensitive(
        s_fake_events[s_fake_event_count - 1U].record.payload,
        "source_auth_kind");
    const cJSON *source_principal = cJSON_GetObjectItemCaseSensitive(
        s_fake_events[s_fake_event_count - 1U].record.payload,
        "source_principal");
    assert(cJSON_IsString(source_auth_kind));
    assert(cJSON_IsString(source_principal));
    assert(strcmp(source_auth_kind->valuestring, "system") == 0);
    assert(strcmp(source_principal->valuestring, "system") == 0);
    const cJSON *criteria_kind = cJSON_GetObjectItemCaseSensitive(
        s_fake_events[s_fake_event_count - 1U].record.payload,
        "completion_criteria_kind");
    assert(cJSON_IsString(criteria_kind));
    assert(strcmp(criteria_kind->valuestring, "observation") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(
               s_fake_events[s_fake_event_count - 1U].record.payload,
               "source_auth_id") == NULL);
    return receipt;
}

static void prepare_active_step(const si_agent_task_receipt_t *receipt)
{
    assert(si_agent_task_service_set_phase(
               receipt->run_id, SI_AGENT_TURN_PLANNING) == ESP_OK);
    assert(si_agent_task_service_begin_plan(
               receipt->run_id, "plan-host") == ESP_OK);
    assert(si_agent_task_service_add_step(
               receipt->run_id, "step-host") == ESP_OK);
    assert(si_agent_task_service_set_phase(
               receipt->run_id, SI_AGENT_TURN_EXECUTING) == ESP_OK);
    assert(si_agent_task_service_set_step_status(
               receipt->run_id, "step-host",
               SI_AGENT_STEP_IN_PROGRESS) == ESP_OK);
}

static const char *payload_string(const cJSON *payload, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(payload, name);
    assert(cJSON_IsString(item));
    return item->valuestring;
}

static void assert_last_unknown(const si_agent_task_receipt_t *receipt,
                                const char *reason,
                                const char *terminal_source,
                                bool step_bound)
{
    assert(s_fake_event_count > 0U);
    const si_agent_event_record_t *event =
        &s_fake_events[s_fake_event_count - 1U].record;
    assert(strcmp(event->event_type, "turn.outcome_unknown") == 0);
    assert(strcmp(event->run_id, receipt->run_id) == 0);
    assert(strcmp(event->step_id, step_bound ? "step-host" : "") == 0);
    assert(strcmp(payload_string(event->payload, "status"),
                  "outcome_unknown") == 0);
    assert(strcmp(payload_string(event->payload, "outcome"), "unknown") == 0);
    assert(strcmp(payload_string(event->payload, "reason"), reason) == 0);
    assert(strcmp(payload_string(event->payload, "terminal_source"),
                  terminal_source) == 0);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
        event->payload, "blind_retry_forbidden")));

    si_agent_task_service_snapshot_t snapshot = {0};
    assert(si_agent_task_service_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.runtime.has_active_turn);
    assert(snapshot.runtime.active_turn.status ==
           SI_AGENT_TURN_OUTCOME_UNKNOWN);
    task_record_t *record = record_by_run(receipt->run_id);
    assert(record != NULL);
    task_dedupe_t *dedupe = dedupe_find(record->execution.idempotency_key);
    assert(dedupe != NULL);
    assert(dedupe->terminal);
}

static size_t event_count(const char *event_type)
{
    size_t count = 0U;
    for (size_t index = 0; index < s_fake_event_count; ++index) {
        if (strcmp(s_fake_events[index].record.event_type,
                   event_type) == 0) {
            count++;
        }
    }
    return count;
}

static void reboot_and_assert_closed(size_t event_count_before)
{
    reset_task_service(false);
    assert(si_agent_task_service_start() == ESP_OK);
    assert(s_fake_event_count == event_count_before);
    assert(event_count("turn.interrupted") == 0U);
    si_agent_task_service_snapshot_t snapshot = {0};
    assert(si_agent_task_service_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.runtime.has_active_turn);
    assert(snapshot.queue_depth == 0U);
}

static void test_step_status_unknown_is_terminal_event(void)
{
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(1U);
    prepare_active_step(&receipt);
    const size_t before = s_fake_event_count;
    assert(si_agent_task_service_set_step_status(
               receipt.run_id, "step-host",
               SI_AGENT_STEP_OUTCOME_UNKNOWN) == ESP_OK);
    assert(s_fake_event_count == before + 1U);
    assert_last_unknown(&receipt, "step_status_outcome_unknown",
                        "step.status_changed", true);
    reboot_and_assert_closed(s_fake_event_count);
}

static void test_step_verification_unknown_is_terminal_event(void)
{
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(2U);
    prepare_active_step(&receipt);
    const size_t before = s_fake_event_count;
    assert(si_agent_task_service_record_step_verification(
               receipt.run_id, "step-host",
               SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN, NULL) == ESP_OK);
    assert(s_fake_event_count == before + 1U);
    assert_last_unknown(&receipt, "step_verification_outcome_unknown",
                        "step.verification_recorded", true);
    reboot_and_assert_closed(s_fake_event_count);
}

static void test_turn_verification_unknown_is_terminal_event(void)
{
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(3U);
    assert(si_agent_task_service_set_phase(
               receipt.run_id, SI_AGENT_TURN_VERIFYING) == ESP_OK);
    const size_t before = s_fake_event_count;
    assert(si_agent_task_service_record_turn_verification(
               receipt.run_id, SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN,
               NULL) == ESP_OK);
    assert(s_fake_event_count == before + 1U);
    assert_last_unknown(&receipt, "turn_verification_outcome_unknown",
                        "turn.verification_recorded", false);
    reboot_and_assert_closed(s_fake_event_count);
}

static void test_explicit_unknown_is_blind_retry_forbidden(void)
{
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(5U);
    assert(si_agent_task_service_mark_outcome_unknown(
               receipt.run_id, "action-host",
               "action_result_missing") == ESP_OK);
    assert_last_unknown(&receipt, "action_result_missing",
                        "turn.outcome_unknown", false);
    reboot_and_assert_closed(s_fake_event_count);
}

static void test_append_failure_does_not_publish_terminal_state(void)
{
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(4U);
    prepare_active_step(&receipt);
    const size_t before = s_fake_event_count;
    s_fake_fail_next_append = true;
    assert(si_agent_task_service_set_step_status(
               receipt.run_id, "step-host",
               SI_AGENT_STEP_OUTCOME_UNKNOWN) == ESP_FAIL);
    assert(s_fake_event_count == before);
    si_agent_task_service_snapshot_t snapshot = {0};
    assert(si_agent_task_service_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.runtime.active_turn.status == SI_AGENT_TURN_EXECUTING);
    assert(snapshot.runtime.active_turn.plan.steps[0].status ==
           SI_AGENT_STEP_IN_PROGRESS);

    assert(si_agent_task_service_set_step_status(
               receipt.run_id, "step-host",
               SI_AGENT_STEP_OUTCOME_UNKNOWN) == ESP_OK);
    assert_last_unknown(&receipt, "step_status_outcome_unknown",
                        "step.status_changed", true);
}

static void test_control_append_failure_requires_local_shadow_demotion(void)
{
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(40U);
    prepare_active_step(&receipt);

    const size_t before_pause = s_fake_event_count;
    s_fake_fail_next_append = true;
    assert(si_agent_task_service_pause(receipt.run_id) == ESP_FAIL);
    assert(s_fake_event_count == before_pause);
    si_agent_task_service_snapshot_t snapshot = {0};
    assert(si_agent_task_service_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.storage_healthy);
    assert(snapshot.runtime.active_turn.status == SI_AGENT_TURN_EXECUTING);
    assert(strstr(si_agent_task_service_last_error(),
                  "injected append failure") != NULL);

    assert(si_agent_task_service_pause(receipt.run_id) == ESP_OK);
    assert(si_agent_task_service_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.runtime.active_turn.status == SI_AGENT_TURN_PAUSED);

    const size_t before_resume = s_fake_event_count;
    s_fake_fail_next_append = true;
    assert(si_agent_task_service_resume(receipt.run_id) == ESP_FAIL);
    assert(s_fake_event_count == before_resume);
    assert(si_agent_task_service_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.runtime.active_turn.status == SI_AGENT_TURN_PAUSED);

    assert(si_agent_task_service_resume(receipt.run_id) == ESP_OK);
    assert(si_agent_task_service_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.runtime.active_turn.status == SI_AGENT_TURN_EXECUTING);

    const size_t before_cancel = s_fake_event_count;
    s_fake_fail_next_append = true;
    assert(si_agent_task_service_cancel(receipt.run_id) == ESP_FAIL);
    assert(s_fake_event_count == before_cancel);
    assert(si_agent_task_service_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.runtime.active_turn.status == SI_AGENT_TURN_EXECUTING);
    assert(si_agent_task_service_cancel(receipt.run_id) == ESP_OK);
}

static void terminalize_by_variant(const si_agent_task_receipt_t *receipt,
                                   unsigned variant)
{
    if (variant == 2U) {
        assert(si_agent_task_service_set_phase(
                   receipt->run_id, SI_AGENT_TURN_VERIFYING) == ESP_OK);
        assert(si_agent_task_service_record_turn_verification(
                   receipt->run_id,
                   SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN, NULL) == ESP_OK);
        return;
    }
    prepare_active_step(receipt);
    if (variant == 0U) {
        assert(si_agent_task_service_set_step_status(
                   receipt->run_id, "step-host",
                   SI_AGENT_STEP_OUTCOME_UNKNOWN) == ESP_OK);
    } else {
        assert(si_agent_task_service_record_step_verification(
                   receipt->run_id, "step-host",
                   SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN, NULL) == ESP_OK);
    }
}

static void test_replay_open_slot_capacity_does_not_leak(void)
{
    reset_task_service(true);
    const unsigned turn_count = SI_AGENT_TASK_RECORD_CAPACITY + 4U;
    for (unsigned index = 0; index < turn_count; ++index) {
        si_agent_task_receipt_t receipt = submit_task(100U + index);
        terminalize_by_variant(&receipt, index % 3U);
        si_agent_task_execution_t next = {0};
        bool started = true;
        assert(si_agent_task_service_advance(&next, &started) == ESP_OK);
        assert(!started);
    }
    assert(event_count("turn.outcome_unknown") == turn_count);
    assert(event_count("turn.interrupted") == 0U);
    const size_t before_reboot = s_fake_event_count;
    reboot_and_assert_closed(before_reboot);

    /* Recovery consumed every submitted/terminal pair, so a new Task starts
     * normally instead of disappearing behind a leaked replay-open slot. */
    si_agent_task_receipt_t after_reboot = submit_task(999U);
    assert(after_reboot.started);
}

static void test_action_result_requires_exact_started_ledger(void)
{
    static const char *const args_hash =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(2000U);
    prepare_active_step(&receipt);

    assert(si_agent_task_service_record_action_result(
               receipt.run_id, "step-host", "action-host", true, NULL,
               "execution_returned") == ESP_ERR_INVALID_STATE);
    assert(si_agent_task_service_record_action_started(
               receipt.run_id, "step-host", "action-host", "observe_status",
               args_hash, "action-idem-host", false) == ESP_OK);
    assert(si_agent_task_service_record_action_started(
               receipt.run_id, "step-host", "action-host", "observe_status",
               args_hash, "action-idem-host", false) ==
           ESP_ERR_INVALID_STATE);
    assert(si_agent_task_service_record_action_result(
               receipt.run_id, "wrong-step", "action-host", true, NULL,
               "execution_returned") == ESP_ERR_INVALID_STATE);

    s_fake_fail_next_append = true;
    assert(si_agent_task_service_record_action_result(
               receipt.run_id, "step-host", "action-host", true, NULL,
               "execution_returned") == ESP_FAIL);
    assert(si_agent_task_service_record_action_result(
               receipt.run_id, "step-host", "action-host", true, NULL,
               "execution_returned") == ESP_OK);
    assert(si_agent_task_service_record_action_result(
               receipt.run_id, "step-host", "action-host", true, NULL,
               "execution_returned") == ESP_ERR_INVALID_STATE);

    assert(si_agent_task_service_record_step_verification(
               receipt.run_id, "step-host",
               SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN, NULL) == ESP_OK);
    for (size_t index = 0; index < SI_AGENT_TASK_ACTION_LEDGER_CAPACITY;
         ++index) {
        assert(!s_action_ledger[index].used);
    }
}

static void test_scalar_api_cannot_self_issue_passed_verification(void)
{
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(2100U);
    prepare_active_step(&receipt);
    assert(si_agent_task_service_record_step_verification(
               receipt.run_id, "step-host", SI_AGENT_VERIFICATION_PASSED,
               "caller-invented-evidence") == ESP_ERR_NOT_SUPPORTED);
    assert(si_agent_task_service_record_turn_verification(
               receipt.run_id, SI_AGENT_VERIFICATION_PASSED,
               "caller-invented-evidence") == ESP_ERR_NOT_SUPPORTED);
    assert(si_agent_task_service_set_step_status(
               receipt.run_id, "step-host",
               SI_AGENT_STEP_OUTCOME_UNKNOWN) == ESP_OK);
}

static void test_observation_response_artifact_cannot_self_verify(void)
{
    static const char *const artifact_hash =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(2200U);
    prepare_active_step(&receipt);

    si_agent_completion_artifact_t artifact = {0};
    assert(si_agent_task_service_record_readonly_completion_artifact(
               receipt.run_id, "step-host", artifact_hash, 19U,
               &artifact) == ESP_OK);
    assert(artifact.event_seq > 0U);
    assert(artifact.issuer == SI_AGENT_EVIDENCE_ISSUER_ARTIFACT_STORE);
    assert(artifact.kind == SI_AGENT_EVIDENCE_ARTIFACT);
    assert(strcmp(payload_string(
                      s_fake_events[s_fake_event_count - 1U].record.payload,
                      "completion_criteria_kind"), "observation") == 0);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(
        s_fake_events[s_fake_event_count - 1U].record.payload,
        "raw_response_persisted")));

    si_agent_verification_result_t verdict = {
        .status = SI_AGENT_VERIFY_PASSED,
    };
    assert(si_agent_task_service_complete_readonly(
               receipt.run_id, "step-host", &artifact, &verdict) ==
           ESP_ERR_NOT_ALLOWED);
    assert(verdict.status == SI_AGENT_VERIFY_INVALID);
    assert(strstr(verdict.reason, "cannot verify observation") != NULL);
    assert(event_count("step.verification_recorded") == 0U);
    assert(event_count("turn.completed") == 0U);
    assert(si_agent_task_service_interrupt(
               receipt.run_id, "test_observation_unverified") == ESP_OK);
}

static void assert_forged_artifact_rejected(
    const si_agent_task_receipt_t *receipt,
    const si_agent_completion_artifact_t *artifact)
{
    si_agent_verification_result_t verdict = {
        .status = SI_AGENT_VERIFY_PASSED,
    };
    assert(si_agent_task_service_complete_readonly(
               receipt->run_id, "step-host", artifact, &verdict) ==
           ESP_ERR_NOT_ALLOWED);
    assert(verdict.status == SI_AGENT_VERIFY_INVALID);
}

static void test_deliverable_artifact_binding_rejects_forgery(void)
{
    static const char *const artifact_hash =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    reset_task_service(true);
    si_agent_task_submit_t input = valid_system_submit("deliverable-binding");
    input.completion_criteria = "bounded response artifact is durable";
    input.completion_criteria_kind = SI_AGENT_TASK_CRITERIA_DELIVERABLE;
    si_agent_task_receipt_t receipt = {0};
    assert(si_agent_task_service_submit(&input, &receipt) == ESP_OK);
    assert(strcmp(payload_string(
                      s_fake_events[s_fake_event_count - 1U].record.payload,
                      "completion_criteria_kind"), "deliverable") == 0);
    prepare_active_step(&receipt);

    si_agent_completion_artifact_t artifact = {0};
    assert(si_agent_task_service_record_readonly_completion_artifact(
               receipt.run_id, "step-host", artifact_hash, 23U,
               &artifact) == ESP_OK);
    assert(strcmp(payload_string(
                      s_fake_events[s_fake_event_count - 1U].record.payload,
                      "completion_criteria_kind"), "deliverable") == 0);

    si_agent_completion_artifact_t forged = artifact;
    forged.kind = SI_AGENT_EVIDENCE_TOOL_RETURN;
    assert_forged_artifact_rejected(&receipt, &forged);
    forged = artifact;
    forged.issuer = SI_AGENT_EVIDENCE_ISSUER_MODEL;
    assert_forged_artifact_rejected(&receipt, &forged);
    forged = artifact;
    forged.captured_ms = s_records[0].execution_started_ms;
    assert_forged_artifact_rejected(&receipt, &forged);
    forged = artifact;
    snprintf(forged.target_identity, sizeof(forged.target_identity),
             "wrong-target");
    assert_forged_artifact_rejected(&receipt, &forged);
    forged = artifact;
    forged.completion_criteria_hash[0] =
        forged.completion_criteria_hash[0] == 'a' ? 'b' : 'a';
    assert_forged_artifact_rejected(&receipt, &forged);
    forged = artifact;
    forged.event_hash[0] = forged.event_hash[0] == 'a' ? 'b' : 'a';
    assert_forged_artifact_rejected(&receipt, &forged);

    si_agent_verification_result_t verdict = {0};
    assert(si_agent_task_service_complete_readonly(
               receipt.run_id, "step-host", &artifact, &verdict) == ESP_OK);
    assert(verdict.status == SI_AGENT_VERIFY_PASSED);
    assert(strstr(verdict.reason, "output-only deliverable") != NULL);
    assert(event_count("step.verification_recorded") == 1U);
    assert(event_count("turn.verification_recorded") == 1U);
    assert(event_count("turn.completed") == 1U);
}

static void test_mutation_and_storage_failure_cannot_issue_artifact(void)
{
    static const char *const artifact_hash =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    static const char *const args_hash =
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";

    reset_task_service(true);
    si_agent_task_receipt_t failed = submit_task(2300U);
    prepare_active_step(&failed);
    si_agent_completion_artifact_t artifact;
    memset(&artifact, 0x5a, sizeof(artifact));
    const size_t before = s_fake_event_count;
    s_fake_fail_next_append = true;
    assert(si_agent_task_service_record_readonly_completion_artifact(
               failed.run_id, "step-host", artifact_hash, 7U,
               &artifact) == ESP_FAIL);
    assert(artifact.event_seq == 0U);
    assert(s_fake_event_count == before);
    assert(si_agent_task_service_record_readonly_completion_artifact(
               failed.run_id, "step-host", artifact_hash, 7U,
               &artifact) == ESP_OK);
    assert(si_agent_task_service_interrupt(
               failed.run_id, "test_artifact_recorded") == ESP_OK);

    reset_task_service(true);
    si_agent_task_receipt_t mutation = submit_task(2301U);
    prepare_active_step(&mutation);
    assert(si_agent_task_service_record_action_started(
               mutation.run_id, "step-host", "action-mutation",
               "future_manager_execute", args_hash,
               "mutation-idempotency", true) == ESP_OK);
    memset(&artifact, 0, sizeof(artifact));
    assert(si_agent_task_service_record_readonly_completion_artifact(
               mutation.run_id, "step-host", artifact_hash, 7U,
               &artifact) == ESP_ERR_INVALID_STATE);
    assert(artifact.event_seq == 0U);
    assert(si_agent_task_service_cancel(mutation.run_id) == ESP_OK);
    assert_last_unknown(&mutation, "cancel_after_action_started",
                        "turn.cancelled", false);
}

static void test_source_auth_binding_is_trusted_and_separate_from_thread(void)
{
    reset_task_service(true);
    si_agent_task_submit_t input = {
        .thread_id = "conversation-from-request",
        .goal = "bind authenticated source",
        .completion_criteria = "source remains independently revocable",
        .idempotency_key = "web-auth-binding",
        .dry_run = true,
        .source = {
            .origin = SI_AGENT_TASK_ORIGIN_WEB,
            .source_account_id = "server-account",
            .source_conversation_id = "conversation-from-request",
            .source_actor_id = "browser",
            .auth_kind = SI_AGENT_TASK_AUTH_DEVICE_SESSION,
            .principal = SI_PRINCIPAL_BROWSER,
            .auth_id = "s-device-generated",
            .auth_generation = 42U,
            .authority_ceiling =
                SI_CAPABILITY_OBSERVE | SI_CAPABILITY_AGENT_RUN,
            .history_policy = SI_AGENT_TASK_HISTORY_THREAD,
        },
    };
    si_agent_task_receipt_t receipt = {0};

    input.source.auth_generation = 0U;
    assert(si_agent_task_service_submit(&input, &receipt) ==
           ESP_ERR_INVALID_ARG);
    input.source.auth_generation = 42U;
    input.source.principal = SI_PRINCIPAL_MCP;
    assert(si_agent_task_service_submit(&input, &receipt) ==
           ESP_ERR_INVALID_ARG);
    input.source.principal = SI_PRINCIPAL_BROWSER;

    si_agent_task_submit_t qq = input;
    qq.idempotency_key = "qq-ceiling";
    qq.source.origin = SI_AGENT_TASK_ORIGIN_QQ;
    qq.source.auth_kind = SI_AGENT_TASK_AUTH_ADAPTER_DELEGATION;
    qq.source.principal = SI_PRINCIPAL_AGENT;
    qq.source.auth_id = "device-qq-delegation";
    qq.source.authority_ceiling =
        SI_AGENT_TASK_QQ_AUTHORITY_CEILING | SI_CAPABILITY_HID;
    assert(si_agent_task_service_submit(&qq, &receipt) ==
           ESP_ERR_INVALID_ARG);

    assert(si_agent_task_service_submit(&input, &receipt) == ESP_OK);

    si_agent_task_execution_t execution = {0};
    assert(si_agent_task_service_get_execution(receipt.run_id, &execution) ==
           ESP_OK);
    assert(strcmp(execution.thread_id, "conversation-from-request") == 0);
    assert(strcmp(execution.source_auth_id, "s-device-generated") == 0);
    assert(execution.source_auth_generation == 42U);
    assert(execution.source_principal == SI_PRINCIPAL_BROWSER);
    assert(execution.auth_kind == SI_AGENT_TASK_AUTH_DEVICE_SESSION);

    const cJSON *payload =
        s_fake_events[s_fake_event_count - 1U].record.payload;
    const char *auth_hash = payload_string(payload, "source_auth_id_hash");
    assert(strlen(auth_hash) == 64U);
    assert(strstr(auth_hash, "s-device-generated") == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(payload, "source_auth_id") ==
           NULL);
}

static void test_start_failure_is_stable_degraded_and_fail_closed(void)
{
    reset_task_service(true);
    s_fake_fail_init = true;
    assert(si_agent_task_service_start() == ESP_FAIL);
    assert(s_lifecycle == TASK_SERVICE_LIFECYCLE_FAILED);
    assert(!s_storage_healthy);
    assert(s_fake_init_calls == 1U);
    assert(strstr(si_agent_task_service_last_error(),
                  "injected init failure") != NULL);

    /* Status/submit calls do not hammer TF recovery after the boot failure. */
    s_fake_fail_init = false;
    si_agent_task_service_snapshot_t snapshot = {0};
    assert(si_agent_task_service_snapshot(&snapshot) == ESP_FAIL);
    assert(!snapshot.available);
    assert(snapshot.recovery_status == SI_AGENT_EVENT_RECOVERY_IO_ERROR);
    si_agent_task_submit_t input = valid_system_submit("degraded-submit");
    si_agent_task_receipt_t receipt = {0};
    assert(si_agent_task_service_submit(&input, &receipt) == ESP_FAIL);
    assert(!receipt.accepted);
    assert(s_fake_init_calls == 1U);
    assert(s_fake_event_count == 0U);
    assert(!s_runtime.has_active_turn);
}

static void test_reserved_recovery_never_runs_on_public_caller(void)
{
    reset_task_service(true);
    assert(si_agent_task_service_reserve_recovery() == ESP_OK);
    assert(si_agent_task_service_start() == ESP_ERR_INVALID_STATE);
    assert(s_fake_init_calls == 0U);
    assert(si_agent_task_service_recover_reserved() == ESP_OK);
    assert(s_fake_init_calls == 1U);
    assert(si_agent_task_service_start() == ESP_OK);
    assert(s_fake_init_calls == 1U);
}

static void test_device_correlated_identity_is_exact_and_never_queued(void)
{
    reset_task_service(true);
    si_agent_task_submit_t input = valid_system_submit("correlated-one");
    input.turn_id = "turn-legacy-exact";
    input.run_id = "run-legacy-exact";
    si_agent_task_receipt_t receipt = {0};
    assert(si_agent_task_service_submit(&input, &receipt) == ESP_OK);
    assert(receipt.started && !receipt.queued && !receipt.deduplicated);
    assert(strcmp(receipt.turn_id, input.turn_id) == 0);
    assert(strcmp(receipt.run_id, input.run_id) == 0);

    const size_t before = s_fake_event_count;
    input.idempotency_key = "correlated-two";
    input.turn_id = "turn-must-not-queue";
    input.run_id = "run-must-not-queue";
    assert(si_agent_task_service_submit(&input, &receipt) ==
           ESP_ERR_INVALID_STATE);
    assert(s_fake_event_count == before);
    assert(s_runtime.pending_count == 0U);
    assert(strcmp(s_runtime.active_turn.run_attempt.run_attempt_id,
                  "run-legacy-exact") == 0);

    input.idempotency_key = "correlated-invalid";
    input.turn_id = NULL;
    assert(si_agent_task_service_submit(&input, &receipt) ==
           ESP_ERR_INVALID_ARG);
}

int main(void)
{
    test_step_status_unknown_is_terminal_event();
    test_step_verification_unknown_is_terminal_event();
    test_turn_verification_unknown_is_terminal_event();
    test_explicit_unknown_is_blind_retry_forbidden();
    test_append_failure_does_not_publish_terminal_state();
    test_control_append_failure_requires_local_shadow_demotion();
    test_replay_open_slot_capacity_does_not_leak();
    test_action_result_requires_exact_started_ledger();
    test_scalar_api_cannot_self_issue_passed_verification();
    test_observation_response_artifact_cannot_self_verify();
    test_deliverable_artifact_binding_rejects_forgery();
    test_mutation_and_storage_failure_cannot_issue_artifact();
    test_source_auth_binding_is_trusted_and_separate_from_thread();
    test_start_failure_is_stable_degraded_and_fail_closed();
    test_reserved_recovery_never_runs_on_public_caller();
    test_device_correlated_identity_is_exact_and_never_queued();
    reset_task_service(true);
    puts("agent task service terminal event tests: OK");
    return 0;
}
