// Transitional Agent module: run status, response loop and result persistence. Shares the run context until its service API is finalized.
static esp_err_t agent_run_prepare_task_service(const char *run_id)
{
    esp_err_t ret = si_agent_task_service_set_phase(
        run_id, SI_AGENT_TURN_PLANNING);
    if (ret == ESP_OK) {
        ret = si_agent_task_service_begin_plan(
            run_id, AGENT_RUNTIME_V2_PLAN_ID);
    }
    if (ret == ESP_OK) {
        ret = si_agent_task_service_add_step(
            run_id, AGENT_RUNTIME_V2_STEP_ID);
    }
    if (ret == ESP_OK) {
        ret = si_agent_task_service_set_phase(
            run_id, SI_AGENT_TURN_EXECUTING);
    }
    if (ret == ESP_OK) {
        ret = si_agent_task_service_set_step_status(
            run_id, AGENT_RUNTIME_V2_STEP_ID,
            SI_AGENT_STEP_IN_PROGRESS);
    }
    if (ret != ESP_OK) {
        (void)si_agent_task_service_fail(run_id,
                                         "executor_plan_prepare_failed");
    }
    return ret;
}

static esp_err_t agent_run_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    char expected_run_id[AGENT_RUN_JOB_ID_MAX_LEN + 1] = {0};
    bool exact = false;
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char query[128] = {0};
        char requested_run_id[128] = {0};
        if (query_len >= sizeof(query) ||
            httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "invalid status query");
        }
        bool run_id_key_present = strncmp(query, "run_id=", 7U) == 0 ||
                                  strstr(query, "&run_id=") != NULL;
        esp_err_t run_id_query_ret = httpd_query_key_value(
            query, "run_id", requested_run_id, sizeof(requested_run_id));
        if (run_id_query_ret == ESP_OK) {
            exact = true;
            if (!requested_run_id[0] ||
                strnlen(requested_run_id, AGENT_RUN_JOB_ID_MAX_LEN + 1U) >
                    AGENT_RUN_JOB_ID_MAX_LEN) {
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                           "valid run_id required");
            }
            strlcpy(expected_run_id, requested_run_id,
                    sizeof(expected_run_id));
        } else if (run_id_key_present) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "valid run_id required");
        }
    }

    cJSON *resp = cJSON_CreateObject();
    bool mismatch = false;
    agent_run_add_status_json(resp, true,
                              exact ? expected_run_id : NULL, &mismatch);
    if (exact && mismatch) {
        /* Drop the temporary object so even fields added before the mismatch
         * can never reveal the currently retained Run. */
        cJSON_Delete(resp);
        resp = cJSON_CreateObject();
        httpd_resp_set_status(req, "404 Not Found");
        cJSON_AddBoolToObject(resp, "found", false);
        cJSON_AddStringToObject(resp, "run_id", expected_run_id);
        cJSON_AddStringToObject(resp, "reason", "run_id_mismatch");
    } else if (exact) {
        cJSON_AddBoolToObject(resp, "found", true);
    }
    esp_err_t ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static char *agent_build_report_request(const char *provider,
                                        const char *model,
                                        const char *user_message,
                                        const char *initial_message,
                                        cJSON *run_resp)
{
    if (!run_resp) {
        return NULL;
    }

    cJSON *input = cJSON_CreateObject();
    if (!input) {
        return NULL;
    }
    cJSON_AddStringToObject(input, "user_message", user_message ? user_message : "");
    cJSON_AddStringToObject(input, "assistant_pre_execution_message",
                            initial_message ? initial_message : "");
    cJSON_AddBoolToObject(input, "dry_run",
                          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(run_resp, "dry_run")));
    cJSON_AddBoolToObject(input, "ok",
                          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(run_resp, "ok")));

    const char *copy_keys[] = {
        "route", "trace", "tool_calls", "tool_results", "actions", "results",
        "tool_executed", "hid_executed", "executed", "screenshot",
        "operability_review"
    };
    for (size_t i = 0; i < sizeof(copy_keys) / sizeof(copy_keys[0]); i++) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(run_resp, copy_keys[i]);
        if (item) {
            cJSON *copy = cJSON_Duplicate(item, true);
            if (copy) {
                cJSON_AddItemToObject(input, copy_keys[i], copy);
            }
        }
    }

    char *input_json = cJSON_PrintUnformatted(input);
    cJSON_Delete(input);
    if (!input_json) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    if (!root || !messages) {
        cJSON_Delete(root);
        free(input_json);
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", model && model[0] ? model : AGENT_API_MODEL_DEFAULT);
    agent_apply_provider_request_parameters(root, provider, model, 0.1);
    agent_add_chat_text(messages, "system",
                        "你是 ExoAnchor 的执行结果报告员。工具和 HID 动作已经执行完毕；"
                        "本轮只允许写给用户看的简短报告，不允许继续请求任何工具或动作。"
                        "请用用户语言总结：做了什么、结果是否成功、关键输出、下一步建议。"
                        "只能依据本次 Agent run JSON 中的 error、loop_error、tool_results、report_cloud_status 等结构化字段判断错误；"
                        "不要把 user_message、历史对话或终端日志文本里提到的旧 HTTP 400/报错当成本次错误。"
                        "如果 operability_review.required=true，必须执行交付验收判断："
                        "只有在 Agent run JSON 证明操作者可从当前 SSH 登录身份实际访问/控制、运行用户已区分、重启/自启动行为已说明时，operability.verified 才能为 true；"
                        "否则 operability.verified 必须为 false，message 里必须明确说可操作性验收未完成，不要声称任务完全交付。"
                        "必须只返回 JSON，Schema: "
                        "{\"message\":\"简短最终回复\",\"summary\":\"一句话总结\","
                        "\"operability\":{\"required\":false,\"verified\":false,"
                        "\"run_user\":\"\",\"operator_user\":\"\",\"access_command\":\"\","
                        "\"reboot_behavior\":\"\",\"missing\":[\"缺失项\"]},"
                        "\"next_steps\":[\"可选下一步\"],\"tool_calls\":[],\"actions\":[]}.",
                        0);

    size_t prompt_size = strlen(input_json) + 384;
    char *prompt = agent_alloc(prompt_size);
    if (!prompt) {
        cJSON_Delete(root);
        free(input_json);
        return NULL;
    }
    snprintf(prompt, prompt_size,
             "请基于下面这次 Agent run 的执行结果生成最终报告。不要再次调用工具。\n%s",
             input_json);
    free(input_json);
    agent_add_chat_text(messages, "user", prompt, 0);
    free(prompt);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static bool agent_run_generate_final_report(const char *job_id,
                                            const char *url,
                                            const agent_api_profile_t *profile,
                                            const char *selected_model,
                                            const char *user_message,
                                            const char *initial_message,
                                            cJSON *run_resp)
{
    if (!url || !profile || !run_resp) {
        return false;
    }

    agent_run_set_stage(job_id, "生成执行报告",
                        "把工具执行结果再次提交给模型，生成最终总结");
    if (!agent_run_page_context_allowed()) {
        cJSON_DeleteItemFromObjectCaseSensitive(run_resp, "page_context");
    }
    cJSON_AddBoolToObject(run_resp, "report_requested", true);

    char api_key[AGENT_API_SECRET_MAX_LEN + 1] = {0};
    esp_err_t ret = agent_api_get_secret_for_profile(profile->id, api_key, sizeof(api_key));
    if (ret != ESP_OK) {
        cJSON_AddBoolToObject(run_resp, "report_ok", false);
        cJSON_AddStringToObject(run_resp, "report_error", "agent api key not configured");
        return false;
    }

    bool report_context_attached =
        cJSON_GetObjectItemCaseSensitive(run_resp, "page_context") != NULL;
    char *body = agent_build_report_request(profile->provider, selected_model,
                                            user_message,
                                            initial_message, run_resp);
    bool report_context_request_lock =
        body && report_context_attached &&
        agent_run_page_context_request_lock(job_id);
    if (body && report_context_attached &&
        !report_context_request_lock) {
        cJSON_DeleteItemFromObjectCaseSensitive(run_resp, "page_context");
        free(body);
        body = agent_build_report_request(profile->provider, selected_model,
                                          user_message, initial_message,
                                          run_resp);
    }
    if (!body) {
        memset(api_key, 0, sizeof(api_key));
        cJSON_AddBoolToObject(run_resp, "report_ok", false);
        cJSON_AddStringToObject(run_resp, "report_error", "report request alloc failed");
        return false;
    }

    char *payload = NULL;
    int status = 0;
    char cloud_err[128] = {0};
    ret = agent_cloud_post_json(job_id, url, api_key, body,
                                report_context_request_lock, &payload, &status,
                                cloud_err, sizeof(cloud_err));
    memset(api_key, 0, sizeof(api_key));
    free(body);
    cJSON_AddNumberToObject(run_resp, "report_cloud_status", status);
    if (ret != ESP_OK || status < 200 || status >= 300) {
        char msg[192];
        if (ret != ESP_OK) {
            snprintf(msg, sizeof(msg), "%s", cloud_err[0] ? cloud_err : esp_err_to_name(ret));
        } else {
            snprintf(msg, sizeof(msg), "report cloud http status %d: %.80s",
                     status, payload ? payload : "");
        }
        cJSON_AddBoolToObject(run_resp, "report_ok", false);
        cJSON_AddStringToObject(run_resp, "report_error", msg);
        free(payload);
        return false;
    }

    cJSON *api_root = cJSON_Parse(payload);
    free(payload);
    if (!api_root) {
        cJSON_AddBoolToObject(run_resp, "report_ok", false);
        cJSON_AddStringToObject(run_resp, "report_error", "report cloud returned invalid json");
        return false;
    }
    const char *content = agent_response_content(api_root);
    cJSON *report_json = agent_parse_json_from_text(content);
    const char *final_message = report_json ?
                                json_string_any(report_json, "message", "summary", "report") :
                                content;
    if (!final_message || !final_message[0]) {
        final_message = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(run_resp, "ok")) ?
                        "执行完成。" : "执行已结束，但存在失败项。";
    }

    cJSON *operability_review = cJSON_GetObjectItemCaseSensitive(run_resp, "operability_review");
    bool operability_required = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(operability_review, "required"));
    bool operability_verified = !operability_required;
    if (operability_required && report_json) {
        cJSON *operability = cJSON_GetObjectItemCaseSensitive(report_json, "operability");
        operability_verified = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(operability, "verified"));
    }
    bool operability_guard = operability_required && !operability_verified;
    char *guarded_message = NULL;
    if (operability_guard) {
        guarded_message = agent_alloc(AGENT_HISTORY_CONTENT_STORE_MAX + 1);
        if (guarded_message) {
            snprintf(guarded_message, AGENT_HISTORY_CONTENT_STORE_MAX + 1,
                     "可操作性验收未完成：本次任务涉及后台服务、会话、容器或独立运行用户，"
                     "但报告没有证明操作者可从当前 SSH 登录身份实际访问/控制，也没有完整确认运行身份和重启后行为。"
                     "请先补充验证。\n\n%.*s",
                     1200, final_message);
            final_message = guarded_message;
        } else {
            final_message = "可操作性验收未完成，且保护性报告缓冲分配失败。请先补充验证。";
        }
        agent_json_set_bool(run_resp, "ok", false);
        cJSON_AddStringToObject(run_resp, "operability_error",
                                "operator access/control handoff not verified");
    }

    cJSON_AddBoolToObject(run_resp, "report_ok", true);
    cJSON *report = cJSON_AddObjectToObject(run_resp, "final_report");
    cJSON_AddStringToObject(report, "message", final_message);
    cJSON_AddBoolToObject(report, "operability_required", operability_required);
    cJSON_AddBoolToObject(report, "operability_verified", operability_verified);
    cJSON_AddBoolToObject(report, "operability_guard_applied", operability_guard);
    if (report_json) {
        const char *summary = json_string_any(report_json, "summary", "outcome", NULL);
        if (summary && summary[0]) {
            cJSON_AddStringToObject(report, "summary", summary);
        }
        cJSON *operability = cJSON_GetObjectItemCaseSensitive(report_json, "operability");
        if (cJSON_IsObject(operability)) {
            cJSON *copy = cJSON_Duplicate(operability, true);
            cJSON_AddItemToObject(report, "operability",
                                  copy ? copy : cJSON_CreateObject());
        }
        cJSON *next_steps = cJSON_GetObjectItemCaseSensitive(report_json, "next_steps");
        if (cJSON_IsArray(next_steps)) {
            cJSON *copy = cJSON_Duplicate(next_steps, true);
            cJSON_AddItemToObject(report, "next_steps", copy ? copy : cJSON_CreateArray());
        }
    }

    cJSON_DeleteItemFromObjectCaseSensitive(run_resp, "message");
    cJSON_AddStringToObject(run_resp, "message", final_message);
    free(guarded_message);
    cJSON_Delete(report_json);
    cJSON_Delete(api_root);
    return true;
}

