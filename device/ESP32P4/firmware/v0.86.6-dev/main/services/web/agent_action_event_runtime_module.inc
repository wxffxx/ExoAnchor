static si_action_phase_t agent_run_legacy_event_phase(const char *kind)
{
    if (kind && strcmp(kind, "cmd") == 0) {
        return SI_ACTION_PHASE_INPUT_SENT;
    }
    if (kind && strcmp(kind, "progress") == 0) {
        return SI_ACTION_PHASE_OUTPUT_RECEIVED;
    }
    if (kind && strcmp(kind, "ok") == 0) {
        return SI_ACTION_PHASE_SUCCEEDED;
    }
    if (kind && strcmp(kind, "bad") == 0) {
        return SI_ACTION_PHASE_FAILED;
    }
    return SI_ACTION_PHASE_STARTED;
}

static bool agent_run_next_action_id(const char *job_id, const char *prefix,
                                     char *out, size_t out_size)
{
    if (!s_agent_run_lock || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(80)) != pdTRUE) {
        return false;
    }
    if (job_id && strcmp(s_agent_run_job.job_id, job_id) != 0) {
        xSemaphoreGive(s_agent_run_lock);
        return false;
    }
    s_agent_run_job.next_action_id++;
    if (s_agent_run_job.next_action_id == 0) {
        s_agent_run_job.next_action_id = 1;
    }
    snprintf(out, out_size, "%s-%lu",
             prefix && prefix[0] ? prefix : "action",
             (unsigned long)s_agent_run_job.next_action_id);
    xSemaphoreGive(s_agent_run_lock);
    return true;
}

static void agent_action_event_persist(const si_action_event_t *event,
                                       const char *session_id)
{
    /* The recovery/HIL candidate keeps action events in the per-run RAM ring.
     * Synchronous HIST.LOG persistence from provider/control/worker paths is
     * disabled until a dedicated storage task with bounded RPC exists. */
    (void)event;
    (void)session_id;
}

static void agent_run_add_action_eventv(const char *job_id,
                                        const char *action_id,
                                        si_action_channel_t channel,
                                        si_action_phase_t phase,
                                        bool redacted,
                                        bool background,
                                        const char *fmt,
                                        va_list ap)
{
    if (!s_agent_run_lock || !fmt) {
        return;
    }
    char summary[SI_ACTION_EVENT_SUMMARY_MAX + 1U];
    vsnprintf(summary, sizeof(summary), fmt, ap);

    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(80)) != pdTRUE) {
        return;
    }
    if (job_id && strcmp(s_agent_run_job.job_id, job_id) != 0) {
        xSemaphoreGive(s_agent_run_lock);
        return;
    }

    si_action_event_input_t input = {
        .run_id = s_agent_run_job.job_id,
        .action_id = action_id,
        .actor = SI_ACTION_ACTOR_AGENT,
        .channel = channel,
        .phase = phase,
        .summary = summary,
        .redacted = redacted,
        .background = background,
    };
    si_action_event_t event = {0};
    char session_id[AGENT_SESSION_ID_MAX_LEN + 1] = {0};
    bool appended = si_action_event_buffer_append(
        &s_agent_run_job.action_events, si_monotonic_ms(), &input, &event);
    if (appended) {
        s_agent_run_job.updated_ms = event.timestamp_ms;
        strlcpy(session_id, s_agent_run_job.session_id, sizeof(session_id));
    }
    xSemaphoreGive(s_agent_run_lock);
    if (appended) {
        agent_action_event_persist(&event, session_id);
    }
}

static void agent_run_add_action_eventf(const char *job_id,
                                        const char *action_id,
                                        si_action_channel_t channel,
                                        si_action_phase_t phase,
                                        bool redacted,
                                        bool background,
                                        const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    agent_run_add_action_eventv(job_id, action_id, channel, phase,
                                redacted, background, fmt, ap);
    va_end(ap);
}

static void agent_run_add_eventf(const char *job_id, const char *kind,
                                 const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    agent_run_add_action_eventv(job_id, NULL, SI_ACTION_CHANNEL_AGENT,
                                agent_run_legacy_event_phase(kind),
                                false, true, fmt, ap);
    va_end(ap);
}
