#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SI_AGENT_REQUEST_BROKER_HOST_TEST 1
#include "../../main/application/agent_request_broker.c"

static uint32_t s_test_now_ms;

static uint32_t test_now_ms(void *context)
{
    (void)context;
    return s_test_now_ms;
}

static void reset_broker(void)
{
    si_agent_request_snapshot_t empty = {
        .schema_version = SI_AGENT_REQUEST_SNAPSHOT_SCHEMA_VERSION,
        .next_sequence = 1U,
    };
    s_test_now_ms = 1000U;
    s_host_lock_unavailable = false;
    assert(si_agent_request_broker_set_now_provider(test_now_ms, NULL) == ESP_OK);
    assert(si_agent_request_broker_import_snapshot(&empty) == ESP_OK);
}

static si_agent_request_v2_input_t make_action(
    const char *run_id, const char *step_id, uint32_t plan_version,
    const char *args_json, const char *idempotency_key)
{
    si_agent_request_v2_input_t input = {
        .binding = {
            .device_id = "device-test-1",
            .thread_id = "thread-1",
            .turn_id = "turn-1",
            .run_id = run_id,
            .step_id = step_id,
            .plan_version = plan_version,
            .normalized_args_json = args_json,
            .idempotency_key = idempotency_key,
        },
        .kind = SI_AGENT_REQUEST_KIND_ACTION,
        .resource = "uart_write",
        .reason = "write an exact test command",
        .expected_effect = "one bounded UART write",
        .verification = "read back command result",
        .risk = SI_AGENT_REQUEST_RISK_MEDIUM,
        .lease_required = false,
        .reviewer_allowed = false,
        .requested_grant_scope = SI_AGENT_REQUEST_GRANT_ONCE,
        .ttl_ms = 5000U,
    };
    set_actor(&input.requester, SI_AGENT_REQUEST_PRINCIPAL_AGENT,
              "agent-session-1", "planner-1");
    return input;
}

static si_agent_request_decision_t browser_decision(
    const char *request_id, bool approved,
    si_agent_request_grant_scope_t scope)
{
    si_agent_request_decision_t decision = {
        .request_id = request_id,
        .approved = approved,
        .response = approved ? "approved in host test" : "denied in host test",
        .grant_scope = scope,
    };
    set_actor(&decision.decider, SI_AGENT_REQUEST_PRINCIPAL_BROWSER,
              "browser-session-1", "workspace-1");
    return decision;
}

static si_agent_request_consume_t make_consume(
    const si_agent_request_status_t *status, uint32_t plan_version,
    const char *args_json)
{
    si_agent_request_consume_t consume = {
        .request_id = status->request_id,
        .binding = {
            .device_id = status->device_id,
            .thread_id = status->thread_id,
            .turn_id = status->turn_id,
            .run_id = status->run_id,
            .step_id = status->step_id,
            .plan_version = plan_version,
            .normalized_args_json = args_json,
            .idempotency_key = status->idempotency_key,
        },
    };
    return consume;
}

static void test_queue_capacity_and_multiple_runs(void)
{
    reset_broker();
    si_agent_request_status_t created[SI_AGENT_REQUEST_QUEUE_CAPACITY] = {0};
    char run_ids[SI_AGENT_REQUEST_QUEUE_CAPACITY][16] = {{0}};
    char idem_keys[SI_AGENT_REQUEST_QUEUE_CAPACITY][16] = {{0}};
    for (size_t index = 0; index < SI_AGENT_REQUEST_QUEUE_CAPACITY; ++index) {
        snprintf(run_ids[index], sizeof(run_ids[index]), "run-%u",
                 (unsigned)index);
        snprintf(idem_keys[index], sizeof(idem_keys[index]), "idem-%u",
                 (unsigned)index);
        si_agent_request_v2_input_t input = make_action(
            run_ids[index], "step-1", 1U, "{\"value\":1}", idem_keys[index]);
        assert(si_agent_request_broker_create_v2(&input, &created[index]) == ESP_OK);
        assert(created[index].state == SI_AGENT_REQUEST_WAITING_USER);
    }
    si_agent_request_v2_input_t overflow = make_action(
        "run-overflow", "step-1", 1U, "{\"value\":1}", "idem-overflow");
    assert(si_agent_request_broker_create_v2(&overflow, NULL) == ESP_ERR_NO_MEM);

    si_agent_request_status_t by_id = {0};
    assert(si_agent_request_broker_get_by_id(created[3].request_id, &by_id) == ESP_OK);
    assert(strcmp(by_id.run_id, "run-3") == 0);
    size_t count = 0U;
    assert(si_agent_request_broker_enumerate("run-3", &by_id, 1U, &count) == ESP_OK);
    assert(count == 1U);
}

