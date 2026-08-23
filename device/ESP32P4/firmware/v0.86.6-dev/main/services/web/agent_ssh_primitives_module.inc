// Transitional Agent module: validated SSH primitive command builder. Shares the run context until its service API is finalized.
static bool agent_safe_service_name(const char *text)
{
    if (!text || !text[0] || strlen(text) > 48) {
        return false;
    }
    for (const char *p = text; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
            return false;
        }
    }
    return true;
}

static bool agent_safe_token(const char *text, size_t max_len, bool allow_colon)
{
    if (!text || !text[0] || strlen(text) > max_len) {
        return false;
    }
    for (const char *p = text; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' &&
            *p != '.' && *p != '+' && !(allow_colon && *p == ':')) {
            return false;
        }
    }
    return true;
}

static bool agent_safe_tool_path(const char *text)
{
    if (!text || text[0] != '/' || strlen(text) > 180) {
        return false;
    }
    if (strstr(text, "..")) {
        return false;
    }
    if (strcmp(text, "/nonexistent") == 0) {
        return true;
    }
    if (strncmp(text, "/opt/", 5) != 0 &&
        strncmp(text, "/tmp/", 5) != 0 &&
        strncmp(text, "/home/", 6) != 0 &&
        strncmp(text, "/etc/systemd/system/", 20) != 0) {
        return false;
    }
    for (const char *p = text; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '/' && *p != '-' &&
            *p != '_' && *p != '.') {
            return false;
        }
    }
    return true;
}

static bool agent_safe_url(const char *text)
{
    return text && (strncmp(text, "https://", 8) == 0 ||
                    strncmp(text, "http://", 7) == 0) &&
           strlen(text) < 900 && !strchr(text, '\'') && !strchr(text, '\n') &&
           !strchr(text, '\r');
}

static bool agent_append_package_list(const cJSON *args, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    const cJSON *packages = cJSON_GetObjectItemCaseSensitive(args, "packages");
    if (cJSON_IsArray(packages)) {
        const cJSON *pkg = NULL;
        cJSON_ArrayForEach(pkg, packages) {
            if (!cJSON_IsString(pkg) ||
                !agent_safe_token(pkg->valuestring, 80, false)) {
                return false;
            }
            if (out[0]) {
                strlcat(out, " ", out_size);
            }
            strlcat(out, pkg->valuestring, out_size);
        }
    } else {
        const char *pkg = json_string_any(args, "package", "name", "pkg");
        if (!pkg || !agent_safe_token(pkg, 80, false)) {
            return false;
        }
        strlcpy(out, pkg, out_size);
    }
    return out[0] != '\0';
}

