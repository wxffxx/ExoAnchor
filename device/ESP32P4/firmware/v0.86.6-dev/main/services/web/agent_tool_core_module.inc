// Transitional Agent module: tool classification, search and operability helpers. Shares the run context until its service API is finalized.
static bool agent_tool_is_ssh_exec(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "ssh_exec") == 0 ||
            strcasecmp(name, "ssh.exec") == 0 ||
            strcasecmp(name, "shell.exec") == 0);
}

static bool agent_tool_is_web_search(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "web_search") == 0 ||
            strcasecmp(name, "web.search") == 0 ||
            strcasecmp(name, "search") == 0);
}

static bool agent_tool_is_memory_search(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "memory_search") == 0 ||
            strcasecmp(name, "memory.search") == 0 ||
            strcasecmp(name, "working_memory.search") == 0);
}

static bool agent_tool_is_history_search(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "history_search") == 0 ||
            strcasecmp(name, "history.search") == 0 ||
            strcasecmp(name, "conversation_history.search") == 0);
}

static bool agent_tool_is_memory_write(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "memory_write") == 0 ||
            strcasecmp(name, "memory.write") == 0 ||
           strcasecmp(name, "working_memory.write") == 0);
}

static bool agent_tool_is_ask_user(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "ask_user") == 0 ||
            strcasecmp(name, "user.ask") == 0 ||
            strcasecmp(name, "request_user_input") == 0);
}

static bool agent_tool_is_host_display(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "host_display") == 0 ||
            strcasecmp(name, "host.display") == 0 ||
            strcasecmp(name, "host_display_mode") == 0);
}

static bool agent_tool_is_boot_key(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "boot_key_sequence") == 0 ||
            strcasecmp(name, "boot_key") == 0 ||
            strcasecmp(name, "host.boot_key") == 0);
}

static bool agent_tool_is_observe_status(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "observe_status") == 0 ||
            strcasecmp(name, "device.status") == 0 ||
            strcasecmp(name, "status.observe") == 0);
}

static bool agent_tool_is_observe_video_status(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "observe_video_status") == 0 ||
            strcasecmp(name, "video.status") == 0);
}

static bool agent_tool_is_observe_hid_status(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "observe_hid_status") == 0 ||
            strcasecmp(name, "hid.status") == 0);
}

static bool agent_tool_is_console_login(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "console_login") == 0 ||
            strcasecmp(name, "console.login") == 0 ||
            strcasecmp(name, "kvm_login") == 0);
}

static bool agent_tool_is_power_action(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "power_action") == 0 ||
            strcasecmp(name, "power.action") == 0);
}

static bool agent_tool_is_wait(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "wait") == 0 ||
            strcasecmp(name, "runtime.wait") == 0);
}

static bool agent_tool_is_uart_status(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "uart_status") == 0 ||
            strcasecmp(name, "uart.status") == 0);
}

static bool agent_tool_is_uart_read(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "uart_read") == 0 ||
            strcasecmp(name, "uart.read") == 0);
}

static bool agent_tool_is_uart_write(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "uart_write") == 0 ||
            strcasecmp(name, "uart.write") == 0);
}

static bool agent_tool_is_uart_auth(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "uart_auth") == 0 ||
            strcasecmp(name, "uart.authenticate") == 0);
}

static bool agent_tool_is_uart_baud(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "uart_baud") == 0 ||
            strcasecmp(name, "uart.baud") == 0);
}

static bool agent_tool_is_ssh_primitive(const cJSON *call)
{
    const char *name = agent_tool_name(call);
    return name &&
           (strcasecmp(name, "ensure_package") == 0 ||
            strcasecmp(name, "ensure_user") == 0 ||
            strcasecmp(name, "ensure_directory") == 0 ||
            strcasecmp(name, "download_file") == 0 ||
            strcasecmp(name, "write_file") == 0 ||
            strcasecmp(name, "ensure_systemd_service") == 0 ||
            strcasecmp(name, "check_service") == 0 ||
           strcasecmp(name, "verify_port") == 0);
}

