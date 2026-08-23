/* Cancellation/action linearization acceptance probe. */
#define main exoanchor_existing_task_service_test_main
#include "test_agent_task_service.c"
#undef main

static const char *const k_args_hash =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

static unsigned s_gate_failures;

static void gate_expect_terminal(const char *scenario,
                                 const char *expected_event_type)
{
    const si_agent_event_record_t *terminal =
        &s_fake_events[s_fake_event_count - 1U].record;
    if (strcmp(terminal->event_type, expected_event_type) != 0) {
        fprintf(stderr,
                "acceptance gate: FAIL: %s ended as %s, expected %s\n",
                scenario, terminal->event_type, expected_event_type);
        s_gate_failures++;
    }
}

static void test_cancel_before_effect_is_known_cancel(void)
{
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(3000U);
    prepare_active_step(&receipt);
    assert(si_agent_task_service_cancel(receipt.run_id) == ESP_OK);
    gate_expect_terminal("cancel before action.started", "turn.cancelled");

    const size_t before_reboot = s_fake_event_count;
    reset_task_service(false);
    assert(si_agent_task_service_start() == ESP_OK);
    if (s_fake_event_count != before_reboot ||
        event_count("turn.outcome_unknown") != 0U) {
        fprintf(stderr,
                "acceptance gate: FAIL: pre-effect cancel changed during reboot\n");
        s_gate_failures++;
    }
}

static void test_cancel_after_started_is_unknown(void)
{
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(3001U);
    prepare_active_step(&receipt);
    assert(si_agent_task_service_record_action_started(
               receipt.run_id, "step-host", "action-cancel-gate",
               "uart_write", k_args_hash, "idem-cancel-gate", true) ==
           ESP_OK);
    assert(si_agent_task_service_cancel(receipt.run_id) == ESP_OK);
    gate_expect_terminal("cancel after durable action.started",
                         "turn.outcome_unknown");
}

static void test_result_then_cancel_is_unknown(void)
{
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(3002U);
    prepare_active_step(&receipt);
    assert(si_agent_task_service_record_action_started(
               receipt.run_id, "step-host", "action-result-first",
               "uart_write", k_args_hash, "idem-result-first", true) ==
           ESP_OK);
    assert(si_agent_task_service_record_action_result(
               receipt.run_id, "step-host", "action-result-first", true,
               NULL, "execution_returned") == ESP_OK);
    assert(si_agent_task_service_cancel(receipt.run_id) == ESP_OK);
    gate_expect_terminal("durable action.result then cancel",
                         "turn.outcome_unknown");
}

static void test_cancel_then_late_result_is_unknown(void)
{
    reset_task_service(true);
    si_agent_task_receipt_t receipt = submit_task(3003U);
    prepare_active_step(&receipt);
    assert(si_agent_task_service_record_action_started(
               receipt.run_id, "step-host", "action-cancel-first",
               "uart_write", k_args_hash, "idem-cancel-first", true) ==
           ESP_OK);
    assert(si_agent_task_service_cancel(receipt.run_id) == ESP_OK);
    assert(si_agent_task_service_record_action_result(
               receipt.run_id, "step-host", "action-cancel-first", true,
               NULL, "execution_returned") == ESP_ERR_INVALID_STATE);
    gate_expect_terminal("cancel won race with late action.result",
                         "turn.outcome_unknown");
}

int main(void)
{
    /* The Task Service mutex linearizes a cancel/result race.  Exercising both
     * sequential orders therefore covers both possible race winners without a
     * timing-dependent host test. */
    test_cancel_before_effect_is_known_cancel();
    test_cancel_after_started_is_unknown();
    test_result_then_cancel_is_unknown();
    test_cancel_then_late_result_is_unknown();

    if (s_gate_failures != 0U) {
        fprintf(stderr, "cancel/action acceptance gate: %u failure(s)\n",
                s_gate_failures);
        return 1;
    }
    puts("cancel/action acceptance gate: PASS");
    return 0;
}
