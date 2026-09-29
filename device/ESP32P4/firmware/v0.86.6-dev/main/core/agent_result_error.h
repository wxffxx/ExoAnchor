#pragma once

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"

static inline const char *si_agent_result_nonempty_string(
    const cJSON *object, const char *name)
{
    const cJSON *item = object && name ?
        cJSON_GetObjectItemCaseSensitive(object, name) : NULL;
    return cJSON_IsString(item) && item->valuestring && item->valuestring[0] ?
        item->valuestring : NULL;
}

static inline const char *si_agent_result_first_failed_item_error(
    const cJSON *result, const char *array_name)
{
    const cJSON *items = result && array_name ?
        cJSON_GetObjectItemCaseSensitive(result, array_name) : NULL;
    if (!cJSON_IsArray(items)) {
        return NULL;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        const cJSON *ok = cJSON_GetObjectItemCaseSensitive(item, "ok");
        if (!cJSON_IsFalse(ok)) {
            continue;
        }
        const char *detail = si_agent_result_nonempty_string(item, "error");
        if (!detail) {
            detail = si_agent_result_nonempty_string(item, "message");
        }
        if (detail) {
            return detail;
        }
    }
    return NULL;
}

/* Convert a structurally failed Agent result into one bounded, user-facing
 * error. ESP_OK is deliberately never rendered as a failure reason: tool
 * failures are represented inside the result JSON, independently of the
 * transport/parser return code. */
static inline void si_agent_result_failure_error(
    esp_err_t execute_ret, const cJSON *result, char *out, size_t out_size)
{
    if (!out || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    const char *detail = si_agent_result_nonempty_string(result, "error");
    if (detail && strcmp(detail, "ESP_OK") == 0) {
        detail = NULL;
    }
    if (!detail) {
        detail = si_agent_result_nonempty_string(result, "loop_error");
    }
    if (!detail) {
        detail = si_agent_result_first_failed_item_error(
            result, "tool_results");
    }
    if (!detail) {
        detail = si_agent_result_first_failed_item_error(result, "results");
    }
    if (!detail && result && cJSON_IsTrue(
            cJSON_GetObjectItemCaseSensitive(result, "had_tool_failure"))) {
        detail = "tool execution failed";
    }
    if (!detail && execute_ret != ESP_OK) {
        detail = esp_err_to_name(execute_ret);
    }
    if (!detail || !detail[0]) {
        detail = "agent execution failed";
    }
    (void)snprintf(out, out_size, "%s", detail);
}