static bool agent_tool_is_runtime_call(const cJSON *call)
{
    return agent_tool_is_ssh_exec(call) ||
           agent_tool_is_web_search(call) ||
           agent_tool_is_memory_search(call) ||
           agent_tool_is_history_search(call) ||
           agent_tool_is_memory_write(call) ||
           agent_tool_is_ask_user(call) ||
           agent_tool_is_host_display(call) ||
           agent_tool_is_boot_key(call) ||
           agent_tool_is_observe_status(call) ||
           agent_tool_is_observe_video_status(call) ||
           agent_tool_is_observe_hid_status(call) ||
           agent_tool_is_console_login(call) ||
           agent_tool_is_power_action(call) ||
           agent_tool_is_wait(call) ||
           agent_tool_is_uart_status(call) ||
           agent_tool_is_uart_read(call) ||
           agent_tool_is_uart_write(call) ||
           agent_tool_is_uart_auth(call) ||
           agent_tool_is_uart_baud(call) ||
           agent_tool_is_ssh_primitive(call);
}

static const char *agent_tool_command(const cJSON *call)
{
    const cJSON *args = agent_tool_args(call);
    const char *command = json_string_any(args, "command", "cmd", "input");
    if (command && command[0]) {
        return command;
    }
    return json_string_any(call, "command", "cmd", "input");
}

static const char *agent_tool_query(const cJSON *call)
{
    const cJSON *args = agent_tool_args(call);
    const char *query = json_string_any(args, "query", "q", "input");
    if (query && query[0]) {
        return query;
    }
    return json_string_any(call, "query", "q", "input");
}

