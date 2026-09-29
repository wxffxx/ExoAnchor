// Agent-local device observation and supervised power tools.
static bool agent_run_control_point(const char *job_id);
static esp_err_t agent_run_acquire_kvm_control(
    const char *job_id, char *err, size_t err_size);
static bool agent_run_hid_owner_snapshot(
    const char *job_id, si_hid_owner_token_t *owner);
static void agent_run_release_hid_control(const char *job_id);

static cJSON *agent_tool_result_begin(cJSON *results, int index,
                                      const char *tool, bool dry_run)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return NULL;
    }
    cJSON_AddNumberToObject(item, "index", index);
    cJSON_AddStringToObject(item, "tool", tool);
    cJSON_AddBoolToObject(item, "dry_run", dry_run);
    cJSON_AddItemToArray(results, item);
    return item;
}

static bool agent_execute_observe_status_tool(const char *job_id,
                                              const cJSON *call, int index,
                                              bool dry_run, cJSON *results,
                                              bool *all_ok)
{
    (void)call;
    cJSON *item =
        agent_tool_result_begin(results, index, "observe_status", dry_run);
    if (!item) {
        if (all_ok) *all_ok = false;
        return false;
    }

    si_device_network_observation_t network;
    si_device_video_observation_t video;
    si_device_video_control_observation_t video_control;
    si_device_hid_observation_t hid;
    si_device_power_observation_t power;
    si_device_control_lease_observation_t lease;
    si_device_performance_observation_t performance;
    si_device_observation_get_network(&network);
    si_device_observation_get_video(&video, &video_control, NULL, 0, NULL);
    si_device_observation_get_hid(&hid);
    si_device_observation_get_power(&power);
    si_device_observation_get_control_lease(&lease);
    si_device_observation_get_performance(&performance);

    cJSON_AddBoolToObject(item, "ok", true);
    cJSON *network_json = cJSON_AddObjectToObject(item, "network");
    cJSON *video_json = cJSON_AddObjectToObject(item, "video");
    cJSON *hid_json = cJSON_AddObjectToObject(item, "hid");
    cJSON *power_json = cJSON_AddObjectToObject(item, "power");
    cJSON *lease_json = cJSON_AddObjectToObject(item, "control_lease");
    cJSON *performance_json = cJSON_AddObjectToObject(item, "performance");
    si_device_http_add_network_json(network_json, &network);
    si_device_http_add_video_json(video_json, &video, &video_control, NULL, 0);
    si_device_http_add_hid_json(hid_json, &hid);
    si_device_http_add_power_json(power_json, &power);
    si_device_http_add_control_lease_json(lease_json, &lease);
    si_device_http_add_performance_json(
        performance_json, &performance, si_diagnostics_web_client_count());
    agent_run_add_action_eventf(
        job_id, NULL, SI_ACTION_CHANNEL_SYSTEM, SI_ACTION_PHASE_SUCCEEDED,
        false, true, "tool[%d] observe_status · device snapshot captured",
        index + 1);
    return true;
}

static bool agent_execute_observe_video_status_tool(
    const char *job_id, const cJSON *call, int index, bool dry_run,
    cJSON *results, bool *all_ok)
{
    (void)call;
    cJSON *item = agent_tool_result_begin(
        results, index, "observe_video_status", dry_run);
    if (!item) {
        if (all_ok) *all_ok = false;
        return false;
    }
    si_device_video_observation_t video;
    si_device_video_control_observation_t control;
    si_device_observation_get_video(&video, &control, NULL, 0, NULL);
    cJSON_AddBoolToObject(item, "ok", true);
    si_device_http_add_video_json(item, &video, &control, NULL, 0);
    agent_run_add_action_eventf(
        job_id, NULL, SI_ACTION_CHANNEL_VIDEO, SI_ACTION_PHASE_SUCCEEDED,
        false, true, "tool[%d] observe_video_status · %ux%u @ %.2ffps",
        index + 1, (unsigned)video.width, (unsigned)video.height,
        video.fps_x100 / 100.0);
    return true;
}