typedef struct {
    const char *job_id;
    const char *session_id;
    const char *message;
    const char *provider;
    const char *selected_model;
    const char *url;
    const char *api_key;
    const si_agent_route_t *route;
    bool dry_run;
    bool allow_screenshot;
    bool allow_web_search;
    cJSON *resp;
    cJSON *trace_out;
    cJSON *tool_calls_out;
    cJSON *hid_actions_out;
    cJSON *tool_results;
    cJSON *results;
    char *latest_message;
    size_t latest_message_size;
    cJSON *previous_assistant_message;
    bool preserve_assistant_message;
    int total_tool_executed;
    int total_hid_executed;
    int last_tool_count;
    int last_hid_count;
    int loop_iterations;
    int failed_batch_count;
    bool last_batch_ok;
    bool had_tool_failure;
} agent_run_loop_context_t;

static void agent_run_preserve_assistant_message(
    agent_run_loop_context_t *ctx, cJSON *api_root)
{
    if (!ctx || !ctx->preserve_assistant_message) {
        return;
    }
    cJSON *message = agent_response_message(api_root);
    cJSON *copy = message ? cJSON_Duplicate(message, true) : NULL;
    if (!copy) {
        return;
    }
    cJSON_Delete(ctx->previous_assistant_message);
    ctx->previous_assistant_message = copy;
}

