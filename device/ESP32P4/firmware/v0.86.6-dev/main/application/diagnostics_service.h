#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "action_event.h"

#define SI_DIAGNOSTICS_AGENT_RUN_ID_MAX 48U
#define SI_DIAGNOSTICS_RESULT_DEFAULT_CHUNK 4096U
#define SI_DIAGNOSTICS_RESULT_MAX_CHUNK 5600U
#define SI_DIAGNOSTICS_RESULT_FRAMING_RESERVE 256U

typedef struct {
    char *out;
    size_t capacity;
    size_t length;
} si_diagnostics_text_builder_t;

typedef struct {
    const char *run_id;
    uint32_t after_seq;
    uint32_t available_latest_seq;
    uint32_t oldest_seq;
    uint32_t dropped;
    bool running;
    bool history_lost;
    const si_action_event_t *events;
    size_t event_count;
} si_diagnostics_agent_event_page_t;

typedef struct {
    size_t offset;
    size_t chunk_bytes;
    size_t next_offset;
    bool eof;
} si_diagnostics_result_chunk_plan_t;

typedef struct {
    uint32_t origin;
    uint32_t auth_kind;
    uint32_t principal;
    const char *auth_id;
    uint32_t auth_generation;
    uint32_t authority_ceiling;
} si_diagnostics_agent_source_identity_t;

bool si_diagnostics_run_id_valid(const char *run_id);
bool si_diagnostics_agent_source_identity_matches(
    const si_diagnostics_agent_source_identity_t *observed,
    const si_diagnostics_agent_source_identity_t *required);
void si_diagnostics_text_builder_init(si_diagnostics_text_builder_t *builder,
                                      char *out,
                                      size_t out_size);
bool si_diagnostics_text_appendf(si_diagnostics_text_builder_t *builder,
                                 const char *fmt, ...);
bool si_diagnostics_base64_encode(const void *data,
                                  size_t data_len,
                                  char *out,
                                  size_t out_size,
                                  size_t *written_out);
bool si_diagnostics_text_append_base64_line(
    si_diagnostics_text_builder_t *builder,
    const char *trusted_prefix,
    const void *data,
    size_t data_len);
bool si_diagnostics_format_agent_event_page(
    const si_diagnostics_agent_event_page_t *page,
    char *out,
    size_t out_size,
    uint32_t *next_after_seq_out,
    bool *truncated_out);
void si_diagnostics_agent_submit_receipt_ids(
    bool accepted,
    bool busy,
    bool deduplicated,
    const char *observed_run_id,
    char *submitted_run_id,
    size_t submitted_run_id_size,
    char *active_run_id,
    size_t active_run_id_size);
bool si_diagnostics_result_chunk_plan(
    size_t total_bytes,
    size_t offset,
    size_t requested_bytes,
    size_t output_size,
    si_diagnostics_result_chunk_plan_t *plan);
uint32_t si_diagnostics_crc32(const void *data, size_t data_len);

typedef struct {
    int (*web_client_count)(void);
    void (*agent_history)(char *out, size_t out_size);
    void (*agent_history_append)(const char *text, char *out, size_t out_size);
    void (*agent_history_clear)(char *out, size_t out_size);
    void (*agent_memory)(char *out, size_t out_size);
    void (*agent_memory_append)(const char *text, char *out, size_t out_size);
    void (*agent_memory_clear)(char *out, size_t out_size);
    void (*agent_data_clear)(char *out, size_t out_size);
    void (*ssh_target)(char *out, size_t out_size);
    void (*ssh_exec)(const char *command, char *out, size_t out_size);
    void (*agent_run_start)(const char *session_id,
                            const char *message,
                            const char *profile,
                            const char *model,
                            bool dry_run,
                            bool include_screenshot,
                            bool allow_web_search,
                            char *out,
                            size_t out_size);
    void (*agent_run_status)(const char *run_id, char *out, size_t out_size);
    void (*agent_run_events)(const char *run_id,
                             uint32_t since_seq,
                             uint32_t *latest_seq,
                             bool *running,
                             char *out,
                             size_t out_size);
    void (*agent_run_pause)(const char *run_id, char *out, size_t out_size);
    void (*agent_run_resume)(const char *run_id, char *out, size_t out_size);
    void (*agent_run_cancel)(const char *run_id, char *out, size_t out_size);
    void (*agent_run_abort)(const char *run_id, char *out, size_t out_size);
    void (*agent_run_steer)(const char *run_id,
                            const char *message,
                            char *out,
                            size_t out_size);
    void (*agent_run_result)(const char *run_id,
                             size_t offset,
                             size_t max_bytes,
                             char *out,
                             size_t out_size);
} si_diagnostics_backend_t;

void si_diagnostics_register_backend(const si_diagnostics_backend_t *backend);
int si_diagnostics_web_client_count(void);

void si_diagnostics_agent_history(char *out, size_t out_size);
void si_diagnostics_agent_history_append(const char *text, char *out, size_t out_size);
void si_diagnostics_agent_history_clear(char *out, size_t out_size);
void si_diagnostics_agent_memory(char *out, size_t out_size);
void si_diagnostics_agent_memory_append(const char *text, char *out, size_t out_size);
void si_diagnostics_agent_memory_clear(char *out, size_t out_size);
void si_diagnostics_agent_data_clear(char *out, size_t out_size);
void si_diagnostics_ssh_target(char *out, size_t out_size);
void si_diagnostics_ssh_exec(const char *command, char *out, size_t out_size);
void si_diagnostics_agent_run_start(const char *session_id,
                                    const char *message,
                                    const char *profile,
                                    const char *model,
                                    bool dry_run,
                                    bool include_screenshot,
                                    bool allow_web_search,
                                    char *out,
                                    size_t out_size);
void si_diagnostics_agent_run_status(const char *run_id,
                                     char *out,
                                     size_t out_size);
void si_diagnostics_agent_run_events(const char *run_id,
                                     uint32_t since_seq,
                                     uint32_t *latest_seq,
                                     bool *running,
                                     char *out,
                                     size_t out_size);
void si_diagnostics_agent_run_pause(const char *run_id,
                                    char *out,
                                    size_t out_size);
void si_diagnostics_agent_run_resume(const char *run_id,
                                     char *out,
                                     size_t out_size);
void si_diagnostics_agent_run_cancel(const char *run_id,
                                     char *out,
                                     size_t out_size);
void si_diagnostics_agent_run_abort(const char *run_id,
                                    char *out,
                                    size_t out_size);
void si_diagnostics_agent_run_steer(const char *run_id,
                                    const char *message,
                                    char *out,
                                    size_t out_size);
void si_diagnostics_agent_run_result(const char *run_id,
                                     size_t offset,
                                     size_t max_bytes,
                                     char *out,
                                     size_t out_size);