static bool agent_execute_observe_hid_status_tool(
    const char *job_id, const cJSON *call, int index, bool dry_run,
    cJSON *results, bool *all_ok)
{
    (void)call;
    cJSON *item = agent_tool_result_begin(
        results, index, "observe_hid_status", dry_run);
    if (!item) {
        if (all_ok) *all_ok = false;
        return false;
    }
    si_device_hid_observation_t hid;
    si_device_observation_get_hid(&hid);
    cJSON_AddBoolToObject(item, "ok", true);
    si_device_http_add_hid_json(item, &hid);
    agent_run_add_action_eventf(
        job_id, NULL, SI_ACTION_CHANNEL_HID, SI_ACTION_PHASE_SUCCEEDED,
        false, true, "tool[%d] observe_hid_status · %s",
        index + 1, hid.ready ? "ready" : "not ready");
    return true;
}

static esp_err_t agent_console_wait_keyboard(const char *job_id)
{
    TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(500);
    for (;;) {
        if (!agent_run_control_point(job_id)) {
            return ESP_ERR_INVALID_STATE;
        }
        si_hid_status_t status;
        si_hid_get_status(&status);
        if (!status.ready) {
            return ESP_ERR_INVALID_STATE;
        }
        if (status.keyboard_writable) {
            return ESP_OK;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static esp_err_t agent_console_hid_key(const char *job_id,
                                       si_hid_command_type_t type,
                                       const char *code)
{
    ESP_RETURN_ON_ERROR(agent_console_wait_keyboard(job_id), TAG,
                        "wait for Console keyboard endpoint");
    si_hid_command_t command = {
        .type = type,
        .code = code,
        .error = "console login HID key failed",
    };
    si_hid_owner_token_t owner = {0};
    if (!agent_run_hid_owner_snapshot(job_id, &owner)) {
        return ESP_ERR_INVALID_STATE;
    }
    return si_hid_execute_owned(&command, &owner, NULL, NULL);
}

static void agent_console_hid_release(const char *job_id)
{
    si_hid_command_t command = {
        .type = SI_HID_COMMAND_RELEASE_ALL,
        .error = "console login HID release failed",
    };
    si_hid_owner_token_t owner = {0};
    if (agent_run_hid_owner_snapshot(job_id, &owner)) {
        (void)si_hid_execute_owned(&command, &owner, NULL, NULL);
    }
}

static esp_err_t agent_console_hid_tap(const char *job_id,
                                       const char *code, bool shift,
                                       uint32_t key_delay_ms)
{
    esp_err_t ret = ESP_OK;
    if (shift) {
        ret = agent_console_hid_key(
            job_id, SI_HID_COMMAND_KEY_DOWN, "ShiftLeft");
    }
    if (ret == ESP_OK) {
        ret = agent_console_hid_key(
            job_id, SI_HID_COMMAND_KEY_DOWN, code);
    }
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(8));
        ret = agent_console_hid_key(
            job_id, SI_HID_COMMAND_KEY_UP, code);
    }
    if (shift) {
        esp_err_t shift_ret =
            agent_console_hid_key(
                job_id, SI_HID_COMMAND_KEY_UP, "ShiftLeft");
        if (ret == ESP_OK) {
            ret = shift_ret;
        }
    }
    if (ret != ESP_OK) {
        agent_console_hid_release(job_id);
        return ret;
    }
    if (key_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(key_delay_ms));
    }
    return ESP_OK;
}

