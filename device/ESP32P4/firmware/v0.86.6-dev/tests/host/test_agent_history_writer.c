#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SI_AGENT_HISTORY_WRITER_HOST_TEST 1
#include "../../main/infrastructure/agent_history_writer.c"

#define TEST_RECORD_CAPACITY 12U

typedef struct {
    char record_id[HISTORY_WRITER_RECORD_ID_MAX + 1U];
    char role[16];
    char json[HISTORY_WRITER_RECORD_MAX + 1U];
} test_record_t;

typedef struct {
    test_record_t records[TEST_RECORD_CAPACITY];
    size_t count;
    unsigned scan_calls;
    unsigned append_calls;
    unsigned fail_append_call;
    bool fail_after_commit;
    esp_err_t scan_status;
} test_store_t;

void agent_session_normalize(const char *session_id, char *out,
                             size_t out_size)
{
    snprintf(out, out_size, "%s",
             session_id && session_id[0] ? session_id : "default");
}

esp_err_t agent_history_file_for_each_locked(
    agent_history_file_record_cb_t cb, void *user_ctx,
    uint32_t *record_count_out)
{
    (void)cb;
    (void)user_ctx;
    (void)record_count_out;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t agent_history_file_append_locked(
    const char *json, size_t json_len, size_t *used_bytes_out,
    uint32_t *record_count_out)
{
    (void)json;
    (void)json_len;
    (void)used_bytes_out;
    (void)record_count_out;
    return ESP_ERR_NOT_SUPPORTED;
}

static bool has_record(const test_store_t *store, const char *record_id)
{
    for (size_t i = 0; i < store->count; ++i) {
        if (strcmp(store->records[i].record_id, record_id) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t scan_records(const char *user_record_id,
                              const char *assistant_record_id,
                              bool *user_found, bool *assistant_found,
                              void *context)
{
    test_store_t *store = (test_store_t *)context;
    assert(store != NULL);
    store->scan_calls++;
    if (store->scan_status != ESP_OK) {
        return store->scan_status;
    }
    *user_found = has_record(store, user_record_id);
    *assistant_found = has_record(store, assistant_record_id);
    return ESP_OK;
}

static esp_err_t append_record(const char *record_id, const char *role,
                               const char *json, size_t json_len,
                               void *context)
{
    test_store_t *store = (test_store_t *)context;
    assert(store != NULL);
    assert(record_id != NULL);
    assert(role != NULL);
    assert(json != NULL);
    assert(json_len == strlen(json));
    store->append_calls++;
    bool fail = store->fail_append_call == store->append_calls;
    if (fail && !store->fail_after_commit) {
        return ESP_FAIL;
    }
    assert(store->count < TEST_RECORD_CAPACITY);
    test_record_t *record = &store->records[store->count++];
    snprintf(record->record_id, sizeof(record->record_id), "%s", record_id);
    snprintf(record->role, sizeof(record->role), "%s", role);
    snprintf(record->json, sizeof(record->json), "%s", json);
    return fail ? ESP_FAIL : ESP_OK;
}

static si_agent_history_turn_write_t make_turn(bool enabled)
{
    si_agent_history_turn_write_t turn = {
        .conversation_history_enabled = enabled,
        .session_id = "thread-1",
        .turn_id = "turn-1",
        .run_id = "run-1",
        .user_content = "hello",
        .assistant_content = "world",
        .profile = "chat-light",
        .model = "deepseek-chat",
        .assistant_kind = "message",
        .ok = true,
        .dry_run = false,
        .actions_count = 2,
        .executed_count = 2,
        .stored_ms = 1234U,
    };
    return turn;
}

static void install_store(test_store_t *store)
{
    si_agent_history_writer_host_reset();
    si_agent_history_writer_host_set_hooks(scan_records, append_record, store);
}

static void assert_record_json(const test_record_t *record,
                               const char *expected_role,
                               const char *expected_content)
{
    cJSON *root = cJSON_Parse(record->json);
    assert(root != NULL);
    const cJSON *record_id =
        cJSON_GetObjectItemCaseSensitive(root, "record_id");
    const cJSON *thread_id =
        cJSON_GetObjectItemCaseSensitive(root, "thread_id");
    const cJSON *turn_id = cJSON_GetObjectItemCaseSensitive(root, "turn_id");
    const cJSON *run_id = cJSON_GetObjectItemCaseSensitive(root, "run_id");
    const cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
    assert(cJSON_IsString(record_id));
    assert(strcmp(record_id->valuestring, record->record_id) == 0);
    assert(cJSON_IsString(thread_id));
    assert(strcmp(thread_id->valuestring, "thread-1") == 0);
    assert(cJSON_IsString(turn_id));
    assert(strcmp(turn_id->valuestring, "turn-1") == 0);
    assert(cJSON_IsString(run_id));
    assert(strcmp(run_id->valuestring, "run-1") == 0);
    assert(cJSON_IsString(role));
    assert(strcmp(role->valuestring, expected_role) == 0);
    assert(cJSON_IsString(content));
    assert(strcmp(content->valuestring, expected_content) == 0);
    cJSON_Delete(root);
}

static void test_order_and_idempotency(void)
{
    test_store_t store = {0};
    install_store(&store);
    assert(si_agent_history_writer_start() == ESP_OK);
    assert(si_agent_history_writer_start() == ESP_OK);

    si_agent_history_turn_write_t turn = make_turn(true);
    si_agent_history_write_result_t result = {0};
    assert(si_agent_history_writer_store_turn(
               &turn, SI_AGENT_HISTORY_WRITER_DEFAULT_WAIT_MS, &result) ==
           ESP_OK);
    assert(result.saved);
    assert(!result.pending);
    assert(result.user_status == ESP_OK);
    assert(result.assistant_status == ESP_OK);
    assert(store.count == 2U);
    assert(strcmp(store.records[0].record_id, "run-1/turn-1/user") == 0);
    assert(strcmp(store.records[0].role, "user") == 0);
    assert(strcmp(store.records[1].record_id,
                  "run-1/turn-1/assistant") == 0);
    assert(strcmp(store.records[1].role, "agent") == 0);
    assert_record_json(&store.records[0], "user", "hello");
    assert_record_json(&store.records[1], "agent", "world");

    memset(&result, 0, sizeof(result));
    assert(si_agent_history_writer_store_turn(&turn, 1U, &result) == ESP_OK);
    assert(result.saved);
    assert(result.user_duplicate);
    assert(result.assistant_duplicate);
    assert(store.count == 2U);
}

static void test_partial_failure_is_recoverable(void)
{
    test_store_t store = {.fail_append_call = 2U};
    install_store(&store);
    si_agent_history_turn_write_t turn = make_turn(true);
    si_agent_history_write_result_t result = {0};
    assert(si_agent_history_writer_store_turn(&turn, 10U, &result) ==
           ESP_FAIL);
    assert(!result.saved);
    assert(result.user_status == ESP_OK);
    assert(result.assistant_status == ESP_FAIL);
    assert(store.count == 1U);
    assert(strcmp(store.records[0].record_id, "run-1/turn-1/user") == 0);

    store.fail_append_call = 0U;
    memset(&result, 0, sizeof(result));
    assert(si_agent_history_writer_store_turn(&turn, 10U, &result) == ESP_OK);
    assert(result.saved);
    assert(result.user_duplicate);
    assert(!result.assistant_duplicate);
    assert(store.count == 2U);
    assert(strcmp(store.records[1].record_id,
                  "run-1/turn-1/assistant") == 0);
}

static void test_ambiguous_commit_is_deduplicated(void)
{
    test_store_t store = {
        .fail_append_call = 2U,
        .fail_after_commit = true,
    };
    install_store(&store);
    si_agent_history_turn_write_t turn = make_turn(true);
    si_agent_history_write_result_t result = {0};
    assert(si_agent_history_writer_store_turn(&turn, 10U, &result) == ESP_OK);
    assert(result.saved);
    assert(result.assistant_duplicate);
    assert(store.count == 2U);

    test_store_t user_store = {
        .fail_append_call = 1U,
        .fail_after_commit = true,
    };
    install_store(&user_store);
    memset(&result, 0, sizeof(result));
    assert(si_agent_history_writer_store_turn(&turn, 10U, &result) == ESP_OK);
    assert(result.saved);
    assert(result.user_duplicate);
    assert(user_store.count == 2U);
    assert(strcmp(user_store.records[0].record_id,
                  "run-1/turn-1/user") == 0);
    assert(strcmp(user_store.records[1].record_id,
                  "run-1/turn-1/assistant") == 0);
}

static void test_corrupt_order_fails_closed(void)
{
    test_store_t store = {0};
    snprintf(store.records[0].record_id, sizeof(store.records[0].record_id),
             "run-1/turn-1/assistant");
    snprintf(store.records[0].role, sizeof(store.records[0].role), "agent");
    store.count = 1U;
    install_store(&store);
    si_agent_history_turn_write_t turn = make_turn(true);
    si_agent_history_write_result_t result = {0};
    assert(si_agent_history_writer_store_turn(&turn, 10U, &result) ==
           ESP_ERR_INVALID_STATE);
    assert(!result.saved);
    assert(result.user_status == ESP_ERR_INVALID_STATE);
    assert(result.assistant_status == ESP_ERR_INVALID_STATE);
    assert(strstr(result.error, "assistant record exists before user") != NULL);
    assert(store.append_calls == 0U);
}

static void test_feature_off_queue_full_and_storage_failure(void)
{
    test_store_t store = {0};
    install_store(&store);
    si_agent_history_turn_write_t turn = make_turn(false);
    si_agent_history_write_result_t result = {0};
    assert(si_agent_history_writer_store_turn(&turn, 10U, &result) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(!result.saved);
    assert(result.user_status == ESP_ERR_NOT_SUPPORTED);
    assert(result.assistant_status == ESP_ERR_NOT_SUPPORTED);
    assert(store.scan_calls == 0U);
    assert(store.append_calls == 0U);

    turn.conversation_history_enabled = true;
    si_agent_history_writer_host_force_queue_full(true);
    assert(si_agent_history_writer_pending_count() ==
           SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY);
    memset(&result, 0, sizeof(result));
    assert(si_agent_history_writer_store_turn(&turn, 10U, &result) ==
           ESP_ERR_TIMEOUT);
    assert(!result.saved);
    assert(result.user_status == ESP_ERR_TIMEOUT);
    assert(result.assistant_status == ESP_ERR_TIMEOUT);
    assert(strstr(result.error, "queue full") != NULL);
    assert(store.scan_calls == 0U);
    si_agent_history_writer_host_force_queue_full(false);

    si_agent_history_writer_host_force_pending(true);
    memset(&result, 0, sizeof(result));
    assert(si_agent_history_writer_store_turn(&turn, 10U, &result) ==
           ESP_ERR_TIMEOUT);
    assert(!result.saved);
    assert(result.pending);
    assert(result.user_status == ESP_ERR_TIMEOUT);
    assert(result.assistant_status == ESP_ERR_TIMEOUT);
    assert(strstr(result.error, "completion timeout") != NULL);
    assert(store.scan_calls == 0U);
    assert(store.append_calls == 0U);
    si_agent_history_writer_host_force_pending(false);

    store.scan_status = ESP_ERR_NOT_FOUND;
    memset(&result, 0, sizeof(result));
    assert(si_agent_history_writer_store_turn(&turn, 10U, &result) ==
           ESP_ERR_NOT_FOUND);
    assert(!result.saved);
    assert(result.user_status == ESP_ERR_NOT_FOUND);
    assert(result.assistant_status == ESP_ERR_NOT_FOUND);
    assert(store.append_calls == 0U);
}

static void test_invalid_requests_do_not_touch_storage(void)
{
    test_store_t store = {0};
    install_store(&store);
    si_agent_history_turn_write_t turn = make_turn(true);
    si_agent_history_write_result_t result = {0};
    assert(si_agent_history_writer_store_turn(&turn, 0U, &result) ==
           ESP_ERR_INVALID_ARG);
    turn.turn_id = "bad/id";
    assert(si_agent_history_writer_store_turn(&turn, 10U, &result) ==
           ESP_ERR_INVALID_ARG);
    assert(store.scan_calls == 0U);
    assert(store.append_calls == 0U);
}

int main(void)
{
    test_order_and_idempotency();
    test_partial_failure_is_recoverable();
    test_ambiguous_commit_is_deduplicated();
    test_corrupt_order_fails_closed();
    test_feature_off_queue_full_and_storage_failure();
    test_invalid_requests_do_not_touch_storage();
    puts("agent history writer tests: PASS");
    return 0;
}
