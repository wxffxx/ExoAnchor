// Embedded Agent autonomy gate: firmware-owned action class for each exact
// tool call (see core/agent_autonomy.h for the level x class matrix).
static si_agent_action_class_t agent_action_class_max(
    si_agent_action_class_t a, si_agent_action_class_t b)
{
    return a > b ? a : b;
}

static si_agent_request_risk_t agent_action_class_risk(
    si_agent_action_class_t action_class)
{
    switch (action_class) {
    case SI_AGENT_ACTION_READ: return SI_AGENT_REQUEST_RISK_LOW;
    case SI_AGENT_ACTION_OPERATE: return SI_AGENT_REQUEST_RISK_MEDIUM;
    case SI_AGENT_ACTION_SYSTEM: return SI_AGENT_REQUEST_RISK_HIGH;
    case SI_AGENT_ACTION_IRREVERSIBLE:
    default: return SI_AGENT_REQUEST_RISK_CRITICAL;
    }
}

/* Firmware-owned action class for one exact call.  The model never supplies
 * it; arguments are read with the same accessors the executors use. */
static si_agent_action_class_t agent_tool_action_class(
    const char *name, const cJSON *call, si_agent_request_risk_t legacy_risk)
{
    if (!name) {
        return SI_AGENT_ACTION_IRREVERSIBLE;
    }
    const cJSON *args = agent_tool_args(call);
    if (strcmp(name, "ssh_exec") == 0) {
        si_agent_action_class_t action_class =
            si_agent_classify_shell_command(agent_tool_command(call));
        /* Redirecting SSH to another host or account is never routine. */
        if (json_string_any(args, "host", "ip", "target") ||
            json_string_any(args, "username", "user", "login") ||
            json_string_any(args, "password", "pass", NULL) ||
            json_string_any(args, "auth_method", "auth", NULL) ||
            json_string_any(args, "key_passphrase", "passphrase", NULL) ||
            json_number_any(args, "port", "ssh_port")) {
            action_class = agent_action_class_max(action_class,
                                                  SI_AGENT_ACTION_SYSTEM);
        }
        return action_class;
    }
    if (strcmp(name, "check_service") == 0 ||
        strcmp(name, "verify_port") == 0 ||
        strcmp(name, "memory_search") == 0 ||
        strcmp(name, "history_search") == 0) {
        return SI_AGENT_ACTION_READ;
    }
    if (strcmp(name, "memory_write") == 0) {
        return SI_AGENT_ACTION_OPERATE;
    }
    if (strcmp(name, "ensure_package") == 0 ||
        strcmp(name, "ensure_user") == 0 ||
        strcmp(name, "ensure_systemd_service") == 0 ||
        strcmp(name, "uart_auth") == 0 ||
        strcmp(name, "uart_baud") == 0) {
        return SI_AGENT_ACTION_SYSTEM;
    }
    if (strcmp(name, "ensure_directory") == 0 ||
        strcmp(name, "download_file") == 0 ||
        strcmp(name, "write_file") == 0) {
        const char *path = json_string_any(args, "path", "dest", "destination");
        if (!path) {
            path = json_string_any(args, "dir", "directory", "file");
        }
        return si_agent_classify_path_write(path);
    }
    if (strcmp(name, "uart_write") == 0) {
        /* Serial input lands in whatever console is attached. */
        return agent_action_class_max(
            si_agent_classify_shell_command(
                json_string_any(args, "data", "text", "input")),
            SI_AGENT_ACTION_OPERATE);
    }
    if (strcmp(name, "power_action") == 0) {
        const char *action =
            json_string_any(args, "action", "command", "button");
        if (action && (strncasecmp(action, "locator_", 8) == 0 ||
                       strncasecmp(action, "identify", 8) == 0)) {
            return SI_AGENT_ACTION_OPERATE;
        }
        if (action && (strcasecmp(action, "force_off") == 0 ||
                       strcasecmp(action, "hold_power") == 0)) {
            return SI_AGENT_ACTION_IRREVERSIBLE;
        }
        return SI_AGENT_ACTION_SYSTEM;
    }
    switch (legacy_risk) {
    case SI_AGENT_REQUEST_RISK_LOW: return SI_AGENT_ACTION_READ;
    case SI_AGENT_REQUEST_RISK_MEDIUM: return SI_AGENT_ACTION_OPERATE;
    case SI_AGENT_REQUEST_RISK_HIGH: return SI_AGENT_ACTION_SYSTEM;
    default: return SI_AGENT_ACTION_IRREVERSIBLE;
    }
}