static bool agent_build_ssh_primitive_command(const cJSON *call,
                                              char *command,
                                              size_t command_size,
                                              char *error,
                                              size_t error_size)
{
    const cJSON *args = agent_tool_args(call);
    const char *name = agent_tool_name(call);
    if (!command || command_size == 0 || !name) {
        return false;
    }
    command[0] = '\0';
    if (error && error_size > 0) {
        error[0] = '\0';
    }

    if (strcasecmp(name, "ensure_package") == 0) {
        char packages[320];
        if (!agent_append_package_list(args, packages, sizeof(packages))) {
            strlcpy(error, "ensure_package packages invalid", error_size);
            return false;
        }
        snprintf(command, command_size,
                 "set -eu; command -v apt-get >/dev/null 2>&1 || { echo 'apt-get not found'; exit 1; }; "
                 "sudo apt-get update; sudo DEBIAN_FRONTEND=noninteractive apt-get install -y %s",
                 packages);
        return true;
    }

    if (strcasecmp(name, "ensure_user") == 0) {
        const char *user = json_string_any(args, "user", "username", "name");
        const char *home = json_string_any(args, "home", "home_dir", NULL);
        const char *shell = json_string_any(args, "shell", NULL, NULL);
        if (!agent_safe_token(user, 32, false)) {
            strlcpy(error, "ensure_user user invalid", error_size);
            return false;
        }
        if (!home || !home[0]) {
            home = "/nonexistent";
        }
        if (!shell || !shell[0]) {
            shell = "/usr/sbin/nologin";
        }
        if (!agent_safe_tool_path(home) ||
            (strcmp(shell, "/usr/sbin/nologin") != 0 &&
             strcmp(shell, "/bin/bash") != 0 &&
             strcmp(shell, "/bin/sh") != 0)) {
            strlcpy(error, "ensure_user home or shell invalid", error_size);
            return false;
        }
        snprintf(command, command_size,
                 "set -eu; id %s >/dev/null 2>&1 || sudo useradd -r %s -d %s -s %s %s; id %s",
                 user, strcmp(home, "/nonexistent") == 0 ? "-M" : "-m",
                 home, shell, user, user);
        return true;
    }

    if (strcasecmp(name, "ensure_directory") == 0) {
        const char *path = json_string_any(args, "path", "dir", "directory");
        const char *owner = json_string_any(args, "owner", NULL, NULL);
        const char *mode = json_string_any(args, "mode", NULL, NULL);
        if (!agent_safe_tool_path(path)) {
            strlcpy(error, "ensure_directory path invalid", error_size);
            return false;
        }
        if (owner && owner[0] && !agent_safe_token(owner, 80, true)) {
            strlcpy(error, "ensure_directory owner invalid", error_size);
            return false;
        }
        if (mode && mode[0] && !agent_safe_token(mode, 8, false)) {
            strlcpy(error, "ensure_directory mode invalid", error_size);
            return false;
        }
        snprintf(command, command_size,
                 "set -eu; sudo mkdir -p %s%s%s%s%s",
                 path,
                 owner && owner[0] ? "; sudo chown -R " : "",
                 owner && owner[0] ? owner : "",
                 owner && owner[0] ? " " : "",
                 owner && owner[0] ? path : "");
        if (mode && mode[0]) {
            strlcat(command, "; sudo chmod ", command_size);
            strlcat(command, mode, command_size);
            strlcat(command, " ", command_size);
            strlcat(command, path, command_size);
        }
        strlcat(command, "; ls -ld ", command_size);
        strlcat(command, path, command_size);
        return true;
    }

    if (strcasecmp(name, "download_file") == 0) {
        const char *url = json_string_any(args, "url", "source", NULL);
        const char *path = json_string_any(args, "path", "dest", "destination");
        const char *sha = json_string_any(args, "sha256", "sha", NULL);
        const char *owner = json_string_any(args, "owner", NULL, NULL);
        char q_url[1024];
        if (!agent_safe_url(url) || !agent_safe_tool_path(path) ||
            (sha && sha[0] && !agent_safe_token(sha, 80, false)) ||
            (owner && owner[0] && !agent_safe_token(owner, 80, true)) ||
            !shell_single_quote(url, q_url, sizeof(q_url))) {
            strlcpy(error, "download_file args invalid", error_size);
            return false;
        }
        snprintf(command, command_size,
                 "set -eu; tmp=$(mktemp /tmp/exoanchor-download.XXXXXX); "
                 "curl -fL --retry 3 --retry-delay 2 -o \"$tmp\" %s; "
                 "%s%s%s"
                 "sudo mkdir -p $(dirname %s); sudo mv \"$tmp\" %s%s%s%s%s; ls -lh %s",
                 q_url,
                 sha && sha[0] ? "echo '" : "",
                 sha && sha[0] ? sha : "",
                 sha && sha[0] ? "  '\"$tmp\" | sha256sum -c -; " : "",
                 path, path,
                 owner && owner[0] ? "; sudo chown " : "",
                 owner && owner[0] ? owner : "",
                 owner && owner[0] ? " " : "",
                 owner && owner[0] ? path : "",
                 path);
        return true;
    }

    if (strcasecmp(name, "write_file") == 0) {
        const char *path = json_string_any(args, "path", "file", NULL);
        const char *content = json_string_any(args, "content", "text", NULL);
        const char *owner = json_string_any(args, "owner", NULL, NULL);
        const char *mode = json_string_any(args, "mode", NULL, NULL);
        char q_content[2600];
        if (!agent_safe_tool_path(path) || !content ||
            strlen(content) > 2200 ||
            (owner && owner[0] && !agent_safe_token(owner, 80, true)) ||
            (mode && mode[0] && !agent_safe_token(mode, 8, false)) ||
            !shell_single_quote(content, q_content, sizeof(q_content))) {
            strlcpy(error, "write_file args invalid", error_size);
            return false;
        }
        snprintf(command, command_size,
                 "set -eu; sudo mkdir -p $(dirname %s); printf %%s %s | sudo tee %s >/dev/null%s%s%s%s%s%s%s%s; ls -l %s",
                 path, q_content, path,
                 owner && owner[0] ? "; sudo chown " : "",
                 owner && owner[0] ? owner : "",
                 owner && owner[0] ? " " : "",
                 owner && owner[0] ? path : "",
                 mode && mode[0] ? "; sudo chmod " : "",
                 mode && mode[0] ? mode : "",
                 mode && mode[0] ? " " : "",
                 mode && mode[0] ? path : "",
                 path);
        return true;
    }

    if (strcasecmp(name, "ensure_systemd_service") == 0) {
        const char *service = json_string_any(args, "name", "service", "unit");
        const char *content = json_string_any(args, "content", "unit_content", NULL);
        bool enable = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "enable"));
        bool start = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "start"));
        char q_content[3000];
        if (!agent_safe_service_name(service) || !content ||
            strlen(content) > 2500 ||
            !shell_single_quote(content, q_content, sizeof(q_content))) {
            strlcpy(error, "ensure_systemd_service args invalid", error_size);
            return false;
        }
        snprintf(command, command_size,
                 "set -eu; unit=/etc/systemd/system/%s.service; "
                 "printf %%s %s | sudo tee \"$unit\" >/dev/null; "
                 "sudo systemctl daemon-reload%s%s%s%s; "
                 "systemctl --no-pager --full status %s.service | sed -n '1,24p' || true",
                 service, q_content,
                 enable ? "; sudo systemctl enable " : "",
                 enable ? service : "",
                 start ? "; sudo systemctl start " : "",
                 start ? service : "",
                 service);
        return true;
    }

    if (strcasecmp(name, "check_service") == 0) {
        const char *service = json_string_any(args, "name", "service", "unit");
        if (!agent_safe_service_name(service)) {
            strlcpy(error, "check_service service invalid", error_size);
            return false;
        }
        snprintf(command, command_size,
                 "set -u; systemctl is-active %s.service; "
                 "systemctl --no-pager --full status %s.service | sed -n '1,40p'; "
                 "journalctl -u %s.service -n 80 --no-pager || true",
                 service, service, service);
        return true;
    }

    if (strcasecmp(name, "verify_port") == 0) {
        const cJSON *port_item = json_number_any(args, "port", "tcp_port");
        int port = port_item ? port_item->valueint : 0;
        if (port < 1 || port > 65535) {
            strlcpy(error, "verify_port port invalid", error_size);
            return false;
        }
        snprintf(command, command_size,
                 "set -eu; (ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null) | grep ':%d '",
                 port);
        return true;
    }

    strlcpy(error, "unknown primitive", error_size);
    return false;
}

