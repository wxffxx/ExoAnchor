static const char *agent_task_run_attempt_status_name(
    si_agent_run_attempt_status_t status)
{
    switch (status) {
    case SI_AGENT_RUN_ATTEMPT_RUNNING: return "running";
    case SI_AGENT_RUN_ATTEMPT_PAUSED: return "paused";
    case SI_AGENT_RUN_ATTEMPT_COMPLETED: return "completed";
    case SI_AGENT_RUN_ATTEMPT_FAILED: return "failed";
    case SI_AGENT_RUN_ATTEMPT_INTERRUPTED: return "interrupted";
    case SI_AGENT_RUN_ATTEMPT_CANCELLED: return "cancelled";
    case SI_AGENT_RUN_ATTEMPT_OUTCOME_UNKNOWN: return "outcome_unknown";
    default: return "invalid";
    }
}

static const char *agent_task_plan_status_name(si_agent_plan_status_t status)
{
    switch (status) {
    case SI_AGENT_PLAN_ACTIVE: return "active";
    case SI_AGENT_PLAN_COMPLETED: return "completed";
    case SI_AGENT_PLAN_FAILED: return "failed";
    case SI_AGENT_PLAN_OUTCOME_UNKNOWN: return "outcome_unknown";
    default: return "invalid";
    }
}

static const char *agent_task_step_status_name(si_agent_step_status_t status)
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

static const char *agent_task_verification_status_name(
    si_agent_verification_status_t status)
{
    switch (status) {
    case SI_AGENT_VERIFICATION_UNRECORDED: return "unrecorded";
    case SI_AGENT_VERIFICATION_PASSED: return "passed";
    case SI_AGENT_VERIFICATION_FAILED: return "failed";
    case SI_AGENT_VERIFICATION_OUTCOME_UNKNOWN: return "outcome_unknown";
    default: return "invalid";
    }
}