static esp_err_t agent_console_hid_type(const char *job_id, const char *text,
                                        uint32_t key_delay_ms)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor; cursor++) {
        if (!agent_run_control_point(job_id)) {
            agent_console_hid_release(job_id);
            return ESP_ERR_INVALID_STATE;
        }
        si_hid_ascii_key_t key;
        if (!si_hid_ascii_map(*cursor, &key)) {
            agent_console_hid_release(job_id);
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t ret =
            agent_console_hid_tap(
                job_id, key.code, key.shift, key_delay_ms);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static bool agent_console_stage_valid(const char *stage)
{
    return stage &&
           (strcmp(stage, "status") == 0 ||
            strcmp(stage, "username") == 0 ||
            strcmp(stage, "password") == 0 ||
            strcmp(stage, "both") == 0 ||
            strcmp(stage, "type_username") == 0 ||
            strcmp(stage, "type_password") == 0);
}

static bool agent_execute_console_login_tool(
    const char *job_id, const cJSON *call, int index, bool dry_run,
    cJSON *results, bool *all_ok)
{
    static const char *const credential_ref = "console://default";
    const cJSON *args = agent_tool_args(call);
    const char *stage = json_string_any(args, "stage", "operation", "mode");
    const char *requested_ref =
        json_string_any(args, "credential_ref", "credential", "ref");
    const char *between =
        json_string_any(args, "between", "username_submit", "next");
    const cJSON *key_delay_item =
        json_number_any(args, "key_delay_ms", "key_delay");
    const cJSON *field_delay_item =
        json_number_any(args, "inter_field_delay_ms", "wait_ms");
    uint32_t key_delay_ms =
        key_delay_item && key_delay_item->valuedouble >= 0 ?
        (uint32_t)key_delay_item->valueint : 12;
    uint32_t field_delay_ms =
        field_delay_item && field_delay_item->valuedouble >= 0 ?
        (uint32_t)field_delay_item->valueint : 700;
    if (!stage || !stage[0]) {
        stage = "status";
    }
    if (!between || !between[0]) {
        between = "enter";
    }

    cJSON *item =
        agent_tool_result_begin(results, index, "console_login", dry_run);
    if (!item) {
        if (all_ok) *all_ok = false;
        return false;
    }
    cJSON_AddStringToObject(item, "credential_ref", credential_ref);
    cJSON_AddStringToObject(item, "stage", stage);
    cJSON_AddBoolToObject(item, "secret_redacted", true);

    si_console_credentials_status_t credentials;
    esp_err_t ret = si_console_credentials_get_status(&credentials);
    if (ret != ESP_OK) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "Console credential status unavailable");
        if (all_ok) *all_ok = false;
        return false;
    }
    cJSON_AddBoolToObject(item, "configured", credentials.configured);
    cJSON_AddBoolToObject(item, "password_configured",
                          credentials.password_configured);

    bool valid =
        agent_console_stage_valid(stage) &&
        (!requested_ref || strcmp(requested_ref, credential_ref) == 0) &&
        key_delay_ms <= 200 && field_delay_ms <= 5000 &&
        (strcmp(between, "tab") == 0 || strcmp(between, "enter") == 0);
    if (!valid) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "invalid Console login arguments");
        if (all_ok) *all_ok = false;
        return false;
    }
    if (strcmp(stage, "status") == 0) {
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddStringToObject(item, "status", "local-only");
        return true;
    }

    bool needs_username =
        strcmp(stage, "username") == 0 ||
        strcmp(stage, "both") == 0 ||
        strcmp(stage, "type_username") == 0;
    bool needs_password =
        strcmp(stage, "password") == 0 ||
        strcmp(stage, "both") == 0 ||
        strcmp(stage, "type_password") == 0;
    if (!credentials.configured ||
        (needs_password && !credentials.password_configured)) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "Console credential is not fully configured");
        if (all_ok) *all_ok = false;
        return false;
    }
    if (dry_run) {
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddStringToObject(item, "status", "dry-run");
        return false;
    }
    if (!si_hid_is_ready()) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "USB HID is not ready");
        if (all_ok) *all_ok = false;
        return false;
    }
    char control_error[96] = {0};
    ret = agent_run_acquire_kvm_control(job_id, control_error,
                                        sizeof(control_error));
    if (ret != ESP_OK) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                control_error[0] ? control_error :
                                "Agent control lease unavailable");
        if (all_ok) *all_ok = false;
        return false;
    }

    char password[SI_CONSOLE_PASSWORD_MAX_LEN] = {0};
    if (needs_password) {
        ret = si_console_credentials_get_password(password,
                                                  sizeof(password));
    }
    agent_console_hid_release(job_id);
    if (ret == ESP_OK && needs_username) {
        ret = agent_console_hid_type(job_id, credentials.username,
                                     key_delay_ms);
    }
    if (ret == ESP_OK && strcmp(stage, "both") == 0) {
        ret = agent_console_hid_tap(
            job_id,
            strcmp(between, "enter") == 0 ? "Enter" : "Tab",
            false, key_delay_ms);
        while (ret == ESP_OK && field_delay_ms > 0) {
            uint32_t slice_ms = MIN(field_delay_ms, 50U);
            vTaskDelay(pdMS_TO_TICKS(slice_ms));
            field_delay_ms -= slice_ms;
            if (!agent_run_control_point(job_id)) {
                ret = ESP_ERR_INVALID_STATE;
            }
        }
    } else if (ret == ESP_OK && strcmp(stage, "username") == 0) {
        ret = agent_console_hid_tap(
            job_id, "Enter", false, key_delay_ms);
    }
    if (ret == ESP_OK && needs_password) {
        ret = agent_console_hid_type(job_id, password, key_delay_ms);
    }
    si_secret_store_clear(password, sizeof(password));
    if (ret == ESP_OK && strcmp(stage, "password") == 0) {
        ret = agent_console_hid_tap(
            job_id, "Enter", false, key_delay_ms);
    } else if (ret == ESP_OK && strcmp(stage, "both") == 0) {
        ret = agent_console_hid_tap(
            job_id, "Enter", false, key_delay_ms);
    }
    agent_console_hid_release(job_id);

    bool ok = ret == ESP_OK;
    cJSON_AddBoolToObject(item, "ok", ok);
    cJSON_AddStringToObject(item, "status",
                            ok ? "injected-locally" : "hid-failed");
    cJSON_AddBoolToObject(item, "submitted",
                          ok && (strcmp(stage, "username") == 0 ||
                                 strcmp(stage, "password") == 0 ||
                                 strcmp(stage, "both") == 0));
    cJSON_AddBoolToObject(item, "verification_required",
                          ok && strcmp(stage, "status") != 0);
    if (ok && strcmp(stage, "status") != 0) {
        cJSON_AddStringToObject(item, "next_observation",
                                "observe_screenshot");
    }
    if (!ok) {
        cJSON_AddStringToObject(item, "error", esp_err_to_name(ret));
        if (all_ok) *all_ok = false;
    }
    agent_run_add_action_eventf(
        job_id, NULL, SI_ACTION_CHANNEL_HID,
        ok ? SI_ACTION_PHASE_SUCCEEDED : SI_ACTION_PHASE_FAILED,
        true, false, "tool[%d] console_login · %s · secret redacted · %s",
        index + 1, stage, ok ? "ok" : esp_err_to_name(ret));
    return true;
}

