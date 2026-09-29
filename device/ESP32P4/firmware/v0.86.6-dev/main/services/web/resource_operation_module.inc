// Cross-page Agent/MCP ownership for non-terminal device resources.  The
// state core is generation-aware; this adapter aggregates the existing HID,
// video and terminal authorities into one browser-visible control surface.
static SemaphoreHandle_t s_resource_operation_lock;
static si_resource_operation_state_t s_resource_operations;

static const char *resource_operation_name(
    si_resource_operation_resource_t resource)
{
    switch (resource) {
    case SI_RESOURCE_OPERATION_POWER: return "power";
    case SI_RESOURCE_OPERATION_VIDEO_CONFIG: return "video-config";
    case SI_RESOURCE_OPERATION_VIDEO_HARDWARE: return "video-hardware";
    case SI_RESOURCE_OPERATION_MAINTENANCE: return "maintenance";
    case SI_RESOURCE_OPERATION_AGENT_DATA: return "agent-data";
    default: return "unknown";
    }
}

static const char *resource_operation_actor_name(
    si_resource_operation_actor_t actor)
{
    switch (actor) {
    case SI_RESOURCE_OPERATION_ACTOR_AGENT: return "agent";
    case SI_RESOURCE_OPERATION_ACTOR_MCP: return "mcp";
    case SI_RESOURCE_OPERATION_ACTOR_SYSTEM: return "system";
    default: return "none";
    }
}

static esp_err_t resource_operation_start(void)
{
    if (s_resource_operation_lock) return ESP_OK;
    s_resource_operation_lock = xSemaphoreCreateMutex();
    if (!s_resource_operation_lock) return ESP_ERR_NO_MEM;
    si_resource_operation_state_init(&s_resource_operations);
    return ESP_OK;
}

