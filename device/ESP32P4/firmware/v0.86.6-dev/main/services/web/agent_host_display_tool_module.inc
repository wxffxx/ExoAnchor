// Transitional Agent module: structured Ubuntu DRM/GRUB display workflow.
// Plan/approval state is owned by host_display_manager, not this adapter.

static void agent_host_display_add_status(
    cJSON *item, const si_host_display_status_t *status)
{
    cJSON_AddStringToObject(item, "state",
                            si_host_display_state_name(status->state));
    cJSON_AddStringToObject(item, "plan_id", status->plan_id);
    cJSON_AddBoolToObject(item, "approval_required",
                          status->approval_required);
    cJSON_AddBoolToObject(item, "approved", status->approved);
    cJSON_AddBoolToObject(item, "reboot_required",
                          status->reboot_required);
    cJSON_AddStringToObject(item, "connector", status->request.connector);
    cJSON_AddNumberToObject(item, "width", status->request.width);
    cJSON_AddNumberToObject(item, "height", status->request.height);
    cJSON_AddNumberToObject(
        item, "refresh_hz",
        (double)status->request.refresh_millihz / 1000.0);
    cJSON_AddBoolToObject(item, "persistent",
                          status->request.persistent);
    if (status->error[0]) {
        cJSON_AddStringToObject(item, "manager_error", status->error);
    }
}

static bool agent_host_display_add_error(
    cJSON *results, int index, const char *operation, const char *error,
    bool *all_ok)
{
    cJSON *item = cJSON_CreateObject();
    if (item) {
        cJSON_AddNumberToObject(item, "index", index);
        cJSON_AddStringToObject(item, "tool", "host_display");
        cJSON_AddStringToObject(item, "operation",
                                operation ? operation : "unknown");
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                error ? error : "host display error");
        cJSON_AddItemToArray(results, item);
    }
    if (all_ok) {
        *all_ok = false;
    }
    return false;
}

static bool agent_host_display_execute_ssh(
    const char *job_id, int index, const char *operation,
    const char *plan_id, const char *command, bool dry_run,
    cJSON *results, bool *all_ok, bool *command_ok_out,
    const char **output_out)
{
    cJSON *call = cJSON_CreateObject();
    cJSON *args = call ? cJSON_AddObjectToObject(call, "args") : NULL;
    if (!call || !args) {
        cJSON_Delete(call);
        return agent_host_display_add_error(
            results, index, operation, "host display tool alloc failed",
            all_ok);
    }
    cJSON_AddStringToObject(call, "tool", "ssh_exec");
    cJSON_AddStringToObject(args, "command", command);
    cJSON_AddNumberToObject(args, "timeout_ms", 120000);
    int before = cJSON_GetArraySize(results);
    bool did_execute = agent_execute_ssh_exec_tool(
        job_id, call, index, dry_run, results, all_ok);
    cJSON_Delete(call);

    cJSON *item = cJSON_GetArrayItem(results, before);
    if (cJSON_IsObject(item)) {
        cJSON_ReplaceItemInObjectCaseSensitive(
            item, "tool", cJSON_CreateString("host_display"));
        cJSON_AddStringToObject(item, "operation", operation);
        if (plan_id && plan_id[0]) {
            cJSON_AddStringToObject(item, "plan_id", plan_id);
        }
        const cJSON *ok_item =
            cJSON_GetObjectItemCaseSensitive(item, "ok");
        const cJSON *output_item =
            cJSON_GetObjectItemCaseSensitive(item, "output");
        if (command_ok_out) {
            *command_ok_out = cJSON_IsTrue(ok_item);
        }
        if (output_out) {
            *output_out = cJSON_IsString(output_item) ?
                              output_item->valuestring :
                              "";
        }
    } else {
        if (command_ok_out) {
            *command_ok_out = false;
        }
        if (output_out) {
            *output_out = "";
        }
    }
    return did_execute;
}