static bool agent_route_text_contains_ci(const char *text, const char *needle)
{
    if (!text || !needle || !needle[0]) {
        return false;
    }
    size_t needle_len = strlen(needle);
    for (const char *p = text; *p; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool agent_text_contains_any_ci(const char *text, const char *const *needles, size_t count)
{
    if (!text) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (agent_route_text_contains_ci(text, needles[i])) {
            return true;
        }
    }
    return false;
}

static void agent_add_operability_review(cJSON *run_resp)
{
    if (!run_resp) {
        return;
    }

    char *state = cJSON_PrintUnformatted(run_resp);
    if (!state) {
        return;
    }

    const char *const screen_needles[] = {
        "screen -dm", "screen -dmS", "/usr/bin/screen -dm",
        "screen -S", "screen -r", "screen -dr", "screen -x"
    };
    const char *const tmux_needles[] = {
        "tmux new", "tmux attach", "tmux a", "tmux send-keys"
    };
    const char *const service_needles[] = {
        "systemctl start", "systemctl stop", "systemctl restart",
        "systemctl enable", "systemctl disable", "systemctl daemon-reload",
        "/etc/systemd/system/", "ensure_systemd_service", "ExecStart="
    };
    const char *const container_needles[] = {
        "docker run", "docker compose up", "docker start", "docker stop",
        "podman run", "podman start", "podman stop"
    };
    const char *const background_needles[] = {
        "nohup", "daemon", "background process", "后台进程"
    };
    const char *const owner_risk_needles[] = {
        "sudo -u", "su ", "User=", "useradd", "usermod",
        "ensure_user", "--user"
    };
    const char *const owner_evidence_needles[] = {
        "sudo -u", "su ", "User=", "useradd", "usermod", "ensure_user",
        "getent passwd", "whoami", " id ", "uid=", "gid="
    };
    const char *const screen_access_needles[] = {
        "screen -r", "screen -dr", "screen -x", "screen -S", "Ctrl+A",
        "ctrl+a", "-X stuff"
    };
    const char *const tmux_access_needles[] = {
        "tmux attach", "tmux a", "tmux send-keys", "tmux ls"
    };
    const char *const service_access_needles[] = {
        "systemctl status", "systemctl is-active", "journalctl -u",
        "systemctl is-enabled"
    };
    const char *const container_access_needles[] = {
        "docker ps", "docker logs", "docker exec", "podman ps",
        "podman logs", "podman exec"
    };
    const char *const generic_access_needles[] = {
        "ss -ltn", "ss -tuln", "curl ", "nc -z", "pgrep", "access_command",
        "访问", "控制", "console", "attach"
    };
    const char *const reboot_needles[] = {
        "systemctl enable", "systemctl disable", "systemctl is-enabled",
        "enabled", "disabled", "Restart=", "restart=", "开机", "自启动",
        "重启后", "reboot"
    };

    bool uses_screen = agent_text_contains_any_ci(state, screen_needles,
                                                  sizeof(screen_needles) / sizeof(screen_needles[0]));
    bool uses_tmux = agent_text_contains_any_ci(state, tmux_needles,
                                                sizeof(tmux_needles) / sizeof(tmux_needles[0]));
    bool uses_service = agent_text_contains_any_ci(state, service_needles,
                                                   sizeof(service_needles) / sizeof(service_needles[0]));
    bool uses_container = agent_text_contains_any_ci(state, container_needles,
                                                     sizeof(container_needles) / sizeof(container_needles[0]));
    bool uses_background = agent_text_contains_any_ci(state, background_needles,
                                                      sizeof(background_needles) / sizeof(background_needles[0]));
    bool owner_relevant = agent_text_contains_any_ci(state, owner_risk_needles,
                                                     sizeof(owner_risk_needles) / sizeof(owner_risk_needles[0]));
    bool required = uses_screen || uses_tmux || uses_service || uses_container ||
                    uses_background || owner_relevant;

    bool screen_ok = !uses_screen ||
                     agent_text_contains_any_ci(state, screen_access_needles,
                                                sizeof(screen_access_needles) / sizeof(screen_access_needles[0]));
    bool tmux_ok = !uses_tmux ||
                   agent_text_contains_any_ci(state, tmux_access_needles,
                                              sizeof(tmux_access_needles) / sizeof(tmux_access_needles[0]));
    bool service_ok = !uses_service ||
                      agent_text_contains_any_ci(state, service_access_needles,
                                                 sizeof(service_access_needles) / sizeof(service_access_needles[0]));
    bool container_ok = !uses_container ||
                        agent_text_contains_any_ci(state, container_access_needles,
                                                   sizeof(container_access_needles) / sizeof(container_access_needles[0]));
    bool generic_ok = !(uses_background || owner_relevant) ||
                      agent_text_contains_any_ci(state, generic_access_needles,
                                                 sizeof(generic_access_needles) / sizeof(generic_access_needles[0]));
    bool owner_ok = !owner_relevant ||
                    agent_text_contains_any_ci(state, owner_evidence_needles,
                                               sizeof(owner_evidence_needles) / sizeof(owner_evidence_needles[0]));
    bool reboot_relevant = uses_service || uses_container || uses_background;
    bool reboot_ok = !reboot_relevant ||
                     agent_text_contains_any_ci(state, reboot_needles,
                                                sizeof(reboot_needles) / sizeof(reboot_needles[0]));
    bool validated = !required || (screen_ok && tmux_ok && service_ok &&
                                   container_ok && generic_ok && owner_ok &&
                                   reboot_ok);

    cJSON_DeleteItemFromObjectCaseSensitive(run_resp, "operability_review");
    cJSON *review = cJSON_AddObjectToObject(run_resp, "operability_review");
    if (review) {
        cJSON_AddBoolToObject(review, "required", required);
        cJSON_AddBoolToObject(review, "validated", validated);
        cJSON_AddBoolToObject(review, "screen_or_tty_session", uses_screen || uses_tmux);
        cJSON_AddBoolToObject(review, "service_or_container", uses_service || uses_container);
        cJSON_AddBoolToObject(review, "separate_run_user_or_sudo", owner_relevant);
        cJSON_AddBoolToObject(review, "background_process", uses_background);
        cJSON_AddBoolToObject(review, "operator_access_evidence", screen_ok && tmux_ok &&
                              service_ok && container_ok && generic_ok);
        cJSON_AddBoolToObject(review, "run_owner_evidence", owner_ok);
        cJSON_AddBoolToObject(review, "reboot_behavior_evidence", reboot_ok);
        cJSON *checks = cJSON_AddArrayToObject(review, "required_checks");
        if (checks) {
            cJSON_AddItemToArray(checks, cJSON_CreateString("identify run owner versus SSH login user"));
            cJSON_AddItemToArray(checks, cJSON_CreateString("verify exact operator access/control command"));
            cJSON_AddItemToArray(checks, cJSON_CreateString("verify service/session/process status"));
            cJSON_AddItemToArray(checks, cJSON_CreateString("report reboot/autostart behavior"));
        }
    }
    free(state);
}

static bool agent_ssh_command_needs_long_timeout(const char *command)
{
    return agent_route_text_contains_ci(command, "apt ") ||
           agent_route_text_contains_ci(command, "apt-get") ||
           agent_route_text_contains_ci(command, "curl ") ||
           agent_route_text_contains_ci(command, "wget ") ||
           agent_route_text_contains_ci(command, "download") ||
           agent_route_text_contains_ci(command, "openjdk") ||
           agent_route_text_contains_ci(command, "install ") ||
           agent_route_text_contains_ci(command, "systemctl start");
}

static void agent_history_add_limited_content(cJSON *entry, const char *content);
static esp_err_t agent_memory_append_entry(cJSON *entry);
static bool agent_run_next_action_id(const char *job_id, const char *prefix,
                                     char *out, size_t out_size);
static void agent_run_add_action_eventf(const char *job_id,
                                        const char *action_id,
                                        si_action_channel_t channel,
                                        si_action_phase_t phase,
                                        bool redacted,
                                        bool background,
                                        const char *fmt, ...);
static void agent_run_add_eventf(const char *job_id, const char *kind,
                                 const char *fmt, ...);

typedef struct {
    cJSON *records;
    const char *query;
    const char *type;
    const char *session_id;
    bool memory;
    bool session_filter;
    uint32_t skip_count;
    uint32_t matched;
    uint32_t returned;
} agent_record_search_ctx_t;

static uint32_t agent_search_limit_from_args(const cJSON *args, uint32_t default_limit,
                                             uint32_t max_limit)
{
    const cJSON *limit_item = json_number_any(args, "limit", "max");
    if (!cJSON_IsNumber(limit_item)) {
        limit_item = json_number_any(args, "count", "return_max");
    }
    uint32_t limit = default_limit;
    if (cJSON_IsNumber(limit_item)) {
        int value = (int)cJSON_GetNumberValue(limit_item);
        if (value > 0) {
            limit = (uint32_t)value;
        }
    }
    if (limit == 0) {
        limit = 1;
    }
    return limit > max_limit ? max_limit : limit;
}

static bool agent_search_match_string(const char *value, const char *query)
{
    return query && query[0] && value && value[0] &&
           agent_route_text_contains_ci(value, query);
}

static bool agent_search_record_matches_query(const cJSON *entry, const char *query)
{
    if (!query || !query[0]) {
        return true;
    }
    if (agent_search_match_string(agent_history_record_content(entry), query) ||
        agent_search_match_string(agent_history_record_session_id(entry), query) ||
        agent_search_match_string(agent_memory_record_type(entry), query)) {
        return true;
    }

    const char *fields[] = {
        "kind", "scope", "type", "role", "source", "job_id", "profile", "model"
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(entry, fields[i]);
        if (cJSON_IsString(item) &&
            agent_search_match_string(item->valuestring, query)) {
            return true;
        }
    }
    return false;
}

static bool agent_search_record_matches_session(const cJSON *entry,
                                                const char *session_id,
                                                bool session_filter)
{
    if (!session_filter || !session_id || !session_id[0]) {
        return true;
    }
    return strcmp(agent_history_record_session_id(entry), session_id) == 0;
}

static bool agent_record_search_match(const cJSON *entry,
                                      const agent_record_search_ctx_t *ctx)
{
    if (!entry || !ctx) {
        return false;
    }
    if (ctx->memory) {
        if (!agent_memory_record_is_visible(entry)) {
            return false;
        }
        if (ctx->type && ctx->type[0] &&
            strcmp(agent_memory_record_type(entry), ctx->type) != 0) {
            return false;
        }
    } else if (!agent_history_record_is_visible(entry)) {
        return false;
    }
    if (!agent_search_record_matches_session(entry, ctx->session_id,
                                             ctx->session_filter)) {
        return false;
    }
    return agent_search_record_matches_query(entry, ctx->query);
}

static esp_err_t agent_record_search_count_cb(cJSON *entry, void *user_ctx)
{
    agent_record_search_ctx_t *ctx = (agent_record_search_ctx_t *)user_ctx;
    if (agent_record_search_match(entry, ctx)) {
        ctx->matched++;
    }
    return ESP_OK;
}

static void agent_search_add_record_summary(cJSON *records, const cJSON *entry,
                                            bool memory)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return;
    }

    const char *session_id = agent_history_record_session_id(entry);
    cJSON_AddStringToObject(item, "session_id", session_id && session_id[0] ?
                            session_id : AGENT_SESSION_DEFAULT_ID);
    cJSON *stored = cJSON_GetObjectItemCaseSensitive(entry, "stored_ms");
    if (cJSON_IsNumber(stored)) {
        cJSON_AddNumberToObject(item, "stored_ms", stored->valuedouble);
    }
    const char *kind = json_string_any(entry, "kind", NULL, NULL);
    if (kind && kind[0]) {
        cJSON_AddStringToObject(item, "kind", kind);
    }
    if (memory) {
        cJSON_AddStringToObject(item, "type", agent_memory_record_type(entry));
        const char *scope = json_string_any(entry, "scope", NULL, NULL);
        if (scope && scope[0]) {
            cJSON_AddStringToObject(item, "scope", scope);
        }
        const char *source = json_string_any(entry, "source", NULL, NULL);
        if (source && source[0]) {
            cJSON_AddStringToObject(item, "source", source);
        }
    } else {
        const char *role = agent_history_chat_role(entry);
        if (role && role[0]) {
            cJSON_AddStringToObject(item, "role", role);
        }
    }

    const char *content = agent_history_record_content(entry);
    char *safe = agent_sanitize_chat_content(content ? content : "",
                                             AGENT_SEARCH_RECORD_TEXT_MAX);
    cJSON_AddStringToObject(item, "content", safe ? safe : "");
    free(safe);
    cJSON_AddItemToArray(records, item);
}