static void agent_run_execute_tool_loop(agent_run_loop_context_t *ctx,
                                        cJSON *tool_calls,
                                        cJSON *hid_actions)
{
    int tool_executed = 0;
    int hid_executed = 0;
    bool tools_ok = true;
    bool hid_ok = true;
    agent_run_set_stage(ctx->job_id, "执行工具",
                        ctx->dry_run ? "dry-run 只规划不执行" :
                        "执行 SSH/HID 工具并收集结果");
    agent_execute_tool_calls(ctx->job_id, tool_calls, ctx->dry_run,
                             ctx->allow_web_search, ctx->session_id,
                             ctx->route,
                             ctx->tool_results, &tool_executed, &tools_ok);
    agent_execute_actions(ctx->job_id, hid_actions, ctx->dry_run, ctx->route,
                          ctx->results,
                          &hid_executed, &hid_ok);
    ctx->total_tool_executed = tool_executed;
    ctx->total_hid_executed = hid_executed;
    ctx->last_batch_ok = tools_ok && hid_ok;
    ctx->had_tool_failure = !ctx->last_batch_ok;
    ctx->failed_batch_count = ctx->last_batch_ok ? 0 : 1;
    ctx->last_tool_count = cJSON_GetArraySize(tool_calls);
    ctx->last_hid_count = cJSON_GetArraySize(hid_actions);
    ctx->loop_iterations = 1;

    agent_json_set_number(ctx->resp, "tool_executed", ctx->total_tool_executed);
    agent_json_set_number(ctx->resp, "hid_executed", ctx->total_hid_executed);
    agent_json_set_number(ctx->resp, "executed", ctx->total_hid_executed);
    agent_json_set_bool(ctx->resp, "ok", ctx->last_batch_ok);
    agent_json_set_bool(ctx->resp, "had_tool_failure", ctx->had_tool_failure);

    int last_batch_work =
        ctx->total_tool_executed + ctx->total_hid_executed;
    agent_run_add_eventf(ctx->job_id, "info",
                         "loop seed · dry_run=%d planned_tools=%d planned_hid=%d executed_tools=%d executed_hid=%d",
                         ctx->dry_run ? 1 : 0, ctx->last_tool_count,
                         ctx->last_hid_count, ctx->total_tool_executed,
                         ctx->total_hid_executed);

    for (int iteration = 2;
         !ctx->dry_run && iteration <= ctx->route->max_tool_loops;
         ++iteration) {
        if (!agent_run_control_point(ctx->job_id)) {
            cJSON_AddStringToObject(ctx->resp, "loop_error",
                                    "run aborted");
            break;
        }
        if (!agent_run_page_context_allowed()) {
            cJSON_DeleteItemFromObjectCaseSensitive(ctx->resp, "page_context");
        }
        char steer[AGENT_RUN_STEER_MAX_LEN + 1] = {0};
        uint32_t intent_revision = 0;
        bool has_steer = agent_run_take_pending_steer(
            ctx->job_id, steer, sizeof(steer), &intent_revision);
        if (last_batch_work <= 0 && !has_steer) {
            if (!ctx->last_batch_ok) {
                cJSON_AddBoolToObject(
                    ctx->resp, "loop_stopped_on_failure", true);
                agent_run_add_eventf(
                    ctx->job_id, "bad",
                    "loop stopped · failed batch produced no new evidence");
            }
            break;
        }
        if (ctx->failed_batch_count >= 2 && !has_steer) {
            cJSON_AddBoolToObject(
                ctx->resp, "loop_stopped_on_repeated_failure", true);
            agent_run_add_eventf(
                ctx->job_id, "bad",
                "loop stopped · repeated failed batches");
            break;
        }
        if (has_steer) {
            bool shadow_bound = false;
            if (xSemaphoreTake(s_agent_run_lock,
                               pdMS_TO_TICKS(200)) == pdTRUE) {
                shadow_bound =
                    strcmp(s_agent_run_job.job_id, ctx->job_id) == 0 &&
                    s_agent_run_job.task_service_bound &&
                    !s_agent_run_job.task_service_control_pending;
                xSemaphoreGive(s_agent_run_lock);
            }
            if (shadow_bound &&
                agent_run_prepare_task_service(ctx->job_id) != ESP_OK) {
                cJSON_AddStringToObject(
                    ctx->resp, "loop_error",
                    "durable replan failed after steer");
                ctx->had_tool_failure = true;
                break;
            }
            cJSON_DeleteItemFromObjectCaseSensitive(ctx->resp, "steer");
            cJSON *steer_item = cJSON_AddObjectToObject(ctx->resp, "steer");
            if (steer_item) {
                cJSON_AddStringToObject(steer_item, "message", steer);
                cJSON_AddNumberToObject(steer_item, "intent_revision",
                                        intent_revision);
                cJSON_AddStringToObject(
                    steer_item, "instruction",
                    "Merge this user update into the current turn at this safe checkpoint.");
            }
            agent_run_add_eventf(
                ctx->job_id, "info",
                "steer applied at safe checkpoint · revision=%" PRIu32,
                intent_revision);
        }
        ctx->loop_iterations = iteration;
        agent_run_set_stage(ctx->job_id, "继续规划",
                            "把工具结果交回模型，获取下一批 tool_calls");
        agent_run_add_eventf(ctx->job_id, "info",
                             "loop[%d] planning next tools · previous tools=%d hid=%d work=%d",
                             iteration, ctx->last_tool_count, ctx->last_hid_count,
                             last_batch_work);
        char *loop_image_b64 = NULL;
        size_t loop_jpeg_len = 0;
        char loop_screenshot_error[128] = {0};
        if (ctx->allow_screenshot && ctx->route->prefer_screenshot) {
            if (agent_provider_allows_screenshot(ctx->provider,
                                                 ctx->selected_model)) {
                esp_err_t shot_ret = agent_capture_screenshot_b64(
                    ctx->job_id, &loop_image_b64, &loop_jpeg_len,
                    loop_screenshot_error,
                    sizeof(loop_screenshot_error));
                if (shot_ret != ESP_OK) {
                    loop_image_b64 = NULL;
                }
            } else {
                snprintf(loop_screenshot_error,
                         sizeof(loop_screenshot_error),
                         "provider %s does not support image_url input",
                         ctx->provider && ctx->provider[0] ?
                         ctx->provider : "unknown");
            }
        }
        char *continue_body = agent_build_continue_request(
            ctx->provider, ctx->selected_model, ctx->session_id, ctx->message,
            loop_image_b64, loop_screenshot_error,
            ctx->resp, ctx->previous_assistant_message,
            ctx->dry_run, ctx->allow_web_search, iteration, ctx->route);
        bool continue_context_attached =
            cJSON_GetObjectItemCaseSensitive(ctx->resp, "page_context") != NULL;
        bool continue_context_request_lock =
            continue_body && continue_context_attached &&
            agent_run_page_context_request_lock(ctx->job_id);
        if (continue_body && continue_context_attached &&
            !continue_context_request_lock) {
            cJSON_DeleteItemFromObjectCaseSensitive(ctx->resp, "page_context");
            free(continue_body);
            continue_body = agent_build_continue_request(
                ctx->provider, ctx->selected_model, ctx->session_id,
                ctx->message, loop_image_b64, loop_screenshot_error,
                ctx->resp, ctx->previous_assistant_message, ctx->dry_run,
                ctx->allow_web_search, iteration, ctx->route);
        }
        free(loop_image_b64);
        if (!continue_body) {
            cJSON_AddStringToObject(ctx->resp, "loop_error",
                                    "continue request alloc failed");
            break;
        }

        char *continue_payload = NULL;
        int continue_status = 0;
        char continue_err[128] = {0};
        esp_err_t ret = agent_cloud_post_json(ctx->job_id, ctx->url,
                                              ctx->api_key, continue_body,
                                              continue_context_request_lock,
                                              &continue_payload,
                                              &continue_status, continue_err,
                                              sizeof(continue_err));
        free(continue_body);
        agent_json_set_number(ctx->resp, "loop_cloud_status", continue_status);
        if (ret != ESP_OK || continue_status < 200 || continue_status >= 300) {
            char msg[192];
            if (ret != ESP_OK) {
                snprintf(msg, sizeof(msg), "%s",
                         continue_err[0] ? continue_err : esp_err_to_name(ret));
            } else {
                snprintf(msg, sizeof(msg), "continue cloud http status %d: %.80s",
                         continue_status, continue_payload ? continue_payload : "");
            }
            cJSON_AddStringToObject(ctx->resp, "loop_error", msg);
            free(continue_payload);
            break;
        }

        cJSON *continue_api_root = cJSON_Parse(continue_payload);
        free(continue_payload);
        if (!continue_api_root) {
            cJSON_AddStringToObject(ctx->resp, "loop_error",
                                    "continue cloud returned invalid json");
            break;
        }
        const char *continue_content = agent_response_content(continue_api_root);
        cJSON *continue_json = agent_parse_json_from_text(continue_content);
        if (!continue_json) {
            cJSON_AddStringToObject(ctx->resp, "loop_error",
                                    continue_content && continue_content[0] ?
                                    continue_content : "continue response was not JSON");
            cJSON_Delete(continue_api_root);
            break;
        }
        agent_run_preserve_assistant_message(ctx, continue_api_root);

        const char *next_message = json_string_any(continue_json, "message", "summary", "reply");
        if (next_message && next_message[0]) {
            strlcpy(ctx->latest_message, next_message, ctx->latest_message_size);
            agent_json_set_string(ctx->resp, "message", ctx->latest_message);
        }
        cJSON *next_trace = cJSON_GetObjectItemCaseSensitive(continue_json, "trace");
        if (!cJSON_IsArray(next_trace)) next_trace = cJSON_GetObjectItemCaseSensitive(continue_json, "plan");
        if (!cJSON_IsArray(next_trace)) next_trace = cJSON_GetObjectItemCaseSensitive(continue_json, "progress");
        agent_array_extend_duplicate(ctx->trace_out, next_trace);

        cJSON *next_actions = cJSON_GetObjectItemCaseSensitive(continue_json, "actions");
        if (!cJSON_IsArray(next_actions)) next_actions = cJSON_GetObjectItemCaseSensitive(continue_json, "steps");
        cJSON *next_tool_calls = cJSON_CreateArray();
        cJSON *next_hid_actions = cJSON_CreateArray();
        if (!next_tool_calls || !next_hid_actions) {
            cJSON_Delete(next_tool_calls);
            cJSON_Delete(next_hid_actions);
            cJSON_Delete(continue_json);
            cJSON_Delete(continue_api_root);
            cJSON_AddStringToObject(ctx->resp, "loop_error",
                                    "continue tool array alloc failed");
            break;
        }
        agent_split_model_actions(continue_json, next_actions,
                                  next_tool_calls, next_hid_actions);
        ctx->last_tool_count = cJSON_GetArraySize(next_tool_calls);
        ctx->last_hid_count = cJSON_GetArraySize(next_hid_actions);
        if (ctx->last_tool_count == 0 && ctx->last_hid_count == 0) {
            cJSON_AddBoolToObject(ctx->resp, "loop_done", true);
            agent_run_add_eventf(ctx->job_id, "info",
                                 "loop[%d] model returned no further tool calls",
                                 iteration);
            cJSON_Delete(next_tool_calls);
            cJSON_Delete(next_hid_actions);
            cJSON_Delete(continue_json);
            cJSON_Delete(continue_api_root);
            break;
        }

        agent_array_extend_duplicate(ctx->tool_calls_out, next_tool_calls);
        agent_array_extend_duplicate(ctx->hid_actions_out, next_hid_actions);
        int batch_tool_executed = 0;
        int batch_hid_executed = 0;
        bool batch_tools_ok = true;
        bool batch_hid_ok = true;
        agent_run_set_stage(ctx->job_id, "执行工具",
                            "执行下一批 SSH/HID 工具并收集结果");
        agent_execute_tool_calls(ctx->job_id, next_tool_calls, ctx->dry_run,
                                 ctx->allow_web_search, ctx->session_id,
                                 ctx->route,
                                 ctx->tool_results, &batch_tool_executed,
                                 &batch_tools_ok);
        agent_execute_actions(ctx->job_id, next_hid_actions, ctx->dry_run,
                              ctx->route,
                              ctx->results,
                              &batch_hid_executed, &batch_hid_ok);
        ctx->total_tool_executed += batch_tool_executed;
        ctx->total_hid_executed += batch_hid_executed;
        last_batch_work = batch_tool_executed + batch_hid_executed;
        ctx->last_batch_ok = batch_tools_ok && batch_hid_ok;
        if (!ctx->last_batch_ok) {
            ctx->had_tool_failure = true;
            ctx->failed_batch_count++;
        } else {
            ctx->failed_batch_count = 0;
        }
        agent_json_set_number(ctx->resp, "tool_executed", ctx->total_tool_executed);
        agent_json_set_number(ctx->resp, "hid_executed", ctx->total_hid_executed);
        agent_json_set_number(ctx->resp, "executed", ctx->total_hid_executed);
        agent_json_set_bool(ctx->resp, "ok", ctx->last_batch_ok);
        agent_json_set_bool(ctx->resp, "had_tool_failure", ctx->had_tool_failure);
        cJSON_Delete(next_tool_calls);
        cJSON_Delete(next_hid_actions);
        cJSON_Delete(continue_json);
        cJSON_Delete(continue_api_root);
    }

    if (!ctx->dry_run && ctx->last_batch_ok &&
        (ctx->last_tool_count > 0 || ctx->last_hid_count > 0) &&
        ctx->loop_iterations >= ctx->route->max_tool_loops) {
        cJSON_AddBoolToObject(ctx->resp, "loop_max_reached", true);
        /*
         * max_tool_loops caps additional planning rounds. Completing the last
         * permitted tool batch successfully is still a successful run; the
         * final report can summarize its verified results.
         */
        cJSON_AddBoolToObject(ctx->resp, "loop_completed_at_limit", true);
    }
    agent_json_set_number(ctx->resp, "loop_iterations", ctx->loop_iterations);
    agent_add_operability_review(ctx->resp);
}