static bool agent_execute_host_display_tool(
    const char *job_id, const cJSON *call, int index, bool dry_run,
    cJSON *results, bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const char *operation =
        json_string_any(args, "operation", "action", "mode");
    if (!operation || !operation[0]) {
        operation = "status";
    }

    if (strcasecmp(operation, "observe") == 0) {
        char command[SI_HOST_DISPLAY_COMMAND_MAX + 1U];
        si_host_display_result_t result =
            si_host_display_build_observe_command(command, sizeof(command));
        if (result != SI_HOST_DISPLAY_OK) {
            return agent_host_display_add_error(
                results, index, operation,
                si_host_display_result_name(result), all_ok);
        }
        return agent_host_display_execute_ssh(
            job_id, index, operation, "", command, dry_run, results,
            all_ok, NULL, NULL);
    }

    if (strcasecmp(operation, "plan") == 0) {
        const char *connector =
            json_string_any(args, "connector", "output", "display");
        const cJSON *width = json_number_any(args, "width", "w");
        const cJSON *height = json_number_any(args, "height", "h");
        const cJSON *refresh =
            json_number_any(args, "refresh_hz", "refresh");
        const cJSON *persistent =
            cJSON_GetObjectItemCaseSensitive(args, "persistent");
        si_host_display_request_t request = {0};
        strlcpy(request.connector, connector ? connector : "",
                sizeof(request.connector));
        if (cJSON_IsNumber(width) &&
            width->valuedouble == (double)width->valueint &&
            width->valueint >= 320 && width->valueint <= 7680) {
            request.width = (uint16_t)width->valueint;
        }
        if (cJSON_IsNumber(height) &&
            height->valuedouble == (double)height->valueint &&
            height->valueint >= 200 && height->valueint <= 4320) {
            request.height = (uint16_t)height->valueint;
        }
        if (cJSON_IsNumber(refresh) &&
            refresh->valuedouble == (double)refresh->valueint &&
            refresh->valueint >= 10 && refresh->valueint <= 240) {
            request.refresh_millihz =
                (uint32_t)refresh->valueint * 1000U;
        }
        request.persistent =
            !cJSON_IsBool(persistent) || cJSON_IsTrue(persistent);
        if (si_host_display_validate_request(&request) !=
            SI_HOST_DISPLAY_OK) {
            return agent_host_display_add_error(
                results, index, operation,
                "connector, width, height, and integer refresh_hz required",
                all_ok);
        }

        cJSON *item = cJSON_CreateObject();
        if (!item) {
            return agent_host_display_add_error(
                results, index, operation, "plan alloc failed", all_ok);
        }
        cJSON_AddNumberToObject(item, "index", index);
        cJSON_AddStringToObject(item, "tool", "host_display");
        cJSON_AddStringToObject(item, "operation", "plan");
        cJSON_AddBoolToObject(item, "dry_run", dry_run);
        cJSON_AddBoolToObject(item, "ok", true);
        if (dry_run) {
            cJSON_AddStringToObject(item, "state", "proposed");
            cJSON_AddBoolToObject(item, "approval_required", true);
            cJSON_AddBoolToObject(item, "approved", false);
            cJSON_AddStringToObject(item, "connector", request.connector);
            cJSON_AddNumberToObject(item, "width", request.width);
            cJSON_AddNumberToObject(item, "height", request.height);
            cJSON_AddNumberToObject(
                item, "refresh_hz",
                (double)request.refresh_millihz / 1000.0);
            cJSON_AddBoolToObject(item, "persistent",
                                  request.persistent);
        } else {
            si_host_display_status_t status;
            esp_err_t ret =
                si_host_display_plan_create(&request, &status);
            if (ret != ESP_OK) {
                cJSON_Delete(item);
                return agent_host_display_add_error(
                    results, index, operation, esp_err_to_name(ret),
                    all_ok);
            }
            agent_host_display_add_status(item, &status);
            agent_run_add_action_eventf(
                job_id, NULL, SI_ACTION_CHANNEL_SYSTEM,
                SI_ACTION_PHASE_WAITING_APPROVAL, false, true,
                "Host display plan %s waiting for exact browser approval · %s %ux%u@%u",
                status.plan_id, status.request.connector,
                (unsigned)status.request.width,
                (unsigned)status.request.height,
                (unsigned)(status.request.refresh_millihz / 1000U));
        }
        cJSON_AddItemToArray(results, item);
        return false;
    }

    si_host_display_status_t status;
    esp_err_t manager_ret = si_host_display_plan_get(&status);
    if (manager_ret != ESP_OK) {
        return agent_host_display_add_error(
            results, index, operation, esp_err_to_name(manager_ret), all_ok);
    }
    const char *plan_id =
        json_string_any(args, "plan_id", "id", "plan");
    if (!plan_id || !plan_id[0]) {
        plan_id = status.plan_id;
    }

    if (strcasecmp(operation, "status") == 0) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            return agent_host_display_add_error(
                results, index, operation, "status alloc failed", all_ok);
        }
        cJSON_AddNumberToObject(item, "index", index);
        cJSON_AddStringToObject(item, "tool", "host_display");
        cJSON_AddStringToObject(item, "operation", "status");
        cJSON_AddBoolToObject(item, "ok", true);
        agent_host_display_add_status(item, &status);
        cJSON_AddItemToArray(results, item);
        return false;
    }

    char command[SI_HOST_DISPLAY_COMMAND_MAX + 1U];
    si_host_display_result_t build_result = SI_HOST_DISPLAY_UNSUPPORTED;
    if (strcasecmp(operation, "apply") == 0) {
        build_result = si_host_display_build_apply_command(
            plan_id, &status.request, command, sizeof(command));
    } else if (strcasecmp(operation, "verify") == 0) {
        build_result = si_host_display_build_verify_command(
            plan_id, &status.request, command, sizeof(command));
    } else if (strcasecmp(operation, "rollback") == 0) {
        build_result = si_host_display_build_rollback_command(
            plan_id, command, sizeof(command));
    } else {
        return agent_host_display_add_error(
            results, index, operation, "unsupported host display operation",
            all_ok);
    }
    if (build_result != SI_HOST_DISPLAY_OK) {
        return agent_host_display_add_error(
            results, index, operation,
            si_host_display_result_name(build_result), all_ok);
    }
    if (dry_run) {
        return agent_host_display_execute_ssh(
            job_id, index, operation, plan_id, command, true, results,
            all_ok, NULL, NULL);
    }

    if (strcasecmp(operation, "apply") == 0) {
        manager_ret = si_host_display_plan_begin_apply(plan_id, &status);
    } else if (strcasecmp(operation, "verify") == 0) {
        manager_ret = si_host_display_plan_begin_verify(plan_id, &status);
    } else {
        manager_ret = si_host_display_plan_begin_rollback(plan_id, &status);
    }
    if (manager_ret != ESP_OK) {
        return agent_host_display_add_error(
            results, index, operation,
            manager_ret == ESP_ERR_INVALID_STATE ?
                "plan is not approved or is in the wrong state" :
                esp_err_to_name(manager_ret),
            all_ok);
    }

    bool command_ok = false;
    const char *output = "";
    bool did_execute = agent_host_display_execute_ssh(
        job_id, index, operation, plan_id, command, false, results, all_ok,
        &command_ok, &output);
    bool verified = command_ok;
    const char *finish_error = command_ok ? "" : "SSH command failed";
    char expected_fb[32] = {0};
    if (strcasecmp(operation, "verify") == 0 && command_ok) {
        snprintf(expected_fb, sizeof(expected_fb), "fb_virtual_size=%u,%u",
                 (unsigned)status.request.width,
                 (unsigned)status.request.height);
        verified = output && strstr(output, "config_token=yes") &&
                   strstr(output, "runtime_token=yes") &&
                   strstr(output, expected_fb);
        finish_error = verified ? "" :
                       "Host read-back does not match approved display mode";
    }

    if (strcasecmp(operation, "apply") == 0) {
        (void)si_host_display_plan_finish_apply(
            plan_id, command_ok, finish_error, &status);
    } else if (strcasecmp(operation, "verify") == 0) {
        (void)si_host_display_plan_finish_verify(
            plan_id, verified, finish_error, &status);
    } else {
        (void)si_host_display_plan_finish_rollback(
            plan_id, command_ok, finish_error, &status);
    }
    if (!verified && all_ok) {
        *all_ok = false;
    }
    agent_run_add_action_eventf(
        job_id, NULL, SI_ACTION_CHANNEL_SYSTEM,
        verified ? SI_ACTION_PHASE_SUCCEEDED : SI_ACTION_PHASE_FAILED,
        false, true, "Host display plan %s %s %s",
        plan_id, operation, verified ? "verified" : finish_error);
    return did_execute;
}