static esp_err_t agent_record_search_add_cb(cJSON *entry, void *user_ctx)
{
    agent_record_search_ctx_t *ctx = (agent_record_search_ctx_t *)user_ctx;
    if (!agent_record_search_match(entry, ctx)) {
        return ESP_OK;
    }
    if (ctx->matched++ < ctx->skip_count) {
        return ESP_OK;
    }
    agent_search_add_record_summary(ctx->records, entry, ctx->memory);
    ctx->returned++;
    return ESP_OK;
}

static esp_err_t agent_search_records_locked(bool memory, cJSON *records,
                                             const char *query,
                                             const char *type,
                                             const char *session_id,
                                             bool session_filter,
                                             uint32_t limit,
                                             uint32_t *matched_out,
                                             uint32_t *returned_out)
{
    if (!records) {
        return ESP_ERR_INVALID_ARG;
    }

    agent_record_search_ctx_t count_ctx = {
        .query = query,
        .type = type,
        .session_id = session_id,
        .memory = memory,
        .session_filter = session_filter,
    };
    esp_err_t ret = memory ?
                    agent_memory_file_for_each_locked(agent_record_search_count_cb,
                                                      &count_ctx, NULL) :
                    agent_history_file_for_each_locked(agent_record_search_count_cb,
                                                       &count_ctx, NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t skip = count_ctx.matched > limit ? count_ctx.matched - limit : 0;
    agent_record_search_ctx_t add_ctx = {
        .records = records,
        .query = query,
        .type = type,
        .session_id = session_id,
        .memory = memory,
        .session_filter = session_filter,
        .skip_count = skip,
    };
    ret = memory ?
          agent_memory_file_for_each_locked(agent_record_search_add_cb,
                                            &add_ctx, NULL) :
          agent_history_file_for_each_locked(agent_record_search_add_cb,
                                             &add_ctx, NULL);
    if (matched_out) {
        *matched_out = count_ctx.matched;
    }
    if (returned_out) {
        *returned_out = add_ctx.returned;
    }
    return ret;
}

static bool agent_normalize_optional_session_arg(const char *requested,
                                                 const char *current_session,
                                                 char *out, size_t out_size,
                                                 bool *filter_out)
{
    if (!out || out_size == 0 || !filter_out) {
        return false;
    }
    out[0] = '\0';
    *filter_out = false;
    if (!requested || !requested[0] ||
        strcasecmp(requested, "all") == 0 ||
        strcmp(requested, "*") == 0) {
        return true;
    }
    const char *value = requested;
    if (strcasecmp(requested, "current") == 0 ||
        strcasecmp(requested, "this") == 0) {
        value = current_session && current_session[0] ?
                current_session : AGENT_SESSION_DEFAULT_ID;
    }
    if (!agent_session_id_valid(value)) {
        return false;
    }
    agent_session_normalize(value, out, out_size);
    *filter_out = true;
    return true;
}