static cJSON *agent_run_repair_model_response(const char *job_id,
                                              const char *provider,
                                              const char *selected_model,
                                              const char *session_id,
                                              const char *message,
                                              const char *page_context_json,
                                              bool dry_run,
                                              bool allow_web_search,
                                              const si_agent_route_t *route,
                                              const char *url,
                                              const char *api_key,
                                              cJSON **api_root,
                                              const char **content,
                                              cJSON *resp)
{
    agent_run_set_stage(job_id, "修复模型响应",
                        "provider 返回了自然语言，正在要求模型改写为 JSON");
    cJSON_AddBoolToObject(resp, "json_repair_requested", true);
    char *repair_body = agent_build_json_repair_request(
        provider, selected_model, session_id, message, page_context_json, *content,
        dry_run, allow_web_search, route);
    bool repair_context_request_lock =
        repair_body && page_context_json && page_context_json[0] &&
        agent_run_page_context_request_lock(job_id);
    if (repair_body && page_context_json && page_context_json[0] &&
        !repair_context_request_lock) {
        free(repair_body);
        repair_body = agent_build_json_repair_request(
            provider, selected_model, session_id, message, "", *content,
            dry_run, allow_web_search, route);
    }
    if (!repair_body) {
        cJSON_AddStringToObject(resp, "json_repair_error",
                                "repair request alloc failed");
        return NULL;
    }

    char *repair_payload = NULL;
    int repair_status = 0;
    char repair_err[128] = {0};
    esp_err_t repair_ret = agent_cloud_post_json(job_id, url, api_key,
                                                 repair_body,
                                                 repair_context_request_lock,
                                                 &repair_payload, &repair_status,
                                                 repair_err, sizeof(repair_err));
    free(repair_body);
    cJSON_AddNumberToObject(resp, "json_repair_cloud_status", repair_status);
    if (repair_ret != ESP_OK || repair_status < 200 || repair_status >= 300 ||
        !repair_payload) {
        char msg[160];
        if (repair_ret != ESP_OK) {
            snprintf(msg, sizeof(msg), "%s",
                     repair_err[0] ? repair_err : esp_err_to_name(repair_ret));
        } else {
            snprintf(msg, sizeof(msg), "repair cloud http status %d", repair_status);
        }
        cJSON_AddStringToObject(resp, "json_repair_error", msg);
        free(repair_payload);
        return NULL;
    }

    cJSON *repair_api_root = cJSON_Parse(repair_payload);
    free(repair_payload);
    if (!repair_api_root) {
        cJSON_AddStringToObject(resp, "json_repair_error",
                                "repair cloud returned invalid json");
        return NULL;
    }
    const char *repair_content = agent_response_content(repair_api_root);
    cJSON *repair_json = agent_parse_json_from_text(repair_content);
    if (!repair_json) {
        cJSON_Delete(repair_api_root);
        cJSON_AddStringToObject(resp, "json_repair_error",
                                "repair response was not JSON");
        return NULL;
    }

    cJSON_Delete(*api_root);
    *api_root = repair_api_root;
    *content = repair_content;
    cJSON_AddBoolToObject(resp, "json_repaired", true);
    return repair_json;
}