static void test_waiting_and_granted_expiry(void)
{
    reset_broker();
    si_agent_request_v2_input_t input = make_action(
        "run-expiry-1", "step-1", 1U, "{}", "idem-expiry-1");
    si_agent_request_status_t status = {0};
    assert(si_agent_request_broker_create_v2(&input, &status) == ESP_OK);
    s_test_now_ms += 5000U;
    assert(si_agent_request_broker_get_by_id(status.request_id, &status) == ESP_OK);
    assert(status.state == SI_AGENT_REQUEST_EXPIRED);

    reset_broker();
    input = make_action(
        "run-expiry-2", "step-1", 1U, "{}", "idem-expiry-2");
    assert(si_agent_request_broker_create_v2(&input, &status) == ESP_OK);
    si_agent_request_decision_t decision = browser_decision(
        status.request_id, true, SI_AGENT_REQUEST_GRANT_ONCE);
    assert(si_agent_request_broker_decide_v2(&decision, &status) == ESP_OK);
    assert(status.state == SI_AGENT_REQUEST_GRANTED);
    s_test_now_ms += 5000U;
    assert(si_agent_request_broker_get_by_id(status.request_id, &status) == ESP_OK);
    assert(status.state == SI_AGENT_REQUEST_EXPIRED);
}

static void test_requester_cannot_approve_self(void)
{
    reset_broker();
    si_agent_request_v2_input_t input = make_action(
        "run-self", "step-1", 1U, "{}", "idem-self");
    si_agent_request_status_t status = {0};
    assert(si_agent_request_broker_create_v2(&input, &status) == ESP_OK);
    si_agent_request_decision_t decision = {
        .request_id = status.request_id,
        .approved = true,
        .grant_scope = SI_AGENT_REQUEST_GRANT_ONCE,
        .decider = input.requester,
    };
    assert(si_agent_request_broker_decide_v2(&decision, &status) ==
           ESP_ERR_INVALID_STATE);
    assert(status.state == SI_AGENT_REQUEST_WAITING_USER);
}

static void test_once_grant_requires_atomic_consume(void)
{
    reset_broker();
    si_agent_request_v2_input_t input = make_action(
        "run-consume", "step-1", 1U, "{\"key\":1}", "idem-consume");
    si_agent_request_status_t status = {0};
    assert(si_agent_request_broker_create_v2(&input, &status) == ESP_OK);
    si_agent_request_decision_t decision = browser_decision(
        status.request_id, true, SI_AGENT_REQUEST_GRANT_ONCE);
    assert(si_agent_request_broker_decide_v2(&decision, &status) == ESP_OK);
    si_agent_request_consume_t consume = make_consume(
        &status, 1U, "{\"key\":1}");
    assert(si_agent_request_broker_consume(&consume, &status) == ESP_OK);
    assert(status.state == SI_AGENT_REQUEST_EXECUTING);
    assert(status.grant_consumed);
    assert(si_agent_request_broker_consume(&consume, &status) ==
           ESP_ERR_INVALID_STATE);
    assert(si_agent_request_broker_mark_verifying(status.request_id, &status) ==
           ESP_OK);
    assert(si_agent_request_broker_complete_verified(
               status.request_id, "fresh read-back matched", &status) == ESP_OK);
    assert(status.state == SI_AGENT_REQUEST_COMPLETED);
    assert(strcmp(status.response, "approved in host test") == 0);
    assert(strcmp(status.verification_evidence,
                  "fresh read-back matched") == 0);

    si_agent_request_status_t replay = {0};
    assert(si_agent_request_broker_create_v2(&input, &replay) == ESP_OK);
    assert(strcmp(replay.request_id, status.request_id) == 0);
    assert(replay.state == SI_AGENT_REQUEST_COMPLETED);
}