static void agent_hid_action_summary(const cJSON *action, int index,
                                     char *out, size_t out_size)
{
    const char *type = json_string_any(action, "type", "action", NULL);
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(action, "x");
    const cJSON *y = cJSON_GetObjectItemCaseSensitive(action, "y");
    const cJSON *dx = cJSON_GetObjectItemCaseSensitive(action, "dx");
    const cJSON *dy = cJSON_GetObjectItemCaseSensitive(action, "dy");
    const cJSON *button = cJSON_GetObjectItemCaseSensitive(action, "button");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(action, "code");

    if (cJSON_IsNumber(x) || cJSON_IsNumber(y)) {
        if (cJSON_IsNumber(button)) {
            snprintf(out, out_size, "hid[%d] %s · x=%d y=%d button=%d",
                     index + 1, type ? type : "unknown",
                     cJSON_IsNumber(x) ? x->valueint : 0,
                     cJSON_IsNumber(y) ? y->valueint : 0,
                     button->valueint);
        } else {
            snprintf(out, out_size, "hid[%d] %s · x=%d y=%d",
                     index + 1, type ? type : "unknown",
                     cJSON_IsNumber(x) ? x->valueint : 0,
                     cJSON_IsNumber(y) ? y->valueint : 0);
        }
    } else if (cJSON_IsNumber(dx) || cJSON_IsNumber(dy)) {
        snprintf(out, out_size, "hid[%d] %s · dx=%d dy=%d",
                 index + 1, type ? type : "unknown",
                 cJSON_IsNumber(dx) ? dx->valueint : 0,
                 cJSON_IsNumber(dy) ? dy->valueint : 0);
    } else if (cJSON_IsString(code)) {
        snprintf(out, out_size, "hid[%d] %s · %s",
                 index + 1, type ? type : "unknown", code->valuestring);
    } else if (cJSON_IsNumber(button)) {
        snprintf(out, out_size, "hid[%d] %s · button=%d",
                 index + 1, type ? type : "unknown", button->valueint);
    } else {
        snprintf(out, out_size, "hid[%d] %s",
                 index + 1, type ? type : "unknown");
    }
}