static bool agent_run_hid_owner_snapshot(
    const char *job_id, si_hid_owner_token_t *owner)
{
    if (!job_id || !job_id[0] || !owner || !s_agent_run_lock ||
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }
    bool matches = strcmp(s_agent_run_job.job_id, job_id) == 0;
    if (matches) *owner = s_agent_run_job.hid_owner;
    xSemaphoreGive(s_agent_run_lock);
    return matches && owner->generation != 0U;
}

static void agent_run_release_hid_control(const char *job_id)
{
    si_hid_owner_token_t owner = {0};
    if (!agent_run_hid_owner_snapshot(job_id, &owner)) return;
    (void)si_control_lease_release_embedded_owner(&owner);
    if (s_agent_run_lock &&
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (strcmp(s_agent_run_job.job_id, job_id) == 0 &&
            si_hid_owner_token_equal(&s_agent_run_job.hid_owner, &owner)) {
            memset(&s_agent_run_job.hid_owner, 0,
                   sizeof(s_agent_run_job.hid_owner));
        }
        xSemaphoreGive(s_agent_run_lock);
    }
}

static esp_err_t agent_run_acquire_kvm_control(
    const char *job_id, char *err, size_t err_size)
{
    /*
     * A visible KVM page owns the normal video lease. Enter takeover mode so
     * it remains an observable, read-only view while the Agent owns HID.
     */
    si_video_control_touch_agent(true);
    esp_err_t ret = si_video_control_apply();
    si_hid_owner_token_t owner = {0};
    (void)agent_run_hid_owner_snapshot(job_id, &owner);
    if (ret == ESP_OK) {
        ret = si_control_lease_touch_agent_owned(
            "supervised", "agent run", &owner);
    }
    if (ret == ESP_OK && s_agent_run_lock &&
        xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (strcmp(s_agent_run_job.job_id, job_id) == 0 &&
            !s_agent_run_job.cancel_requested) {
            s_agent_run_job.hid_owner = owner;
        } else {
            ret = ESP_ERR_INVALID_STATE;
        }
        xSemaphoreGive(s_agent_run_lock);
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_TIMEOUT;
    }
    if (ret != ESP_OK && owner.generation != 0U) {
        (void)si_control_lease_release_embedded_owner(&owner);
    }
    if (ret != ESP_OK) {
        si_video_control_release_agent();
        (void)si_video_control_apply();
        strlcpy(err, "agent control lease unavailable", err_size);
    }
    return ret;
}

