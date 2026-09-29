#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "agent_task_runtime.h"

static void assert_result(si_agent_task_result_t actual,
                          si_agent_task_result_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "expected %s, got %s\n",
                si_agent_task_result_name(expected),
                si_agent_task_result_name(actual));
        assert(actual == expected);
    }
}

static void init_with_active(si_agent_task_runtime_t *runtime)
{
    bool started = false;
    assert_result(si_agent_task_runtime_init(runtime, "runtime-1", 100U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_submit(runtime, "thread-1", "turn-1",
                                               "run-1", 101U, &started),
                  SI_AGENT_TASK_OK);
    assert(started);
}

static void test_validation_and_server_time(void)
{
    si_agent_task_runtime_t runtime;
    bool started = true;

    assert_result(si_agent_task_runtime_init(NULL, "runtime-1", 1U),
                  SI_AGENT_TASK_ERR_INVALID_ARGUMENT);
    assert_result(si_agent_task_runtime_init(&runtime, "", 1U),
                  SI_AGENT_TASK_ERR_INVALID_ID);
    assert_result(si_agent_task_runtime_init(&runtime, "bad id", 1U),
                  SI_AGENT_TASK_ERR_INVALID_ID);
    assert_result(si_agent_task_runtime_init(&runtime, "runtime-1", 10U),
                  SI_AGENT_TASK_OK);
    assert(strcmp(runtime.runtime_id, "runtime-1") == 0);
    assert(runtime.created_at_ms == 10U);
    assert(si_agent_task_runtime_active(&runtime) == NULL);

    assert_result(si_agent_task_runtime_submit(&runtime, "thread-1", "turn-1",
                                               "run-1", 9U, &started),
                  SI_AGENT_TASK_ERR_TIME_REGRESSION);
    assert(!started);
    assert_result(si_agent_task_runtime_submit(&runtime, "thread-1", "turn-1",
                                               "run-1", 11U, &started),
                  SI_AGENT_TASK_OK);
    assert(started);
    assert(strcmp(runtime.active_turn.thread_id, "thread-1") == 0);
    assert(runtime.active_turn.intent_revision == 1U);
    assert(runtime.active_turn.plan_version == 1U);
    assert(runtime.active_turn.run_attempt.attempt_number == 1U);
    assert(runtime.active_turn.created_at_ms == 11U);
    assert(strcmp(si_agent_turn_status_name(runtime.active_turn.status),
                  "understanding") == 0);
}

static void test_multithread_fifo_and_capacity(void)
{
    si_agent_task_runtime_t runtime;
    bool started = false;
    const char *threads[SI_AGENT_TASK_PENDING_CAPACITY] = {
        "thread-b", "thread-c", "thread-d", "thread-e",
        "thread-f", "thread-g", "thread-h", "thread-i",
    };
    const char *turns[SI_AGENT_TASK_PENDING_CAPACITY] = {
        "turn-b", "turn-c", "turn-d", "turn-e",
        "turn-f", "turn-g", "turn-h", "turn-i",
    };
    const char *runs[SI_AGENT_TASK_PENDING_CAPACITY] = {
        "run-b", "run-c", "run-d", "run-e",
        "run-f", "run-g", "run-h", "run-i",
    };

    assert_result(si_agent_task_runtime_init(&runtime, "runtime-fifo", 1U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_submit(&runtime, "thread-a", "turn-a",
                                               "run-a", 2U, &started),
                  SI_AGENT_TASK_OK);
    assert(started);

    for (size_t i = 0U; i < SI_AGENT_TASK_PENDING_CAPACITY; i++) {
        started = true;
        assert_result(si_agent_task_runtime_submit(
                          &runtime, threads[i], turns[i], runs[i], 3U + i,
                          &started),
                      SI_AGENT_TASK_OK);
        assert(!started);
    }
    assert(si_agent_task_runtime_pending_count(&runtime) ==
           SI_AGENT_TASK_PENDING_CAPACITY);
    for (size_t i = 0U; i < SI_AGENT_TASK_PENDING_CAPACITY; i++) {
        const si_agent_task_pending_turn_t *pending =
            si_agent_task_runtime_pending_at(&runtime, i);
        assert(pending != NULL);
        assert(strcmp(pending->thread_id, threads[i]) == 0);
        assert(strcmp(pending->turn_id, turns[i]) == 0);
    }
    assert(si_agent_task_runtime_pending_at(
               &runtime, SI_AGENT_TASK_PENDING_CAPACITY) == NULL);

    assert_result(si_agent_task_runtime_submit(&runtime, "thread-j", "turn-j",
                                               "run-j", 20U, &started),
                  SI_AGENT_TASK_ERR_QUEUE_FULL);
    assert_result(si_agent_task_runtime_submit(&runtime, "thread-x", "turn-a",
                                               "run-x", 20U, &started),
                  SI_AGENT_TASK_ERR_CONFLICT);

    assert_result(si_agent_task_runtime_advance_queue(&runtime, 21U, &started),
                  SI_AGENT_TASK_ERR_CONFLICT);
    assert_result(si_agent_task_runtime_cancel(&runtime, 22U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_advance_queue(&runtime, 23U, &started),
                  SI_AGENT_TASK_OK);
    assert(started);
    assert(strcmp(runtime.active_turn.thread_id, "thread-b") == 0);
    assert(strcmp(runtime.active_turn.turn_id, "turn-b") == 0);
    assert(runtime.active_turn.created_at_ms == 3U);
    assert(runtime.active_turn.run_attempt.started_at_ms == 23U);
    assert(si_agent_task_runtime_pending_count(&runtime) == 7U);

    /* The freed ring slot is reusable without disturbing FIFO order. */
    assert_result(si_agent_task_runtime_submit(&runtime, "thread-j", "turn-j",
                                               "run-j", 24U, &started),
                  SI_AGENT_TASK_OK);
    assert(!started);
    assert(si_agent_task_runtime_pending_count(&runtime) == 8U);
    assert(strcmp(si_agent_task_runtime_pending_at(&runtime, 0U)->thread_id,
                  "thread-c") == 0);
    assert(strcmp(si_agent_task_runtime_pending_at(&runtime, 7U)->thread_id,
                  "thread-j") == 0);
}

static void test_plan_steps_pause_resume_and_steer(void)
{
    si_agent_task_runtime_t runtime;
    init_with_active(&runtime);

    assert_result(si_agent_task_runtime_set_phase(
                      &runtime, SI_AGENT_TURN_COMPLETED, 102U),
                  SI_AGENT_TASK_ERR_INVALID_TRANSITION);
    assert_result(si_agent_task_runtime_set_phase(
                      &runtime, SI_AGENT_TURN_PLANNING, 102U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_begin_plan(&runtime, "plan-1", 103U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_add_step(&runtime, "step-1", 104U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_add_step(&runtime, "step-2", 105U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_add_step(&runtime, "step-1", 106U),
                  SI_AGENT_TASK_ERR_CONFLICT);

    assert_result(si_agent_task_runtime_set_phase(
                      &runtime, SI_AGENT_TURN_EXECUTING, 106U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_set_step_status(
                      &runtime, "step-1", SI_AGENT_STEP_IN_PROGRESS, 107U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_set_step_status(
                      &runtime, "step-2", SI_AGENT_STEP_IN_PROGRESS, 108U),
                  SI_AGENT_TASK_ERR_CONFLICT);
    assert_result(si_agent_task_runtime_pause(&runtime, 109U),
                  SI_AGENT_TASK_OK);
    assert(runtime.active_turn.status == SI_AGENT_TURN_PAUSED);
    assert(runtime.active_turn.run_attempt.status ==
           SI_AGENT_RUN_ATTEMPT_PAUSED);
    assert_result(si_agent_task_runtime_set_step_status(
                      &runtime, "step-1", SI_AGENT_STEP_BLOCKED, 110U),
                  SI_AGENT_TASK_ERR_INVALID_TRANSITION);

    assert_result(si_agent_task_runtime_steer(&runtime, 111U),
                  SI_AGENT_TASK_OK);
    assert(runtime.active_turn.status == SI_AGENT_TURN_PAUSED);
    assert(runtime.active_turn.resume_status == SI_AGENT_TURN_PLANNING);
    assert(runtime.active_turn.intent_revision == 2U);
    assert(runtime.active_turn.plan_version == 2U);
    assert(!runtime.active_turn.has_plan);
    assert_result(si_agent_task_runtime_resume(&runtime, 112U),
                  SI_AGENT_TASK_OK);
    assert(runtime.active_turn.status == SI_AGENT_TURN_PLANNING);
    assert(runtime.active_turn.run_attempt.status ==
           SI_AGENT_RUN_ATTEMPT_RUNNING);
    assert_result(si_agent_task_runtime_begin_plan(&runtime, "plan-2", 113U),
                  SI_AGENT_TASK_OK);
    assert(runtime.active_turn.plan.version == 2U);
    assert(strcmp(runtime.active_turn.plan.plan_id, "plan-2") == 0);
}

static void test_verification_gate(void)
{
    si_agent_task_runtime_t runtime;
    init_with_active(&runtime);

    assert_result(si_agent_task_runtime_set_phase(
                      &runtime, SI_AGENT_TURN_PLANNING, 102U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_begin_plan(&runtime, "plan-1", 103U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_add_step(&runtime, "step-1", 104U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_add_step(&runtime, "step-2", 105U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_set_phase(
                      &runtime, SI_AGENT_TURN_EXECUTING, 106U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_set_step_status(
                      &runtime, "step-1", SI_AGENT_STEP_IN_PROGRESS, 107U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_set_step_status(
                      &runtime, "step-1", SI_AGENT_STEP_COMPLETED, 108U),
                  SI_AGENT_TASK_ERR_VERIFICATION_REQUIRED);
    assert_result(si_agent_task_runtime_record_step_verification(
                      &runtime, "step-1", SI_AGENT_VERIFICATION_FAILED, 109U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_set_step_status(
                      &runtime, "step-1", SI_AGENT_STEP_COMPLETED, 110U),
                  SI_AGENT_TASK_ERR_VERIFICATION_REQUIRED);
    assert_result(si_agent_task_runtime_record_step_verification(
                      &runtime, "step-1", SI_AGENT_VERIFICATION_PASSED, 111U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_set_step_status(
                      &runtime, "step-1", SI_AGENT_STEP_COMPLETED, 112U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_set_step_status(
                      &runtime, "step-2", SI_AGENT_STEP_SKIPPED, 113U),
                  SI_AGENT_TASK_OK);
    assert(runtime.active_turn.plan.status == SI_AGENT_PLAN_COMPLETED);

    assert_result(si_agent_task_runtime_set_phase(
                      &runtime, SI_AGENT_TURN_VERIFYING, 114U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_complete(&runtime, 115U),
                  SI_AGENT_TASK_ERR_VERIFICATION_REQUIRED);
    assert_result(si_agent_task_runtime_record_turn_verification(
                      &runtime, SI_AGENT_VERIFICATION_PASSED, 116U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_complete(&runtime, 117U),
                  SI_AGENT_TASK_OK);
    assert(runtime.active_turn.status == SI_AGENT_TURN_COMPLETED);
    assert(runtime.active_turn.run_attempt.status ==
           SI_AGENT_RUN_ATTEMPT_COMPLETED);
    assert(runtime.active_turn.ended_at_ms == 117U);
}

static void test_simple_turn_still_needs_verification(void)
{
    si_agent_task_runtime_t runtime;
    init_with_active(&runtime);
    assert_result(si_agent_task_runtime_set_phase(
                      &runtime, SI_AGENT_TURN_VERIFYING, 102U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_complete(&runtime, 103U),
                  SI_AGENT_TASK_ERR_VERIFICATION_REQUIRED);
    assert_result(si_agent_task_runtime_record_turn_verification(
                      &runtime, SI_AGENT_VERIFICATION_PASSED, 104U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_complete(&runtime, 105U),
                  SI_AGENT_TASK_OK);
}

static void test_outcome_unknown_and_retry(void)
{
    si_agent_task_runtime_t runtime;
    init_with_active(&runtime);

    assert_result(si_agent_task_runtime_set_phase(
                      &runtime, SI_AGENT_TURN_PLANNING, 102U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_begin_plan(&runtime, "plan-1", 103U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_add_step(&runtime, "write-1", 104U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_set_phase(
                      &runtime, SI_AGENT_TURN_EXECUTING, 105U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_set_step_status(
                      &runtime, "write-1", SI_AGENT_STEP_IN_PROGRESS, 106U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_record_step_verification(
                      &runtime, "write-1",
                      SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN, 107U),
                  SI_AGENT_TASK_OK);
    assert(runtime.active_turn.status == SI_AGENT_TURN_OUTCOME_UNKNOWN);
    assert(runtime.active_turn.run_attempt.status ==
           SI_AGENT_RUN_ATTEMPT_OUTCOME_UNKNOWN);
    assert(runtime.active_turn.plan.steps[0].status ==
           SI_AGENT_STEP_OUTCOME_UNKNOWN);
    assert_result(si_agent_task_runtime_complete(&runtime, 108U),
                  SI_AGENT_TASK_ERR_INVALID_TRANSITION);

    assert_result(si_agent_task_runtime_retry(&runtime, "run-2", 109U),
                  SI_AGENT_TASK_OK);
    assert(strcmp(runtime.active_turn.thread_id, "thread-1") == 0);
    assert(strcmp(runtime.active_turn.turn_id, "turn-1") == 0);
    assert(strcmp(runtime.active_turn.run_attempt.run_attempt_id,
                  "run-2") == 0);
    assert(runtime.active_turn.run_attempt.attempt_number == 2U);
    assert(runtime.active_turn.status == SI_AGENT_TURN_UNDERSTANDING);
    assert(runtime.active_turn.plan_version == 2U);
    assert(!runtime.active_turn.has_plan);
    assert(runtime.active_turn.verification ==
           SI_AGENT_VERIFICATION_UNRECORDED);
}

static void test_interrupt_cancel_and_fail_are_explicit(void)
{
    si_agent_task_runtime_t runtime;
    bool started = false;

    init_with_active(&runtime);
    assert_result(si_agent_task_runtime_interrupt(&runtime, 102U),
                  SI_AGENT_TASK_OK);
    assert(runtime.active_turn.status == SI_AGENT_TURN_INTERRUPTED);
    assert_result(si_agent_task_runtime_pause(&runtime, 103U),
                  SI_AGENT_TASK_ERR_INVALID_TRANSITION);
    assert_result(si_agent_task_runtime_retry(&runtime, "run-2", 104U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_fail(&runtime, 105U),
                  SI_AGENT_TASK_OK);
    assert(runtime.active_turn.status == SI_AGENT_TURN_FAILED);
    assert_result(si_agent_task_runtime_retry(&runtime, "run-3", 106U),
                  SI_AGENT_TASK_OK);
    assert_result(si_agent_task_runtime_cancel(&runtime, 107U),
                  SI_AGENT_TASK_OK);
    assert(runtime.active_turn.status == SI_AGENT_TURN_CANCELLED);
    assert_result(si_agent_task_runtime_advance_queue(&runtime, 108U, &started),
                  SI_AGENT_TASK_OK);
    assert(!started);
    assert(si_agent_task_runtime_active(&runtime) == NULL);
}

int main(void)
{
    test_validation_and_server_time();
    test_multithread_fifo_and_capacity();
    test_plan_steps_pause_resume_and_steer();
    test_verification_gate();
    test_simple_turn_still_needs_verification();
    test_outcome_unknown_and_retry();
    test_interrupt_cancel_and_fail_are_explicit();
    puts("agent task runtime tests: OK");
    return 0;
}