static bool agent_execute_ssh_primitive_tool(const char *job_id,
                                             const cJSON *call,
                                             int index,
                                             bool dry_run,
                                             cJSON *results,
                                             bool *all_ok)
{
    const char *name = agent_tool_name(call);
    char command[SI_SSH_COMMAND_MAX_LEN] = {0};
    char error[128] = {0};
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddNumberToObject(item, "index", index);
    cJSON_AddStringToObject(item, "tool", name ? name : "ssh_primitive");
    cJSON_AddBoolToObject(item, "dry_run", dry_run);

    if (!agent_build_ssh_primitive_command(call, command, sizeof(command),
                                           error, sizeof(error))) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", error[0] ? error : "primitive args invalid");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] %s invalid · %s",
                             index + 1, name ? name : "ssh_primitive",
                             error[0] ? error : "invalid args");
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddStringToObject(item, "compiled_to", "ssh_exec");
    cJSON_AddStringToObject(item, "command", command);

    if (dry_run) {
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddStringToObject(item, "status", "dry-run");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "cmd", "tool[%d] %s dry-run -> ssh_exec $ %s",
                             index + 1, name ? name : "ssh_primitive", command);
        return false;
    }

    cJSON *ssh_call = cJSON_CreateObject();
    cJSON *ssh_args = ssh_call ? cJSON_AddObjectToObject(ssh_call, "args") : NULL;
    cJSON *tmp_results = cJSON_CreateArray();
    if (!ssh_call || !ssh_args || !tmp_results) {
        cJSON_Delete(ssh_call);
        cJSON_Delete(tmp_results);
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "primitive alloc failed");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddStringToObject(ssh_call, "tool", "ssh_exec");
    cJSON_AddStringToObject(ssh_args, "command", command);
    cJSON_AddNumberToObject(ssh_args, "timeout_ms",
                            agent_ssh_command_needs_long_timeout(command) ?
                            SSH_TARGET_MAX_TIMEOUT_MS : 120000);

    bool ssh_ok = true;
    bool did_execute = agent_execute_ssh_exec_tool(job_id, ssh_call, index,
                                                   false, tmp_results, &ssh_ok);
    cJSON *ssh_result = cJSON_DetachItemFromArray(tmp_results, 0);
    bool ok = ssh_ok && cJSON_IsObject(ssh_result) &&
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ssh_result, "ok"));
    cJSON_AddBoolToObject(item, "ok", ok);
    if (cJSON_IsObject(ssh_result)) {
        cJSON_AddItemToObject(item, "ssh_result", ssh_result);
    } else {
        cJSON_Delete(ssh_result);
        cJSON_AddStringToObject(item, "error", "ssh_result missing");
    }
    cJSON_Delete(tmp_results);
    cJSON_Delete(ssh_call);
    cJSON_AddItemToArray(results, item);
    if (!ok && all_ok) {
        *all_ok = false;
    }
    return did_execute;
}
