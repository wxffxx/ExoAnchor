#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef esp_err_t (*agent_history_file_record_cb_t)(cJSON *entry, void *user_ctx);

typedef struct {
    bool history_cleared;
    bool memory_cleared;
    bool run_checkpoint_cleared;
} si_agent_data_clear_result_t;

esp_err_t agent_history_start(void);
SemaphoreHandle_t si_agent_repository_lock(void);
const char *si_agent_repository_last_error(void);
bool agent_history_tf_mounted(void);
void agent_history_set_error(const char *fmt, ...);
esp_err_t si_agent_repository_clear_history(bool *cleared);
esp_err_t si_agent_repository_clear_memory(bool *cleared);
esp_err_t si_agent_repository_clear_all(si_agent_data_clear_result_t *result);
esp_err_t si_agent_repository_run_checkpoint_write(const char *json,
                                                   size_t json_len);
esp_err_t si_agent_repository_run_checkpoint_read(char **json_out);
esp_err_t si_agent_repository_run_checkpoint_revoke_page_context(void);
esp_err_t si_agent_repository_run_checkpoint_clear_if_job_id(
    const char *expected_job_id, bool *cleared);
esp_err_t si_agent_repository_run_checkpoint_clear(bool *cleared);

bool agent_session_id_valid(const char *session_id);
void agent_session_normalize(const char *session_id, char *out, size_t out_size);
void agent_session_title_from_content(const char *content, char *out, size_t out_size);
const char *agent_history_record_session_id(const cJSON *record);
bool agent_history_record_is_session_control(const cJSON *record);
bool agent_history_record_matches_session(const cJSON *record, const char *session_id);
bool agent_history_record_is_visible(const cJSON *record);
const char *agent_memory_type_normalize(const char *type);
const char *agent_memory_record_type(const cJSON *record);
bool agent_memory_record_is_visible(const cJSON *entry);

esp_err_t agent_history_file_ensure_ready(bool *formatted);
esp_err_t agent_history_file_stats_locked(size_t *used_bytes_out, uint32_t *record_count_out);
esp_err_t agent_history_file_for_each_locked(agent_history_file_record_cb_t cb,
                                             void *user_ctx, uint32_t *record_count_out);
esp_err_t agent_history_file_append_locked(const char *json, size_t json_len,
                                           size_t *used_bytes_out, uint32_t *record_count_out);
esp_err_t agent_history_file_count_visible_locked(const char *session_id,
                                                  uint32_t *record_count);
esp_err_t agent_history_file_add_records_json_locked(cJSON *records, uint32_t skip_count,
                                                     const char *session_id, uint32_t *returned_out);

esp_err_t agent_memory_file_ensure_ready(bool *formatted);
esp_err_t agent_memory_file_stats_locked(size_t *used_bytes_out, uint32_t *record_count_out);
esp_err_t agent_memory_file_for_each_locked(agent_history_file_record_cb_t cb,
                                            void *user_ctx, uint32_t *record_count_out);
esp_err_t agent_memory_file_append_locked(const char *json, size_t json_len,
                                          size_t *used_bytes_out, uint32_t *record_count_out);
esp_err_t agent_memory_file_add_records_json_locked(cJSON *records, uint32_t skip_count,
                                                    uint32_t *returned_out);
