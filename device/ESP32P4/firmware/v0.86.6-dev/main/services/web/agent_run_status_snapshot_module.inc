// Copy one exact Run state under the mutex, then serialize it without blocking
// execution. The comparison and copy deliberately share the same lock so a
// replacement Run can never be returned for an exact-ID request.
static agent_run_job_t *agent_run_status_snapshot_take(
    bool include_result, const char *expected_run_id, bool *mismatch_out)
{
    if (mismatch_out) {
        *mismatch_out = false;
    }
    agent_run_job_t *snapshot = agent_alloc(sizeof(*snapshot));
    if (!snapshot || !s_agent_run_lock ||
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(snapshot);
        return NULL;
    }

    if (expected_run_id && expected_run_id[0] &&
        strcmp(expected_run_id, s_agent_run_job.job_id) != 0) {
        if (mismatch_out) {
            *mismatch_out = true;
        }
        xSemaphoreGive(s_agent_run_lock);
        free(snapshot);
        return NULL;
    }

    memcpy(snapshot, &s_agent_run_job, sizeof(*snapshot));
    if (snapshot->task_service_control_pending) {
        snapshot->task_service_bound = false;
    }
    snapshot->task = NULL;
    snapshot->result_json = NULL;
    if (include_result && s_agent_run_job.result_json) {
        size_t result_len = strlen(s_agent_run_job.result_json);
        snapshot->result_json = agent_alloc(result_len + 1U);
        if (snapshot->result_json) {
            memcpy(snapshot->result_json, s_agent_run_job.result_json,
                   result_len + 1U);
        }
    }
    xSemaphoreGive(s_agent_run_lock);
    return snapshot;
}
