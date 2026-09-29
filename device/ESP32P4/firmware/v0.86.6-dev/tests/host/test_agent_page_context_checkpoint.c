#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_page_context_checkpoint.h"
#include "cJSON.h"

static cJSON *sanitize(const char *input, bool *changed)
{
    char *output = NULL;
    assert(si_agent_page_context_strip_checkpoint(input, &output, changed) ==
           ESP_OK);
    assert(output != NULL);
    cJSON *root = cJSON_Parse(output);
    free(output);
    assert(cJSON_IsObject(root));
    return root;
}

static void test_context_is_removed_without_losing_run_recovery(void)
{
    const char *input =
        "{\"schema_version\":1,\"job_id\":\"r7\",\"turn_id\":\"t7\","
        "\"session_id\":\"default\",\"message\":\"keep the ordinary goal\","
        "\"page_context_json\":\"{\\\"page\\\":\\\"settings\\\"}\","
        "\"profile\":\"deepseek\",\"dry_run\":false,"
        "\"plan\":{\"version\":3}}";
    bool changed = false;
    cJSON *root = sanitize(input, &changed);
    assert(changed);
    cJSON *context =
        cJSON_GetObjectItemCaseSensitive(root, "page_context_json");
    cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    cJSON *job_id = cJSON_GetObjectItemCaseSensitive(root, "job_id");
    cJSON *plan = cJSON_GetObjectItemCaseSensitive(root, "plan");
    assert(cJSON_IsString(context) && strcmp(context->valuestring, "") == 0);
    assert(cJSON_IsString(message) &&
           strcmp(message->valuestring, "keep the ordinary goal") == 0);
    assert(cJSON_IsString(job_id) && strcmp(job_id->valuestring, "r7") == 0);
    assert(cJSON_IsObject(plan));
    cJSON_Delete(root);
}

static void test_legacy_context_object_is_removed(void)
{
    bool changed = false;
    cJSON *root = sanitize(
        "{\"message\":\"keep\",\"page_context\":{\"page\":\"kvm\"}}",
        &changed);
    assert(changed);
    assert(cJSON_GetObjectItemCaseSensitive(root, "page_context") == NULL);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(root, "message")->valuestring,
                  "keep") == 0);
    cJSON_Delete(root);
}

static void test_empty_or_absent_context_is_idempotent(void)
{
    bool changed = true;
    cJSON *root = sanitize(
        "{\"message\":\"keep\",\"page_context_json\":\"\"}", &changed);
    assert(!changed);
    cJSON_Delete(root);

    changed = true;
    root = sanitize("{\"message\":\"keep\"}", &changed);
    assert(!changed);
    cJSON_Delete(root);
}

static void test_invalid_checkpoint_is_rejected(void)
{
    char *output = (char *)0x1;
    bool changed = true;
    assert(si_agent_page_context_strip_checkpoint("not-json", &output,
                                                  &changed) ==
           ESP_ERR_INVALID_ARG);
    assert(output == NULL);
    assert(!changed);
}

int main(void)
{
    test_context_is_removed_without_losing_run_recovery();
    test_legacy_context_object_is_removed();
    test_empty_or_absent_context_is_idempotent();
    test_invalid_checkpoint_is_rejected();
    puts("agent page-context checkpoint tests: OK");
    return 0;
}