static void test_args_or_plan_change_invalidates_grant(void)
{
    reset_broker();
    si_agent_request_v2_input_t input = make_action(
        "run-change-args", "step-1", 4U, "{\"key\":1}", "idem-change-args");
    si_agent_request_status_t status = {0};
    assert(si_agent_request_broker_create_v2(&input, &status) == ESP_OK);
    si_agent_request_decision_t decision = browser_decision(
        status.request_id, true, SI_AGENT_REQUEST_GRANT_ONCE);
    assert(si_agent_request_broker_decide_v2(&decision, &status) == ESP_OK);
    si_agent_request_consume_t consume = make_consume(
        &status, 4U, "{\"key\":2}");
    assert(si_agent_request_broker_consume(&consume, &status) ==
           ESP_ERR_INVALID_STATE);
    assert(status.state == SI_AGENT_REQUEST_EXPIRED);

    reset_broker();
    input = make_action(
        "run-change-plan", "step-1", 4U, "{\"key\":1}", "idem-change-plan");
    assert(si_agent_request_broker_create_v2(&input, &status) == ESP_OK);
    decision = browser_decision(
        status.request_id, true, SI_AGENT_REQUEST_GRANT_ONCE);
    assert(si_agent_request_broker_decide_v2(&decision, &status) == ESP_OK);
    consume = make_consume(&status, 5U, "{\"key\":1}");
    assert(si_agent_request_broker_consume(&consume, &status) ==
           ESP_ERR_INVALID_STATE);
    assert(status.state == SI_AGENT_REQUEST_EXPIRED);
}

static void test_reviewer_is_separate_and_run_grant_is_narrowed(void)
{
    reset_broker();
    si_agent_request_v2_input_t input = make_action(
        "run-review", "step-1", 2U, "{\"key\":1}", "idem-review-1");
    input.reviewer_allowed = true;
    input.requested_grant_scope = SI_AGENT_REQUEST_GRANT_RUN;
    si_agent_request_status_t status = {0};
    assert(si_agent_request_broker_create_v2(&input, &status) == ESP_OK);
    assert(status.state == SI_AGENT_REQUEST_REVIEWING);

    si_agent_request_decision_t decision = {
        .request_id = status.request_id,
        .approved = true,
        .response = "delegated review approved",
        .grant_scope = SI_AGENT_REQUEST_GRANT_RUN,
    };
    set_actor(&decision.decider, SI_AGENT_REQUEST_PRINCIPAL_REVIEWER,
              "review-session-1", "reviewer-model-1");
    assert(si_agent_request_broker_decide_v2(&decision, &status) == ESP_OK);
    assert(status.reviewer.principal == SI_AGENT_REQUEST_PRINCIPAL_REVIEWER);
    assert(status.grant_scope == SI_AGENT_REQUEST_GRANT_RUN);
    si_agent_request_consume_t consume = make_consume(
        &status, 2U, "{\"key\":1}");
    assert(si_agent_request_broker_consume(&consume, &status) == ESP_OK);
    assert(si_agent_request_broker_mark_verifying(status.request_id, &status) ==
           ESP_OK);
    assert(si_agent_request_broker_complete_verified(
               status.request_id, "read-back matched", &status) == ESP_OK);

    input = make_action(
        "run-review", "step-2", 2U, "{\"key\":1}", "idem-review-2");
    input.reviewer_allowed = true;
    input.requested_grant_scope = SI_AGENT_REQUEST_GRANT_ONCE;
    si_agent_request_status_t reused = {0};
    assert(si_agent_request_broker_create_v2(&input, &reused) == ESP_OK);
    assert(reused.state == SI_AGENT_REQUEST_GRANTED);
    assert(strcmp(reused.parent_grant_request_id, status.request_id) == 0);
    assert(reused.grant_scope == SI_AGENT_REQUEST_GRANT_ONCE);
}

