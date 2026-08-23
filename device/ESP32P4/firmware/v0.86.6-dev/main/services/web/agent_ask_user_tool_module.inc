// Bounded context request tool; all authority remains in Request Broker.
static bool agent_execute_ask_user_tool(const char *job_id,
                                        const cJSON *call, int index,
                                        bool dry_run, cJSON *results,
                                        bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const char *question =
        json_string_any(args, "question", "prompt", "message");
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddNumberToObject(item, "index", index);
    cJSON_AddStringToObject(item, "tool", "ask_user");
    cJSON_AddBoolToObject(item, "dry_run", dry_run);
    if (!question || !question[0] ||
        strlen(question) > SI_AGENT_REQUEST_REASON_MAX_LEN) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(
            item, "error",
            "ask_user question is required and must be at most 160 bytes");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddStringToObject(item, "question", question);
    if (dry_run) {
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddStringToObject(item, "status", "dry-run");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "cmd",
                             "tool[%d] ask_user planned", index + 1);
        return false;
    }

    char *params = cJSON_PrintUnformatted(call);
    char request_id[SI_AGENT_REQUEST_ID_MAX_LEN + 1] = {0};
    char answer[SI_AGENT_REQUEST_RESPONSE_MAX_LEN + 1] = {0};
    char error[96] = {0};
    bool granted = params &&
        agent_request_authorize_ex(
            job_id, "ask_user", question, params ? params : "{}",
            "Provide one bounded human answer to the current task step",
            "Return the answer only to the same Run",
            SI_AGENT_REQUEST_RISK_MEDIUM, false,
            SI_AGENT_REQUEST_KIND_CONTEXT,
            request_id, sizeof(request_id),
            answer, sizeof(answer), error, sizeof(error));
    free(params);
    if (!granted || !answer[0]) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(
            item, "error", error[0] ? error :
            "ask_user request completed without an answer");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddBoolToObject(item, "ok", true);
    cJSON_AddStringToObject(item, "answer", answer);
    cJSON_AddItemToArray(results, item);
    agent_request_finish(job_id, request_id, true, NULL);
    agent_run_add_eventf(job_id, "ok",
                         "tool[%d] ask_user answered", index + 1);
    return true;
}