static bool resource_operation_begin(
    si_resource_operation_resource_t resource,
    si_resource_operation_actor_t actor, const char *operation_id,
    const char *run_id, const char *label, bool cancellable,
    si_resource_operation_token_t *token)
{
    if (!s_resource_operation_lock ||
        xSemaphoreTake(s_resource_operation_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool ok = si_resource_operation_state_begin(
        &s_resource_operations, resource, actor, operation_id, run_id,
        label, cancellable, si_monotonic_ms(), token);
    xSemaphoreGive(s_resource_operation_lock);
    return ok;
}

static bool resource_operation_finish(
    const si_resource_operation_token_t *token)
{
    if (!token || !s_resource_operation_lock ||
        xSemaphoreTake(s_resource_operation_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool ok = si_resource_operation_state_finish(&s_resource_operations,
                                                  token);
    xSemaphoreGive(s_resource_operation_lock);
    return ok;
}

static bool resource_operation_cancel_requested(
    const si_resource_operation_token_t *token)
{
    if (!token || !s_resource_operation_lock ||
        xSemaphoreTake(s_resource_operation_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return true;
    bool cancelled = si_resource_operation_state_cancel_requested(
        &s_resource_operations, token);
    xSemaphoreGive(s_resource_operation_lock);
    return cancelled;
}

static bool resource_operation_manual_allowed(
    si_resource_operation_resource_t resource)
{
    if (!s_resource_operation_lock ||
        xSemaphoreTake(s_resource_operation_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool allowed = si_resource_operation_state_manual_allowed(
        &s_resource_operations, resource);
    xSemaphoreGive(s_resource_operation_lock);
    return allowed;
}

static bool resource_operation_get(
    si_resource_operation_resource_t resource,
    si_resource_operation_status_t *status)
{
    if (!status || !s_resource_operation_lock ||
        xSemaphoreTake(s_resource_operation_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool ok = si_resource_operation_state_get(&s_resource_operations,
                                               resource, status);
    xSemaphoreGive(s_resource_operation_lock);
    return ok;
}

static bool automation_manual_power_allowed(void)
{
    if (!resource_operation_manual_allowed(SI_RESOURCE_OPERATION_POWER))
        return false;
    si_terminal_control_status_t terminal = {0};
    if (!terminal_control_get(SI_TERMINAL_CHANNEL_SSH, &terminal) ||
        terminal.active) return false;
#if SI_CFG_UART_TERMINAL_ENABLED
    if (!terminal_control_get(SI_TERMINAL_CHANNEL_UART, &terminal) ||
        terminal.active) return false;
#endif
    si_control_lease_status_t lease = {0};
    si_control_lease_get_status(&lease);
    return !lease.input_control_active;
}

static bool resource_operation_request_cancel(
    si_resource_operation_resource_t resource, uint32_t generation)
{
    if (!s_resource_operation_lock ||
        xSemaphoreTake(s_resource_operation_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool ok = si_resource_operation_state_request_cancel(
        &s_resource_operations, resource, generation);
    xSemaphoreGive(s_resource_operation_lock);
    return ok;
}

#if SI_CFG_EMBEDDED_AGENT_ENABLED
static bool automation_agent_run_active(void)
{
    if (!s_agent_run_lock ||
        xSemaphoreTake(s_agent_run_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool active = agent_run_control_state_active(s_agent_run_job.state) &&
                  s_agent_run_job.job_id[0];
    xSemaphoreGive(s_agent_run_lock);
    return active;
}

static bool automation_abort_active_agent(const char *expected_run_id)
{
    char run_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    if (!s_agent_run_lock ||
        xSemaphoreTake(s_agent_run_lock,
                       pdMS_TO_TICKS(200)) != pdTRUE) return false;
    bool active = agent_run_control_state_active(s_agent_run_job.state) &&
                  s_agent_run_job.job_id[0];
    if (active) strlcpy(run_id, s_agent_run_job.job_id, sizeof(run_id));
    xSemaphoreGive(s_agent_run_lock);
    if (!active || (expected_run_id && expected_run_id[0] &&
                    strcmp(expected_run_id, run_id) != 0)) return false;
    char controlled[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    bool shadow_degraded = false;
    return agent_run_control_exact(
               AGENT_RUN_CONTROL_ABORT, run_id, controlled,
               sizeof(controlled), &shadow_degraded) == ESP_OK;
}
#else
static bool automation_agent_run_active(void)
{
    return false;
}

static bool automation_abort_active_agent(const char *expected_run_id)
{
    (void)expected_run_id;
    return false;
}
#endif

static bool automation_request_stop_actor(
    si_resource_operation_actor_t actor, const char *reason)
{
    (void)reason;
    bool changed = actor == SI_RESOURCE_OPERATION_ACTOR_AGENT ?
                   automation_abort_active_agent(NULL) : false;
    if (s_resource_operation_lock &&
        xSemaphoreTake(s_resource_operation_lock,
                       pdMS_TO_TICKS(100)) == pdTRUE) {
        for (size_t i = 0; i < SI_RESOURCE_OPERATION_COUNT; i++) {
            si_resource_operation_status_t *slot =
                &s_resource_operations.slots[i];
            if (slot->active && slot->cancellable && slot->actor == actor) {
                slot->cancel_requested = true;
                changed = true;
            }
        }
        xSemaphoreGive(s_resource_operation_lock);
    }
    if (s_terminal_control_lock &&
        xSemaphoreTake(s_terminal_control_lock,
                       pdMS_TO_TICKS(100)) == pdTRUE) {
        for (size_t i = 0; i < SI_TERMINAL_CHANNEL_COUNT; i++) {
            si_terminal_control_status_t *slot =
                &s_terminal_control.channels[i];
            bool actor_matches =
                (actor == SI_RESOURCE_OPERATION_ACTOR_AGENT &&
                 slot->actor == SI_TERMINAL_ACTOR_AGENT) ||
                (actor == SI_RESOURCE_OPERATION_ACTOR_MCP &&
                 slot->actor == SI_TERMINAL_ACTOR_MCP);
            if (slot->active && actor_matches) {
                slot->cancel_requested = true;
                changed = true;
            }
        }
        xSemaphoreGive(s_terminal_control_lock);
    }
    si_control_lease_status_t lease = {0};
    si_control_lease_get_status(&lease);
    const char *actor_name = resource_operation_actor_name(actor);
    if (lease.agent_active && lease.session_id[0] &&
        strcasecmp(lease.agent_owner, actor_name) == 0) {
        changed = si_control_lease_revoke_auth_session(
                      lease.session_id, 0U) || changed;
    }
    si_video_control_status_t video = {0};
    si_video_control_get_status(&video);
    if (video.agent_active &&
        strcasecmp(video.agent_actor, actor_name) == 0) {
        si_video_control_release_agent();
        (void)si_video_control_apply();
        changed = true;
    }
    terminal_control_publish_state(SI_TERMINAL_CHANNEL_SSH);
#if SI_CFG_UART_TERMINAL_ENABLED
    terminal_control_publish_state(SI_TERMINAL_CHANNEL_UART);
#endif
    return changed;
}

static bool automation_request_stop_all(const char *reason)
{
    bool changed = automation_request_stop_actor(
        SI_RESOURCE_OPERATION_ACTOR_AGENT, reason);
    changed = automation_request_stop_actor(
                  SI_RESOURCE_OPERATION_ACTOR_MCP, reason) || changed;
    return changed;
}

static bool automation_is_quiescent(void)
{
    bool resources_idle = false;
    if (s_resource_operation_lock &&
        xSemaphoreTake(s_resource_operation_lock,
                       pdMS_TO_TICKS(100)) == pdTRUE) {
        resources_idle =
            si_resource_operation_state_active_count(
                &s_resource_operations) == 0U;
        xSemaphoreGive(s_resource_operation_lock);
    }
    if (!resources_idle) return false;
    si_terminal_control_status_t terminal = {0};
    if (!terminal_control_get(SI_TERMINAL_CHANNEL_SSH, &terminal) ||
        terminal.active) return false;
#if SI_CFG_UART_TERMINAL_ENABLED
    if (!terminal_control_get(SI_TERMINAL_CHANNEL_UART, &terminal) ||
        terminal.active) return false;
#endif
    si_control_lease_status_t lease = {0};
    si_control_lease_get_status(&lease);
    if (lease.agent_active) return false;
    si_video_control_status_t video = {0};
    si_video_control_get_status(&video);
    if (video.agent_active) return false;
#if SI_CFG_EMBEDDED_AGENT_ENABLED
    if (s_agent_run_lock &&
        xSemaphoreTake(s_agent_run_lock,
                       pdMS_TO_TICKS(100)) == pdTRUE) {
        bool active = agent_run_control_state_active(s_agent_run_job.state);
        xSemaphoreGive(s_agent_run_lock);
        if (active) return false;
    }
#endif
    return true;
}

static bool automation_begin_maintenance(
    const char *operation_id, const char *label, bool cancellable,
    si_resource_operation_token_t *token)
{
    (void)automation_request_stop_all(label);
    uint32_t waited_ms = 0U;
    while (!automation_is_quiescent() && waited_ms < 3000U) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20U;
    }
    if (!automation_is_quiescent()) return false;
    return resource_operation_begin(
        SI_RESOURCE_OPERATION_MAINTENANCE,
        SI_RESOURCE_OPERATION_ACTOR_SYSTEM, operation_id, "", label,
        cancellable, token);
}

static void automation_add_operation(cJSON *items, const char *resource,
                                     const char *actor, const char *label,
                                     const char *operation_id,
                                     const char *run_id, uint32_t generation,
                                     uint32_t started_ms, bool cancellable,
                                     bool cancel_requested,
                                     bool blocks_manual)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) return;
    cJSON_AddStringToObject(item, "resource", resource ? resource : "");
    cJSON_AddStringToObject(item, "actor", actor ? actor : "none");
    cJSON_AddStringToObject(item, "label", label ? label : "");
    cJSON_AddStringToObject(item, "operation_id",
                            operation_id ? operation_id : "");
    cJSON_AddStringToObject(item, "run_id", run_id ? run_id : "");
    cJSON_AddNumberToObject(item, "generation", generation);
    cJSON_AddNumberToObject(item, "started_ms", started_ms);
    cJSON_AddBoolToObject(item, "cancellable", cancellable);
    cJSON_AddBoolToObject(item, "cancel_requested", cancel_requested);
    cJSON_AddBoolToObject(item, "blocks_manual", blocks_manual);
    cJSON_AddItemToArray(items, item);
}

static void automation_add_status_json(cJSON *root)
{
    cJSON_AddStringToObject(root, "schema", "exoanchor.automation.v1");
    cJSON_AddStringToObject(root, "access_mode",
                            si_agent_access_mode_name(
                                si_agent_access_mode_get()));
    cJSON *items = cJSON_AddArrayToObject(root, "operations");
    if (!items) return;
    for (size_t i = 0; i < SI_RESOURCE_OPERATION_COUNT; i++) {
        si_resource_operation_status_t status = {0};
        if (!resource_operation_get((si_resource_operation_resource_t)i,
                                    &status) || !status.active) continue;
        automation_add_operation(
            items, resource_operation_name((si_resource_operation_resource_t)i),
            resource_operation_actor_name(status.actor), status.label,
            status.operation_id, status.run_id, status.generation,
            status.started_ms, status.cancellable, status.cancel_requested,
            true);
    }
    for (size_t i = 0; i < SI_TERMINAL_CHANNEL_COUNT; i++) {
        si_terminal_control_status_t status = {0};
        if (!terminal_control_get((si_terminal_channel_t)i, &status) ||
            !status.active) continue;
        automation_add_operation(
            items, terminal_channel_name((si_terminal_channel_t)i),
            terminal_actor_name(status.actor), status.label,
            status.operation_id, status.run_id, status.generation,
            status.started_ms, true, status.cancel_requested, true);
    }
    si_control_lease_status_t lease = {0};
    si_control_lease_get_status(&lease);
    if (lease.agent_active && lease.input_control_active) {
        automation_add_operation(
            items, "hid", lease.agent_owner, lease.reason[0] ? lease.reason :
            "自动控制键盘与鼠标", "hid-control", "", lease.epoch, 0U,
            true, false, true);
    }
    si_video_control_status_t video = {0};
    si_video_control_get_status(&video);
    if (video.agent_active && !lease.input_control_active) {
        const bool mcp_video = strcmp(video.agent_actor, "mcp") == 0;
        automation_add_operation(
            items, "video-observe", mcp_video ? "mcp" : "agent",
            mcp_video ? "MCP 正在查看视频" : "Agent 正在查看视频",
            "video-observe", "", si_video_control_agent_stream_epoch(),
            0U, true, false, false);
    }
    cJSON_AddNumberToObject(root, "active_count",
                            cJSON_GetArraySize(items));
    bool takeover_available = false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                item, "cancellable"))) {
            takeover_available = true;
            break;
        }
    }
    cJSON_AddBoolToObject(root, "manual_takeover_available",
                          takeover_available);
}

static esp_err_t automation_status_handler(httpd_req_t *req)
{
    esp_err_t ret = si_http_require_capability(
        req, SI_CAPABILITY_OBSERVE, NULL);
    if (ret != ESP_OK) return ret;
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    automation_add_status_json(root);
    ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static bool automation_stop_terminal(si_terminal_channel_t channel,
                                     uint32_t generation,
                                     char *run_id, size_t run_id_size)
{
    bool stopped = false;
    if (!s_terminal_control_lock ||
        xSemaphoreTake(s_terminal_control_lock,
                       pdMS_TO_TICKS(100)) != pdTRUE) return false;
    si_terminal_control_status_t status = {0};
    (void)si_terminal_control_state_get(&s_terminal_control, channel,
                                         &status);
    if (status.active && status.generation == generation) {
        if (run_id && run_id_size > 0)
            strlcpy(run_id, status.run_id, run_id_size);
        stopped = si_terminal_control_state_request_cancel(
            &s_terminal_control, channel, generation);
    }
    xSemaphoreGive(s_terminal_control_lock);
    if (stopped) terminal_control_publish_state(channel);
    return stopped;
}

static bool automation_browser_hid_auth_live(void *opaque)
{
    const si_auth_session_context_t *expected = opaque;
    si_auth_session_context_t live = {0};
    return expected && expected->principal == SI_PRINCIPAL_BROWSER &&
        si_auth_session_get_live_context(
            expected->session_id, expected->generation, &live) &&
        live.authenticated && live.principal == SI_PRINCIPAL_BROWSER &&
        live.generation == expected->generation &&
        strcmp(live.session_id, expected->session_id) == 0 &&
        si_capability_set_has(live.capabilities, SI_CAPABILITY_HID) &&
        si_capability_set_has(live.capabilities,
                              SI_CAPABILITY_CONTROL_LEASE);
}

static esp_err_t automation_stop_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t ret = si_http_require_capability(
        req, SI_CAPABILITY_OBSERVE, &session);
    if (ret != ESP_OK) return ret;
    if (session.principal != SI_PRINCIPAL_BROWSER)
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "browser operator required");
    char body[256] = {0};
    cJSON *root = NULL;
    ret = si_http_recv_json(req, body, sizeof(body), &root);
    if (ret != ESP_OK) return ret;
    const char *resource_value =
        json_string_any(root, "resource", "channel", NULL);
    char resource[32] = {0};
    if (resource_value) strlcpy(resource, resource_value, sizeof(resource));
    const cJSON *generation_item =
        cJSON_GetObjectItemCaseSensitive(root, "generation");
    uint32_t generation = cJSON_IsNumber(generation_item) &&
                          generation_item->valuedouble > 0 &&
                          generation_item->valuedouble <= UINT32_MAX ?
                          (uint32_t)generation_item->valuedouble : 0U;
    cJSON_Delete(root);
    if (!resource[0] || generation == 0U)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "resource and generation required");

    bool stopped = false;
    char run_id[SI_RESOURCE_OPERATION_RUN_ID_MAX_LEN + 1] = {0};
    if (strcmp(resource, "ssh") == 0 || strcmp(resource, "uart") == 0) {
        stopped = automation_stop_terminal(
            strcmp(resource, "uart") == 0 ? SI_TERMINAL_CHANNEL_UART :
                                             SI_TERMINAL_CHANNEL_SSH,
            generation, run_id, sizeof(run_id));
    } else if (strcmp(resource, "hid") == 0) {
        si_control_lease_status_t lease = {0};
        si_control_lease_get_status(&lease);
        if (lease.agent_active && lease.epoch == generation) {
            stopped = si_control_lease_update_for_auth_session(
                          "agent", session.session_id, session.generation,
                          "supervised", "manual global takeover", false,
                          true, automation_browser_hid_auth_live, &session) ==
                      SI_CONTROL_LEASE_UPDATE_OK;
        }
    } else if (strcmp(resource, "video-observe") == 0) {
        if (si_video_control_agent_stream_epoch() == generation) {
            si_video_control_release_agent();
            (void)si_video_control_apply();
            stopped = true;
        }
    } else {
        for (size_t i = 0; i < SI_RESOURCE_OPERATION_COUNT; i++) {
            if (strcmp(resource, resource_operation_name(
                                     (si_resource_operation_resource_t)i)) != 0)
                continue;
            si_resource_operation_status_t status = {0};
            if (resource_operation_get(
                    (si_resource_operation_resource_t)i, &status) &&
                status.active && status.generation == generation) {
                strlcpy(run_id, status.run_id, sizeof(run_id));
                stopped = resource_operation_request_cancel(
                    (si_resource_operation_resource_t)i, generation);
            }
            break;
        }
    }
    if (!stopped)
        return si_http_send_text_status(req, "409 Conflict",
                                        "operation changed, ended, or cannot be cancelled");
    if (run_id[0]) (void)automation_abort_active_agent(run_id);
    else if (strcmp(resource, "hid") == 0 ||
             strcmp(resource, "video-observe") == 0)
        (void)automation_abort_active_agent(NULL);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "accepted", true);
    automation_add_status_json(response);
    ret = si_http_send_json(req, response);
    cJSON_Delete(response);
    return ret;
}
