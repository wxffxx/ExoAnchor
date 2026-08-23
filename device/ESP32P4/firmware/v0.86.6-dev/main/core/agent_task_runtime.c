#include "agent_task_runtime.h"

#include <limits.h>
#include <string.h>

static bool id_valid(const char *value)
{
    if (!value || value[0] == '\0') {
        return false;
    }

    size_t length = 0U;
    while (value[length] != '\0') {
        unsigned char ch = (unsigned char)value[length];
        if (length >= SI_AGENT_TASK_ID_MAX || ch <= 0x20U || ch >= 0x7fU) {
            return false;
        }
        length++;
    }
    return true;
}

static void copy_id(char out[SI_AGENT_TASK_ID_MAX + 1U], const char *value)
{
    size_t length = strlen(value);
    memcpy(out, value, length + 1U);
}

static bool runtime_initialized(const si_agent_task_runtime_t *runtime)
{
    return runtime && runtime->runtime_id[0] != '\0';
}

static bool time_valid(const si_agent_task_runtime_t *runtime, uint64_t now_ms)
{
    return runtime_initialized(runtime) && now_ms >= runtime->updated_at_ms;
}

bool si_agent_turn_status_is_terminal(si_agent_turn_status_t status)
{
    return status == SI_AGENT_TURN_COMPLETED ||
           status == SI_AGENT_TURN_FAILED ||
           status == SI_AGENT_TURN_INTERRUPTED ||
           status == SI_AGENT_TURN_CANCELLED ||
           status == SI_AGENT_TURN_OUTCOME_UNKNOWN;
}

static bool phase_status_valid(si_agent_turn_status_t status)
{
    return status == SI_AGENT_TURN_UNDERSTANDING ||
           status == SI_AGENT_TURN_PLANNING ||
           status == SI_AGENT_TURN_GATHERING_CONTEXT ||
           status == SI_AGENT_TURN_EXECUTING ||
           status == SI_AGENT_TURN_WAITING_REQUEST ||
           status == SI_AGENT_TURN_WAITING_USER ||
           status == SI_AGENT_TURN_VERIFYING ||
           status == SI_AGENT_TURN_BLOCKED;
}

static bool phase_transition_allowed(si_agent_turn_status_t from,
                                     si_agent_turn_status_t to)
{
    if (from == to) {
        return true;
    }

    switch (from) {
    case SI_AGENT_TURN_UNDERSTANDING:
        return to == SI_AGENT_TURN_PLANNING ||
               to == SI_AGENT_TURN_GATHERING_CONTEXT ||
               to == SI_AGENT_TURN_EXECUTING ||
               to == SI_AGENT_TURN_WAITING_REQUEST ||
               to == SI_AGENT_TURN_WAITING_USER ||
               to == SI_AGENT_TURN_VERIFYING ||
               to == SI_AGENT_TURN_BLOCKED;
    case SI_AGENT_TURN_PLANNING:
        return to == SI_AGENT_TURN_GATHERING_CONTEXT ||
               to == SI_AGENT_TURN_EXECUTING ||
               to == SI_AGENT_TURN_WAITING_REQUEST ||
               to == SI_AGENT_TURN_WAITING_USER ||
               to == SI_AGENT_TURN_VERIFYING ||
               to == SI_AGENT_TURN_BLOCKED;
    case SI_AGENT_TURN_GATHERING_CONTEXT:
        return to == SI_AGENT_TURN_PLANNING ||
               to == SI_AGENT_TURN_EXECUTING ||
               to == SI_AGENT_TURN_WAITING_REQUEST ||
               to == SI_AGENT_TURN_WAITING_USER ||
               to == SI_AGENT_TURN_VERIFYING ||
               to == SI_AGENT_TURN_BLOCKED;
    case SI_AGENT_TURN_EXECUTING:
        return to == SI_AGENT_TURN_PLANNING ||
               to == SI_AGENT_TURN_GATHERING_CONTEXT ||
               to == SI_AGENT_TURN_WAITING_REQUEST ||
               to == SI_AGENT_TURN_WAITING_USER ||
               to == SI_AGENT_TURN_VERIFYING ||
               to == SI_AGENT_TURN_BLOCKED;
    case SI_AGENT_TURN_WAITING_REQUEST:
        return to == SI_AGENT_TURN_PLANNING ||
               to == SI_AGENT_TURN_GATHERING_CONTEXT ||
               to == SI_AGENT_TURN_EXECUTING ||
               to == SI_AGENT_TURN_WAITING_USER ||
               to == SI_AGENT_TURN_BLOCKED;
    case SI_AGENT_TURN_WAITING_USER:
        return to == SI_AGENT_TURN_PLANNING ||
               to == SI_AGENT_TURN_GATHERING_CONTEXT ||
               to == SI_AGENT_TURN_EXECUTING ||
               to == SI_AGENT_TURN_WAITING_REQUEST ||
               to == SI_AGENT_TURN_BLOCKED;
    case SI_AGENT_TURN_VERIFYING:
        return to == SI_AGENT_TURN_PLANNING ||
               to == SI_AGENT_TURN_GATHERING_CONTEXT ||
               to == SI_AGENT_TURN_EXECUTING ||
               to == SI_AGENT_TURN_WAITING_REQUEST ||
               to == SI_AGENT_TURN_WAITING_USER ||
               to == SI_AGENT_TURN_BLOCKED;
    case SI_AGENT_TURN_BLOCKED:
        return to == SI_AGENT_TURN_UNDERSTANDING ||
               to == SI_AGENT_TURN_PLANNING ||
               to == SI_AGENT_TURN_GATHERING_CONTEXT ||
               to == SI_AGENT_TURN_EXECUTING ||
               to == SI_AGENT_TURN_WAITING_REQUEST ||
               to == SI_AGENT_TURN_WAITING_USER ||
               to == SI_AGENT_TURN_VERIFYING;
    case SI_AGENT_TURN_QUEUED:
    case SI_AGENT_TURN_PAUSED:
    case SI_AGENT_TURN_COMPLETED:
    case SI_AGENT_TURN_FAILED:
    case SI_AGENT_TURN_INTERRUPTED:
    case SI_AGENT_TURN_CANCELLED:
    case SI_AGENT_TURN_OUTCOME_UNKNOWN:
    default:
        return false;
    }
}

