/*
 * RUN.CHECKPOINT belonged to the legacy executor and is not a resumable
 * Runtime V2 state snapshot. It was committed before the executor task was
 * launched and remained present while tools could run, so after a reboot its
 * action outcome is necessarily unknown. Migration records that fact before
 * removing the checkpoint; the original goal and page context are never
 * copied into Runtime V2 or replayed; resume is forbidden.
 */
typedef struct {
    const char *thread_id;
    const char *turn_id;
    const char *run_id;
    bool found;
} agent_run_checkpoint_migration_lookup_t;

static bool agent_run_checkpoint_event_id_valid(const char *value)
{
    if (!value || !value[0] ||
        strlen(value) > SI_AGENT_EVENT_ID_MAX_LEN) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor; ++cursor) {
        const unsigned char ch = *cursor;
        const bool alnum = (ch >= '0' && ch <= '9') ||
                           (ch >= 'A' && ch <= 'Z') ||
                           (ch >= 'a' && ch <= 'z');
        if (!alnum && ch != '-' && ch != '_' && ch != '.') {
            return false;
        }
    }
    return true;
}

static esp_err_t agent_run_checkpoint_migration_lookup(
    const si_agent_event_record_t *record, void *user_ctx)
{
    agent_run_checkpoint_migration_lookup_t *lookup =
        (agent_run_checkpoint_migration_lookup_t *)user_ctx;
    if (!record || !lookup) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(
        record->payload, "reason");
    if (strcmp(record->event_type, "turn.outcome_unknown") == 0 &&
        strcmp(record->thread_id, lookup->thread_id) == 0 &&
        strcmp(record->turn_id, lookup->turn_id) == 0 &&
        strcmp(record->run_id, lookup->run_id) == 0 &&
        cJSON_IsString(reason) &&
        strcmp(reason->valuestring, "legacy_checkpoint_migrated") == 0) {
        lookup->found = true;
    }
    return ESP_OK;
}

static esp_err_t agent_run_checkpoint_clear_malformed(const char *reason)
{
    bool cleared = false;
    esp_err_t ret = si_agent_repository_run_checkpoint_clear(&cleared);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Reject legacy Agent Run checkpoint (%s), but clear "
                 "failed: %s", reason, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGW(TAG, "Rejected and cleared malformed legacy Agent Run "
             "checkpoint: %s", reason);
    return ESP_OK;
}

static esp_err_t agent_run_checkpoint_append_unknown(
    const char *thread_id, const char *turn_id, const char *run_id)
{
    /* A previous boot may have lost power after the journal append but before
     * checkpoint deletion. Do not append the same terminal migration twice. */
    agent_run_checkpoint_migration_lookup_t lookup = {
        .thread_id = thread_id,
        .turn_id = turn_id,
        .run_id = run_id,
    };
    si_agent_event_recovery_t recovery = {0};
    esp_err_t ret = si_agent_task_service_replay(
        0U, agent_run_checkpoint_migration_lookup, &lookup, &recovery);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Verify legacy checkpoint migration journal failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }
    if (lookup.found) {
        return ESP_OK;
    }

    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    bool payload_ok =
        cJSON_AddStringToObject(payload, "reason",
                               "legacy_checkpoint_migrated") != NULL &&
        cJSON_AddStringToObject(payload, "outcome", "unknown") != NULL &&
        cJSON_AddNumberToObject(payload, "legacy_schema_version", 1) != NULL &&
        cJSON_AddBoolToObject(payload, "execution_may_have_started", true) !=
            NULL &&
        cJSON_AddBoolToObject(payload, "blind_retry_forbidden", true) != NULL &&
        cJSON_AddBoolToObject(payload, "raw_goal_replayed", false) != NULL &&
        cJSON_AddBoolToObject(payload, "page_context_replayed", false) != NULL;
    if (!payload_ok) {
        cJSON_Delete(payload);
        return ESP_ERR_NO_MEM;
    }
    const si_agent_event_append_t event = {
        .thread_id = thread_id,
        .turn_id = turn_id,
        .run_id = run_id,
        .event_type = "turn.outcome_unknown",
        /* SNTP may not be ready when the web service starts. */
        .server_time_ms = 0,
        .monotonic_time_ms = (int64_t)si_monotonic_ms(),
        .payload = payload,
    };
    ret = si_agent_event_store_append(&event, NULL);
    cJSON_Delete(payload);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Persist legacy Agent Run outcome_unknown failed: %s "
                 "(%s)", esp_err_to_name(ret),
                 si_agent_event_store_last_error());
    }
    return ret;
}

/* Kept under the old internal name because agent_run_start is its only caller;
 * this function migrates a terminal fact and never restores executable work. */