static void test_delegated_access_mode_thresholds(void)
{
    reset_broker();
    si_agent_request_v2_input_t assisted = make_action(
        "run-assisted", "step-1", 1U, "{}", "idem-assisted-medium");
    assisted.delegated_auto_approval = true;
    assisted.auto_approve_through = SI_AGENT_REQUEST_RISK_MEDIUM;
    si_agent_request_status_t status = {0};
    assert(si_agent_request_broker_create_v2(&assisted, &status) == ESP_OK);
    assert(status.state == SI_AGENT_REQUEST_GRANTED);
    assert(status.decider.principal == SI_AGENT_REQUEST_PRINCIPAL_POLICY);
    assert(strcmp(status.decision_source, "delegated_policy") == 0);
    assert(status.grant_scope == SI_AGENT_REQUEST_GRANT_ONCE);

    reset_broker();
    assisted = make_action(
        "run-assisted-high", "step-1", 1U, "{}", "idem-assisted-high");
    assisted.risk = SI_AGENT_REQUEST_RISK_HIGH;
    assisted.delegated_auto_approval = true;
    assisted.auto_approve_through = SI_AGENT_REQUEST_RISK_MEDIUM;
    assert(si_agent_request_broker_create_v2(&assisted, &status) == ESP_OK);
    assert(status.state == SI_AGENT_REQUEST_WAITING_USER);

    reset_broker();
    si_agent_request_v2_input_t full = make_action(
        "run-full", "step-1", 1U, "{}", "idem-full-critical");
    full.risk = SI_AGENT_REQUEST_RISK_CRITICAL;
    full.delegated_auto_approval = true;
    full.auto_approve_through = SI_AGENT_REQUEST_RISK_CRITICAL;
    assert(si_agent_request_broker_create_v2(&full, &status) == ESP_OK);
    assert(status.state == SI_AGENT_REQUEST_GRANTED);
    assert(strcmp(status.decision_source, "delegated_policy") == 0);
}

static void test_cancel_run_covers_all_requests(void)
{
    reset_broker();
    si_agent_request_status_t first = {0};
    si_agent_request_status_t second = {0};
    si_agent_request_status_t other = {0};
    si_agent_request_v2_input_t input = make_action(
        "run-cancel", "step-1", 1U, "{}", "idem-cancel-1");
    assert(si_agent_request_broker_create_v2(&input, &first) == ESP_OK);
    input = make_action(
        "run-cancel", "step-2", 1U, "{}", "idem-cancel-2");
    assert(si_agent_request_broker_create_v2(&input, &second) == ESP_OK);
    si_agent_request_decision_t decision = browser_decision(
        second.request_id, true, SI_AGENT_REQUEST_GRANT_ONCE);
    assert(si_agent_request_broker_decide_v2(&decision, &second) == ESP_OK);
    input = make_action(
        "run-other", "step-1", 1U, "{}", "idem-other");
    assert(si_agent_request_broker_create_v2(&input, &other) == ESP_OK);

    si_agent_request_broker_cancel_run("run-cancel");
    si_agent_request_status_t listed[2] = {0};
    size_t count = 0U;
    assert(si_agent_request_broker_enumerate(
               "run-cancel", listed, 2U, &count) == ESP_OK);
    assert(count == 2U);
    assert(listed[0].state == SI_AGENT_REQUEST_CANCELLED);
    assert(listed[1].state == SI_AGENT_REQUEST_CANCELLED);
    assert(si_agent_request_broker_get_by_id(other.request_id, &other) == ESP_OK);
    assert(other.state == SI_AGENT_REQUEST_WAITING_USER);
}

