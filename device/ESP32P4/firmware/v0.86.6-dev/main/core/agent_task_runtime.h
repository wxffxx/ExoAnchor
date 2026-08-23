#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SI_AGENT_TASK_ID_MAX 48U
#define SI_AGENT_TASK_PENDING_CAPACITY 8U
#define SI_AGENT_TASK_STEP_CAPACITY 16U

typedef enum {
    SI_AGENT_TASK_OK = 0,
    SI_AGENT_TASK_ERR_INVALID_ARGUMENT,
    SI_AGENT_TASK_ERR_INVALID_ID,
    SI_AGENT_TASK_ERR_TIME_REGRESSION,
    SI_AGENT_TASK_ERR_QUEUE_FULL,
    SI_AGENT_TASK_ERR_NO_ACTIVE_TURN,
    SI_AGENT_TASK_ERR_CONFLICT,
    SI_AGENT_TASK_ERR_INVALID_TRANSITION,
    SI_AGENT_TASK_ERR_CAPACITY,
    SI_AGENT_TASK_ERR_NOT_FOUND,
    SI_AGENT_TASK_ERR_VERIFICATION_REQUIRED,
} si_agent_task_result_t;

typedef enum {
    SI_AGENT_THREAD_ACTIVE = 0,
    SI_AGENT_THREAD_CLOSED,
} si_agent_thread_status_t;

typedef enum {
    SI_AGENT_TURN_QUEUED = 0,
    SI_AGENT_TURN_UNDERSTANDING,
    SI_AGENT_TURN_PLANNING,
    SI_AGENT_TURN_GATHERING_CONTEXT,
    SI_AGENT_TURN_EXECUTING,
    SI_AGENT_TURN_WAITING_REQUEST,
    SI_AGENT_TURN_WAITING_USER,
    SI_AGENT_TURN_PAUSED,
    SI_AGENT_TURN_VERIFYING,
    SI_AGENT_TURN_BLOCKED,
    SI_AGENT_TURN_COMPLETED,
    SI_AGENT_TURN_FAILED,
    SI_AGENT_TURN_INTERRUPTED,
    SI_AGENT_TURN_CANCELLED,
    SI_AGENT_TURN_OUTCOME_UNKNOWN,
} si_agent_turn_status_t;

typedef enum {
    SI_AGENT_RUN_ATTEMPT_RUNNING = 0,
    SI_AGENT_RUN_ATTEMPT_PAUSED,
    SI_AGENT_RUN_ATTEMPT_COMPLETED,
    SI_AGENT_RUN_ATTEMPT_FAILED,
    SI_AGENT_RUN_ATTEMPT_INTERRUPTED,
    SI_AGENT_RUN_ATTEMPT_CANCELLED,
    SI_AGENT_RUN_ATTEMPT_OUTCOME_UNKNOWN,
} si_agent_run_attempt_status_t;

typedef enum {
    SI_AGENT_PLAN_ACTIVE = 0,
    SI_AGENT_PLAN_COMPLETED,
    SI_AGENT_PLAN_FAILED,
    SI_AGENT_PLAN_OUTCOME_UNKNOWN,
} si_agent_plan_status_t;

typedef enum {
    SI_AGENT_STEP_PENDING = 0,
    SI_AGENT_STEP_IN_PROGRESS,
    SI_AGENT_STEP_COMPLETED,
    SI_AGENT_STEP_BLOCKED,
    SI_AGENT_STEP_SKIPPED,
    SI_AGENT_STEP_FAILED,
    SI_AGENT_STEP_OUTCOME_UNKNOWN,
} si_agent_step_status_t;

typedef enum {
    SI_AGENT_VERIFICATION_UNRECORDED = 0,
    SI_AGENT_VERIFICATION_PASSED,
    SI_AGENT_VERIFICATION_FAILED,
    SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN,
} si_agent_verification_status_t;

typedef struct {
    char thread_id[SI_AGENT_TASK_ID_MAX + 1U];
    si_agent_thread_status_t status;
    uint64_t created_at_ms;
    uint64_t updated_at_ms;
} si_agent_task_thread_t;

typedef struct {
    char run_attempt_id[SI_AGENT_TASK_ID_MAX + 1U];
    uint32_t attempt_number;
    si_agent_run_attempt_status_t status;
    uint64_t started_at_ms;
    uint64_t updated_at_ms;
    uint64_t ended_at_ms;
} si_agent_task_run_attempt_t;

