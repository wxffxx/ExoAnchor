// Transitional Agent adapter for deterministic BIOS/Boot Menu entry.
// The Agent may inspect or propose a plan. Approval, start, cancellation and
// success confirmation remain authenticated browser actions.

static void agent_boot_key_add_status(cJSON *item,
                                      const si_boot_key_status_t *status)
{
    cJSON_AddStringToObject(item, "state",
                            si_boot_key_state_name(status->state));
    cJSON_AddStringToObject(item, "plan_id", status->plan_id);
    cJSON_AddBoolToObject(item, "approval_required",
                          status->approval_required);
    cJSON_AddBoolToObject(item, "approved", status->approved);
    cJSON_AddNumberToObject(item, "attempts_sent",
                            status->attempts_sent);
    cJSON_AddStringToObject(item, "profile_id",
                            status->config.profile_id);
    cJSON_AddStringToObject(item, "target",
                            si_boot_key_target_name(status->config.target));
    cJSON_AddStringToObject(item, "trigger",
                            si_boot_key_trigger_name(status->config.trigger));
    cJSON_AddStringToObject(item, "key_code",
                            status->config.key_code);
    cJSON_AddNumberToObject(item, "start_delay_ms",
                            status->config.start_delay_ms);
    cJSON_AddNumberToObject(item, "interval_ms",
                            status->config.interval_ms);
    cJSON_AddNumberToObject(item, "max_attempts",
                            status->config.max_attempts);
    cJSON_AddNumberToObject(item, "total_timeout_ms",
                            status->config.total_timeout_ms);
    cJSON_AddStringToObject(item, "verification_mode", "human_kvm");
    cJSON_AddBoolToObject(item, "agent_can_start", false);
    cJSON_AddBoolToObject(item, "agent_can_confirm", false);
    if (status->error[0]) {
        cJSON_AddStringToObject(item, "manager_error", status->error);
    }
}

static bool agent_boot_key_add_error(cJSON *results, int index,
                                     const char *operation,
                                     const char *error, bool *all_ok)
{
    cJSON *item = cJSON_CreateObject();
    if (item) {
        cJSON_AddNumberToObject(item, "index", index);
        cJSON_AddStringToObject(item, "tool", "boot_key_sequence");
        cJSON_AddStringToObject(item, "operation",
                                operation ? operation : "unknown");
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                error ? error : "boot-key error");
        cJSON_AddItemToArray(results, item);
    }
    if (all_ok) {
        *all_ok = false;
    }
    return false;
}

static si_boot_key_target_t agent_boot_key_target(const char *value)
{
    if (value && strcasecmp(value, "bios_setup") == 0) {
        return SI_BOOT_KEY_TARGET_BIOS_SETUP;
    }
    if (value && strcasecmp(value, "boot_menu") == 0) {
        return SI_BOOT_KEY_TARGET_BOOT_MENU;
    }
    return SI_BOOT_KEY_TARGET_INVALID;
}

static si_boot_key_trigger_t agent_boot_key_trigger(const char *value)
{
    if (value && strcasecmp(value, "none") == 0) {
        return SI_BOOT_KEY_TRIGGER_NONE;
    }
    if (value && strcasecmp(value, "reset") == 0) {
        return SI_BOOT_KEY_TRIGGER_RESET;
    }
    return (si_boot_key_trigger_t)-1;
}

static bool agent_boot_key_parse_uint(const cJSON *item, uint32_t min,
                                      uint32_t max, uint32_t *out)
{
    if (!cJSON_IsNumber(item) ||
        item->valuedouble != (double)item->valueint ||
        item->valueint < 0 || (uint32_t)item->valueint < min ||
        (uint32_t)item->valueint > max) {
        return false;
    }
    *out = (uint32_t)item->valueint;
    return true;
}