static void set_run_status(si_agent_task_turn_t *turn,
                           si_agent_run_attempt_status_t status,
                           uint64_t now_ms,
                           bool ended)
{
    turn->run_attempt.status = status;
    turn->run_attempt.updated_at_ms = now_ms;
    turn->run_attempt.ended_at_ms = ended ? now_ms : 0U;
}

static void apply_turn_status(si_agent_task_runtime_t *runtime,
                              si_agent_turn_status_t status,
                              uint64_t now_ms)
{
    si_agent_task_turn_t *turn = &runtime->active_turn;
    turn->status = status;
    turn->updated_at_ms = now_ms;
    runtime->updated_at_ms = now_ms;

    switch (status) {
    case SI_AGENT_TURN_PAUSED:
        set_run_status(turn, SI_AGENT_RUN_ATTEMPT_PAUSED, now_ms, false);
        break;
    case SI_AGENT_TURN_COMPLETED:
        turn->ended_at_ms = now_ms;
        set_run_status(turn, SI_AGENT_RUN_ATTEMPT_COMPLETED, now_ms, true);
        break;
    case SI_AGENT_TURN_FAILED:
        turn->ended_at_ms = now_ms;
        set_run_status(turn, SI_AGENT_RUN_ATTEMPT_FAILED, now_ms, true);
        break;
    case SI_AGENT_TURN_INTERRUPTED:
        turn->ended_at_ms = now_ms;
        set_run_status(turn, SI_AGENT_RUN_ATTEMPT_INTERRUPTED, now_ms, true);
        break;
    case SI_AGENT_TURN_CANCELLED:
        turn->ended_at_ms = now_ms;
        set_run_status(turn, SI_AGENT_RUN_ATTEMPT_CANCELLED, now_ms, true);
        break;
    case SI_AGENT_TURN_OUTCOME_UNKNOWN:
        turn->ended_at_ms = now_ms;
        set_run_status(turn, SI_AGENT_RUN_ATTEMPT_OUTCOME_UNKNOWN, now_ms,
                       true);
        break;
    case SI_AGENT_TURN_QUEUED:
    case SI_AGENT_TURN_UNDERSTANDING:
    case SI_AGENT_TURN_PLANNING:
    case SI_AGENT_TURN_GATHERING_CONTEXT:
    case SI_AGENT_TURN_EXECUTING:
    case SI_AGENT_TURN_WAITING_REQUEST:
    case SI_AGENT_TURN_WAITING_USER:
    case SI_AGENT_TURN_VERIFYING:
    case SI_AGENT_TURN_BLOCKED:
    default:
        turn->ended_at_ms = 0U;
        set_run_status(turn, SI_AGENT_RUN_ATTEMPT_RUNNING, now_ms, false);
        break;
    }
}