typedef struct {
    char step_id[SI_AGENT_TASK_ID_MAX + 1U];
    si_agent_step_status_t status;
    si_agent_verification_status_t verification;
    uint64_t created_at_ms;
    uint64_t updated_at_ms;
} si_agent_task_step_t;

typedef struct {
    char plan_id[SI_AGENT_TASK_ID_MAX + 1U];
    uint32_t version;
    si_agent_plan_status_t status;
    size_t step_count;
    si_agent_task_step_t steps[SI_AGENT_TASK_STEP_CAPACITY];
    uint64_t created_at_ms;
    uint64_t updated_at_ms;
} si_agent_task_plan_t;

typedef struct {
    char thread_id[SI_AGENT_TASK_ID_MAX + 1U];
    char turn_id[SI_AGENT_TASK_ID_MAX + 1U];
    si_agent_turn_status_t status;
    si_agent_turn_status_t resume_status;
    uint32_t intent_revision;
    uint32_t plan_version;
    si_agent_verification_status_t verification;
    si_agent_task_run_attempt_t run_attempt;
    bool has_plan;
    si_agent_task_plan_t plan;
    uint64_t created_at_ms;
    uint64_t updated_at_ms;
    uint64_t ended_at_ms;
} si_agent_task_turn_t;

typedef struct {
    char thread_id[SI_AGENT_TASK_ID_MAX + 1U];
    char turn_id[SI_AGENT_TASK_ID_MAX + 1U];
    char run_attempt_id[SI_AGENT_TASK_ID_MAX + 1U];
    uint64_t submitted_at_ms;
} si_agent_task_pending_turn_t;

typedef struct {
    char runtime_id[SI_AGENT_TASK_ID_MAX + 1U];
    uint64_t created_at_ms;
    uint64_t updated_at_ms;
    bool has_active_turn;
    si_agent_task_turn_t active_turn;
    si_agent_task_pending_turn_t pending[SI_AGENT_TASK_PENDING_CAPACITY];
    size_t pending_head;
    size_t pending_count;
} si_agent_task_runtime_t;

si_agent_task_result_t si_agent_task_runtime_init(
    si_agent_task_runtime_t *runtime,
    const char *runtime_id,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_submit(
    si_agent_task_runtime_t *runtime,
    const char *thread_id,
    const char *turn_id,
    const char *run_attempt_id,
    uint64_t now_ms,
    bool *started_out);

/* Retires a terminal active Turn and starts the oldest queued Turn, if any. */
si_agent_task_result_t si_agent_task_runtime_advance_queue(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms,
    bool *started_out);

const si_agent_task_turn_t *si_agent_task_runtime_active(
    const si_agent_task_runtime_t *runtime);

const si_agent_task_pending_turn_t *si_agent_task_runtime_pending_at(
    const si_agent_task_runtime_t *runtime,
    size_t index);

size_t si_agent_task_runtime_pending_count(
    const si_agent_task_runtime_t *runtime);

si_agent_task_result_t si_agent_task_runtime_set_phase(
    si_agent_task_runtime_t *runtime,
    si_agent_turn_status_t status,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_begin_plan(
    si_agent_task_runtime_t *runtime,
    const char *plan_id,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_add_step(
    si_agent_task_runtime_t *runtime,
    const char *step_id,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_set_step_status(
    si_agent_task_runtime_t *runtime,
    const char *step_id,
    si_agent_step_status_t status,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_record_step_verification(
    si_agent_task_runtime_t *runtime,
    const char *step_id,
    si_agent_verification_status_t verification,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_record_turn_verification(
    si_agent_task_runtime_t *runtime,
    si_agent_verification_status_t verification,
    uint64_t now_ms);

/* Steer invalidates the active Plan and advances both visible revisions. */
si_agent_task_result_t si_agent_task_runtime_steer(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_pause(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_resume(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_cancel(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_interrupt(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_mark_outcome_unknown(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_fail(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_complete(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms);

si_agent_task_result_t si_agent_task_runtime_retry(
    si_agent_task_runtime_t *runtime,
    const char *run_attempt_id,
    uint64_t now_ms);

bool si_agent_turn_status_is_terminal(si_agent_turn_status_t status);
const char *si_agent_turn_status_name(si_agent_turn_status_t status);
const char *si_agent_task_result_name(si_agent_task_result_t result);