static bool agent_execute_boot_key_tool(
    const char *job_id, const cJSON *call, int index, bool dry_run,
    cJSON *results, bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const char *operation =
        json_string_any(args, "operation", "action", "mode");
    if (!operation || !operation[0]) {
        operation = "status";
    }

    if (strcasecmp(operation, "status") == 0) {
        si_boot_key_status_t status;
        if (si_boot_key_plan_get(&status) != ESP_OK) {
            return agent_boot_key_add_error(
                results, index, operation, "boot-key manager unavailable",
                all_ok);
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            return agent_boot_key_add_error(
                results, index, operation, "boot-key result alloc failed",
                all_ok);
        }
        cJSON_AddNumberToObject(item, "index", index);
        cJSON_AddStringToObject(item, "tool", "boot_key_sequence");
        cJSON_AddStringToObject(item, "operation", "status");
        cJSON_AddBoolToObject(item, "ok", true);
        agent_boot_key_add_status(item, &status);
        cJSON_AddItemToArray(results, item);
        return true;
    }

    if (strcasecmp(operation, "plan") != 0) {
        return agent_boot_key_add_error(
            results, index, operation,
            "Agent may only inspect or propose; use browser UI to approve, start, cancel, or confirm",
            all_ok);
    }

    const char *profile_id =
        json_string_any(args, "profile_id", "profile", "host_profile");
    const char *target =
        json_string_any(args, "target", "destination", NULL);
    const char *trigger =
        json_string_any(args, "trigger", "power_trigger", NULL);
    const char *key_code =
        json_string_any(args, "key_code", "key", NULL);
    const cJSON *start_delay =
        json_number_any(args, "start_delay_ms", "delay_ms");
    const cJSON *interval =
        json_number_any(args, "interval_ms", "key_interval_ms");
    const cJSON *max_attempts =
        json_number_any(args, "max_attempts", "attempts");
    const cJSON *total_timeout =
        json_number_any(args, "total_timeout_ms", "timeout_ms");

    si_boot_key_sequence_config_t config = {
        .target = agent_boot_key_target(target),
        .trigger = agent_boot_key_trigger(trigger),
    };
    strlcpy(config.profile_id, profile_id ? profile_id : "",
            sizeof(config.profile_id));
    strlcpy(config.key_code, key_code ? key_code : "",
            sizeof(config.key_code));
    uint32_t attempts_value = 0;
    bool parsed =
        agent_boot_key_parse_uint(
            start_delay, 0, 10000, &config.start_delay_ms) &&
        agent_boot_key_parse_uint(
            interval, 100, 5000, &config.interval_ms) &&
        agent_boot_key_parse_uint(
            max_attempts, 1, 20, &attempts_value) &&
        agent_boot_key_parse_uint(
            total_timeout, 1000, 60000, &config.total_timeout_ms);
    config.max_attempts = parsed ? (uint8_t)attempts_value : 0U;
    if (!parsed || !si_boot_key_sequence_validate(&config)) {
        return agent_boot_key_add_error(
            results, index, operation,
            "exact profile_id, target, trigger, allowed key, delay, interval, attempts, and timeout required",
            all_ok);
    }

    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return agent_boot_key_add_error(
            results, index, operation, "boot-key result alloc failed",
            all_ok);
    }
    cJSON_AddNumberToObject(item, "index", index);
    cJSON_AddStringToObject(item, "tool", "boot_key_sequence");
    cJSON_AddStringToObject(item, "operation", "plan");
    cJSON_AddBoolToObject(item, "dry_run", dry_run);
    if (dry_run) {
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddStringToObject(item, "state", "proposed");
        cJSON_AddStringToObject(item, "profile_id", config.profile_id);
        cJSON_AddStringToObject(item, "target",
                                si_boot_key_target_name(config.target));
        cJSON_AddStringToObject(item, "trigger",
                                si_boot_key_trigger_name(config.trigger));
        cJSON_AddStringToObject(item, "key_code", config.key_code);
        cJSON_AddItemToArray(results, item);
        return true;
    }

    si_boot_key_status_t status;
    esp_err_t ret = si_boot_key_plan_create(&config, &status);
    if (ret != ESP_OK) {
        cJSON_Delete(item);
        return agent_boot_key_add_error(
            results, index, operation, esp_err_to_name(ret), all_ok);
    }
    cJSON_AddBoolToObject(item, "ok", true);
    agent_boot_key_add_status(item, &status);
    cJSON_AddItemToArray(results, item);
    agent_run_add_action_eventf(
        job_id, status.plan_id, SI_ACTION_CHANNEL_HID,
        SI_ACTION_PHASE_WAITING_APPROVAL, false, true,
        "boot-key plan %s · %s %s · browser approval and start required",
        status.plan_id, status.config.key_code,
        si_boot_key_target_name(status.config.target));
    return true;
}
