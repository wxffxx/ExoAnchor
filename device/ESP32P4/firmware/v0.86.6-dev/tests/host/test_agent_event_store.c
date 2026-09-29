#include "agent_event_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    unsigned count;
    uint64_t expected_seq;
} replay_state_t;

static esp_err_t replay_event(const si_agent_event_record_t *record,
                              void *user_ctx)
{
    replay_state_t *state = (replay_state_t *)user_ctx;
    assert(record != NULL);
    assert(state != NULL);
    assert(record->seq == state->expected_seq++);
    assert(strcmp(record->thread_id, "thread-test") == 0);
    assert(strcmp(record->turn_id, "turn-test") == 0);
    state->count++;
    return ESP_OK;
}

static void append(const char *event_type, unsigned value)
{
    cJSON *payload = cJSON_CreateObject();
    assert(payload != NULL);
    assert(cJSON_AddNumberToObject(payload, "value", value) != NULL);
    si_agent_event_append_t input = {
        .thread_id = "thread-test",
        .turn_id = "turn-test",
        .run_id = "run-test",
        .event_type = event_type,
        .server_time_ms = 0,
        .monotonic_time_ms = value,
        .payload = payload,
    };
    si_agent_event_record_t stored = {0};
    assert(si_agent_event_store_append(&input, &stored) == ESP_OK);
    assert(stored.seq == value);
    assert(strlen(stored.event_hash) == SI_AGENT_EVENT_HASH_HEX_LEN);
    cJSON_Delete(payload);
}

int main(void)
{
    (void)remove(si_agent_event_store_path());
    (void)rmdir(SI_AGENT_EVENT_STORE_DIR);

    si_agent_event_recovery_t recovery = {0};
    assert(si_agent_event_store_init(&recovery) == ESP_OK);
    assert(recovery.status == SI_AGENT_EVENT_RECOVERY_EMPTY);

    append("turn.submitted", 1);
    append("turn.phase_changed", 2);

    uint64_t latest = 0;
    char hash[SI_AGENT_EVENT_HASH_HEX_LEN + 1U] = {0};
    assert(si_agent_event_store_latest(&latest, hash) == ESP_OK);
    assert(latest == 2);
    assert(strlen(hash) == SI_AGENT_EVENT_HASH_HEX_LEN);

    /* A long replay may own the store mutex, but another caller receives a
     * bounded timeout instead of waiting forever behind the scan. */
    assert(si_agent_event_store_host_hold_lock());
    assert(si_agent_event_store_latest(&latest, hash) == ESP_ERR_TIMEOUT);
    assert(strstr(si_agent_event_store_last_error(), "mutex timeout") != NULL);
    si_agent_event_store_host_release_lock();

    replay_state_t replay = {.expected_seq = 2};
    assert(si_agent_event_store_replay(1, replay_event, &replay,
                                       &recovery) == ESP_OK);
    assert(replay.count == 1);

    cJSON *unsafe = cJSON_CreateObject();
    assert(unsafe != NULL);
    assert(cJSON_AddStringToObject(unsafe, "api_key", "must-not-persist") !=
           NULL);
    si_agent_event_append_t rejected = {
        .thread_id = "thread-test",
        .turn_id = "turn-test",
        .run_id = "run-test",
        .event_type = "unsafe",
        .payload = unsafe,
    };
    assert(si_agent_event_store_append(&rejected, NULL) ==
           ESP_ERR_INVALID_ARG);
    cJSON_Delete(unsafe);

    struct stat status;
    assert(stat(si_agent_event_store_path(), &status) == 0);
    assert(status.st_size > 1);
    assert(truncate(si_agent_event_store_path(), status.st_size - 1) == 0);
    assert(si_agent_event_store_recover(NULL, NULL, &recovery) ==
           ESP_ERR_INVALID_STATE);
    assert(recovery.status == SI_AGENT_EVENT_RECOVERY_TAIL_TRUNCATED);

    assert(remove(si_agent_event_store_path()) == 0);
    assert(rmdir(SI_AGENT_EVENT_STORE_DIR) == 0);
    puts("agent event store tests: PASS");
    return 0;
}