static void test_retire_run_is_atomic_and_run_scoped(void)
{
    reset_broker();
    si_agent_request_status_t first = {0};
    si_agent_request_status_t second = {0};
    si_agent_request_status_t other = {0};
    si_agent_request_v2_input_t input = make_action(
        "run-retire", "step-1", 1U, "{}", "idem-retire-1");
    assert(si_agent_request_broker_create_v2(&input, &first) == ESP_OK);
    input = make_action(
        "run-retire", "step-2", 1U, "{}", "idem-retire-2");
    assert(si_agent_request_broker_create_v2(&input, &second) == ESP_OK);
    input = make_action(
        "run-retire-other", "step-1", 1U, "{}", "idem-retire-other");
    assert(si_agent_request_broker_create_v2(&input, &other) == ESP_OK);

    si_agent_request_decision_t decision = browser_decision(
        first.request_id, false, SI_AGENT_REQUEST_GRANT_ONCE);
    assert(si_agent_request_broker_decide_v2(&decision, &first) == ESP_OK);
    assert(first.state == SI_AGENT_REQUEST_DENIED);
    assert(si_agent_request_broker_retire_run("run-retire") ==
           ESP_ERR_INVALID_STATE);

    si_agent_request_status_t preserved = {0};
    assert(si_agent_request_broker_get_by_id(first.request_id, &preserved) ==
           ESP_OK);
    assert(preserved.state == SI_AGENT_REQUEST_DENIED);
    assert(si_agent_request_broker_get_by_id(second.request_id, &preserved) ==
           ESP_OK);
    assert(preserved.state == SI_AGENT_REQUEST_WAITING_USER);

    decision = browser_decision(
        second.request_id, false, SI_AGENT_REQUEST_GRANT_ONCE);
    assert(si_agent_request_broker_decide_v2(&decision, &second) == ESP_OK);
    assert(si_agent_request_broker_retire_run("run-retire") == ESP_OK);
    assert(si_agent_request_broker_get_by_id(first.request_id, &preserved) ==
           ESP_ERR_NOT_FOUND);
    assert(si_agent_request_broker_get_by_id(second.request_id, &preserved) ==
           ESP_ERR_NOT_FOUND);
    assert(si_agent_request_broker_get_by_id(other.request_id, &preserved) ==
           ESP_OK);
    assert(preserved.state == SI_AGENT_REQUEST_WAITING_USER);
    assert(si_agent_request_broker_retire_run("run-retire") == ESP_OK);

    input = make_action(
        "run-reuse-slot", "step-1", 1U, "{}", "idem-reuse-slot");
    assert(si_agent_request_broker_create_v2(&input, &preserved) == ESP_OK);
}

