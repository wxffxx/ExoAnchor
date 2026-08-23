// Transitional Agent module: tool dispatch, HID actions and run events. Shares the run context until its service API is finalized.
static bool agent_run_control_point(const char *job_id);
static si_capability_t agent_tool_required_runtime_capability(
    const char *name);
static bool agent_run_captured_capability(
    const char *job_id, si_capability_t required);

static bool agent_compat_dispatch_allowed(
    const char *name, const cJSON *call, si_agent_request_risk_t risk,
    bool lease_required);

#include "agent_ask_user_tool_module.inc"

#include "agent_wait_tool_module.inc"

static si_agent_request_risk_t agent_tool_request_risk(
    const char *name, const cJSON *call, bool *lease_required,
    char *resource, size_t resource_size,
    char *effect, size_t effect_size,
    char *verification, size_t verification_size)
{
    if (lease_required) {
        *lease_required = false;
    }
    strlcpy(resource, name && name[0] ? name : "unknown", resource_size);
    strlcpy(effect, "Execute one exact Agent tool call", effect_size);
    strlcpy(verification, "Capture the structured tool result",
            verification_size);
    if (!name) {
        return SI_AGENT_REQUEST_RISK_HIGH;
    }
    if (strcmp(name, "web_search") == 0 ||
        strcmp(name, "observe_status") == 0 ||
        strcmp(name, "observe_video_status") == 0 ||
        strcmp(name, "observe_hid_status") == 0 ||
        strcmp(name, "wait") == 0 ||
        strcmp(name, "uart_status") == 0 ||
        strcmp(name, "uart_read") == 0 ||
        strcmp(name, "check_service") == 0 ||
        strcmp(name, "verify_port") == 0) {
        strlcpy(effect, "Read a bounded source without changing target state",
                effect_size);
        strlcpy(verification, "Return a bounded observation",
                verification_size);
        return SI_AGENT_REQUEST_RISK_LOW;
    }
    if (strcmp(name, "power_action") == 0) {
        const cJSON *args = agent_tool_args(call);
        const char *action =
            json_string_any(args, "action", "command", "button");
        if (action &&
            (strncasecmp(action, "locator_", 8) == 0 ||
             strncasecmp(action, "identify", 8) == 0)) {
            strlcpy(effect, "Change the ExoAnchor locator LED state",
                    effect_size);
            strlcpy(verification, "Read back locator state",
                    verification_size);
            return SI_AGENT_REQUEST_RISK_MEDIUM;
        }
        strlcpy(effect, "Press a target-host power or reset control",
                effect_size);
        strlcpy(verification,
                "Read back power-control status and inspect KVM/host state",
                verification_size);
        return SI_AGENT_REQUEST_RISK_HIGH;
    }
    if (strcmp(name, "console_login") == 0) {
        const cJSON *args = agent_tool_args(call);
        const char *stage =
            json_string_any(args, "stage", "operation", "mode");
        strlcpy(resource, "console.login", resource_size);
        if (!stage || !stage[0] || strcmp(stage, "status") == 0) {
            strlcpy(effect,
                    "Read masked Console credential availability",
                    effect_size);
            strlcpy(verification,
                    "Return configuration flags without secret material",
                    verification_size);
            return SI_AGENT_REQUEST_RISK_LOW;
        }
        if (lease_required) {
            *lease_required = true;
        }
        strlcpy(effect,
                "Inject the locally stored Console credential through USB HID",
                effect_size);
        strlcpy(verification,
                "Capture a fresh KVM frame; never return the credential",
                verification_size);
        return SI_AGENT_REQUEST_RISK_HIGH;
    }
    if (strcmp(name, "uart_write") == 0) {
        strlcpy(resource, "uart.write", resource_size);
        strlcpy(effect, "Write exact bytes to the target-host serial console",
                effect_size);
        strlcpy(verification,
                "Capture non-destructive UART journal output after the write",
                verification_size);
        return SI_AGENT_REQUEST_RISK_HIGH;
    }
    if (strcmp(name, "uart_auth") == 0) {
        strlcpy(resource, "uart.auth", resource_size);
        strlcpy(effect,
                "Submit one device-local credential only after an exact UART password prompt match",
                effect_size);
        strlcpy(verification,
                "Return redacted UART journal output after submission",
                verification_size);
        return SI_AGENT_REQUEST_RISK_HIGH;
    }
    if (strcmp(name, "uart_baud") == 0) {
        strlcpy(resource, "uart.baud", resource_size);
        strlcpy(effect, "Change the target-host serial baud mode",
                effect_size);
        strlcpy(verification, "Read back UART status and generation",
                verification_size);
        return SI_AGENT_REQUEST_RISK_HIGH;
    }
    if (strcmp(name, "host_display") == 0 ||
        strcmp(name, "boot_key_sequence") == 0) {
        const cJSON *args = agent_tool_args(call);
        const char *operation =
            json_string_any(args, "operation", "action", "op");
        if (operation &&
            (strcmp(operation, "observe") == 0 ||
             strcmp(operation, "status") == 0 ||
             strcmp(operation, "plan") == 0)) {
            snprintf(resource, resource_size, "%s.%s", name, operation);
            strlcpy(effect,
                    strcmp(operation, "plan") == 0 ?
                    "Create an exact device-held plan; execution remains separately gated" :
                    "Read current manager state",
                    effect_size);
            strlcpy(verification, "Read back the manager status",
                    verification_size);
            return SI_AGENT_REQUEST_RISK_LOW;
        }
        strlcpy(effect, "Change target host state through a managed plan",
                effect_size);
        strlcpy(verification, "Read back state and attach verification evidence",
                verification_size);
        return SI_AGENT_REQUEST_RISK_HIGH;
    }
    if (strcmp(name, "memory_write") == 0) {
        strlcpy(effect, "Persist a bounded Agent memory record on this device",
                effect_size);
        strlcpy(verification, "Read back the stored memory record count",
                verification_size);
        return SI_AGENT_REQUEST_RISK_MEDIUM;
    }
    if (strcmp(name, "ssh_exec") == 0 ||
        agent_tool_is_ssh_primitive(call)) {
        strlcpy(effect, "Execute an exact bounded operation on the configured SSH target",
                effect_size);
        strlcpy(verification, "Capture exit status and perform task-specific readback",
                verification_size);
        return SI_AGENT_REQUEST_RISK_HIGH;
    }
    return SI_AGENT_REQUEST_RISK_MEDIUM;
}