static char *agent_run_build_initial_request(
    const char *provider, const char *model, const char *message,
    const char *session_id, const char *page_context_json,
    const char *image_b64, const char *screenshot_error, bool dry_run,
    bool allow_web_search, const si_agent_route_t *route,
    bool *page_context_attached_out)
{
    bool attached = agent_run_page_context_allowed() && page_context_json &&
                    page_context_json[0];
    if (page_context_attached_out) {
        *page_context_attached_out = attached;
    }
    return agent_build_chat_request(
        provider, model, message, session_id,
        attached ? page_context_json : "", image_b64, screenshot_error,
        dry_run, allow_web_search, route);
}

static esp_err_t agent_run_execute_to_json(const char *job_id,
                                           const char *session_id,
                                           const char *message,
                                           const char *page_context_json,
                                           const char *profile_id,
                                           const char *model_override,
                                           bool dry_run,
                                           bool include_screenshot,
                                           bool allow_web_search,
                                           cJSON **out_resp,
                                           char *err,
                                           size_t err_size)
{
    int64_t started_us = esp_timer_get_time();
    esp_err_t ret = ESP_OK;
    *out_resp = NULL;
    if (!agent_run_control_point(job_id)) {
        strlcpy(err, "run aborted", err_size);
        return ESP_ERR_INVALID_STATE;
    }
    si_agent_route_t route;
    si_agent_route_detect(message, include_screenshot, allow_web_search, &route);
    bool screenshot_effective =
        include_screenshot && route.prefer_screenshot;
    agent_run_set_stage(job_id, "意图路由", "根据用户输入选择 Agent prompt profile 和工具策略");
    agent_run_add_eventf(job_id, "info",
                         "router · route=%s · intent=%s · profile=%s · confidence=%d · loops=%d",
                         route.name ? route.name : "chat",
                         route.intent ? route.intent : "",
                         route.prompt_profile ? route.prompt_profile : "",
                         route.confidence,
                         route.max_tool_loops);
    agent_run_set_stage(job_id, "准备控制权",
                        dry_run ? "dry-run 不申请 HID/KVM 控制权" :
                        "保持观察态；仅在获批的 HID 写操作前按需申请控制权");

    agent_run_set_stage(job_id, "读取模型配置", "读取 API Profile、模型和本地 API Key");
    agent_api_profile_t profile;
    ret = agent_api_get_profile(profile_id, &profile);
    if (ret != ESP_OK) {
        strlcpy(err, "invalid agent api profile", err_size);
        return ret;
    }
    ret = agent_api_validate_endpoint(profile.endpoint, false);
    if (ret != ESP_OK) {
        strlcpy(err, "agent api endpoint must be non-empty HTTPS", err_size);
        return ret;
    }
    char selected_model[AGENT_API_MODEL_MAX_LEN + 1];
    strlcpy(selected_model, model_override && model_override[0] ? model_override : profile.model,
            sizeof(selected_model));
    ret = agent_api_validate_text(selected_model, AGENT_API_MODEL_MAX_LEN, false);
    if (ret != ESP_OK) {
        strlcpy(err, "invalid agent model", err_size);
        return ret;
    }

    char api_key[AGENT_API_SECRET_MAX_LEN + 1] = {0};
    ret = agent_api_get_secret_for_profile(profile.id, api_key, sizeof(api_key));
    if (ret != ESP_OK) {
        memset(api_key, 0, sizeof(api_key));
        strlcpy(err, "agent api key not configured", err_size);
        return ret;
    }
    if (!agent_run_control_point(job_id)) {
        memset(api_key, 0, sizeof(api_key));
        strlcpy(err, "run source revoked or aborted", err_size);
        return ESP_ERR_INVALID_STATE;
    }

    char *image_b64 = NULL;
    size_t jpeg_len = 0;
    char screenshot_error[128] = {0};
    if (screenshot_effective) {
        agent_run_set_stage(job_id, "采集截图", "根据开关为模型准备当前屏幕图像");
        if (agent_provider_allows_screenshot(profile.provider,
                                             selected_model)) {
            ret = agent_capture_screenshot_b64(job_id, &image_b64, &jpeg_len,
                                               screenshot_error, sizeof(screenshot_error));
            if (ret != ESP_OK) {
                image_b64 = NULL;
            }
        } else {
            snprintf(screenshot_error, sizeof(screenshot_error),
                     "provider %s does not support image_url input",
                     profile.provider[0] ? profile.provider : "unknown");
        }
    }

    agent_run_set_stage(job_id, "构建请求", "组装 system prompt、历史上下文和用户消息");
    char url[AGENT_API_ENDPOINT_MAX_LEN + 32];
    ret = agent_build_chat_url(profile.endpoint, url, sizeof(url));
    if (ret != ESP_OK) {
        free(image_b64);
        memset(api_key, 0, sizeof(api_key));
        strlcpy(err, "invalid agent api endpoint", err_size);
        return ret;
    }
    bool page_context_attached = false;
    char *body = agent_run_build_initial_request(
        profile.provider, selected_model, message, session_id,
        page_context_json, image_b64, screenshot_error, dry_run,
        allow_web_search, &route, &page_context_attached);
    if (!body) {
        free(image_b64);
        memset(api_key, 0, sizeof(api_key));
        strlcpy(err, "agent request alloc failed", err_size);
        return ESP_ERR_NO_MEM;
    }
    if (!agent_run_control_point(job_id)) {
        free(body);
        free(image_b64);
        memset(api_key, 0, sizeof(api_key));
        strlcpy(err, "run aborted", err_size);
        return ESP_ERR_INVALID_STATE;
    }
    agent_run_set_stage(job_id, "发送云端请求", "等待 provider 返回 Chat Completions JSON");
    bool initial_context_request_lock = page_context_attached && page_context_json[0] &&
        agent_run_page_context_request_lock(job_id);
    if (page_context_attached && !initial_context_request_lock) {
        free(body);
        body = agent_run_build_initial_request(
            profile.provider, selected_model, message, session_id, "",
            image_b64, screenshot_error, dry_run, allow_web_search, &route,
            &page_context_attached);
        if (!body) {
            free(image_b64);
            memset(api_key, 0, sizeof(api_key));
            strlcpy(err, "agent request rebuild after context revoke failed",
                    err_size);
            return ESP_ERR_NO_MEM;
        }
    }
    free(image_b64);

    char *cloud_payload = NULL;
    int cloud_status = 0;
    char cloud_err[128] = {0};
    ret = agent_cloud_post_json(job_id, url, api_key, body, initial_context_request_lock,
                                &cloud_payload, &cloud_status, cloud_err, sizeof(cloud_err));
    int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    if (elapsed_ms > 1000) {
        ESP_LOGW(TAG, "Agent run cloud call took %" PRId64 " ms profile=%s model=%s status=%d",
                 elapsed_ms, profile.id, selected_model, cloud_status);
    }
    free(body);
    if (!agent_run_control_point(job_id)) {
        free(cloud_payload);
        memset(api_key, 0, sizeof(api_key));
        strlcpy(err, "run aborted", err_size);
        return ESP_ERR_INVALID_STATE;
    }
    if (ret != ESP_OK) {
        memset(api_key, 0, sizeof(api_key));
        const char *msg = cloud_err[0] ? cloud_err : esp_err_to_name(ret);
        strlcpy(err, msg, err_size);
        return ret;
    }
    if (cloud_status < 200 || cloud_status >= 300) {
        memset(api_key, 0, sizeof(api_key));
        char msg[192];
        snprintf(msg, sizeof(msg), "agent cloud http status %d: %.120s",
                 cloud_status, cloud_payload ? cloud_payload : "");
        free(cloud_payload);
        strlcpy(err, msg, err_size);
        return ESP_FAIL;
    }

    agent_run_set_stage(job_id, "解析模型响应", "解析 message、trace、tool_calls 和 HID actions");
    cJSON *api_root = cJSON_Parse(cloud_payload);
    free(cloud_payload);
    if (!api_root) {
        memset(api_key, 0, sizeof(api_key));
        strlcpy(err, "agent cloud returned invalid json", err_size);
        return ESP_FAIL;
    }
    const char *content = agent_response_content(api_root);
    cJSON *model_json = agent_parse_json_from_text(content);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "session_id", session_id);
    cJSON_AddStringToObject(resp, "profile", profile.id);
    cJSON_AddStringToObject(resp, "provider", profile.provider);
    cJSON_AddStringToObject(resp, "model", selected_model);
    si_model_capabilities_t model_capabilities;
    si_model_capabilities_resolve(profile.provider, selected_model,
                                  &model_capabilities);
    cJSON *compatibility =
        cJSON_AddObjectToObject(resp, "provider_compatibility");
    cJSON_AddBoolToObject(compatibility, "image_input",
                          model_capabilities.image_input);
    cJSON_AddBoolToObject(compatibility, "base64_image_input",
                          model_capabilities.base64_image_input);
    cJSON_AddBoolToObject(compatibility, "preserve_assistant_message",
                          model_capabilities.preserve_assistant_message);
    cJSON_AddBoolToObject(compatibility, "fixed_sampling_parameters",
                          model_capabilities.fixed_sampling_parameters);
    if (model_capabilities.default_reasoning_effort) {
        cJSON_AddStringToObject(
            compatibility, "reasoning_effort",
            model_capabilities.default_reasoning_effort);
    }
    cJSON_AddBoolToObject(resp, "dry_run", dry_run);
    cJSON_AddNumberToObject(resp, "cloud_status", cloud_status);
    if (agent_run_page_context_allowed() && page_context_json &&
        page_context_json[0]) {
        cJSON *page_context = cJSON_Parse(page_context_json);
        if (page_context) {
            cJSON_AddItemToObject(resp, "page_context", page_context);
        }
    }
    agent_route_add_json(resp, &route);
    cJSON *shot = cJSON_AddObjectToObject(resp, "screenshot");
    cJSON_AddBoolToObject(shot, "requested", screenshot_effective);
    cJSON_AddBoolToObject(shot, "automatic", false);
    cJSON_AddBoolToObject(shot, "included", jpeg_len > 0);
    cJSON_AddNumberToObject(shot, "bytes", jpeg_len);
    if (screenshot_error[0]) {
        cJSON_AddStringToObject(shot, "error", screenshot_error);
    }
    agent_web_search_settings_t web_search_state;
    agent_web_search_defaults(&web_search_state);
    (void)agent_web_search_load(&web_search_state);
    bool web_search_available = agent_web_search_available(&web_search_state);
    cJSON *web = cJSON_AddObjectToObject(resp, "web_search");
    cJSON_AddBoolToObject(web, "requested", allow_web_search);
    cJSON_AddBoolToObject(web, "settings_enabled", web_search_state.enabled);
    cJSON_AddBoolToObject(web, "available", web_search_available);
    cJSON_AddBoolToObject(web, "effective",
                          allow_web_search && web_search_state.enabled &&
                          web_search_available);
    cJSON_AddStringToObject(web, "profile", web_search_state.profile);
    cJSON_AddStringToObject(web, "model", web_search_state.model);

    if (!model_json) {
        model_json = agent_run_repair_model_response(
            job_id, profile.provider, selected_model, session_id, message,
            agent_run_page_context_allowed() && page_context_json ?
            page_context_json : "",
            dry_run,
            allow_web_search, &route, url, api_key, &api_root, &content, resp);
    }

    if (!model_json) {
        memset(api_key, 0, sizeof(api_key));
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "model did not return JSON");
        cJSON_AddStringToObject(resp, "message", content ? content : "");
        cJSON_AddItemToObject(resp, "trace", cJSON_CreateArray());
        cJSON_AddItemToObject(resp, "actions", cJSON_CreateArray());
        cJSON_AddItemToObject(resp, "tool_calls", cJSON_CreateArray());
        cJSON_AddItemToObject(resp, "tool_results", cJSON_CreateArray());
        cJSON_AddItemToObject(resp, "results", cJSON_CreateArray());
        cJSON_Delete(api_root);
        *out_resp = resp;
        return ESP_OK;
    }

    const char *model_message = json_string_any(model_json, "message", "summary", "reply");
    char *latest_message = agent_alloc(AGENT_HISTORY_CONTENT_STORE_MAX + 1);
    if (!latest_message) {
        memset(api_key, 0, sizeof(api_key));
        cJSON_Delete(resp);
        cJSON_Delete(model_json);
        cJSON_Delete(api_root);
        strlcpy(err, "agent message buffer alloc failed", err_size);
        return ESP_ERR_NO_MEM;
    }
    latest_message[0] = '\0';
    if (model_message && model_message[0]) {
        strlcpy(latest_message, model_message,
                AGENT_HISTORY_CONTENT_STORE_MAX + 1);
    }
    cJSON *model_trace = cJSON_GetObjectItemCaseSensitive(model_json, "trace");
    if (!cJSON_IsArray(model_trace)) {
        model_trace = cJSON_GetObjectItemCaseSensitive(model_json, "plan");
    }
    if (!cJSON_IsArray(model_trace)) {
        model_trace = cJSON_GetObjectItemCaseSensitive(model_json, "progress");
    }
    cJSON *actions = cJSON_GetObjectItemCaseSensitive(model_json, "actions");
    if (!cJSON_IsArray(actions)) {
        actions = cJSON_GetObjectItemCaseSensitive(model_json, "steps");
    }
    cJSON *tool_calls = cJSON_CreateArray();
    cJSON *hid_actions = cJSON_CreateArray();
    if (!tool_calls || !hid_actions) {
        memset(api_key, 0, sizeof(api_key));
        cJSON_Delete(tool_calls);
        cJSON_Delete(hid_actions);
        cJSON_Delete(resp);
        cJSON_Delete(model_json);
        cJSON_Delete(api_root);
        free(latest_message);
        strlcpy(err, "agent tool array alloc failed", err_size);
        return ESP_ERR_NO_MEM;
    }
    agent_split_model_actions(model_json, actions, tool_calls, hid_actions);
    cJSON_AddStringToObject(resp, "message", latest_message);
    cJSON_AddStringToObject(resp, "initial_message", model_message ? model_message : "");
    cJSON *trace_out = cJSON_AddArrayToObject(resp, "trace");
    cJSON *tool_calls_out = cJSON_AddArrayToObject(resp, "tool_calls");
    cJSON *hid_actions_out = cJSON_AddArrayToObject(resp, "actions");
    if (!trace_out || !tool_calls_out || !hid_actions_out) {
        memset(api_key, 0, sizeof(api_key));
        cJSON_Delete(tool_calls);
        cJSON_Delete(hid_actions);
        cJSON_Delete(resp);
        cJSON_Delete(model_json);
        cJSON_Delete(api_root);
        free(latest_message);
        strlcpy(err, "agent response array alloc failed", err_size);
        return ESP_ERR_NO_MEM;
    }
    agent_array_extend_duplicate(trace_out, model_trace);
    agent_array_extend_duplicate(tool_calls_out, tool_calls);
    agent_array_extend_duplicate(hid_actions_out, hid_actions);
    cJSON *tool_results = cJSON_AddArrayToObject(resp, "tool_results");
    cJSON *results = cJSON_AddArrayToObject(resp, "results");
    if (!tool_results || !results) {
        memset(api_key, 0, sizeof(api_key));
        cJSON_Delete(tool_calls);
        cJSON_Delete(hid_actions);
        cJSON_Delete(resp);
        cJSON_Delete(model_json);
        cJSON_Delete(api_root);
        free(latest_message);
        strlcpy(err, "agent result array alloc failed", err_size);
        return ESP_ERR_NO_MEM;
    }
    agent_run_loop_context_t loop = {
        .job_id = job_id,
        .session_id = session_id,
        .message = message,
        .provider = profile.provider,
        .selected_model = selected_model,
        .url = url,
        .api_key = api_key,
        .route = &route,
        .dry_run = dry_run,
        .allow_screenshot = include_screenshot,
        .allow_web_search = allow_web_search,
        .resp = resp,
        .trace_out = trace_out,
        .tool_calls_out = tool_calls_out,
        .hid_actions_out = hid_actions_out,
        .tool_results = tool_results,
        .results = results,
        .latest_message = latest_message,
        .latest_message_size = AGENT_HISTORY_CONTENT_STORE_MAX + 1,
        .preserve_assistant_message =
            model_capabilities.preserve_assistant_message,
    };
    agent_run_preserve_assistant_message(&loop, api_root);
    if (agent_run_control_point(job_id)) {
        agent_run_execute_tool_loop(&loop, tool_calls, hid_actions);
    } else {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "run aborted");
        loop.last_batch_ok = false;
        loop.had_tool_failure = true;
    }
    cJSON_Delete(loop.previous_assistant_message);
    loop.previous_assistant_message = NULL;
    memset(api_key, 0, sizeof(api_key));

    int tool_count = cJSON_GetArraySize(tool_calls_out);
    int hid_count = cJSON_GetArraySize(hid_actions_out);
    if ((tool_count > 0 || hid_count > 0) &&
        agent_run_control_point(job_id)) {
        (void)agent_run_generate_final_report(job_id, url, &profile, selected_model,
                                              message, latest_message, resp);
    }
    agent_run_set_stage(job_id, "完成处理", "准备持久化公开的当前 turn 结果");
    cJSON_AddBoolToObject(resp, "memory_saved", false);
    cJSON_AddStringToObject(resp, "memory_status", "ESP_ERR_NOT_SUPPORTED");

    cJSON_Delete(tool_calls);
    cJSON_Delete(hid_actions);
    cJSON_Delete(model_json);
    cJSON_Delete(api_root);
    free(latest_message);
    *out_resp = resp;
    return ESP_OK;
}
