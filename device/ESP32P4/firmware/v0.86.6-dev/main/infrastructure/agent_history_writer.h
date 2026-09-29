#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Automatic chat persistence is a bounded RPC to one TF-card writer task.
 * The producer never opens, scans, appends, flushes, or fsyncs HIST.LOG. */
#define SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY 4U
#define SI_AGENT_HISTORY_WRITER_DEFAULT_WAIT_MS 4000U
#define SI_AGENT_HISTORY_WRITER_CONTENT_MAX 1800U
#define SI_AGENT_HISTORY_WRITER_ERROR_MAX 128U

typedef struct {
    /* Read the product feature on the caller side. The writer intentionally
     * has no Settings/NVS dependency because it is allowed to touch TF only. */
    bool conversation_history_enabled;
    const char *session_id;
    const char *turn_id;
    const char *run_id;
    const char *user_content;
    const char *assistant_content;
    const char *profile;
    const char *model;
    const char *assistant_kind;
    bool ok;
    bool dry_run;
    int actions_count;
    int executed_count;
    uint32_t stored_ms;
} si_agent_history_turn_write_t;

typedef struct {
    bool saved;
    /* True means the caller's bounded wait expired after the writer had
     * accepted ownership. The writer will still finish safely in background. */
    bool pending;
    bool user_duplicate;
    bool assistant_duplicate;
    esp_err_t user_status;
    esp_err_t assistant_status;
    char error[SI_AGENT_HISTORY_WRITER_ERROR_MAX];
} si_agent_history_write_result_t;

esp_err_t si_agent_history_writer_start(void);

/* Enqueue one ordered user -> assistant pair and wait only for the bounded
 * writer result. A true result.saved means both records are durably present.
 * On a wait timeout result.pending is true; the slot lifetime remains owned by
 * the writer and no notification targets caller-owned memory or task state. */
esp_err_t si_agent_history_writer_store_turn(
    const si_agent_history_turn_write_t *turn,
    uint32_t wait_ms,
    si_agent_history_write_result_t *result);

/* Diagnostics only. This is RAM state and performs no storage/settings I/O. */
size_t si_agent_history_writer_pending_count(void);

#ifdef SI_AGENT_HISTORY_WRITER_HOST_TEST
typedef esp_err_t (*si_agent_history_writer_host_scan_fn)(
    const char *user_record_id, const char *assistant_record_id,
    bool *user_found, bool *assistant_found, void *ctx);
typedef esp_err_t (*si_agent_history_writer_host_append_fn)(
    const char *record_id, const char *role, const char *json,
    size_t json_len, void *ctx);

void si_agent_history_writer_host_reset(void);
void si_agent_history_writer_host_set_hooks(
    si_agent_history_writer_host_scan_fn scan_fn,
    si_agent_history_writer_host_append_fn append_fn,
    void *ctx);
void si_agent_history_writer_host_force_queue_full(bool full);
void si_agent_history_writer_host_force_pending(bool pending);
#endif