static bool agent_run_task_service_is_bound(const char *job_id)
{
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    /* Recovery is diagnostics-only. Never let a Run-lock timeout default a
     * legacy read into Broker/Task journal I/O on the provider worker. */
    (void)job_id;
    return false;
#else
    /* Unknown lock state stays on the normal fail-closed Broker path. */
    bool bound = true;
    if (job_id && s_agent_run_lock &&
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(s_agent_run_job.job_id, job_id) == 0) {
            bound = s_agent_run_job.task_service_bound &&
                    !s_agent_run_job.task_service_control_pending;
        }
        xSemaphoreGive(s_agent_run_lock);
    }
    return bound;
#endif
}

static bool agent_recovery_tool_allowed(const char *name, const cJSON *call)
{
    if (!name) {
        return false;
    }
    if (strcmp(name, "web_search") == 0 ||
        strcmp(name, "observe_status") == 0 ||
        strcmp(name, "observe_video_status") == 0 ||
        strcmp(name, "observe_hid_status") == 0 ||
        strcmp(name, "wait") == 0 ||
        strcmp(name, "uart_status") == 0 ||
        strcmp(name, "uart_read") == 0) {
        return true;
    }
    const cJSON *args = agent_tool_args(call);
    if (strcmp(name, "console_login") == 0) {
        const char *stage = json_string_any(args, "stage", "operation", "mode");
        return !stage || !stage[0] || strcmp(stage, "status") == 0;
    }
    return false;
}

static bool agent_compat_degraded_readonly(
    const char *name, const cJSON *call, si_agent_request_risk_t risk,
    bool lease_required)
{
    return risk == SI_AGENT_REQUEST_RISK_LOW && !lease_required &&
           agent_recovery_tool_allowed(name, call);
}

/* Runtime V2 is shadow-only at this seam. Risk metadata is necessary but is
 * not authority: a future classifier bug must not turn a new mutation into a
 * compatibility write. Admit only the explicit read allowlist above plus the
 * two exact manager plan-creation operations while the journal is healthy. */
static bool agent_compat_dispatch_allowed(
    const char *name, const cJSON *call, si_agent_request_risk_t risk,
    bool lease_required)
{
    if (agent_compat_degraded_readonly(name, call, risk, lease_required)) {
        return true;
    }
    if (name && strcmp(name, "uart_auth") == 0 &&
        risk == SI_AGENT_REQUEST_RISK_HIGH && !lease_required) {
        return true;
    }
    if (!name || risk != SI_AGENT_REQUEST_RISK_LOW || lease_required ||
        (strcmp(name, "host_display") != 0 &&
         strcmp(name, "boot_key_sequence") != 0)) {
        return false;
    }
    const cJSON *args = agent_tool_args(call);
    const char *operation = json_string_any(args, "operation", "action", "op");
    return operation && strcmp(operation, "plan") == 0;
}

