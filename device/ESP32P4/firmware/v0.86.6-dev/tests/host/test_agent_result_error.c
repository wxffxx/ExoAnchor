#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "agent_result_error.h"

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    default:
        return "ESP_FAIL";
    }
}

static cJSON *parse(const char *text)
{
    cJSON *result = cJSON_Parse(text);
    assert(result != NULL);
    return result;
}

static void expect_error(esp_err_t execute_ret, const char *json,
                         const char *expected)
{
    cJSON *result = parse(json);
    char error[64] = {0};
    si_agent_result_failure_error(execute_ret, result, error, sizeof(error));
    assert(strcmp(error, expected) == 0);
    cJSON_Delete(result);
}

int main(void)
{
    expect_error(ESP_OK,
                 "{\"error\":\"top-level failure\","
                 "\"loop_error\":\"later failure\"}",
                 "top-level failure");
    expect_error(ESP_OK, "{\"error\":\"\","
                         "\"loop_error\":\"loop failed\"}",
                 "loop failed");
    expect_error(
        ESP_OK,
        "{\"tool_results\":[{\"ok\":true,\"error\":\"ignore\"},"
        "{\"ok\":false,\"error\":\"wait ms out of range\"}],"
        "\"had_tool_failure\":true}",
        "wait ms out of range");
    expect_error(
        ESP_OK,
        "{\"error\":\"ESP_OK\",\"tool_results\":[{\"ok\":false,"
        "\"error\":\"bounded tool failure\"}]}",
        "bounded tool failure");
    expect_error(
        ESP_OK,
        "{\"tool_results\":[{\"ok\":false,\"message\":\"tool failed\"}]}",
        "tool failed");
    expect_error(
        ESP_OK,
        "{\"tool_results\":[{\"ok\":true}],"
        "\"results\":[{\"ok\":false,\"error\":\"HID denied\"}]}",
        "HID denied");
    expect_error(ESP_OK, "{\"had_tool_failure\":true}",
                 "tool execution failed");
    expect_error(ESP_ERR_TIMEOUT, "{\"ok\":false}",
                 "ESP_ERR_TIMEOUT");
    expect_error(ESP_ERR_TIMEOUT,
                 "{\"ok\":false,\"error\":\"ESP_OK\"}",
                 "ESP_ERR_TIMEOUT");
    expect_error(ESP_OK, "{\"ok\":false}",
                 "agent execution failed");

    cJSON *result = parse("{\"error\":\"123456789\"}");
    char truncated[5] = {0};
    si_agent_result_failure_error(ESP_OK, result, truncated,
                                  sizeof(truncated));
    assert(strcmp(truncated, "1234") == 0);
    cJSON_Delete(result);

    puts("agent result error tests: PASS");
    return 0;
}
