#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Agent task event journal v1.
 *
 * TASKS.LOG is an append-only, hash-chained journal.  It is deliberately
 * separate from conversational HIST.LOG: callers must only put bounded,
 * redacted task facts in payload.  Secrets, credentials, full screen/log
 * captures and model private reasoning are forbidden.
 */
#define SI_AGENT_EVENT_SCHEMA "exoanchor.agent.event.v1"
#define SI_AGENT_EVENT_HASH_HEX_LEN 64U
#define SI_AGENT_EVENT_ID_MAX_LEN 64U
#define SI_AGENT_EVENT_TYPE_MAX_LEN 48U
#define SI_AGENT_EVENT_PAYLOAD_MAX_BYTES 4096U
#define SI_AGENT_EVENT_RECORD_MAX_BYTES 8192U
#define SI_AGENT_EVENT_JOURNAL_MAX_BYTES (64U * 1024U * 1024U)

/* Stable v1 Item/Event names. Lifecycle-specific producers may add dotted
 * names that obey the same bounded grammar (for example, "turn.created"). */
#define SI_AGENT_EVENT_TYPE_USER_MESSAGE "user_message"
#define SI_AGENT_EVENT_TYPE_AGENT_COMMENTARY "agent_commentary"
#define SI_AGENT_EVENT_TYPE_PLAN_UPDATE "plan_update"
#define SI_AGENT_EVENT_TYPE_CONTEXT_REQUEST "context_request"
#define SI_AGENT_EVENT_TYPE_APPROVAL_DECISION "approval_decision"
#define SI_AGENT_EVENT_TYPE_TOOL_CALL "tool_call"
#define SI_AGENT_EVENT_TYPE_TOOL_RESULT "tool_result"
#define SI_AGENT_EVENT_TYPE_OBSERVATION "observation"
#define SI_AGENT_EVENT_TYPE_ARTIFACT "artifact"
#define SI_AGENT_EVENT_TYPE_VERIFICATION "verification"
#define SI_AGENT_EVENT_TYPE_ERROR "error"
#define SI_AGENT_EVENT_TYPE_FINAL_RESPONSE "final_response"
#define SI_AGENT_EVENT_TYPE_COMPACTION "compaction"

typedef enum {
    SI_AGENT_EVENT_RECOVERY_CLEAN = 0,
    SI_AGENT_EVENT_RECOVERY_EMPTY,
    SI_AGENT_EVENT_RECOVERY_TAIL_TRUNCATED,
    SI_AGENT_EVENT_RECOVERY_CORRUPT,
    SI_AGENT_EVENT_RECOVERY_IO_ERROR,
} si_agent_event_recovery_status_t;

/* Optional relationship IDs are represented by an empty string. */
typedef struct {
    uint64_t seq;
    char thread_id[SI_AGENT_EVENT_ID_MAX_LEN + 1U];
    char turn_id[SI_AGENT_EVENT_ID_MAX_LEN + 1U];
    char run_id[SI_AGENT_EVENT_ID_MAX_LEN + 1U];
    char step_id[SI_AGENT_EVENT_ID_MAX_LEN + 1U];
    char request_id[SI_AGENT_EVENT_ID_MAX_LEN + 1U];
    char action_id[SI_AGENT_EVENT_ID_MAX_LEN + 1U];
    char artifact_id[SI_AGENT_EVENT_ID_MAX_LEN + 1U];
    char event_type[SI_AGENT_EVENT_TYPE_MAX_LEN + 1U];
    int64_t server_time_ms;
    int64_t monotonic_time_ms;
    char payload_hash[SI_AGENT_EVENT_HASH_HEX_LEN + 1U];
    char previous_hash[SI_AGENT_EVENT_HASH_HEX_LEN + 1U];
    char event_hash[SI_AGENT_EVENT_HASH_HEX_LEN + 1U];
    const cJSON *payload; /* Borrowed: callback duration, or append input alias. */
} si_agent_event_record_t;

/*
 * Fields supplied by an event producer.  thread_id, turn_id, event_type and
 * payload are required.  Other IDs are optional and may be NULL/empty.
 * Times are supplied by the trusted service, not copied from a model.
 */
typedef struct {
    const char *thread_id;
    const char *turn_id;
    const char *run_id;
    const char *step_id;
    const char *request_id;
    const char *action_id;
    const char *artifact_id;
    const char *event_type;
    int64_t server_time_ms;
    int64_t monotonic_time_ms;
    const cJSON *payload;
} si_agent_event_append_t;

typedef esp_err_t (*si_agent_event_replay_cb_t)(
    const si_agent_event_record_t *record, void *user_ctx);

typedef struct {
    si_agent_event_recovery_status_t status;
    uint64_t latest_seq;
    char latest_hash[SI_AGENT_EVENT_HASH_HEX_LEN + 1U];
    uint64_t valid_record_count;
    uint64_t valid_bytes;
    uint64_t file_bytes;
    uint64_t fault_offset;
} si_agent_event_recovery_t;

/*
 * Creates the process mutex, verifies TASKS.LOG from disk and restores the
 * in-memory latest sequence/hash.  A corrupt or truncated tail is reported in
 * recovery_out and returned as ESP_ERR_INVALID_STATE; it is never ignored or
 * repaired automatically.  A missing/unmounted TF card fails closed.
 */
esp_err_t si_agent_event_store_init(si_agent_event_recovery_t *recovery_out);

/*
 * Re-reads and verifies the complete journal, restoring latest sequence/hash.
 * This is intended for boot recovery and explicit diagnostics, not concurrent
 * iteration.  Callback delivery stops on its first non-ESP_OK result. Replay
 * callbacks execute under the store mutex and must not call store APIs.
 */
esp_err_t si_agent_event_store_recover(si_agent_event_replay_cb_t callback,
                                       void *user_ctx,
                                       si_agent_event_recovery_t *recovery_out);

/*
 * Appends exactly one v1 envelope and fflushes it before returning ESP_OK.
 * Sequence and hash-chain fields are assigned by the store.  The append is
 * rejected when recovery has not completed cleanly or any limit is exceeded.
 */
esp_err_t si_agent_event_store_append(const si_agent_event_append_t *event,
                                      si_agent_event_record_t *stored_out);

/* Replays verified records with seq > after_seq, in journal order. The
 * callback is non-reentrant and must not call another event-store API. */
esp_err_t si_agent_event_store_replay(uint64_t after_seq,
                                      si_agent_event_replay_cb_t callback,
                                      void *user_ctx,
                                      si_agent_event_recovery_t *recovery_out);

/* Returns the last verified record cursor. Empty journals return seq 0/zero hash. */
esp_err_t si_agent_event_store_latest(uint64_t *seq_out,
                                      char hash_out[SI_AGENT_EVENT_HASH_HEX_LEN + 1U]);

const char *si_agent_event_store_path(void);
const char *si_agent_event_store_last_error(void);
const char *si_agent_event_recovery_status_name(si_agent_event_recovery_status_t status);

#ifdef SI_AGENT_EVENT_STORE_HOST_TEST
bool si_agent_event_store_host_hold_lock(void);
void si_agent_event_store_host_release_lock(void);
#endif

#ifdef __cplusplus
}
#endif
