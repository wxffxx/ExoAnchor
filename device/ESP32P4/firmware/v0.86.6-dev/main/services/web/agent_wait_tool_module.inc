// Transitional bounded wait runtime tool.
static bool agent_execute_wait_tool(const char *job_id, const cJSON *call,
                                    int index, bool dry_run, cJSON *results,
                                    bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const cJSON *duration = json_number_any(args, "ms", "duration_ms");
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddNumberToObject(item, "index", index);
    cJSON_AddStringToObject(item, "tool", "wait");
    cJSON_AddBoolToObject(item, "dry_run", dry_run);
    if (!cJSON_IsNumber(duration) ||
        duration->valuedouble != (double)duration->valueint ||
        duration->valueint < 1 || duration->valueint > 30000) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(
            item, "error",
            "wait ms is required and must be an integer from 1 to 30000");
        cJSON_AddItemToArray(results, item);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    uint32_t requested_ms = (uint32_t)duration->valueint;
    cJSON_AddNumberToObject(item, "requested_ms", requested_ms);
    if (dry_run) {
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddNumberToObject(item, "waited_ms", 0);
        cJSON_AddStringToObject(item, "status", "dry-run");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "cmd",
                             "tool[%d] wait planned · %" PRIu32 " ms",
                             index + 1, requested_ms);
        return false;
    }

    uint32_t remaining_ms = requested_ms;
    while (remaining_ms > 0) {
        uint32_t slice_ms = MIN(remaining_ms, 50U);
        vTaskDelay(pdMS_TO_TICKS(slice_ms));
        remaining_ms -= slice_ms;
        if (!agent_run_control_point(job_id)) {
            cJSON_AddBoolToObject(item, "ok", false);
            cJSON_AddNumberToObject(
                item, "waited_ms", requested_ms - remaining_ms);
            cJSON_AddStringToObject(item, "error", "run interrupted");
            cJSON_AddItemToArray(results, item);
            if (all_ok) {
                *all_ok = false;
            }
            return false;
        }
    }
    cJSON_AddBoolToObject(item, "ok", true);
    cJSON_AddNumberToObject(item, "waited_ms", requested_ms);
    cJSON_AddItemToArray(results, item);
    agent_run_add_eventf(job_id, "ok",
                         "tool[%d] wait completed · %" PRIu32 " ms",
                         index + 1, requested_ms);
    return true;
}
