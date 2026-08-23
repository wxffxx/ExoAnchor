// Transitional Agent module: SSH execution tool. Shares the run context until its service API is finalized.
static bool agent_ssh_command_event_is_sensitive(const char *command)
{
    const char *const needles[] = {
        "password", "passwd", "api_key", "apikey", "token",
        "authorization:", "private key", "secret="
    };
    return agent_text_contains_any_ci(command, needles,
                                      sizeof(needles) / sizeof(needles[0]));
}

static bool agent_ssh_cancel_requested(void *user_ctx)
{
    const agent_ssh_event_ctx_t *ctx =
        (const agent_ssh_event_ctx_t *)user_ctx;
    return !ctx || agent_run_cancel_requested_now(ctx->job_id) ||
           terminal_control_cancel_requested(&ctx->terminal_control);
}

static si_ssh_exec_config_t *agent_ssh_exec_config_alloc(void)
{
    si_ssh_exec_config_t *config = agent_alloc(sizeof(*config));
    if (config) {
        memset(config, 0, sizeof(*config));
    }
    return config;
}

static void agent_ssh_exec_config_free(si_ssh_exec_config_t *config)
{
    if (!config) {
        return;
    }
    si_secret_store_clear(config, sizeof(*config));
    free(config);
}

static bool agent_execute_ssh_exec_tool(const char *job_id, const cJSON *call,
                                        int index, bool dry_run,
                                        cJSON *results, bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const char *command = agent_tool_command(call);
    char action_id[SI_ACTION_EVENT_ACTION_ID_MAX + 1U] = {0};
    (void)agent_run_next_action_id(job_id, "ssh", action_id,
                                   sizeof(action_id));
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddNumberToObject(item, "index", index);
    cJSON_AddStringToObject(item, "tool", "ssh_exec");
    cJSON_AddBoolToObject(item, "dry_run", dry_run);
    if (command) {
        cJSON_AddStringToObject(item, "command", command);
    }

    if (!command || !command[0]) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "ssh_exec command required");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    if (strlen(command) >= SI_SSH_COMMAND_MAX_LEN) {
        char msg[96];
        snprintf(msg, sizeof(msg), "ssh_exec command too long (%u >= %u)",
                 (unsigned)strlen(command), (unsigned)SI_SSH_COMMAND_MAX_LEN);
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", msg);
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    ssh_target_settings_t target;
    esp_err_t ret = ssh_target_load(&target);
    if (ret != ESP_OK) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", esp_err_to_name(ret));
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    const char *host = json_string_any(args, "host", "ip", "target");
    const char *username = json_string_any(args, "username", "user", "login");
    const char *password_arg = json_string_any(args, "password", "pass", NULL);
    const char *auth_method = json_string_any(args, "auth_method", "auth", NULL);
    const char *key_passphrase_arg = json_string_any(args, "key_passphrase", "passphrase", NULL);
    const cJSON *port_item = json_number_any(args, "port", "ssh_port");
    const cJSON *timeout_item = json_number_any(args, "timeout_ms", "timeout");
    bool timeout_explicit = timeout_item && timeout_item->valueint > 0;
    bool using_stored_target = false;
    if ((!host || !host[0]) && target.host[0]) {
        host = target.host;
        using_stored_target = true;
    }
    if ((!username || !username[0]) && target.username[0]) {
        username = target.username;
        using_stored_target = true;
    }
    if ((!auth_method || !auth_method[0]) && target.auth_method[0]) {
        auth_method = target.auth_method;
    }
    if (!host || !host[0] || !username || !username[0]) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "ssh target not configured");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    uint16_t port = port_item && port_item->valueint > 0 ? (uint16_t)port_item->valueint :
                    (target.port ? target.port : SSH_TARGET_DEFAULT_PORT);
    int timeout_value = timeout_explicit ? timeout_item->valueint :
                        (int)(target.timeout_ms ? target.timeout_ms :
                              SSH_TARGET_DEFAULT_TIMEOUT_MS);
    if (!timeout_explicit && agent_ssh_command_needs_long_timeout(command)) {
        timeout_value = (int)SSH_TARGET_MAX_TIMEOUT_MS;
    }
    if (timeout_value < 5000) {
        timeout_value = 5000;
    } else if (timeout_value > (int)SSH_TARGET_MAX_TIMEOUT_MS) {
        timeout_value = (int)SSH_TARGET_MAX_TIMEOUT_MS;
    }

    cJSON_AddStringToObject(item, "target_source", using_stored_target ? "settings" : "call");
    cJSON_AddStringToObject(item, "host", host);
    cJSON_AddNumberToObject(item, "port", port);
    cJSON_AddStringToObject(item, "username", username);
    cJSON_AddNumberToObject(item, "timeout_ms", timeout_value);
    cJSON_AddBoolToObject(item, "password_configured", target.password_configured);
    cJSON_AddBoolToObject(item, "sudo_password_configured",
                          target.sudo_password_configured);
    if (password_arg && password_arg[0]) {
        cJSON_AddBoolToObject(item, "password_arg_ignored", true);
    }
    if (key_passphrase_arg && key_passphrase_arg[0]) {
        cJSON_AddBoolToObject(item, "key_passphrase_arg_ignored", true);
    }
    if (dry_run) {
        bool redacted = agent_ssh_command_event_is_sensitive(command);
        agent_run_add_action_eventf(
            job_id, action_id, SI_ACTION_CHANNEL_SSH,
            SI_ACTION_PHASE_PLANNED, redacted, true,
            redacted ? "tool[%d] ssh_exec dry-run · command redacted" :
                       "tool[%d] ssh_exec dry-run $ %s",
            index + 1, command);
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddStringToObject(item, "status", "dry-run");
        cJSON_AddItemToArray(results, item);
        return false;
    }

    si_ssh_exec_config_t *config = agent_ssh_exec_config_alloc();
    if (!config) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "ssh config alloc failed");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    strlcpy(config->host, host, sizeof(config->host));
    config->port = port;
    strlcpy(config->username, username, sizeof(config->username));
    bool using_stored_password = false;
    bool has_password = ssh_target_apply_password(config->password, sizeof(config->password),
                                                  NULL, &using_stored_password);
    char sudo_password_buf[SI_SSH_PASSWORD_MAX_LEN] = {0};
    bool sudo_password_available = ssh_target_apply_sudo_password(sudo_password_buf,
                                                                  sizeof(sudo_password_buf));
    bool sudo_applied = false;
    if (!ssh_prepare_sudo_command(command,
                                  sudo_password_available ? sudo_password_buf : NULL,
                                  config, &sudo_applied)) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "ssh command too long after sudo wrapping");
        cJSON_AddItemToArray(results, item);
        memset(sudo_password_buf, 0, sizeof(sudo_password_buf));
        agent_ssh_exec_config_free(config);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    memset(sudo_password_buf, 0, sizeof(sudo_password_buf));
    config->timeout_ms = (uint32_t)timeout_value;
    bool command_redacted = agent_ssh_command_event_is_sensitive(command);
    agent_run_add_action_eventf(
        job_id, action_id, SI_ACTION_CHANNEL_SSH,
        SI_ACTION_PHASE_INPUT_SENT, command_redacted, true,
        command_redacted ? "tool[%d] ssh_exec · command redacted" :
                           "tool[%d] ssh_exec $ %s",
        index + 1, command);
    agent_run_add_action_eventf(
        job_id, action_id, SI_ACTION_CHANNEL_SSH,
        SI_ACTION_PHASE_STARTED, false, true,
        "tool[%d] running · timeout=%dms%s",
        index + 1, timeout_value,
        agent_ssh_command_needs_long_timeout(command) ?
        " · long operation" : "");

    bool use_key = auth_method && (strcasecmp(auth_method, "key") == 0 ||
                                   strcasecmp(auth_method, "stored_key") == 0 ||
                                   strcasecmp(auth_method, "publickey") == 0);
    bool auto_key = (!auth_method || strcasecmp(auth_method, "auto") == 0) &&
                    !has_password;
    if (use_key || auto_key) {
        ret = ssh_key_apply_to_exec(config, NULL);
        if (ret != ESP_OK && use_key) {
            cJSON_AddBoolToObject(item, "ok", false);
            cJSON_AddStringToObject(item, "error", "ssh private key not configured");
            cJSON_AddItemToArray(results, item);
            agent_ssh_exec_config_free(config);
            if (all_ok) {
                *all_ok = false;
            }
            return false;
        }
        if (ret != ESP_OK && auto_key) {
            cJSON_AddBoolToObject(item, "ok", false);
            cJSON_AddStringToObject(item, "error",
                                    "ssh credential not configured in Settings");
            cJSON_AddItemToArray(results, item);
            agent_ssh_exec_config_free(config);
            if (all_ok) {
                *all_ok = false;
            }
            return false;
        }
    } else if (!has_password && auth_method && strcasecmp(auth_method, "password") == 0) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "ssh password not configured in Settings");
        cJSON_AddItemToArray(results, item);
        agent_ssh_exec_config_free(config);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    char *output = heap_caps_calloc(1, AGENT_TOOL_OUTPUT_MAX + 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output) {
        output = calloc(1, AGENT_TOOL_OUTPUT_MAX + 1);
    }
    if (!output) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "ssh output alloc failed");
        cJSON_AddItemToArray(results, item);
        agent_ssh_exec_config_free(config);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    si_ssh_exec_result_t result = {
        .output = output,
        .output_size = AGENT_TOOL_OUTPUT_MAX + 1,
        .exit_status = -1,
    };
    agent_ssh_event_ctx_t event_ctx = {
        .job_id = job_id,
        .index = index,
        .last_emit_ms = 0,
    };
    strlcpy(event_ctx.action_id, action_id, sizeof(event_ctx.action_id));
    if (!terminal_control_begin(
            SI_TERMINAL_CHANNEL_SSH, SI_TERMINAL_ACTOR_AGENT, action_id,
            job_id, "Agent 执行 SSH 命令", &event_ctx.terminal_control)) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "SSH automation already active");
        cJSON_AddItemToArray(results, item);
        agent_ssh_exec_config_free(config);
        free(output);
        if (all_ok) *all_ok = false;
        return false;
    }
    terminal_control_publish_input(
        &event_ctx.terminal_control, command, true, command_redacted);
    config->output_cb = agent_ssh_output_event_cb;
    config->output_user_ctx = &event_ctx;
    config->cancel_cb = agent_ssh_cancel_requested;
    config->cancel_user_ctx = &event_ctx;
    ret = si_ssh_exec(config, &result);
    (void)terminal_control_finish(&event_ctx.terminal_control);
    bool ok = ret == ESP_OK && result.ok;
    cJSON_AddBoolToObject(item, "ok", ok);
    cJSON_AddStringToObject(item, "auth_method", config->use_private_key ? "key" : "password");
    cJSON_AddStringToObject(item, "credential_source",
                            config->use_private_key ? "stored_key" :
                            (using_stored_password ? "settings_password" : "none"));
    cJSON_AddBoolToObject(item, "sudo_applied", sudo_applied);
    cJSON_AddNumberToObject(item, "exit_status", result.exit_status);
    cJSON_AddNumberToObject(item, "ssh_rc", result.ssh_rc);
    cJSON_AddNumberToObject(item, "elapsed_ms", result.elapsed_ms);
    cJSON_AddBoolToObject(item, "truncated", result.truncated);
    cJSON_AddStringToObject(item, "output", result.output ? result.output : "");
    if (result.error[0]) {
        cJSON_AddStringToObject(item, "error", result.error);
    } else if (ret != ESP_OK) {
        cJSON_AddStringToObject(item, "error", esp_err_to_name(ret));
    }
    cJSON_AddItemToArray(results, item);
    agent_run_add_action_eventf(
        job_id, action_id, SI_ACTION_CHANNEL_SSH,
        ok ? SI_ACTION_PHASE_SUCCEEDED : SI_ACTION_PHASE_FAILED,
        false, true,
        "tool[%d] ssh_exec %s · exit=%d · %lldms%s%s",
        index + 1,
        ok ? "ok" : "failed",
        result.exit_status,
        (long long)result.elapsed_ms,
        result.error[0] ? " · " : "",
        result.error[0] ? result.error : "");
    agent_ssh_exec_config_free(config);
    free(output);
    if (!ok && all_ok) {
        *all_ok = false;
    }
    return true;
}