static bool agent_execute_power_action_tool(const char *job_id,
                                            const cJSON *call, int index,
                                            bool dry_run, cJSON *results,
                                            bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const char *action = json_string_any(args, "action", "command", "button");
    const cJSON *duration = json_number_any(args, "duration_ms", "duration");
    uint32_t duration_ms =
        duration && duration->valuedouble >= 0 ?
        (uint32_t)duration->valueint : 0;
    cJSON *item =
        agent_tool_result_begin(results, index, "power_action", dry_run);
    if (!item) {
        if (all_ok) *all_ok = false;
        return false;
    }
    if (!action || !action[0] ||
        (duration && duration->valuedouble < 0)) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "power_action action and non-negative duration_ms required");
        if (all_ok) *all_ok = false;
        return false;
    }
    bool action_supported =
        strcasecmp(action, "power") == 0 ||
        strcasecmp(action, "press_power") == 0 ||
        strcasecmp(action, "reset") == 0 ||
        strcasecmp(action, "press_reset") == 0 ||
        strcasecmp(action, "force_off") == 0 ||
        strcasecmp(action, "forceoff") == 0 ||
        strcasecmp(action, "locator_on") == 0 ||
        strcasecmp(action, "identify_on") == 0 ||
        strcasecmp(action, "locator_off") == 0 ||
        strcasecmp(action, "identify_off") == 0 ||
        strcasecmp(action, "locator_toggle") == 0 ||
        strcasecmp(action, "identify") == 0;
    if (!action_supported) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "unsupported power action");
        if (all_ok) *all_ok = false;
        return false;
    }
    cJSON_AddStringToObject(item, "action", action);
    cJSON_AddNumberToObject(item, "duration_ms", duration_ms);
    char action_id[SI_ACTION_EVENT_ACTION_ID_MAX + 1U] = {0};
    (void)agent_run_next_action_id(job_id, "power", action_id,
                                   sizeof(action_id));
    if (dry_run) {
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddStringToObject(item, "status", "dry-run");
        agent_run_add_action_eventf(
            job_id, action_id, SI_ACTION_CHANNEL_SYSTEM,
            SI_ACTION_PHASE_PLANNED, false, true,
            "tool[%d] power_action dry-run · %s", index + 1, action);
        return false;
    }

    si_resource_operation_token_t operation = {0};
    if (!resource_operation_begin(
            SI_RESOURCE_OPERATION_POWER,
            SI_RESOURCE_OPERATION_ACTOR_AGENT,
            action_id[0] ? action_id : "agent-power", job_id,
            "Agent 正在操作主机电源", true, &operation)) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(
            item, "error",
            "power or maintenance operation already active");
        if (all_ok) *all_ok = false;
        return false;
    }

    agent_run_add_action_eventf(
        job_id, action_id, SI_ACTION_CHANNEL_SYSTEM,
        SI_ACTION_PHASE_INPUT_SENT, false, true,
        "tool[%d] power_action · %s", index + 1, action);
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    if (strcasecmp(action, "power") == 0 ||
        strcasecmp(action, "press_power") == 0) {
        ret = si_power_press_power_controlled(
            duration_ms, power_resource_cancel_cb, &operation);
    } else if (strcasecmp(action, "reset") == 0 ||
               strcasecmp(action, "press_reset") == 0) {
        ret = si_power_press_reset_controlled(
            duration_ms, power_resource_cancel_cb, &operation);
    } else if (strcasecmp(action, "force_off") == 0 ||
               strcasecmp(action, "forceoff") == 0) {
        ret = si_power_force_off_controlled(
            duration_ms, power_resource_cancel_cb, &operation);
    } else if (strcasecmp(action, "locator_on") == 0 ||
               strcasecmp(action, "identify_on") == 0) {
        ret = si_power_set_locator(true);
    } else if (strcasecmp(action, "locator_off") == 0 ||
               strcasecmp(action, "identify_off") == 0) {
        ret = si_power_set_locator(false);
    } else if (strcasecmp(action, "locator_toggle") == 0 ||
               strcasecmp(action, "identify") == 0) {
        ret = si_power_toggle_locator();
    }
    (void)resource_operation_finish(&operation);
    bool ok = ret == ESP_OK;
    cJSON_AddBoolToObject(item, "ok", ok);
    if (!ok) {
        cJSON_AddStringToObject(item, "error", esp_err_to_name(ret));
        if (all_ok) *all_ok = false;
    }
    cJSON *status = cJSON_AddObjectToObject(item, "status");
    add_power_json(status);
    agent_run_add_action_eventf(
        job_id, action_id, SI_ACTION_CHANNEL_SYSTEM,
        ok ? SI_ACTION_PHASE_SUCCEEDED : SI_ACTION_PHASE_FAILED,
        false, true, "tool[%d] power_action %s · %s",
        index + 1, ok ? "ok" : "failed", action);
    return true;
}