static void test_terminalize_and_retire_run_is_atomic_and_retryable(void)
{
    reset_broker();
    si_agent_request_status_t waiting = {0};
    si_agent_request_status_t executing = {0};
    si_agent_request_status_t other = {0};
    si_agent_request_v2_input_t input = make_action(
        "run-terminalize", "step-waiting", 1U, "{}", "idem-term-wait");
    assert(si_agent_request_broker_create_v2(&input, &waiting) == ESP_OK);
    input = make_action(
        "run-terminalize", "step-executing", 1U, "{}", "idem-term-exec");
    assert(si_agent_request_broker_create_v2(&input, &executing) == ESP_OK);
    si_agent_request_decision_t decision = browser_decision(
        executing.request_id, true, SI_AGENT_REQUEST_GRANT_ONCE);
    assert(si_agent_request_broker_decide_v2(&decision, &executing) == ESP_OK);
    si_agent_request_consume_t consume = make_consume(&executing, 1U, "{}");
    assert(si_agent_request_broker_consume(&consume, &executing) == ESP_OK);
    assert(executing.state == SI_AGENT_REQUEST_EXECUTING);

    input = make_action(
        "run-terminalize-other", "step-1", 1U, "{}", "idem-term-other");
    assert(si_agent_request_broker_create_v2(&input, &other) == ESP_OK);

    s_host_lock_unavailable = true;
    assert(si_agent_request_broker_terminalize_and_retire_run(
               "run-terminalize") == ESP_ERR_TIMEOUT);
    s_host_lock_unavailable = false;
    si_agent_request_status_t preserved = {0};
    assert(si_agent_request_broker_get_by_id(waiting.request_id, &preserved) ==
           ESP_OK);
    assert(preserved.state == SI_AGENT_REQUEST_WAITING_USER);
    assert(si_agent_request_broker_get_by_id(executing.request_id,
                                             &preserved) == ESP_OK);
    assert(preserved.state == SI_AGENT_REQUEST_EXECUTING);

    assert(si_agent_request_broker_terminalize_and_retire_run(
               "run-terminalize") == ESP_OK);
    assert(si_agent_request_broker_get_by_id(waiting.request_id, &preserved) ==
           ESP_ERR_NOT_FOUND);
    assert(si_agent_request_broker_get_by_id(executing.request_id,
                                             &preserved) ==
           ESP_ERR_NOT_FOUND);
    assert(si_agent_request_broker_get_by_id(other.request_id, &preserved) ==
           ESP_OK);
    assert(preserved.state == SI_AGENT_REQUEST_WAITING_USER);
    assert(si_agent_request_broker_terminalize_and_retire_run(
               "run-terminalize") == ESP_OK);

    input = make_action(
        "run-terminalize-reuse", "step-1", 1U, "{}", "idem-term-reuse");
    assert(si_agent_request_broker_create_v2(&input, &preserved) == ESP_OK);
}

static void test_snapshot_restore_fails_closed(void)
{
    reset_broker();
    si_agent_request_v2_input_t input = make_action(
        "run-restore", "step-1", 1U, "{}", "idem-restore");
    si_agent_request_status_t status = {0};
    assert(si_agent_request_broker_create_v2(&input, &status) == ESP_OK);
    si_agent_request_decision_t decision = browser_decision(
        status.request_id, true, SI_AGENT_REQUEST_GRANT_ONCE);
    assert(si_agent_request_broker_decide_v2(&decision, &status) == ESP_OK);
    si_agent_request_consume_t consume = make_consume(&status, 1U, "{}");
    assert(si_agent_request_broker_consume(&consume, &status) == ESP_OK);

    si_agent_request_snapshot_t snapshot = {0};
    assert(si_agent_request_broker_export_snapshot(&snapshot) == ESP_OK);
    s_test_now_ms = 2000U;
    assert(si_agent_request_broker_import_snapshot(&snapshot) == ESP_OK);
    assert(si_agent_request_broker_get_by_id(status.request_id, &status) == ESP_OK);
    assert(status.state == SI_AGENT_REQUEST_UNKNOWN);
}

int main(void)
{
    test_queue_capacity_and_multiple_runs();
    test_waiting_and_granted_expiry();
    test_requester_cannot_approve_self();
    test_once_grant_requires_atomic_consume();
    test_args_or_plan_change_invalidates_grant();
    test_reviewer_is_separate_and_run_grant_is_narrowed();
    test_delegated_access_mode_thresholds();
    test_cancel_run_covers_all_requests();
    test_retire_run_is_atomic_and_run_scoped();
    test_terminalize_and_retire_run_is_atomic_and_retryable();
    test_snapshot_restore_fails_closed();
    puts("agent request broker tests: PASS");
    return 0;
}