static void start_turn(si_agent_task_runtime_t *runtime,
                       const si_agent_task_pending_turn_t *submission,
                       uint64_t now_ms)
{
    si_agent_task_turn_t *turn = &runtime->active_turn;
    memset(turn, 0, sizeof(*turn));
    copy_id(turn->thread_id, submission->thread_id);
    copy_id(turn->turn_id, submission->turn_id);
    copy_id(turn->run_attempt.run_attempt_id,
            submission->run_attempt_id);
    turn->status = SI_AGENT_TURN_UNDERSTANDING;
    turn->resume_status = SI_AGENT_TURN_UNDERSTANDING;
    turn->intent_revision = 1U;
    turn->plan_version = 1U;
    turn->verification = SI_AGENT_VERIFICATION_UNRECORDED;
    turn->created_at_ms = submission->submitted_at_ms;
    turn->updated_at_ms = now_ms;
    turn->run_attempt.attempt_number = 1U;
    turn->run_attempt.status = SI_AGENT_RUN_ATTEMPT_RUNNING;
    turn->run_attempt.started_at_ms = now_ms;
    turn->run_attempt.updated_at_ms = now_ms;
    runtime->has_active_turn = true;
    runtime->updated_at_ms = now_ms;
}

static bool id_in_use(const si_agent_task_runtime_t *runtime,
                      const char *turn_id,
                      const char *run_attempt_id)
{
    if (runtime->has_active_turn &&
        (strcmp(runtime->active_turn.turn_id, turn_id) == 0 ||
         strcmp(runtime->active_turn.run_attempt.run_attempt_id,
                run_attempt_id) == 0)) {
        return true;
    }

    for (size_t i = 0U; i < runtime->pending_count; i++) {
        size_t pos = (runtime->pending_head + i) %
                     SI_AGENT_TASK_PENDING_CAPACITY;
        const si_agent_task_pending_turn_t *pending = &runtime->pending[pos];
        if (strcmp(pending->turn_id, turn_id) == 0 ||
            strcmp(pending->run_attempt_id, run_attempt_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool run_attempt_id_in_use(const si_agent_task_runtime_t *runtime,
                                  const char *run_attempt_id)
{
    if (runtime->has_active_turn &&
        strcmp(runtime->active_turn.run_attempt.run_attempt_id,
               run_attempt_id) == 0) {
        return true;
    }
    for (size_t i = 0U; i < runtime->pending_count; i++) {
        size_t pos = (runtime->pending_head + i) %
                     SI_AGENT_TASK_PENDING_CAPACITY;
        if (strcmp(runtime->pending[pos].run_attempt_id, run_attempt_id) == 0) {
            return true;
        }
    }
    return false;
}

static si_agent_task_result_t active_for_update(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms,
    si_agent_task_turn_t **turn_out)
{
    if (!runtime_initialized(runtime)) {
        return SI_AGENT_TASK_ERR_INVALID_ARGUMENT;
    }
    if (!time_valid(runtime, now_ms)) {
        return SI_AGENT_TASK_ERR_TIME_REGRESSION;
    }
    if (!runtime->has_active_turn) {
        return SI_AGENT_TASK_ERR_NO_ACTIVE_TURN;
    }
    if (turn_out) {
        *turn_out = &runtime->active_turn;
    }
    return SI_AGENT_TASK_OK;
}

static si_agent_task_step_t *find_step(si_agent_task_plan_t *plan,
                                       const char *step_id)
{
    for (size_t i = 0U; i < plan->step_count; i++) {
        if (strcmp(plan->steps[i].step_id, step_id) == 0) {
            return &plan->steps[i];
        }
    }
    return NULL;
}

static bool step_transition_allowed(si_agent_step_status_t from,
                                    si_agent_step_status_t to)
{
    if (from == to) {
        return true;
    }

    switch (from) {
    case SI_AGENT_STEP_PENDING:
        return to == SI_AGENT_STEP_IN_PROGRESS ||
               to == SI_AGENT_STEP_SKIPPED;
    case SI_AGENT_STEP_IN_PROGRESS:
        return to == SI_AGENT_STEP_COMPLETED ||
               to == SI_AGENT_STEP_BLOCKED ||
               to == SI_AGENT_STEP_FAILED ||
               to == SI_AGENT_STEP_OUTCOME_UNKNOWN;
    case SI_AGENT_STEP_BLOCKED:
        return to == SI_AGENT_STEP_IN_PROGRESS ||
               to == SI_AGENT_STEP_SKIPPED ||
               to == SI_AGENT_STEP_FAILED ||
               to == SI_AGENT_STEP_OUTCOME_UNKNOWN;
    case SI_AGENT_STEP_OUTCOME_UNKNOWN:
        return to == SI_AGENT_STEP_IN_PROGRESS ||
               to == SI_AGENT_STEP_FAILED;
    case SI_AGENT_STEP_COMPLETED:
    case SI_AGENT_STEP_SKIPPED:
    case SI_AGENT_STEP_FAILED:
    default:
        return false;
    }
}

static bool another_step_in_progress(const si_agent_task_plan_t *plan,
                                     const si_agent_task_step_t *selected)
{
    for (size_t i = 0U; i < plan->step_count; i++) {
        const si_agent_task_step_t *step = &plan->steps[i];
        if (step != selected && step->status == SI_AGENT_STEP_IN_PROGRESS) {
            return true;
        }
    }
    return false;
}

static void refresh_plan_status(si_agent_task_plan_t *plan, uint64_t now_ms)
{
    bool all_finished = plan->step_count > 0U;
    bool has_failed = false;
    bool has_unknown = false;

    for (size_t i = 0U; i < plan->step_count; i++) {
        switch (plan->steps[i].status) {
        case SI_AGENT_STEP_COMPLETED:
        case SI_AGENT_STEP_SKIPPED:
            break;
        case SI_AGENT_STEP_FAILED:
            has_failed = true;
            all_finished = false;
            break;
        case SI_AGENT_STEP_OUTCOME_UNKNOWN:
            has_unknown = true;
            all_finished = false;
            break;
        case SI_AGENT_STEP_PENDING:
        case SI_AGENT_STEP_IN_PROGRESS:
        case SI_AGENT_STEP_BLOCKED:
        default:
            all_finished = false;
            break;
        }
    }

    if (has_unknown) {
        plan->status = SI_AGENT_PLAN_OUTCOME_UNKNOWN;
    } else if (has_failed) {
        plan->status = SI_AGENT_PLAN_FAILED;
    } else if (all_finished) {
        plan->status = SI_AGENT_PLAN_COMPLETED;
    } else {
        plan->status = SI_AGENT_PLAN_ACTIVE;
    }
    plan->updated_at_ms = now_ms;
}

static void set_outcome_unknown(si_agent_task_runtime_t *runtime,
                                uint64_t now_ms)
{
    si_agent_task_turn_t *turn = &runtime->active_turn;
    turn->verification = SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN;
    if (turn->has_plan) {
        for (size_t i = 0U; i < turn->plan.step_count; i++) {
            si_agent_task_step_t *step = &turn->plan.steps[i];
            if (step->status == SI_AGENT_STEP_IN_PROGRESS) {
                step->status = SI_AGENT_STEP_OUTCOME_UNKNOWN;
                step->verification = SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN;
                step->updated_at_ms = now_ms;
            }
        }
        turn->plan.status = SI_AGENT_PLAN_OUTCOME_UNKNOWN;
        turn->plan.updated_at_ms = now_ms;
    }
    apply_turn_status(runtime, SI_AGENT_TURN_OUTCOME_UNKNOWN, now_ms);
}

si_agent_task_result_t si_agent_task_runtime_init(
    si_agent_task_runtime_t *runtime,
    const char *runtime_id,
    uint64_t now_ms)
{
    if (!runtime) {
        return SI_AGENT_TASK_ERR_INVALID_ARGUMENT;
    }
    if (!id_valid(runtime_id)) {
        return SI_AGENT_TASK_ERR_INVALID_ID;
    }

    memset(runtime, 0, sizeof(*runtime));
    copy_id(runtime->runtime_id, runtime_id);
    runtime->created_at_ms = now_ms;
    runtime->updated_at_ms = now_ms;
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_submit(
    si_agent_task_runtime_t *runtime,
    const char *thread_id,
    const char *turn_id,
    const char *run_attempt_id,
    uint64_t now_ms,
    bool *started_out)
{
    if (started_out) {
        *started_out = false;
    }
    if (!runtime_initialized(runtime)) {
        return SI_AGENT_TASK_ERR_INVALID_ARGUMENT;
    }
    if (!id_valid(thread_id) || !id_valid(turn_id) ||
        !id_valid(run_attempt_id)) {
        return SI_AGENT_TASK_ERR_INVALID_ID;
    }
    if (!time_valid(runtime, now_ms)) {
        return SI_AGENT_TASK_ERR_TIME_REGRESSION;
    }
    if (id_in_use(runtime, turn_id, run_attempt_id)) {
        return SI_AGENT_TASK_ERR_CONFLICT;
    }

    si_agent_task_pending_turn_t submission;
    memset(&submission, 0, sizeof(submission));
    copy_id(submission.thread_id, thread_id);
    copy_id(submission.turn_id, turn_id);
    copy_id(submission.run_attempt_id, run_attempt_id);
    submission.submitted_at_ms = now_ms;

    if (!runtime->has_active_turn) {
        start_turn(runtime, &submission, now_ms);
        if (started_out) {
            *started_out = true;
        }
        return SI_AGENT_TASK_OK;
    }
    if (runtime->pending_count >= SI_AGENT_TASK_PENDING_CAPACITY) {
        return SI_AGENT_TASK_ERR_QUEUE_FULL;
    }

    size_t tail = (runtime->pending_head + runtime->pending_count) %
                  SI_AGENT_TASK_PENDING_CAPACITY;
    runtime->pending[tail] = submission;
    runtime->pending_count++;
    runtime->updated_at_ms = now_ms;
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_advance_queue(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms,
    bool *started_out)
{
    if (started_out) {
        *started_out = false;
    }
    if (!runtime_initialized(runtime)) {
        return SI_AGENT_TASK_ERR_INVALID_ARGUMENT;
    }
    if (!time_valid(runtime, now_ms)) {
        return SI_AGENT_TASK_ERR_TIME_REGRESSION;
    }
    if (runtime->has_active_turn &&
        !si_agent_turn_status_is_terminal(runtime->active_turn.status)) {
        return SI_AGENT_TASK_ERR_CONFLICT;
    }

    if (runtime->has_active_turn) {
        memset(&runtime->active_turn, 0, sizeof(runtime->active_turn));
        runtime->has_active_turn = false;
    }
    if (runtime->pending_count == 0U) {
        runtime->updated_at_ms = now_ms;
        return SI_AGENT_TASK_OK;
    }

    si_agent_task_pending_turn_t submission =
        runtime->pending[runtime->pending_head];
    memset(&runtime->pending[runtime->pending_head], 0,
           sizeof(runtime->pending[runtime->pending_head]));
    runtime->pending_head =
        (runtime->pending_head + 1U) % SI_AGENT_TASK_PENDING_CAPACITY;
    runtime->pending_count--;
    start_turn(runtime, &submission, now_ms);
    if (started_out) {
        *started_out = true;
    }
    return SI_AGENT_TASK_OK;
}

const si_agent_task_turn_t *si_agent_task_runtime_active(
    const si_agent_task_runtime_t *runtime)
{
    if (!runtime_initialized(runtime) || !runtime->has_active_turn) {
        return NULL;
    }
    return &runtime->active_turn;
}

const si_agent_task_pending_turn_t *si_agent_task_runtime_pending_at(
    const si_agent_task_runtime_t *runtime,
    size_t index)
{
    if (!runtime_initialized(runtime) || index >= runtime->pending_count) {
        return NULL;
    }
    size_t pos = (runtime->pending_head + index) %
                 SI_AGENT_TASK_PENDING_CAPACITY;
    return &runtime->pending[pos];
}

size_t si_agent_task_runtime_pending_count(
    const si_agent_task_runtime_t *runtime)
{
    return runtime_initialized(runtime) ? runtime->pending_count : 0U;
}

si_agent_task_result_t si_agent_task_runtime_set_phase(
    si_agent_task_runtime_t *runtime,
    si_agent_turn_status_t status,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (!phase_status_valid(status) ||
        !phase_transition_allowed(turn->status, status)) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }

    apply_turn_status(runtime, status, now_ms);
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_begin_plan(
    si_agent_task_runtime_t *runtime,
    const char *plan_id,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (!id_valid(plan_id)) {
        return SI_AGENT_TASK_ERR_INVALID_ID;
    }
    if (turn->status != SI_AGENT_TURN_PLANNING) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }
    if (turn->has_plan) {
        if (turn->plan_version == UINT32_MAX) {
            return SI_AGENT_TASK_ERR_CONFLICT;
        }
        turn->plan_version++;
    }

    memset(&turn->plan, 0, sizeof(turn->plan));
    copy_id(turn->plan.plan_id, plan_id);
    turn->plan.version = turn->plan_version;
    turn->plan.status = SI_AGENT_PLAN_ACTIVE;
    turn->plan.created_at_ms = now_ms;
    turn->plan.updated_at_ms = now_ms;
    turn->has_plan = true;
    turn->verification = SI_AGENT_VERIFICATION_UNRECORDED;
    turn->updated_at_ms = now_ms;
    turn->run_attempt.updated_at_ms = now_ms;
    runtime->updated_at_ms = now_ms;
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_add_step(
    si_agent_task_runtime_t *runtime,
    const char *step_id,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (!id_valid(step_id)) {
        return SI_AGENT_TASK_ERR_INVALID_ID;
    }
    if (turn->status != SI_AGENT_TURN_PLANNING || !turn->has_plan ||
        turn->plan.status != SI_AGENT_PLAN_ACTIVE) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }
    if (find_step(&turn->plan, step_id)) {
        return SI_AGENT_TASK_ERR_CONFLICT;
    }
    if (turn->plan.step_count >= SI_AGENT_TASK_STEP_CAPACITY) {
        return SI_AGENT_TASK_ERR_CAPACITY;
    }

    si_agent_task_step_t *step =
        &turn->plan.steps[turn->plan.step_count++];
    memset(step, 0, sizeof(*step));
    copy_id(step->step_id, step_id);
    step->status = SI_AGENT_STEP_PENDING;
    step->verification = SI_AGENT_VERIFICATION_UNRECORDED;
    step->created_at_ms = now_ms;
    step->updated_at_ms = now_ms;
    turn->plan.updated_at_ms = now_ms;
    turn->updated_at_ms = now_ms;
    turn->run_attempt.updated_at_ms = now_ms;
    runtime->updated_at_ms = now_ms;
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_set_step_status(
    si_agent_task_runtime_t *runtime,
    const char *step_id,
    si_agent_step_status_t status,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (!id_valid(step_id)) {
        return SI_AGENT_TASK_ERR_INVALID_ID;
    }
    if (si_agent_turn_status_is_terminal(turn->status) ||
        turn->status == SI_AGENT_TURN_PAUSED || !turn->has_plan) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }
    si_agent_task_step_t *step = find_step(&turn->plan, step_id);
    if (!step) {
        return SI_AGENT_TASK_ERR_NOT_FOUND;
    }
    if (status < SI_AGENT_STEP_PENDING ||
        status > SI_AGENT_STEP_OUTCOME_UNKNOWN ||
        !step_transition_allowed(step->status, status)) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }
    if (status == SI_AGENT_STEP_IN_PROGRESS &&
        another_step_in_progress(&turn->plan, step)) {
        return SI_AGENT_TASK_ERR_CONFLICT;
    }
    if (status == SI_AGENT_STEP_COMPLETED &&
        step->verification != SI_AGENT_VERIFICATION_PASSED) {
        return SI_AGENT_TASK_ERR_VERIFICATION_REQUIRED;
    }
    if (status == SI_AGENT_STEP_OUTCOME_UNKNOWN) {
        step->verification = SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN;
    }

    step->status = status;
    step->updated_at_ms = now_ms;
    refresh_plan_status(&turn->plan, now_ms);
    if (status == SI_AGENT_STEP_OUTCOME_UNKNOWN) {
        set_outcome_unknown(runtime, now_ms);
    } else {
        turn->updated_at_ms = now_ms;
        turn->run_attempt.updated_at_ms = now_ms;
        runtime->updated_at_ms = now_ms;
    }
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_record_step_verification(
    si_agent_task_runtime_t *runtime,
    const char *step_id,
    si_agent_verification_status_t verification,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (!id_valid(step_id)) {
        return SI_AGENT_TASK_ERR_INVALID_ID;
    }
    if (verification <= SI_AGENT_VERIFICATION_UNRECORDED ||
        verification > SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN) {
        return SI_AGENT_TASK_ERR_INVALID_ARGUMENT;
    }
    if (si_agent_turn_status_is_terminal(turn->status) ||
        turn->status == SI_AGENT_TURN_PAUSED || !turn->has_plan) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }

    si_agent_task_step_t *step = find_step(&turn->plan, step_id);
    if (!step) {
        return SI_AGENT_TASK_ERR_NOT_FOUND;
    }
    if (step->status != SI_AGENT_STEP_IN_PROGRESS) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }

    step->verification = verification;
    step->updated_at_ms = now_ms;
    turn->plan.updated_at_ms = now_ms;
    if (verification == SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN) {
        set_outcome_unknown(runtime, now_ms);
    } else {
        turn->updated_at_ms = now_ms;
        turn->run_attempt.updated_at_ms = now_ms;
        runtime->updated_at_ms = now_ms;
    }
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_record_turn_verification(
    si_agent_task_runtime_t *runtime,
    si_agent_verification_status_t verification,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (verification <= SI_AGENT_VERIFICATION_UNRECORDED ||
        verification > SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN) {
        return SI_AGENT_TASK_ERR_INVALID_ARGUMENT;
    }
    if (turn->status != SI_AGENT_TURN_VERIFYING) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }
    if (verification == SI_AGENT_VERIFICATION_PASSED && turn->has_plan &&
        turn->plan.status != SI_AGENT_PLAN_COMPLETED) {
        return SI_AGENT_TASK_ERR_VERIFICATION_REQUIRED;
    }

    turn->verification = verification;
    if (verification == SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN) {
        set_outcome_unknown(runtime, now_ms);
    } else {
        turn->updated_at_ms = now_ms;
        turn->run_attempt.updated_at_ms = now_ms;
        runtime->updated_at_ms = now_ms;
    }
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_steer(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (si_agent_turn_status_is_terminal(turn->status)) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }
    if (turn->intent_revision == UINT32_MAX ||
        turn->plan_version == UINT32_MAX) {
        return SI_AGENT_TASK_ERR_CONFLICT;
    }

    turn->intent_revision++;
    turn->plan_version++;
    turn->verification = SI_AGENT_VERIFICATION_UNRECORDED;
    turn->has_plan = false;
    memset(&turn->plan, 0, sizeof(turn->plan));
    if (turn->status == SI_AGENT_TURN_PAUSED) {
        turn->resume_status = SI_AGENT_TURN_PLANNING;
        turn->updated_at_ms = now_ms;
        turn->run_attempt.updated_at_ms = now_ms;
        runtime->updated_at_ms = now_ms;
    } else {
        turn->resume_status = SI_AGENT_TURN_PLANNING;
        apply_turn_status(runtime, SI_AGENT_TURN_PLANNING, now_ms);
    }
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_pause(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (turn->status == SI_AGENT_TURN_PAUSED ||
        si_agent_turn_status_is_terminal(turn->status)) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }

    turn->resume_status = turn->status;
    apply_turn_status(runtime, SI_AGENT_TURN_PAUSED, now_ms);
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_resume(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (turn->status != SI_AGENT_TURN_PAUSED ||
        !phase_status_valid(turn->resume_status)) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }

    apply_turn_status(runtime, turn->resume_status, now_ms);
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_cancel(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (si_agent_turn_status_is_terminal(turn->status)) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }
    apply_turn_status(runtime, SI_AGENT_TURN_CANCELLED, now_ms);
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_interrupt(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (si_agent_turn_status_is_terminal(turn->status)) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }
    apply_turn_status(runtime, SI_AGENT_TURN_INTERRUPTED, now_ms);
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_mark_outcome_unknown(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (si_agent_turn_status_is_terminal(turn->status)) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }
    set_outcome_unknown(runtime, now_ms);
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_fail(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (si_agent_turn_status_is_terminal(turn->status)) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }
    if (turn->has_plan && turn->plan.status == SI_AGENT_PLAN_ACTIVE) {
        turn->plan.status = SI_AGENT_PLAN_FAILED;
        turn->plan.updated_at_ms = now_ms;
    }
    apply_turn_status(runtime, SI_AGENT_TURN_FAILED, now_ms);
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_complete(
    si_agent_task_runtime_t *runtime,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (turn->status != SI_AGENT_TURN_VERIFYING) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }
    if (turn->verification != SI_AGENT_VERIFICATION_PASSED ||
        (turn->has_plan &&
         turn->plan.status != SI_AGENT_PLAN_COMPLETED)) {
        return SI_AGENT_TASK_ERR_VERIFICATION_REQUIRED;
    }

    apply_turn_status(runtime, SI_AGENT_TURN_COMPLETED, now_ms);
    return SI_AGENT_TASK_OK;
}

si_agent_task_result_t si_agent_task_runtime_retry(
    si_agent_task_runtime_t *runtime,
    const char *run_attempt_id,
    uint64_t now_ms)
{
    si_agent_task_turn_t *turn = NULL;
    si_agent_task_result_t result = active_for_update(runtime, now_ms, &turn);
    if (result != SI_AGENT_TASK_OK) {
        return result;
    }
    if (!id_valid(run_attempt_id)) {
        return SI_AGENT_TASK_ERR_INVALID_ID;
    }
    if (turn->status != SI_AGENT_TURN_FAILED &&
        turn->status != SI_AGENT_TURN_INTERRUPTED &&
        turn->status != SI_AGENT_TURN_OUTCOME_UNKNOWN) {
        return SI_AGENT_TASK_ERR_INVALID_TRANSITION;
    }
    if (run_attempt_id_in_use(runtime, run_attempt_id) ||
        turn->run_attempt.attempt_number == UINT32_MAX ||
        turn->plan_version == UINT32_MAX) {
        return SI_AGENT_TASK_ERR_CONFLICT;
    }

    uint32_t attempt_number = turn->run_attempt.attempt_number + 1U;
    turn->plan_version++;
    turn->verification = SI_AGENT_VERIFICATION_UNRECORDED;
    turn->has_plan = false;
    memset(&turn->plan, 0, sizeof(turn->plan));
    memset(&turn->run_attempt, 0, sizeof(turn->run_attempt));
    copy_id(turn->run_attempt.run_attempt_id, run_attempt_id);
    turn->run_attempt.attempt_number = attempt_number;
    turn->run_attempt.status = SI_AGENT_RUN_ATTEMPT_RUNNING;
    turn->run_attempt.started_at_ms = now_ms;
    turn->run_attempt.updated_at_ms = now_ms;
    turn->status = SI_AGENT_TURN_UNDERSTANDING;
    turn->resume_status = SI_AGENT_TURN_UNDERSTANDING;
    turn->updated_at_ms = now_ms;
    turn->ended_at_ms = 0U;
    runtime->updated_at_ms = now_ms;
    return SI_AGENT_TASK_OK;
}

const char *si_agent_turn_status_name(si_agent_turn_status_t status)
{
    switch (status) {
    case SI_AGENT_TURN_QUEUED:
        return "queued";
    case SI_AGENT_TURN_UNDERSTANDING:
        return "understanding";
    case SI_AGENT_TURN_PLANNING:
        return "planning";
    case SI_AGENT_TURN_GATHERING_CONTEXT:
        return "gathering_context";
    case SI_AGENT_TURN_EXECUTING:
        return "executing";
    case SI_AGENT_TURN_WAITING_REQUEST:
        return "waiting_request";
    case SI_AGENT_TURN_WAITING_USER:
        return "waiting_user";
    case SI_AGENT_TURN_PAUSED:
        return "paused";
    case SI_AGENT_TURN_VERIFYING:
        return "verifying";
    case SI_AGENT_TURN_BLOCKED:
        return "blocked";
    case SI_AGENT_TURN_COMPLETED:
        return "completed";
    case SI_AGENT_TURN_FAILED:
        return "failed";
    case SI_AGENT_TURN_INTERRUPTED:
        return "interrupted";
    case SI_AGENT_TURN_CANCELLED:
        return "cancelled";
    case SI_AGENT_TURN_OUTCOME_UNKNOWN:
        return "outcome_unknown";
    default:
        return "invalid";
    }
}

const char *si_agent_task_result_name(si_agent_task_result_t result)
{
    switch (result) {
    case SI_AGENT_TASK_OK:
        return "ok";
    case SI_AGENT_TASK_ERR_INVALID_ARGUMENT:
        return "invalid_argument";
    case SI_AGENT_TASK_ERR_INVALID_ID:
        return "invalid_id";
    case SI_AGENT_TASK_ERR_TIME_REGRESSION:
        return "time_regression";
    case SI_AGENT_TASK_ERR_QUEUE_FULL:
        return "queue_full";
    case SI_AGENT_TASK_ERR_NO_ACTIVE_TURN:
        return "no_active_turn";
    case SI_AGENT_TASK_ERR_CONFLICT:
        return "conflict";
    case SI_AGENT_TASK_ERR_INVALID_TRANSITION:
        return "invalid_transition";
    case SI_AGENT_TASK_ERR_CAPACITY:
        return "capacity";
    case SI_AGENT_TASK_ERR_NOT_FOUND:
        return "not_found";
    case SI_AGENT_TASK_ERR_VERIFICATION_REQUIRED:
        return "verification_required";
    default:
        return "invalid";
    }
}