static esp_err_t agent_run_checkpoint_restore(void)
{
    char *json = NULL;
    esp_err_t ret = si_agent_repository_run_checkpoint_read(&json);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret == ESP_ERR_INVALID_SIZE) {
        free(json);
        return agent_run_checkpoint_clear_malformed("invalid_size");
    }
    if (ret != ESP_OK || !json) {
        ESP_LOGE(TAG, "Legacy Agent Run checkpoint read failed closed: %s",
                 esp_err_to_name(ret));
        free(json);
        return ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : ret;
    }

    cJSON *root = cJSON_Parse(json);
    memset(json, 0, strlen(json));
    free(json);
    if (!root) {
        return agent_run_checkpoint_clear_malformed("invalid_json");
    }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(
        root, "schema_version");
    const cJSON *job_id = cJSON_GetObjectItemCaseSensitive(root, "job_id");
    const cJSON *turn_id = cJSON_GetObjectItemCaseSensitive(root, "turn_id");
    const cJSON *session_id = cJSON_GetObjectItemCaseSensitive(
        root, "session_id");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    const bool valid = cJSON_IsNumber(schema) && schema->valueint == 1 &&
        cJSON_IsString(job_id) &&
        agent_run_checkpoint_event_id_valid(job_id->valuestring) &&
        cJSON_IsString(turn_id) &&
        agent_run_checkpoint_event_id_valid(turn_id->valuestring) &&
        cJSON_IsString(session_id) &&
        agent_session_id_valid(session_id->valuestring) &&
        agent_run_checkpoint_event_id_valid(session_id->valuestring) &&
        cJSON_IsString(message) && message->valuestring[0];
    if (!valid) {
        cJSON_Delete(root);
        return agent_run_checkpoint_clear_malformed("invalid_fields");
    }

    char run_copy[SI_AGENT_EVENT_ID_MAX_LEN + 1U] = {0};
    char turn_copy[SI_AGENT_EVENT_ID_MAX_LEN + 1U] = {0};
    char thread_copy[SI_AGENT_EVENT_ID_MAX_LEN + 1U] = {0};
    strlcpy(run_copy, job_id->valuestring, sizeof(run_copy));
    strlcpy(turn_copy, turn_id->valuestring, sizeof(turn_copy));
    strlcpy(thread_copy, session_id->valuestring, sizeof(thread_copy));
    cJSON_Delete(root);

    /* Do not inspect, copy or reconstruct message/page_context fields. */
    ret = agent_run_checkpoint_append_unknown(thread_copy, turn_copy,
                                              run_copy);
    if (ret != ESP_OK) {
        return ret;
    }

    bool cleared = false;
    ret = si_agent_repository_run_checkpoint_clear_if_job_id(run_copy,
                                                              &cleared);
    if (ret != ESP_OK || !cleared) {
        ESP_LOGE(TAG, "Legacy checkpoint terminal event is durable, but "
                 "identity-safe clear failed: %s cleared=%d",
                 esp_err_to_name(ret), cleared);
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }

    if (!s_agent_run_lock ||
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* Recovery is asynchronous. Never overwrite a new legacy Run that was
     * accepted while the old checkpoint terminal fact was being migrated. */
    bool publish_projection =
        s_agent_run_job.state == AGENT_RUN_STATE_IDLE &&
        s_agent_run_job.task == NULL && !s_agent_run_job.job_id[0];
    if (publish_projection) {
        memset(&s_agent_run_job, 0, sizeof(s_agent_run_job));
        s_agent_run_job.state = AGENT_RUN_STATE_OUTCOME_UNKNOWN;
        strlcpy(s_agent_run_job.job_id, run_copy,
                sizeof(s_agent_run_job.job_id));
        strlcpy(s_agent_run_job.turn_id, turn_copy,
                sizeof(s_agent_run_job.turn_id));
        strlcpy(s_agent_run_job.session_id, thread_copy,
                sizeof(s_agent_run_job.session_id));
        s_agent_run_job.started_ms = si_monotonic_ms();
        s_agent_run_job.updated_ms = s_agent_run_job.started_ms;
        s_agent_run_job.finished_ms = s_agent_run_job.started_ms;
        strlcpy(s_agent_run_job.stage, "重启后结果未知",
                sizeof(s_agent_run_job.stage));
        strlcpy(s_agent_run_job.detail,
                "旧检查点已迁移；禁止自动重放，需人工确认后新建任务",
                sizeof(s_agent_run_job.detail));
        strlcpy(s_agent_run_job.error,
                "legacy run outcome unknown after reboot",
                sizeof(s_agent_run_job.error));
        si_action_event_buffer_init(&s_agent_run_job.action_events);
    }
    xSemaphoreGive(s_agent_run_lock);
    ESP_LOGW(TAG, "Migrated legacy Agent Run %s as outcome_unknown; blind "
             "resume is forbidden (projection=%d)", run_copy,
             publish_projection);
    return ESP_OK;
}

static esp_err_t agent_run_start(void)
{
    if (!__atomic_load_n(&s_agent_run_lock, __ATOMIC_ACQUIRE)) {
        SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(candidate, ESP_ERR_NO_MEM, TAG,
                            "create agent run mutex");
        SemaphoreHandle_t expected = NULL;
        if (!__atomic_compare_exchange_n(
                &s_agent_run_lock, &expected, candidate, false,
                __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            vSemaphoreDelete(candidate);
        }
    }
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_agent_run_initialized) {
        xSemaphoreGive(s_agent_run_lock);
        return ESP_OK;
    }
    memset(&s_agent_run_job, 0, sizeof(s_agent_run_job));
    s_agent_run_job.state = AGENT_RUN_STATE_IDLE;
    strlcpy(s_agent_run_job.task_service_error,
            "Runtime V2 recovery initializing; legacy checkpoint quarantined",
            sizeof(s_agent_run_job.task_service_error));
    s_agent_run_initialized = true;
    xSemaphoreGive(s_agent_run_lock);
    return ESP_OK;
}