static void agent_execute_tool_calls(const char *job_id, const cJSON *tool_calls,
                                     bool dry_run,
                                     bool allow_web_search,
                                     const char *session_id,
                                     const si_agent_route_t *route,
                                     cJSON *results, int *executed_out,
                                     bool *all_ok_out)
{
    int executed = 0;
    bool all_ok = true;
    int count = cJSON_IsArray(tool_calls) ? cJSON_GetArraySize(tool_calls) : 0;
    if (count > AGENT_TOOL_CALLS_MAX_ITEMS) {
        count = AGENT_TOOL_CALLS_MAX_ITEMS;
        all_ok = false;
    }

    for (int i = 0; i < count; i++) {
        if (!agent_run_control_point(job_id)) {
            agent_add_tool_error(results, i, "run", "run aborted");
            all_ok = false;
            break;
        }
        const cJSON *call = cJSON_GetArrayItem(tool_calls, i);
        if (!cJSON_IsObject(call)) {
            agent_add_tool_error(results, i, "unknown", "tool call must be object");
            agent_run_add_eventf(job_id, "bad", "tool[%d] invalid tool call",
                                 i + 1);
            all_ok = false;
            continue;
        }
        const char *name = agent_tool_name(call);
        const char *policy_name =
            agent_tool_is_ssh_exec(call) ? "ssh_exec" :
            agent_tool_is_web_search(call) ? "web_search" :
            agent_tool_is_memory_search(call) ? "memory_search" :
            agent_tool_is_history_search(call) ? "history_search" :
            agent_tool_is_memory_write(call) ? "memory_write" :
            agent_tool_is_ask_user(call) ? "ask_user" :
            agent_tool_is_host_display(call) ? "host_display" :
            agent_tool_is_boot_key(call) ? "boot_key_sequence" :
            agent_tool_is_observe_status(call) ? "observe_status" :
            agent_tool_is_observe_video_status(call) ? "observe_video_status" :
            agent_tool_is_observe_hid_status(call) ? "observe_hid_status" :
            agent_tool_is_console_login(call) ? "console_login" :
            agent_tool_is_power_action(call) ? "power_action" :
            agent_tool_is_wait(call) ? "wait" :
            agent_tool_is_uart_status(call) ? "uart_status" :
            agent_tool_is_uart_read(call) ? "uart_read" :
            agent_tool_is_uart_write(call) ? "uart_write" :
            agent_tool_is_uart_auth(call) ? "uart_auth" :
            agent_tool_is_uart_baud(call) ? "uart_baud" :
            agent_tool_is_ssh_primitive(call) ? name : NULL;
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
        if (agent_tool_is_ask_user(call)) {
            agent_add_tool_error(
                results, i, "ask_user",
                "ask_user is unavailable in the RAM-only recovery candidate");
            agent_run_add_eventf(
                job_id, "bad",
                "tool[%d] ask_user blocked in recovery candidate", i + 1);
            all_ok = false;
            continue;
        }
        if (!agent_recovery_tool_allowed(policy_name, call)) {
            agent_add_tool_error(
                results, i, policy_name ? policy_name : "unknown",
                "tool is unavailable in the RAM-only recovery candidate");
            agent_run_add_eventf(
                job_id, "bad",
                "tool[%d] blocked by recovery allowlist · %s",
                i + 1, policy_name ? policy_name : "unknown");
            all_ok = false;
            continue;
        }
#endif
        char policy_reason[96] = {0};
        if (policy_name &&
            !si_agent_route_allows_tool(route, policy_name)) {
            agent_add_tool_error(results, i, policy_name,
                                 "tool not allowed for active route");
            agent_run_add_eventf(
                job_id, "bad",
                "tool[%d] rejected by route · route=%s tool=%s",
                i + 1,
                route && route->name ? route->name : "unknown",
                policy_name);
            all_ok = false;
            continue;
        }
        if (policy_name &&
            !si_agent_tool_policy_allows(policy_name, dry_run,
                                         policy_reason,
                                         sizeof(policy_reason))) {
            agent_add_tool_error(results, i, policy_name,
                                 policy_reason[0] ? policy_reason :
                                 "tool denied by policy");
            agent_run_add_eventf(job_id, "bad",
                                 "tool[%d] denied by policy · %s",
                                 i + 1, policy_name);
            all_ok = false;
            continue;
        }
        if (agent_tool_is_uart_write(call)) {
            const cJSON *args = agent_tool_args(call);
            const char *data = json_string_any(args, "data", "text", "input");
            if (agent_uart_payload_is_sensitive(data)) {
                agent_add_tool_error(
                    results, i, "uart_write",
                    "sensitive UART payloads are prohibited");
                agent_run_add_eventf(
                    job_id, "bad",
                    "tool[%d] uart_write rejected · sensitive payload",
                    i + 1);
                all_ok = false;
                continue;
            }
        }
        char request_id[SI_AGENT_REQUEST_ID_MAX_LEN + 1] = {0};
        bool lease_required = false;
        char request_resource[SI_AGENT_REQUEST_RESOURCE_MAX_LEN + 1] = {0};
        char request_effect[SI_AGENT_REQUEST_EFFECT_MAX_LEN + 1] = {0};
        char request_verification[SI_AGENT_REQUEST_VERIFY_MAX_LEN + 1] = {0};
        char request_error[96] = {0};
        char *request_params = NULL;
        if (!dry_run && policy_name && !agent_tool_is_ask_user(call)) {
            si_agent_request_risk_t risk = agent_tool_request_risk(
                policy_name, call, &lease_required,
                request_resource, sizeof(request_resource),
                request_effect, sizeof(request_effect),
                request_verification, sizeof(request_verification));
            si_capability_t required_capability =
                agent_tool_required_runtime_capability(policy_name);
            if (!agent_run_captured_capability(
                    job_id, required_capability)) {
                agent_add_tool_error(
                    results, i, policy_name,
                    "source capability ceiling denies this tool");
                agent_run_add_eventf(
                    job_id, "bad",
                    "tool[%d] denied by exact source capability · %s",
                    i + 1, policy_name);
                all_ok = false;
                continue;
            }
            if (!agent_compat_dispatch_allowed(
                    policy_name, call, risk, lease_required)) {
                agent_add_tool_error(
                    results, i, policy_name,
                    "mutation execution is disabled until the target-bound manager gateway and typed readback are available");
                agent_run_add_eventf(
                    job_id, "bad",
                    "tool[%d] blocked at shadow-only cutover · %s",
                    i + 1, policy_name);
                all_ok = false;
                continue;
            }
            const bool task_service_bound =
                agent_run_task_service_is_bound(job_id);
            if (!task_service_bound &&
                !agent_compat_degraded_readonly(
                    policy_name, call, risk, lease_required)) {
                agent_add_tool_error(
                    results, i, policy_name,
                    "tool requires a healthy task journal; degraded mode is read-only");
                all_ok = false;
                continue;
            }
            if (task_service_bound) {
                /* Healthy shadow state keeps exact Broker/Guard/action-journal
                 * binding even for read-only and pure plan calls. */
                request_params = cJSON_PrintUnformatted(call);
                if (!request_params ||
                    !agent_request_authorize(
                        job_id, request_resource,
                        "Agent needs this exact tool call to continue the current task step.",
                        request_params ? request_params : "{}",
                        request_effect, request_verification, risk,
                        lease_required, request_id, sizeof(request_id),
                        request_error, sizeof(request_error))) {
                    agent_add_tool_error(
                        results, i, policy_name,
                        request_error[0] ? request_error :
                        "tool request was not approved");
                    agent_run_add_eventf(
                        job_id, "bad",
                        "tool[%d] request not granted · %s",
                        i + 1, policy_name);
                    free(request_params);
                    all_ok = false;
                    continue;
                }
                free(request_params);
            } else {
                agent_run_add_eventf(
                    job_id, "info",
                    "tool[%d] degraded read-only compatibility path · %s",
                    i + 1, policy_name);
            }
        }
        bool call_ok = true;
        if (agent_tool_is_ask_user(call)) {
            bool did_execute = agent_execute_ask_user_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_ssh_exec(call)) {
            bool did_execute = agent_execute_ssh_exec_tool(job_id, call, i, dry_run,
                                                           results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_web_search(call)) {
            bool did_execute = agent_execute_web_search_tool(job_id, call, i, dry_run,
                                                             allow_web_search,
                                                             results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_memory_search(call)) {
            bool did_execute = agent_execute_memory_search_tool(job_id, call, i,
                                                               session_id,
                                                               results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_history_search(call)) {
            bool did_execute = agent_execute_history_search_tool(job_id, call, i,
                                                                session_id,
                                                                results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_memory_write(call)) {
            bool did_execute = agent_execute_memory_write_tool(job_id, call, i,
                                                              dry_run, session_id,
                                                              results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_host_display(call)) {
            bool did_execute = agent_execute_host_display_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_boot_key(call)) {
            bool did_execute = agent_execute_boot_key_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_observe_status(call)) {
            bool did_execute = agent_execute_observe_status_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_observe_video_status(call)) {
            bool did_execute = agent_execute_observe_video_status_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_observe_hid_status(call)) {
            bool did_execute = agent_execute_observe_hid_status_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_console_login(call)) {
            bool did_execute = agent_execute_console_login_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_power_action(call)) {
            bool did_execute = agent_execute_power_action_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_wait(call)) {
            bool did_execute = agent_execute_wait_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_uart_status(call)) {
            bool did_execute = agent_execute_uart_status_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_uart_read(call)) {
            bool did_execute = agent_execute_uart_read_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_uart_write(call)) {
            bool did_execute = agent_execute_uart_write_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_uart_auth(call)) {
            bool did_execute = agent_execute_uart_auth_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_uart_baud(call)) {
            bool did_execute = agent_execute_uart_baud_tool(
                job_id, call, i, dry_run, results, &call_ok);
            if (did_execute) {
                executed++;
            }
        } else if (agent_tool_is_ssh_primitive(call)) {
            bool did_execute = agent_execute_ssh_primitive_tool(job_id, call, i,
                                                                dry_run, results,
                                                                &call_ok);
            if (did_execute) {
                executed++;
            }
        } else {
            agent_add_tool_error(results, i, name, "unsupported tool");
            agent_run_add_eventf(job_id, "bad", "tool[%d] unsupported tool · %s",
                                 i + 1, name ? name : "unknown");
            call_ok = false;
        }
        if (!call_ok) {
            all_ok = false;
        }
        agent_request_finish(job_id, request_id, call_ok,
                             call_ok ? NULL : "tool execution failed");
    }

    *executed_out = executed;
    *all_ok_out = all_ok;
}

static void agent_execute_actions(const char *job_id,
                                  const cJSON *actions, bool dry_run,
                                  const si_agent_route_t *route,
                                  cJSON *results, int *executed_out,
                                  bool *all_ok_out)
{
    int executed = 0;
    bool all_ok = true;
    int count = cJSON_IsArray(actions) ? cJSON_GetArraySize(actions) : 0;
    char policy_reason[96] = {0};
    if (count > 0 && !si_agent_route_allows_hid(route)) {
        si_hid_json_add_action_result(
            results, 0, ESP_ERR_NOT_ALLOWED,
            "HID actions not allowed for active route");
        agent_run_add_action_eventf(
            job_id, NULL, SI_ACTION_CHANNEL_HID, SI_ACTION_PHASE_FAILED,
            false, true, "HID actions rejected by route · route=%s",
            route && route->name ? route->name : "unknown");
        *executed_out = 0;
        *all_ok_out = false;
        return;
    }
    if (count > 0 &&
        !si_agent_tool_policy_allows("hid_actions", dry_run,
                                     policy_reason,
                                     sizeof(policy_reason))) {
        si_hid_json_add_action_result(
            results, 0, ESP_ERR_NOT_ALLOWED,
            policy_reason[0] ? policy_reason : "HID denied by tool policy");
        agent_run_add_action_eventf(
            job_id, NULL, SI_ACTION_CHANNEL_HID, SI_ACTION_PHASE_FAILED,
            false, true, "HID actions denied by policy · %s",
            policy_reason[0] ? policy_reason : "not allowed");
        *executed_out = 0;
        *all_ok_out = false;
        return;
    }
    if (count > HID_ACTIONS_MAX_ITEMS) {
        count = HID_ACTIONS_MAX_ITEMS;
        all_ok = false;
    }

    if (!dry_run && count > 0) {
        si_hid_json_add_action_result(
            results, 0, ESP_ERR_NOT_ALLOWED,
            "HID execution is disabled until target-bound readback is available");
        agent_run_add_action_eventf(
            job_id, NULL, SI_ACTION_CHANNEL_HID, SI_ACTION_PHASE_FAILED,
            false, true, "HID actions blocked at shadow-only cutover");
        *executed_out = 0;
        *all_ok_out = false;
        return;
    }

    for (int i = 0; i < count; i++) {
        if (!agent_run_control_point(job_id)) {
            agent_run_release_hid_control(job_id);
            si_hid_json_add_action_result(results, i, ESP_ERR_INVALID_STATE,
                                          "run aborted");
            all_ok = false;
            break;
        }
        const cJSON *action = cJSON_GetArrayItem(actions, i);
        char action_id[SI_ACTION_EVENT_ACTION_ID_MAX + 1U] = {0};
        char summary[SI_ACTION_EVENT_SUMMARY_MAX + 1U] = {0};
        (void)agent_run_next_action_id(job_id, "hid", action_id,
                                       sizeof(action_id));
        if (!cJSON_IsObject(action)) {
            si_hid_json_add_action_result(results, i, ESP_ERR_INVALID_ARG,
                                          "action must be object");
            all_ok = false;
            executed++;
            break;
        }
        agent_hid_action_summary(action, i, summary, sizeof(summary));

        esp_err_t action_ret = ESP_OK;
        char request_id[SI_AGENT_REQUEST_ID_MAX_LEN + 1] = {0};
        if (dry_run) {
            si_hid_json_add_action_result(results, i, ESP_OK, "dry-run");
            agent_run_add_action_eventf(
                job_id, action_id, SI_ACTION_CHANNEL_HID,
                SI_ACTION_PHASE_PLANNED, false, true,
                "%s · dry-run", summary);
            continue;
        }
        char *request_params = cJSON_PrintUnformatted(action);
        char request_error[96] = {0};
        bool request_granted =
            request_params &&
            agent_request_authorize(
                job_id, "hid_actions",
                "Agent needs this exact HID input to continue the current task step.",
                request_params ? request_params : "{}",
                summary,
                "Observe the next KVM frame and preserve the action event",
                SI_AGENT_REQUEST_RISK_HIGH, true,
                request_id, sizeof(request_id),
                request_error, sizeof(request_error));
        free(request_params);
        if (!request_granted) {
            si_hid_json_add_action_result(
                results, i, ESP_ERR_NOT_ALLOWED,
                request_error[0] ? request_error :
                "HID request was not approved");
            agent_run_add_action_eventf(
                job_id, action_id, SI_ACTION_CHANNEL_HID,
                SI_ACTION_PHASE_CANCELLED, false, true,
                "%s · request not granted", summary);
            all_ok = false;
            break;
        }

        cJSON *type = cJSON_GetObjectItemCaseSensitive(action, "type");
        const bool wait_only = cJSON_IsString(type) && type->valuestring && strcasecmp(type->valuestring, "wait") == 0;
        char control_error[96] = {0};
        action_ret = wait_only ? ESP_OK : agent_run_acquire_kvm_control(
            job_id, control_error, sizeof(control_error));
        if (action_ret != ESP_OK) {
            si_hid_json_add_action_result(results, i, action_ret,
                                          control_error[0] ? control_error :
                                          "agent control lease unavailable");
            agent_run_add_action_eventf(
                job_id, action_id, SI_ACTION_CHANNEL_HID,
                SI_ACTION_PHASE_FAILED, false, true,
                "%s · control lease unavailable", summary);
            all_ok = false;
            agent_request_finish(
                job_id, request_id, false,
                "agent control lease unavailable");
            executed++;
            break;
        }

        agent_run_add_action_eventf(
            job_id, action_id, SI_ACTION_CHANNEL_HID,
            SI_ACTION_PHASE_INPUT_SENT, false, true, "%s", summary);
        if (cJSON_IsString(type) && type->valuestring &&
            strcasecmp(type->valuestring, "wait") == 0) {
            uint32_t wait_ms = si_hid_json_action_wait_ms(action);
            while (wait_ms > 0) {
                uint32_t slice_ms = MIN(wait_ms, 50U);
                vTaskDelay(pdMS_TO_TICKS(slice_ms));
                wait_ms -= slice_ms;
                if (!agent_run_control_point(job_id)) {
                    action_ret = ESP_ERR_INVALID_STATE;
                    break;
                }
            }
        } else {
            si_hid_owner_token_t action_owner = {0};
            action_ret = agent_run_hid_owner_snapshot(
                job_id, &action_owner) ?
                si_hid_json_execute_owned(
                    action, &action_owner, NULL, NULL) :
                ESP_ERR_INVALID_STATE;
        }

        si_hid_json_add_action_result(results, i, action_ret, NULL);
        agent_run_add_action_eventf(
            job_id, action_id, SI_ACTION_CHANNEL_HID,
            action_ret == ESP_OK ? SI_ACTION_PHASE_SUCCEEDED :
                                   SI_ACTION_PHASE_FAILED,
            false, true, "%s · %s", summary,
            action_ret == ESP_OK ? "ok" : esp_err_to_name(action_ret));
        agent_request_finish(job_id, request_id,
                             action_ret == ESP_OK,
                             action_ret == ESP_OK ? NULL :
                             esp_err_to_name(action_ret));
        executed++;
        if (action_ret != ESP_OK) {
            all_ok = false;
            break;
        }
    }
    *executed_out = executed;
    *all_ok_out = all_ok;
}

static size_t agent_history_utf8_prefix_len(const char *text, size_t max_len)
{
    size_t len = strlen(text);
    if (len <= max_len) {
        return len;
    }
    size_t cut = max_len;
    while (cut > 0 && (((uint8_t)text[cut]) & 0xc0U) == 0x80U) {
        cut--;
    }
    return cut > 0 ? cut : max_len;
}

static void agent_history_add_limited_content(cJSON *entry, const char *content)
{
    const char *text = content ? content : "";
    size_t len = strlen(text);
    if (len <= AGENT_HISTORY_CONTENT_STORE_MAX) {
        cJSON_AddStringToObject(entry, "content", text);
        return;
    }

    size_t cut = agent_history_utf8_prefix_len(text, AGENT_HISTORY_CONTENT_STORE_MAX);
    char *copy = agent_alloc(cut + 16);
    if (!copy) {
        cJSON_AddStringToObject(entry, "content", "[history content allocation failed]");
        return;
    }
    snprintf(copy, cut + 16, "%.*s\n[truncated]", (int)cut, text);
    cJSON_AddStringToObject(entry, "content", copy);
    free(copy);
}

static esp_err_t agent_memory_append_entry(cJSON *entry)
{
    if (!si_long_term_memory_is_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!entry) {
        return ESP_ERR_INVALID_ARG;
    }
    char *payload = cJSON_PrintUnformatted(entry);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    size_t payload_len = strlen(payload);
    if (payload_len == 0 || payload_len > AGENT_HISTORY_MAX_RECORD_BYTES) {
        free(payload);
        return ESP_ERR_INVALID_SIZE;
    }

    if (!s_agent_history_lock ||
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        free(payload);
        return ESP_ERR_TIMEOUT;
    }
    bool formatted = false;
    size_t used_bytes = 0;
    uint32_t record_count = 0;
    esp_err_t ret = agent_memory_file_ensure_ready(&formatted);
    if (ret == ESP_OK) {
        ret = agent_memory_file_append_locked(payload, payload_len,
                                             &used_bytes, &record_count);
    }
    xSemaphoreGive(s_agent_history_lock);
    free(payload);
    return ret;
}

static esp_err_t agent_memory_append_manual_note(const char *session_id,
                                                 const char *content,
                                                 const char *source)
{
    if (!content || !content[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *entry = cJSON_CreateObject();
    if (!entry) {
        return ESP_ERR_NO_MEM;
    }
    char normalized_session[AGENT_SESSION_ID_MAX_LEN + 1];
    agent_session_normalize(session_id, normalized_session, sizeof(normalized_session));
    cJSON_AddStringToObject(entry, "kind", "memory");
    cJSON_AddStringToObject(entry, "scope", "manual_note");
    cJSON_AddStringToObject(entry, "type", "manual_note");
    cJSON_AddStringToObject(entry, "source", source && source[0] ? source : "runtime");
    cJSON_AddStringToObject(entry, "session_id", normalized_session);
    agent_history_add_limited_content(entry, content);
    cJSON_AddNumberToObject(entry, "stored_ms", si_monotonic_ms());
    esp_err_t ret = agent_memory_append_entry(entry);
    cJSON_Delete(entry);
    return ret;
}

static esp_err_t __attribute__((unused))
agent_memory_append_task_summary(const char *session_id,
                                                  const char *job_id,
                                                  const char *user_message,
                                                  const char *assistant_summary,
                                                  const char *profile,
                                                  const char *model,
                                                  bool ok,
                                                  bool dry_run,
                                                  int tool_count,
                                                  int hid_count,
                                                  int executed_count)
{
    /* Same boundary as chat/action history: never block a provider worker on
     * MEMORY.LOG in this recovery candidate. Manual memory HTTP APIs retain
     * the explicit bounded repository path. */
    (void)session_id;
    (void)job_id;
    (void)user_message;
    (void)assistant_summary;
    (void)profile;
    (void)model;
    (void)ok;
    (void)dry_run;
    (void)tool_count;
    (void)hid_count;
    (void)executed_count;
    return ESP_ERR_NOT_SUPPORTED;
}

static void agent_history_add_run_status_json(cJSON *resp, esp_err_t writer_ret,
    const si_agent_history_write_result_t *write_result)
{
    cJSON_DeleteItemFromObjectCaseSensitive(resp, "history_saved");
    cJSON_DeleteItemFromObjectCaseSensitive(resp, "history_pending");
    cJSON_DeleteItemFromObjectCaseSensitive(resp, "history");
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    (void)writer_ret;
    (void)write_result;
    cJSON_AddBoolToObject(resp, "history_saved", false);
    cJSON_AddBoolToObject(resp, "history_pending", false);
    cJSON *history = cJSON_AddObjectToObject(resp, "history");
    cJSON_AddStringToObject(history, "storage", "ram_only_recovery_candidate");
    cJSON_AddStringToObject(history, "user_status", "ESP_ERR_NOT_SUPPORTED");
    cJSON_AddStringToObject(history, "reply_status", "ESP_ERR_NOT_SUPPORTED");
    return;
#else
    bool saved = write_result && write_result->saved && !write_result->pending &&
                 write_result->user_status == ESP_OK &&
                 write_result->assistant_status == ESP_OK;
    bool pending = write_result && write_result->pending;
    cJSON_AddBoolToObject(resp, "history_saved", saved);
    cJSON_AddBoolToObject(resp, "history_pending", pending);
    cJSON *history = cJSON_AddObjectToObject(resp, "history");
    cJSON_AddStringToObject(history, "storage", "tf_card");
    cJSON_AddStringToObject(history, "path", AGENT_HISTORY_TF_FILE);
    cJSON_AddStringToObject(history, "writer_status", esp_err_to_name(writer_ret));
    cJSON_AddStringToObject(
        history, "user_status",
        esp_err_to_name(write_result ? write_result->user_status : writer_ret));
    cJSON_AddStringToObject(
        history, "reply_status",
        esp_err_to_name(write_result ? write_result->assistant_status : writer_ret));
    if (write_result) {
        cJSON_AddBoolToObject(history, "user_duplicate",
                              write_result->user_duplicate);
        cJSON_AddBoolToObject(history, "reply_duplicate",
                              write_result->assistant_duplicate);
        if (write_result->error[0]) {
            cJSON_AddStringToObject(history, "error", write_result->error);
        }
    }
#endif
}
static int agent_history_result_number(const cJSON *result, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(result, name);
    return cJSON_IsNumber(item) && item->valuedouble >= 0 &&
           item->valuedouble <= INT_MAX ? item->valueint : 0;
}

static esp_err_t agent_history_store_run_turn(const agent_run_task_arg_t *run,
                                              cJSON *result, bool aborted)
{
    si_agent_history_write_result_t write_result = {
        .user_status = ESP_ERR_INVALID_STATE,
        .assistant_status = ESP_ERR_INVALID_STATE,
    };
#if AGENT_RUNTIME_RECOVERY_RAM_ONLY
    (void)run;
    (void)aborted;
    write_result.user_status = ESP_ERR_NOT_SUPPORTED;
    write_result.assistant_status = ESP_ERR_NOT_SUPPORTED;
    agent_history_add_run_status_json(result, ESP_ERR_NOT_SUPPORTED, &write_result);
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!run || !result) {
        agent_history_add_run_status_json(result, ESP_ERR_INVALID_ARG, &write_result);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *message =
        cJSON_GetObjectItemCaseSensitive(result, "message");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(result, "error");
    const char *assistant_content = aborted ? "run aborted" :
        (cJSON_IsString(message) && message->valuestring && message->valuestring[0] ?
            message->valuestring :
        (cJSON_IsString(error) && error->valuestring && error->valuestring[0] ?
            error->valuestring :
         "Agent completed without a textual response."));
    const cJSON *ok_item = cJSON_GetObjectItemCaseSensitive(result, "ok");
    const cJSON *tool_calls =
        cJSON_GetObjectItemCaseSensitive(result, "tool_calls");
    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(result, "actions");
    int action_count =
        (cJSON_IsArray(tool_calls) ? cJSON_GetArraySize(tool_calls) : 0) +
        (cJSON_IsArray(actions) ? cJSON_GetArraySize(actions) : 0);
    int executed_count =
        agent_history_result_number(result, "tool_executed") +
        agent_history_result_number(result, "hid_executed");
    bool history_enabled = si_conversation_history_is_enabled();
    si_agent_history_turn_write_t turn = {
        .conversation_history_enabled = history_enabled,
        .session_id = run->session_id,
        .turn_id = run->turn_id,
        .run_id = run->job_id,
        .user_content = run->message,
        .assistant_content = assistant_content,
        .profile = run->profile,
        .model = run->model,
        .assistant_kind = aborted || !cJSON_IsTrue(ok_item) ?
                          "error" : "message",
        .ok = !aborted && cJSON_IsTrue(ok_item),
        .dry_run = run->dry_run,
        .actions_count = action_count,
        .executed_count = executed_count,
        .stored_ms = si_monotonic_ms(),
    };
    agent_run_set_stage(run->job_id, "保存历史",
                        "等待专用 TF writer 持久化当前 turn");
    esp_err_t writer_ret = si_agent_history_writer_store_turn(
        &turn, SI_AGENT_HISTORY_WRITER_DEFAULT_WAIT_MS, &write_result);
    agent_history_add_run_status_json(result, writer_ret, &write_result);
    return writer_ret;
#endif
}

static void diag_append(char *out, size_t out_size, const char *fmt, ...)
{
    if (!out || out_size == 0 || !fmt) {
        return;
    }
    size_t used = strnlen(out, out_size);
    if (used >= out_size - 1) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(out + used, out_size - used, fmt, args);
    va_end(args);
}

static void web_debug_agent_history(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    si_storage_tf_status_t tf_status;
    si_storage_get_tf_status(&tf_status);
    diag_append(out, out_size,
                "tf mounted=%d total=%llu free=%llu used=%llu err=%s\n",
                tf_status.mounted,
                (unsigned long long)tf_status.total_bytes,
                (unsigned long long)tf_status.free_bytes,
                (unsigned long long)tf_status.used_bytes,
                tf_status.last_error[0] ? tf_status.last_error : "-");
    diag_append(out, out_size, "history path=%s\n", AGENT_HISTORY_TF_FILE);

    if (!s_agent_history_lock) {
        diag_append(out, out_size, "history lock=not-started\n");
        return;
    }
    if (xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        diag_append(out, out_size, "history lock=timeout\n");
        return;
    }

    bool formatted = false;
    size_t used_bytes = 0;
    uint32_t record_count = 0;
    esp_err_t ret = agent_history_file_ensure_ready(&formatted);
    if (ret == ESP_OK) {
        ret = agent_history_file_stats_locked(&used_bytes, &record_count);
    }
    xSemaphoreGive(s_agent_history_lock);

    diag_append(out, out_size,
                "history ret=%s formatted=%d bytes=%u records=%" PRIu32 "\n",
                esp_err_to_name(ret), formatted, (unsigned)used_bytes, record_count);
    if (s_agent_history_last_error[0]) {
        diag_append(out, out_size, "history error=%s\n", s_agent_history_last_error);
    }
}

static void web_debug_agent_history_append(const char *text, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    const char *content = text && text[0] ? text : "uart history test";

    cJSON *entry = cJSON_CreateObject();
    if (!entry) {
        strlcpy(out, "history append alloc failed\n", out_size);
        return;
    }
    cJSON_AddStringToObject(entry, "kind", "message");
    cJSON_AddStringToObject(entry, "role", "system");
    cJSON_AddStringToObject(entry, "session_id", AGENT_SESSION_DEFAULT_ID);
    cJSON_AddStringToObject(entry, "content", content);
    cJSON_AddNumberToObject(entry, "stored_ms", si_monotonic_ms());
    char *payload = cJSON_PrintUnformatted(entry);
    cJSON_Delete(entry);
    if (!payload) {
        strlcpy(out, "history append payload alloc failed\n", out_size);
        return;
    }

    if (!s_agent_history_lock ||
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        free(payload);
        strlcpy(out, "history lock unavailable\n", out_size);
        return;
    }
    bool formatted = false;
    size_t used_bytes = 0;
    uint32_t record_count = 0;
    esp_err_t ret = agent_history_file_ensure_ready(&formatted);
    if (ret == ESP_OK) {
        ret = agent_history_file_append_locked(payload, strlen(payload),
                                               &used_bytes, &record_count);
    }
    xSemaphoreGive(s_agent_history_lock);
    free(payload);

    diag_append(out, out_size,
                "history append ret=%s formatted=%d bytes=%u records=%" PRIu32 "\n",
                esp_err_to_name(ret), formatted, (unsigned)used_bytes, record_count);
    if (s_agent_history_last_error[0]) {
        diag_append(out, out_size, "history error=%s\n", s_agent_history_last_error);
    }
}

static void web_debug_agent_history_clear(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    bool cleared = false;
    esp_err_t ret = si_agent_repository_clear_history(&cleared);
    diag_append(out, out_size, "history clear ret=%s formatted=%d path=%s\n",
                esp_err_to_name(ret), cleared, AGENT_HISTORY_TF_FILE);
    if (s_agent_history_last_error[0]) {
        diag_append(out, out_size, "history error=%s\n", s_agent_history_last_error);
    }
}

static void web_debug_agent_memory(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    si_storage_tf_status_t tf_status;
    si_storage_get_tf_status(&tf_status);
    diag_append(out, out_size,
                "tf mounted=%d total=%llu free=%llu used=%llu err=%s\n",
                tf_status.mounted,
                (unsigned long long)tf_status.total_bytes,
                (unsigned long long)tf_status.free_bytes,
                (unsigned long long)tf_status.used_bytes,
                tf_status.last_error[0] ? tf_status.last_error : "-");
    diag_append(out, out_size, "memory path=%s\n", AGENT_MEMORY_TF_FILE);

    if (!s_agent_history_lock) {
        diag_append(out, out_size, "memory lock=not-started\n");
        return;
    }
    if (xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        diag_append(out, out_size, "memory lock=timeout\n");
        return;
    }

    bool formatted = false;
    size_t used_bytes = 0;
    uint32_t record_count = 0;
    uint32_t returned = 0;
    cJSON *records = cJSON_CreateArray();
    esp_err_t ret = records ? agent_memory_file_ensure_ready(&formatted) : ESP_ERR_NO_MEM;
    if (ret == ESP_OK) {
        ret = agent_memory_file_stats_locked(&used_bytes, &record_count);
    }
    if (ret == ESP_OK && records) {
        uint32_t skip = record_count > 5U ? record_count - 5U : 0;
        ret = agent_memory_file_add_records_json_locked(records, skip, &returned);
    }
    xSemaphoreGive(s_agent_history_lock);

    diag_append(out, out_size,
                "memory ret=%s formatted=%d bytes=%u records=%" PRIu32 " returned=%" PRIu32 "\n",
                esp_err_to_name(ret), formatted, (unsigned)used_bytes,
                record_count, returned);
    if (s_agent_history_last_error[0]) {
        diag_append(out, out_size, "memory error=%s\n", s_agent_history_last_error);
    }
    if (records && returned > 0) {
        cJSON *record = NULL;
        cJSON_ArrayForEach(record, records) {
            const char *scope = json_string_any(record, "scope", "kind", "type");
            const char *session_id = agent_history_record_session_id(record);
            const char *content = agent_history_record_content(record);
            diag_append(out, out_size, "- [%s/%s] %.220s\n",
                        scope && scope[0] ? scope : "memory",
                        session_id && session_id[0] ? session_id : "global",
                        content && content[0] ? content : "(empty)");
        }
    }
    cJSON_Delete(records);
}

static void web_debug_agent_memory_append(const char *text, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    const char *content = text && text[0] ? text : "uart memory note";
    esp_err_t ret = agent_memory_append_manual_note(AGENT_SESSION_DEFAULT_ID,
                                                    content, "uart");
    diag_append(out, out_size, "memory append ret=%s\n", esp_err_to_name(ret));
    if (s_agent_history_last_error[0]) {
        diag_append(out, out_size, "memory error=%s\n", s_agent_history_last_error);
    }
}

static void web_debug_agent_memory_clear(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    bool cleared = false;
    esp_err_t ret = si_agent_repository_clear_memory(&cleared);
    diag_append(out, out_size, "memory clear ret=%s formatted=%d path=%s\n",
                esp_err_to_name(ret), cleared, AGENT_MEMORY_TF_FILE);
    if (s_agent_history_last_error[0]) {
        diag_append(out, out_size, "memory error=%s\n", s_agent_history_last_error);
    }
}

static void web_debug_agent_data_clear(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    si_agent_data_clear_result_t cleared = {0};
    esp_err_t ret = agent_data_clear_all(&cleared);
    diag_append(
        out, out_size,
        "agent data clear ret=%s history=%d memory=%d\n",
        esp_err_to_name(ret), cleared.history_cleared, cleared.memory_cleared);
    if (ret == ESP_ERR_INVALID_STATE) {
        diag_append(out, out_size,
                    "agent data clear refused: stop the active Agent task first\n");
    } else if (si_agent_repository_last_error()[0]) {
        diag_append(out, out_size, "agent data error=%s\n",
                    si_agent_repository_last_error());
    }
}

static void web_debug_ssh_target(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    ssh_target_settings_t target;
    esp_err_t ret = ssh_target_load(&target);
    diag_append(out, out_size, "ssh target ret=%s configured=%d\n",
                esp_err_to_name(ret), ret == ESP_OK && target.configured);
    if (ret == ESP_OK) {
        diag_append(out, out_size, "host=%s port=%u user=%s auth=%s timeout=%" PRIu32 "\n",
                    target.host[0] ? target.host : "-",
                    target.port,
                    target.username[0] ? target.username : "-",
                    target.auth_method[0] ? target.auth_method : "-",
                    target.timeout_ms);
        diag_append(out, out_size,
                    "private_key=%d public_key=%d password_configured=%d sudo_password_configured=%d password_storage=device_local_secret_store\n",
                    ssh_key_string_exists(SSH_KEY_PRIVATE_KEY),
                    ssh_key_string_exists(SSH_KEY_PUBLIC_KEY),
                    target.password_configured,
                    target.sudo_password_configured);
    }
}

static void web_debug_ssh_exec(const char *command, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!command || !command[0]) {
        strlcpy(out, "ssh command required\n", out_size);
        return;
    }

    ssh_target_settings_t target;
    esp_err_t ret = ssh_target_load(&target);
    if (ret != ESP_OK || !target.configured) {
        diag_append(out, out_size, "ssh target unavailable ret=%s configured=%d\n",
                    esp_err_to_name(ret), ret == ESP_OK && target.configured);
        return;
    }

    si_ssh_exec_config_t *config = heap_caps_calloc(1, sizeof(*config),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!config) {
        config = calloc(1, sizeof(*config));
    }
    if (!config) {
        strlcpy(out, "ssh config alloc failed\n", out_size);
        return;
    }
    strlcpy(config->host, target.host, sizeof(config->host));
    config->port = target.port ? target.port : SSH_TARGET_DEFAULT_PORT;
    strlcpy(config->username, target.username, sizeof(config->username));
    char sudo_password_buf[SI_SSH_PASSWORD_MAX_LEN] = {0};
    bool sudo_password_available = ssh_target_apply_sudo_password(sudo_password_buf,
                                                                  sizeof(sudo_password_buf));
    bool sudo_applied = false;
    if (!ssh_prepare_sudo_command(command,
                                  sudo_password_available ? sudo_password_buf : NULL,
                                  config, &sudo_applied)) {
        memset(sudo_password_buf, 0, sizeof(sudo_password_buf));
        memset(config, 0, sizeof(*config));
        free(config);
        strlcpy(out, "ssh command too long after sudo wrapping\n", out_size);
        return;
    }
    memset(sudo_password_buf, 0, sizeof(sudo_password_buf));
    config->timeout_ms = target.timeout_ms ? target.timeout_ms : SSH_TARGET_DEFAULT_TIMEOUT_MS;

    bool using_stored_password = false;
    bool has_password = ssh_target_apply_password(config->password, sizeof(config->password),
                                                  NULL, &using_stored_password);
    bool use_key = strcasecmp(target.auth_method, "key") == 0 ||
                   strcasecmp(target.auth_method, "stored_key") == 0 ||
                   strcasecmp(target.auth_method, "publickey") == 0;
    bool auto_key = strcasecmp(target.auth_method, "auto") == 0 && !has_password;
    if (use_key || auto_key) {
        ret = ssh_key_apply_to_exec(config, NULL);
        if (ret != ESP_OK) {
            diag_append(out, out_size,
                        "ssh credential not available ret=%s; save SSH password or upload SSH key in Settings\n",
                        esp_err_to_name(ret));
            memset(config, 0, sizeof(*config));
            free(config);
            return;
        }
    } else if (!has_password) {
        diag_append(out, out_size,
                    "ssh password not configured in Settings; save password or upload SSH key\n");
        memset(config, 0, sizeof(*config));
        free(config);
        return;
    }

    char *output = heap_caps_calloc(1, 2049, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output) {
        output = calloc(1, 2049);
    }
    if (!output) {
        memset(config, 0, sizeof(*config));
        free(config);
        strlcpy(out, "ssh output alloc failed\n", out_size);
        return;
    }

    si_ssh_exec_result_t result = {
        .output = output,
        .output_size = 2049,
        .exit_status = -1,
    };
    ret = si_ssh_exec(config, &result);
    diag_append(out, out_size,
                "ssh ret=%s ok=%d host=%s port=%u user=%s auth=%s exit=%d rc=%d elapsed=%" PRId64 "ms\n",
                esp_err_to_name(ret), ret == ESP_OK && result.ok,
                config->host, config->port, config->username,
                config->use_private_key ? "key" : "password",
                result.exit_status, result.ssh_rc, result.elapsed_ms);
    diag_append(out, out_size, "credential_source=%s\n",
                config->use_private_key ? "stored_key" :
                (using_stored_password ? "settings_password" : "none"));
    diag_append(out, out_size, "sudo_applied=%d sudo_password_configured=%d\n",
                sudo_applied, target.sudo_password_configured);
    if (result.error[0]) {
        diag_append(out, out_size, "ssh error=%s\n", result.error);
    }
    if (result.output && result.output[0]) {
        diag_append(out, out_size, "output:\n%s\n", result.output);
    }
    memset(config, 0, sizeof(*config));
    free(config);
    free(output);
}

static const char *agent_run_state_name(agent_run_state_t state)
{
    switch (state) {
    case AGENT_RUN_STATE_RUNNING:
        return "running";
    case AGENT_RUN_STATE_WAITING_REQUEST:
        return "waiting_request";
    case AGENT_RUN_STATE_PAUSED:
        return "paused";
    case AGENT_RUN_STATE_DONE:
        return "done";
    case AGENT_RUN_STATE_FAILED:
        return "failed";
    case AGENT_RUN_STATE_ABORTED:
        return "aborted";
    case AGENT_RUN_STATE_OUTCOME_UNKNOWN:
        return "outcome_unknown";
    case AGENT_RUN_STATE_IDLE:
    default:
        return "idle";
    }
}

static void agent_run_set_stage(const char *job_id, const char *stage, const char *detail)
{
    if (!s_agent_run_lock) {
        return;
    }
    if (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (!job_id || strcmp(s_agent_run_job.job_id, job_id) == 0) {
        strlcpy(s_agent_run_job.stage, stage ? stage : "", sizeof(s_agent_run_job.stage));
        strlcpy(s_agent_run_job.detail, detail ? detail : "", sizeof(s_agent_run_job.detail));
        s_agent_run_job.updated_ms = si_monotonic_ms();
    }
    xSemaphoreGive(s_agent_run_lock);
}

static bool agent_run_take_pending_steer(const char *job_id,
                                         char *out, size_t out_size,
                                         uint32_t *revision_out)
{
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    if (revision_out) {
        *revision_out = 0;
    }
    if (!s_agent_run_lock || !out || out_size == 0 ||
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    bool available =
        (!job_id || strcmp(s_agent_run_job.job_id, job_id) == 0) &&
        s_agent_run_job.pending_steer[0] != '\0';
    if (available) {
        strlcpy(out, s_agent_run_job.pending_steer, out_size);
        s_agent_run_job.pending_steer[0] = '\0';
        if (revision_out) {
            *revision_out = s_agent_run_job.intent_revision;
        }
        s_agent_run_job.updated_ms = si_monotonic_ms();
    }
    xSemaphoreGive(s_agent_run_lock);
    return available;
}

#include "agent_action_event_runtime_module.inc"
#include "agent_run_control_module.inc"

#include "agent_status_v2_projection_module.inc"