static void agent_run_add_status_json(cJSON *root, bool include_result,
                                      const char *expected_run_id,
                                      bool *mismatch_out)
{
    if (!root) {
        return;
    }
    agent_run_job_t *snapshot = agent_run_status_snapshot_take(
        include_result, expected_run_id, mismatch_out);
    if (!snapshot) {
        cJSON_AddStringToObject(root, "state", "unavailable");
        cJSON_AddBoolToObject(root, "running", false);
        return;
    }
    si_agent_task_service_snapshot_t *task_snapshot = NULL;
    esp_err_t task_snapshot_result = ESP_ERR_NOT_SUPPORTED;
#if !AGENT_RUNTIME_RECOVERY_RAM_ONLY
    task_snapshot = agent_alloc(sizeof(*task_snapshot));
    if (task_snapshot) {
        memset(task_snapshot, 0, sizeof(*task_snapshot));
        task_snapshot_result = si_agent_task_service_snapshot(task_snapshot);
    } else {
        task_snapshot_result = ESP_ERR_NO_MEM;
    }
#endif
    bool task_service_available =
        task_snapshot_result == ESP_OK && task_snapshot->available;
    bool task_recovery_known = task_snapshot &&
        task_snapshot_result != ESP_ERR_TIMEOUT;
    const si_agent_task_turn_t *candidate_active_turn =
        task_service_available && task_snapshot->runtime.has_active_turn ?
        &task_snapshot->runtime.active_turn : NULL;
    const si_agent_task_turn_t *active_turn =
        candidate_active_turn && snapshot->job_id[0] &&
        strcmp(candidate_active_turn->run_attempt.run_attempt_id,
               snapshot->job_id) == 0 ? candidate_active_turn : NULL;
    uint32_t now_ms = si_monotonic_ms();
    cJSON_AddNumberToObject(root, "runtime_schema_version", 2);
    cJSON_AddStringToObject(root, "runtime_owner", "legacy_embedded_agent");
    cJSON_AddStringToObject(root, "task_service_mode", "shadow");
    cJSON_AddStringToObject(
        root, "admission_scope",
        AGENT_RUNTIME_RECOVERY_RAM_ONLY ?
            "full_cap_browser_only" : "capability_projected");
    cJSON_AddBoolToObject(root, "task_service_bound",
                          snapshot->task_service_bound);
    cJSON_AddBoolToObject(root, "task_service_available",
                          task_service_available);
    if (snapshot->task_service_error[0]) {
        cJSON_AddStringToObject(root, "task_service_error",
                                snapshot->task_service_error);
    } else if (!AGENT_RUNTIME_RECOVERY_RAM_ONLY && !task_service_available &&
               si_agent_task_service_last_error()[0]) {
        cJSON_AddStringToObject(root, "task_service_error",
                                si_agent_task_service_last_error());
    }
    cJSON_AddStringToObject(root, "task_state",
                            active_turn ?
                            si_agent_turn_status_name(active_turn->status) :
                            task_service_available ? "idle" : "unavailable");
    cJSON_AddNumberToObject(root, "queue_depth",
                            task_service_available ?
                            task_snapshot->queue_depth : 0U);
    cJSON_AddNumberToObject(root, "task_event_seq",
                            task_service_available ?
                            task_snapshot->latest_event_seq : 0U);
    cJSON_AddBoolToObject(root, "task_event_store_healthy",
                          task_service_available &&
                          task_snapshot->storage_healthy);
    cJSON_AddStringToObject(
        root, "task_recovery_status",
        task_recovery_known ?
        si_agent_event_recovery_status_name(task_snapshot->recovery_status) :
        "unavailable");
    cJSON *task_runtime = cJSON_AddObjectToObject(root, "task_runtime");
    if (task_runtime) {
        cJSON_AddStringToObject(task_runtime, "runtime_id",
                                task_service_available ?
                                task_snapshot->runtime.runtime_id : "");
        cJSON_AddBoolToObject(task_runtime, "has_active_turn",
                              active_turn != NULL);
        cJSON_AddNumberToObject(task_runtime, "created_ms",
                                task_service_available ?
                                task_snapshot->runtime.created_at_ms : 0U);
        cJSON_AddNumberToObject(task_runtime, "updated_ms",
                                task_service_available ?
                                task_snapshot->runtime.updated_at_ms : 0U);
        cJSON_AddNumberToObject(task_runtime, "queue_depth",
                                task_service_available ?
                                task_snapshot->queue_depth : 0U);
        cJSON *event_store =
            cJSON_AddObjectToObject(task_runtime, "event_store");
        if (event_store) {
            cJSON_AddBoolToObject(event_store, "healthy",
                                  task_service_available &&
                                  task_snapshot->storage_healthy);
            cJSON_AddStringToObject(
                event_store, "recovery_status",
                task_recovery_known ?
                si_agent_event_recovery_status_name(
                    task_snapshot->recovery_status) : "unavailable");
            cJSON_AddNumberToObject(event_store, "latest_seq",
                                    task_service_available ?
                                    task_snapshot->latest_event_seq : 0U);
        }
    }
    cJSON_AddStringToObject(root, "state",
                            agent_run_state_name(snapshot->state));
    bool active = snapshot->state == AGENT_RUN_STATE_RUNNING ||
                  snapshot->state == AGENT_RUN_STATE_WAITING_REQUEST ||
                  snapshot->state == AGENT_RUN_STATE_PAUSED;
    cJSON_AddBoolToObject(root, "running",
                          snapshot->state == AGENT_RUN_STATE_RUNNING);
    cJSON_AddBoolToObject(
        root, "waiting_request",
        snapshot->state == AGENT_RUN_STATE_WAITING_REQUEST);
    cJSON_AddBoolToObject(root, "paused",
                          snapshot->state == AGENT_RUN_STATE_PAUSED);
    cJSON_AddBoolToObject(root, "busy", active);
    cJSON_AddBoolToObject(root, "pause_requested",
                          snapshot->pause_requested);
    cJSON_AddBoolToObject(root, "cancel_requested",
                          snapshot->cancel_requested);
    cJSON_AddBoolToObject(root, "abort_requested",
                          snapshot->cancel_requested);
    cJSON_AddBoolToObject(root, "recovered_after_reboot",
                          snapshot->recovered_after_reboot);
    cJSON_AddStringToObject(root, "job_id", snapshot->job_id);
    cJSON_AddStringToObject(root, "run_id", snapshot->job_id);
    cJSON_AddStringToObject(root, "turn_id", snapshot->turn_id);
    cJSON_AddStringToObject(root, "thread_id",
                            snapshot->session_id);
    cJSON_AddStringToObject(root, "session_id", snapshot->session_id);
    cJSON_AddStringToObject(root, "goal", snapshot->goal);
    cJSON_AddNumberToObject(root, "intent_revision",
                            snapshot->intent_revision);
    cJSON_AddNumberToObject(root, "plan_version",
                            snapshot->plan_version);
    cJSON_AddStringToObject(root, "profile", snapshot->profile);
    cJSON_AddStringToObject(root, "model", snapshot->model);
    cJSON_AddBoolToObject(root, "dry_run", snapshot->dry_run);
    cJSON_AddBoolToObject(root, "include_screenshot",
                          snapshot->include_screenshot);
    cJSON_AddBoolToObject(root, "include_web_search",
                          snapshot->allow_web_search);
    cJSON_AddBoolToObject(root, "page_context_attached",
                          snapshot->page_context_json[0] != '\0');
    if (snapshot->page_context_json[0]) {
        cJSON *page_context = cJSON_Parse(snapshot->page_context_json);
        if (page_context) {
            cJSON_AddItemToObject(root, "page_context", page_context);
        }
    }
    cJSON_AddNumberToObject(root, "started_ms", snapshot->started_ms);
    cJSON_AddNumberToObject(root, "updated_ms", snapshot->updated_ms);
    cJSON_AddNumberToObject(root, "finished_ms", snapshot->finished_ms);
    cJSON_AddNumberToObject(root, "elapsed_ms",
                            snapshot->started_ms ?
                            (snapshot->finished_ms ? snapshot->finished_ms :
                             now_ms) - snapshot->started_ms : 0);
    cJSON_AddStringToObject(root, "stage", snapshot->stage);
    cJSON_AddStringToObject(root, "detail", snapshot->detail);
    if (active_turn) {
        cJSON *thread = cJSON_AddObjectToObject(root, "thread");
        if (thread) {
            cJSON_AddStringToObject(thread, "thread_id",
                                    active_turn->thread_id);
            cJSON_AddStringToObject(thread, "status", "active");
            cJSON_AddNumberToObject(thread, "created_ms",
                                    active_turn->created_at_ms);
            cJSON_AddNumberToObject(thread, "updated_ms",
                                    active_turn->updated_at_ms);
        }
        cJSON *turn = cJSON_AddObjectToObject(root, "turn");
        if (turn) {
            cJSON_AddStringToObject(turn, "thread_id",
                                    active_turn->thread_id);
            cJSON_AddStringToObject(turn, "turn_id",
                                    active_turn->turn_id);
            cJSON_AddStringToObject(
                turn, "run_id",
                active_turn->run_attempt.run_attempt_id);
            cJSON_AddStringToObject(turn, "status",
                                    si_agent_turn_status_name(
                                        active_turn->status));
            cJSON_AddNumberToObject(turn, "intent_revision",
                                    active_turn->intent_revision);
            cJSON_AddNumberToObject(turn, "plan_version",
                                    active_turn->plan_version);
            cJSON_AddStringToObject(
                turn, "verification",
                agent_task_verification_status_name(
                    active_turn->verification));
            cJSON_AddNumberToObject(turn, "created_ms",
                                    active_turn->created_at_ms);
            cJSON_AddNumberToObject(turn, "updated_ms",
                                    active_turn->updated_at_ms);
            cJSON_AddNumberToObject(turn, "ended_ms",
                                    active_turn->ended_at_ms);
            if (task_snapshot->active.used) {
                cJSON_AddStringToObject(turn, "goal",
                                        task_snapshot->active.goal);
                cJSON_AddStringToObject(
                    turn, "completion_criteria",
                    task_snapshot->active.completion_criteria);
                cJSON_AddStringToObject(
                    turn, "origin",
                    si_agent_task_origin_name(task_snapshot->active.origin));
                cJSON_AddStringToObject(
                    turn, "history_policy",
                    si_agent_task_history_policy_name(
                        task_snapshot->active.history_policy));
            }
            cJSON *run_attempt =
                cJSON_AddObjectToObject(turn, "run_attempt");
            if (run_attempt) {
                cJSON_AddStringToObject(
                    run_attempt, "run_attempt_id",
                    active_turn->run_attempt.run_attempt_id);
                cJSON_AddNumberToObject(
                    run_attempt, "attempt_number",
                    active_turn->run_attempt.attempt_number);
                cJSON_AddStringToObject(
                    run_attempt, "status",
                    agent_task_run_attempt_status_name(
                        active_turn->run_attempt.status));
                cJSON_AddNumberToObject(
                    run_attempt, "started_ms",
                    active_turn->run_attempt.started_at_ms);
                cJSON_AddNumberToObject(
                    run_attempt, "updated_ms",
                    active_turn->run_attempt.updated_at_ms);
                cJSON_AddNumberToObject(
                    run_attempt, "ended_ms",
                    active_turn->run_attempt.ended_at_ms);
            }
        }
        if (active_turn->has_plan) {
            cJSON *plan = cJSON_AddObjectToObject(root, "plan");
            if (plan) {
                cJSON_AddStringToObject(plan, "plan_id",
                                        active_turn->plan.plan_id);
                cJSON_AddNumberToObject(plan, "version",
                                        active_turn->plan.version);
                cJSON_AddStringToObject(
                    plan, "status",
                    agent_task_plan_status_name(active_turn->plan.status));
                cJSON_AddNumberToObject(plan, "created_ms",
                                        active_turn->plan.created_at_ms);
                cJSON_AddNumberToObject(plan, "updated_ms",
                                        active_turn->plan.updated_at_ms);
                cJSON *steps = cJSON_AddArrayToObject(plan, "steps");
                size_t step_count = active_turn->plan.step_count;
                if (step_count > SI_AGENT_TASK_STEP_CAPACITY) {
                    step_count = SI_AGENT_TASK_STEP_CAPACITY;
                }
                for (size_t i = 0; steps && i < step_count; i++) {
                    const si_agent_task_step_t *task_step =
                        &active_turn->plan.steps[i];
                    cJSON *step = cJSON_CreateObject();
                    if (!step) {
                        continue;
                    }
                    cJSON_AddStringToObject(step, "step_id",
                                            task_step->step_id);
                    cJSON_AddStringToObject(
                        step, "status",
                        agent_task_step_status_name(task_step->status));
                    cJSON_AddStringToObject(
                        step, "verification",
                        agent_task_verification_status_name(
                            task_step->verification));
                    cJSON_AddNumberToObject(step, "created_ms",
                                            task_step->created_at_ms);
                    cJSON_AddNumberToObject(step, "updated_ms",
                                            task_step->updated_at_ms);
                    cJSON_AddItemToArray(steps, step);
                }
            }
        } else {
            cJSON_AddItemToObject(root, "plan", cJSON_CreateNull());
        }
        cJSON *verification =
            cJSON_AddObjectToObject(root, "verification");
        if (verification) {
            cJSON_AddStringToObject(
                verification, "turn",
                agent_task_verification_status_name(
                    active_turn->verification));
            cJSON_AddNumberToObject(verification, "plan_version",
                                    active_turn->plan_version);
            cJSON_AddBoolToObject(
                verification, "completion_gate_passed",
                active_turn->verification == SI_AGENT_VERIFICATION_PASSED);
        }
    } else {
        cJSON_AddItemToObject(root, "thread", cJSON_CreateNull());
        cJSON_AddItemToObject(root, "turn", cJSON_CreateNull());
        cJSON_AddItemToObject(root, "plan", cJSON_CreateNull());
        cJSON_AddItemToObject(root, "verification", cJSON_CreateNull());
    }
    si_agent_request_status_t request_status = {0};
    if (si_agent_request_broker_get(
            snapshot->job_id[0] ? snapshot->job_id : NULL,
            &request_status) == ESP_OK &&
        request_status.state != SI_AGENT_REQUEST_EMPTY) {
        cJSON *request = cJSON_AddObjectToObject(root, "request");
        agent_request_add_json(request, &request_status);
    }
    if (snapshot->error[0]) {
        cJSON_AddStringToObject(root, "error", snapshot->error);
    }
    cJSON_AddNumberToObject(root, "event_seq",
                            snapshot->action_events.latest_seq);
    cJSON_AddNumberToObject(root, "event_schema_version",
                            SI_ACTION_EVENT_SCHEMA_VERSION);
    cJSON_AddNumberToObject(root, "event_dropped",
                            snapshot->action_events.dropped);
    cJSON *events = cJSON_AddArrayToObject(root, "events");
    if (events) {
        uint32_t count = snapshot->action_events.count;
        uint32_t start =
            (snapshot->action_events.write_pos +
             SI_ACTION_EVENT_CAPACITY - count) % SI_ACTION_EVENT_CAPACITY;
        for (uint32_t i = 0; i < count; i++) {
            const si_action_event_t *ev =
                &snapshot->action_events.events[
                    (start + i) % SI_ACTION_EVENT_CAPACITY];
            cJSON *item = cJSON_CreateObject();
            if (!item) {
                continue;
            }
            cJSON_AddNumberToObject(item, "schema_version", ev->schema_version);
            cJSON_AddNumberToObject(item, "seq", ev->seq);
            cJSON_AddNumberToObject(item, "ms", ev->timestamp_ms);
            cJSON_AddStringToObject(item, "run_id", ev->run_id);
            cJSON_AddStringToObject(item, "action_id", ev->action_id);
            cJSON_AddStringToObject(item, "actor",
                                    si_action_actor_name(ev->actor));
            cJSON_AddStringToObject(item, "channel",
                                    si_action_channel_name(ev->channel));
            cJSON_AddStringToObject(item, "phase",
                                    si_action_phase_name(ev->phase));
            cJSON_AddBoolToObject(item, "redacted", ev->redacted);
            cJSON_AddBoolToObject(item, "background", ev->background);
            cJSON_AddStringToObject(item, "kind",
                                    si_action_phase_legacy_kind(ev->phase));
            cJSON_AddStringToObject(item, "text", ev->summary);
            cJSON_AddItemToArray(events, item);
        }
    }
    if (include_result && snapshot->result_json) {
        cJSON *result = cJSON_Parse(snapshot->result_json);
        cJSON_AddItemToObject(root, "result", result ? result : cJSON_CreateNull());
    }
    free(task_snapshot);
    free(snapshot->result_json);
    free(snapshot);
}
