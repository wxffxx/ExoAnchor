#include "agent_history_writer.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_repository.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define HISTORY_WRITER_TASK_STACK 6144U
#define HISTORY_WRITER_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define HISTORY_WRITER_LOCK_WAIT_MS 1500U
#define HISTORY_WRITER_POOL_WAIT_MS 50U
#define HISTORY_WRITER_RESULT_LOCK_WAIT_MS 250U
#define HISTORY_WRITER_REAP_INTERVAL_MS 100U
#define HISTORY_WRITER_RECORD_MAX 3072U
#define HISTORY_WRITER_ID_MAX 48U
#define HISTORY_WRITER_SESSION_MAX 48U
#define HISTORY_WRITER_PROFILE_MAX 24U
#define HISTORY_WRITER_MODEL_MAX 96U
#define HISTORY_WRITER_KIND_MAX 16U
#define HISTORY_WRITER_RECORD_ID_MAX (HISTORY_WRITER_ID_MAX * 2U + 24U)

typedef enum {
    HISTORY_SLOT_FREE = 0,
    HISTORY_SLOT_QUEUED,
    HISTORY_SLOT_WRITING,
    HISTORY_SLOT_COMPLETE,
} history_slot_state_t;

typedef struct {
    char session_id[HISTORY_WRITER_SESSION_MAX + 1U];
    char turn_id[HISTORY_WRITER_ID_MAX + 1U];
    char run_id[HISTORY_WRITER_ID_MAX + 1U];
    char user_content[SI_AGENT_HISTORY_WRITER_CONTENT_MAX + 1U];
    char assistant_content[SI_AGENT_HISTORY_WRITER_CONTENT_MAX + 1U];
    char profile[HISTORY_WRITER_PROFILE_MAX + 1U];
    char model[HISTORY_WRITER_MODEL_MAX + 1U];
    char assistant_kind[HISTORY_WRITER_KIND_MAX + 1U];
    bool ok;
    bool dry_run;
    int actions_count;
    int executed_count;
    uint32_t stored_ms;
} history_turn_owned_t;

typedef struct {
    history_slot_state_t state;
    uint32_t generation;
    bool caller_waiting;
    SemaphoreHandle_t done;
    si_agent_history_write_result_t result;
} history_writer_slot_t;

typedef struct {
    const char *user_record_id;
    const char *assistant_record_id;
    bool user_found;
    bool assistant_found;
} history_id_scan_t;

static const char *TAG = "si-agent-history-writer";
static SemaphoreHandle_t s_writer_lock;
static QueueHandle_t s_writer_queue;
static TaskHandle_t s_writer_task;
/* Only small lifecycle/result metadata is internal. The four large, bounded
 * turn payloads live in PSRAM; atomic abandonment flags never target PSRAM. */
static history_writer_slot_t s_writer_slots[
    SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY];
static history_turn_owned_t *s_writer_turns;
static uint32_t s_writer_stack_min_hwm = UINT32_MAX;

#ifdef SI_AGENT_HISTORY_WRITER_HOST_TEST
static si_agent_history_writer_host_scan_fn s_host_scan;
static si_agent_history_writer_host_append_fn s_host_append;
static void *s_host_ctx;
static bool s_host_force_queue_full;
static bool s_host_force_pending;
#endif

static void history_result_defaults(si_agent_history_write_result_t *result)
{
    if (!result) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->user_status = ESP_ERR_INVALID_STATE;
    result->assistant_status = ESP_ERR_INVALID_STATE;
}

static bool history_id_valid(const char *value)
{
    if (!value || !value[0] || strlen(value) > HISTORY_WRITER_ID_MAX) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor; ++cursor) {
        if (!isalnum(*cursor) && *cursor != '-' && *cursor != '_' &&
            *cursor != '.' && *cursor != ':') {
            return false;
        }
    }
    return true;
}

static void history_copy_content(char *out, size_t out_size, const char *content)
{
    if (!out || out_size == 0U) {
        return;
    }
    const char *text = content ? content : "";
    size_t length = strlen(text);
    if (length < out_size) {
        memcpy(out, text, length + 1U);
        return;
    }
    static const char suffix[] = "\n[truncated]";
    size_t cut = out_size - sizeof(suffix);
    while (cut > 0U && (((uint8_t)text[cut]) & 0xc0U) == 0x80U) {
        --cut;
    }
    memcpy(out, text, cut);
    memcpy(out + cut, suffix, sizeof(suffix));
}

static void history_copy_turn(history_turn_owned_t *out,
                              const si_agent_history_turn_write_t *turn)
{
    memset(out, 0, sizeof(*out));
    agent_session_normalize(turn->session_id, out->session_id,
                            sizeof(out->session_id));
    strlcpy(out->turn_id, turn->turn_id, sizeof(out->turn_id));
    strlcpy(out->run_id, turn->run_id, sizeof(out->run_id));
    history_copy_content(out->user_content, sizeof(out->user_content),
                         turn->user_content);
    history_copy_content(out->assistant_content,
                         sizeof(out->assistant_content),
                         turn->assistant_content);
    strlcpy(out->profile, turn->profile ? turn->profile : "",
            sizeof(out->profile));
    strlcpy(out->model, turn->model ? turn->model : "",
            sizeof(out->model));
    strlcpy(out->assistant_kind,
            turn->assistant_kind && turn->assistant_kind[0] ?
                turn->assistant_kind : "message",
            sizeof(out->assistant_kind));
    out->ok = turn->ok;
    out->dry_run = turn->dry_run;
    out->actions_count = turn->actions_count;
    out->executed_count = turn->executed_count;
    out->stored_ms = turn->stored_ms;
}

static void history_record_id(char *out, size_t out_size,
                              const history_turn_owned_t *turn,
                              const char *role)
{
    snprintf(out, out_size, "%s/%s/%s", turn->run_id, turn->turn_id, role);
}

static esp_err_t history_scan_record_cb(cJSON *entry, void *user_ctx)
{
    history_id_scan_t *scan = (history_id_scan_t *)user_ctx;
    const cJSON *record_id =
        cJSON_GetObjectItemCaseSensitive(entry, "record_id");
    if (!scan || !cJSON_IsString(record_id) || !record_id->valuestring) {
        return ESP_OK;
    }
    if (strcmp(record_id->valuestring, scan->user_record_id) == 0) {
        scan->user_found = true;
    } else if (strcmp(record_id->valuestring,
                      scan->assistant_record_id) == 0) {
        scan->assistant_found = true;
    }
    return ESP_OK;
}

static esp_err_t history_scan_ids_locked(history_id_scan_t *scan)
{
#ifdef SI_AGENT_HISTORY_WRITER_HOST_TEST
    if (s_host_scan) {
        return s_host_scan(scan->user_record_id, scan->assistant_record_id,
                           &scan->user_found, &scan->assistant_found,
                           s_host_ctx);
    }
#endif
    return agent_history_file_for_each_locked(history_scan_record_cb, scan,
                                               NULL);
}

static esp_err_t history_build_record(const history_turn_owned_t *turn,
                                      const char *record_id,
                                      const char *role,
                                      const char *kind,
                                      const char *content,
                                      uint32_t stored_ms,
                                      bool include_run_meta,
                                      char **json_out,
                                      size_t *json_len_out)
{
    *json_out = NULL;
    *json_len_out = 0U;
    cJSON *entry = cJSON_CreateObject();
    if (!entry) {
        return ESP_ERR_NO_MEM;
    }
    bool complete =
        cJSON_AddStringToObject(entry, "kind", kind) &&
        cJSON_AddStringToObject(entry, "session_id", turn->session_id) &&
        cJSON_AddStringToObject(entry, "thread_id", turn->session_id) &&
        cJSON_AddStringToObject(entry, "turn_id", turn->turn_id) &&
        cJSON_AddStringToObject(entry, "run_id", turn->run_id) &&
        cJSON_AddStringToObject(entry, "record_id", record_id) &&
        cJSON_AddStringToObject(entry, "role", role) &&
        cJSON_AddStringToObject(entry, "content", content) &&
        cJSON_AddNumberToObject(entry, "stored_ms", stored_ms);
    if (turn->profile[0]) {
        complete = complete &&
                   cJSON_AddStringToObject(entry, "profile", turn->profile);
    }
    if (turn->model[0]) {
        complete = complete &&
                   cJSON_AddStringToObject(entry, "model", turn->model);
    }
    if (include_run_meta) {
        complete = complete && cJSON_AddBoolToObject(entry, "ok", turn->ok) &&
                   cJSON_AddBoolToObject(entry, "dry_run", turn->dry_run) &&
                   cJSON_AddNumberToObject(entry, "actions",
                                           turn->actions_count) &&
                   cJSON_AddNumberToObject(entry, "executed",
                                           turn->executed_count);
    }
    if (!complete) {
        cJSON_Delete(entry);
        return ESP_ERR_NO_MEM;
    }
    char *json = cJSON_PrintUnformatted(entry);
    cJSON_Delete(entry);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    size_t json_len = strlen(json);
    if (json_len == 0U || json_len > HISTORY_WRITER_RECORD_MAX) {
        free(json);
        return ESP_ERR_INVALID_SIZE;
    }
    *json_out = json;
    *json_len_out = json_len;
    return ESP_OK;
}

static esp_err_t history_append_record_locked(
    const history_turn_owned_t *turn, const char *record_id,
    const char *role, const char *kind, const char *content,
    uint32_t stored_ms, bool include_run_meta)
{
    char *json = NULL;
    size_t json_len = 0U;
    esp_err_t ret = history_build_record(
        turn, record_id, role, kind, content, stored_ms, include_run_meta,
        &json, &json_len);
    if (ret != ESP_OK) {
        return ret;
    }
#ifdef SI_AGENT_HISTORY_WRITER_HOST_TEST
    if (s_host_append) {
        ret = s_host_append(record_id, role, json, json_len, s_host_ctx);
    } else
#endif
    {
        ret = agent_history_file_append_locked(json, json_len, NULL, NULL);
    }
    free(json);
    return ret;
}

static bool history_record_present_after_error_locked(
    const char *user_record_id, const char *assistant_record_id,
    bool check_user)
{
    history_id_scan_t retry = {
        .user_record_id = user_record_id,
        .assistant_record_id = assistant_record_id,
    };
    return history_scan_ids_locked(&retry) == ESP_OK &&
           (check_user ? retry.user_found : retry.assistant_found);
}

static esp_err_t history_write_turn_locked(
    const history_turn_owned_t *turn,
    si_agent_history_write_result_t *result)
{
    char user_record_id[HISTORY_WRITER_RECORD_ID_MAX + 1U];
    char assistant_record_id[HISTORY_WRITER_RECORD_ID_MAX + 1U];
    history_record_id(user_record_id, sizeof(user_record_id), turn, "user");
    history_record_id(assistant_record_id, sizeof(assistant_record_id), turn,
                      "assistant");

    history_id_scan_t scan = {
        .user_record_id = user_record_id,
        .assistant_record_id = assistant_record_id,
    };
    esp_err_t ret = history_scan_ids_locked(&scan);
    if (ret != ESP_OK) {
        result->user_status = ret;
        result->assistant_status = ret;
        return ret;
    }
    result->user_duplicate = scan.user_found;
    result->assistant_duplicate = scan.assistant_found;
    if (scan.assistant_found && !scan.user_found) {
        strlcpy(result->error, "assistant record exists before user record",
                sizeof(result->error));
        result->user_status = ESP_ERR_INVALID_STATE;
        result->assistant_status = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }

    if (!scan.user_found) {
        ret = history_append_record_locked(
            turn, user_record_id, "user", "message", turn->user_content,
            turn->stored_ms, false);
        if (ret != ESP_OK && history_record_present_after_error_locked(
                                 user_record_id, assistant_record_id, true)) {
            ret = ESP_OK;
            result->user_duplicate = true;
        }
        if (ret != ESP_OK) {
            result->user_status = ret;
            result->assistant_status = ESP_ERR_INVALID_STATE;
            return ret;
        }
    }
    result->user_status = ESP_OK;

    if (!scan.assistant_found) {
        uint32_t assistant_ms = turn->stored_ms == UINT32_MAX ?
                                UINT32_MAX : turn->stored_ms + 1U;
        ret = history_append_record_locked(
            turn, assistant_record_id, "agent", turn->assistant_kind,
            turn->assistant_content, assistant_ms, true);
        if (ret != ESP_OK && history_record_present_after_error_locked(
                                 user_record_id, assistant_record_id, false)) {
            ret = ESP_OK;
            result->assistant_duplicate = true;
        }
        if (ret != ESP_OK) {
            result->assistant_status = ret;
            return ret;
        }
    }
    result->assistant_status = ESP_OK;
    result->saved = true;
    return ESP_OK;
}

static esp_err_t history_write_turn(
    const history_turn_owned_t *turn,
    si_agent_history_write_result_t *result)
{
    history_result_defaults(result);
#ifndef SI_AGENT_HISTORY_WRITER_HOST_TEST
    SemaphoreHandle_t repository_lock = si_agent_repository_lock();
    if (!repository_lock ||
        xSemaphoreTake(repository_lock,
                       pdMS_TO_TICKS(HISTORY_WRITER_LOCK_WAIT_MS)) != pdTRUE) {
        result->user_status = ESP_ERR_TIMEOUT;
        result->assistant_status = ESP_ERR_TIMEOUT;
        strlcpy(result->error, "agent history repository lock timeout",
                sizeof(result->error));
        return ESP_ERR_TIMEOUT;
    }
    bool formatted = false;
    esp_err_t ret = agent_history_file_ensure_ready(&formatted);
    if (ret == ESP_OK) {
        ret = history_write_turn_locked(turn, result);
    } else {
        result->user_status = ret;
        result->assistant_status = ret;
    }
    if (ret != ESP_OK && !result->error[0]) {
        strlcpy(result->error, si_agent_repository_last_error(),
                sizeof(result->error));
    }
    xSemaphoreGive(repository_lock);
    return ret;
#else
    return history_write_turn_locked(turn, result);
#endif
}

#ifndef SI_AGENT_HISTORY_WRITER_HOST_TEST
static void history_writer_reap_abandoned(void)
{
    if (xSemaphoreTake(s_writer_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (size_t i = 0; i < SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY; ++i) {
        history_writer_slot_t *slot = &s_writer_slots[i];
        if (slot->state == HISTORY_SLOT_COMPLETE &&
            !__atomic_load_n(&slot->caller_waiting, __ATOMIC_ACQUIRE)) {
            slot->state = HISTORY_SLOT_FREE;
        }
    }
    xSemaphoreGive(s_writer_lock);
}

static void history_writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t slot_index = UINT8_MAX;
        if (xQueueReceive(s_writer_queue, &slot_index,
                          pdMS_TO_TICKS(HISTORY_WRITER_REAP_INTERVAL_MS)) !=
            pdTRUE) {
            history_writer_reap_abandoned();
            continue;
        }
        if (slot_index >= SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY) {
            continue;
        }
        uint32_t generation = 0U;
        if (xSemaphoreTake(s_writer_lock, portMAX_DELAY) == pdTRUE) {
            history_writer_slot_t *slot = &s_writer_slots[slot_index];
            if (slot->state == HISTORY_SLOT_QUEUED) {
                slot->state = HISTORY_SLOT_WRITING;
                generation = slot->generation;
            }
            xSemaphoreGive(s_writer_lock);
        }
        if (generation == 0U) {
            continue;
        }

        si_agent_history_write_result_t result;
        esp_err_t ret = history_write_turn(&s_writer_turns[slot_index],
                                           &result);
        if (xSemaphoreTake(s_writer_lock, portMAX_DELAY) == pdTRUE) {
            history_writer_slot_t *slot = &s_writer_slots[slot_index];
            if (slot->state == HISTORY_SLOT_WRITING &&
                slot->generation == generation) {
                slot->result = result;
                if (__atomic_load_n(&slot->caller_waiting,
                                    __ATOMIC_ACQUIRE)) {
                    slot->state = HISTORY_SLOT_COMPLETE;
                    xSemaphoreGive(slot->done);
                } else {
                    slot->state = HISTORY_SLOT_FREE;
                }
            }
            xSemaphoreGive(s_writer_lock);
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "history turn write failed: %s (%s)",
                     esp_err_to_name(ret), result.error);
        }
        uint32_t hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        if (hwm < s_writer_stack_min_hwm) {
            s_writer_stack_min_hwm = hwm;
            ESP_LOGI(TAG, "history writer external stack min high-water=%u",
                     (unsigned)hwm);
        }
    }
}
#endif

esp_err_t si_agent_history_writer_start(void)
{
#ifdef SI_AGENT_HISTORY_WRITER_HOST_TEST
    return ESP_OK;
#else
    if (s_writer_task && s_writer_queue && s_writer_turns && s_writer_lock) {
        return ESP_OK;
    }
    if (!si_agent_repository_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_writer_lock) {
        s_writer_lock = xSemaphoreCreateMutex();
    }
    if (!s_writer_lock) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_writer_turns) {
        s_writer_turns = heap_caps_calloc(
            SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY,
            sizeof(*s_writer_turns), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_writer_turns) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY; ++i) {
        if (!s_writer_slots[i].done) {
            s_writer_slots[i].done = xSemaphoreCreateBinary();
        }
        if (!s_writer_slots[i].done) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_writer_queue) {
        s_writer_queue = xQueueCreate(SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY,
                                      sizeof(uint8_t));
    }
    if (!s_writer_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_writer_task) {
        BaseType_t created = xTaskCreateWithCaps(
            history_writer_task, "si_agent_hist", HISTORY_WRITER_TASK_STACK,
            NULL, HISTORY_WRITER_TASK_PRIORITY, &s_writer_task,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (created != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
#endif
}

esp_err_t si_agent_history_writer_store_turn(
    const si_agent_history_turn_write_t *turn,
    uint32_t wait_ms,
    si_agent_history_write_result_t *result)
{
    history_result_defaults(result);
    if (!turn || !result || !history_id_valid(turn->turn_id) ||
        !history_id_valid(turn->run_id) || !turn->user_content ||
        !turn->assistant_content || !turn->user_content[0] ||
        !turn->assistant_content[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!turn->conversation_history_enabled) {
        result->user_status = ESP_ERR_NOT_SUPPORTED;
        result->assistant_status = ESP_ERR_NOT_SUPPORTED;
        strlcpy(result->error, "conversation history disabled",
                sizeof(result->error));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (wait_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
#ifdef SI_AGENT_HISTORY_WRITER_HOST_TEST
    if (s_host_force_queue_full) {
        result->user_status = ESP_ERR_TIMEOUT;
        result->assistant_status = ESP_ERR_TIMEOUT;
        strlcpy(result->error, "history writer queue full",
                sizeof(result->error));
        return ESP_ERR_TIMEOUT;
    }
    if (s_host_force_pending) {
        result->pending = true;
        result->user_status = ESP_ERR_TIMEOUT;
        result->assistant_status = ESP_ERR_TIMEOUT;
        strlcpy(result->error, "history writer completion timeout",
                sizeof(result->error));
        return ESP_ERR_TIMEOUT;
    }
    history_turn_owned_t owned;
    history_copy_turn(&owned, turn);
    return history_write_turn(&owned, result);
#else
    esp_err_t ret = si_agent_history_writer_start();
    if (ret != ESP_OK) {
        result->user_status = ret;
        result->assistant_status = ret;
        strlcpy(result->error, "history writer unavailable",
                sizeof(result->error));
        return ret;
    }
    if (xSemaphoreTake(s_writer_lock,
                       pdMS_TO_TICKS(HISTORY_WRITER_POOL_WAIT_MS)) != pdTRUE) {
        result->user_status = ESP_ERR_TIMEOUT;
        result->assistant_status = ESP_ERR_TIMEOUT;
        strlcpy(result->error, "history writer pool lock timeout",
                sizeof(result->error));
        return ESP_ERR_TIMEOUT;
    }
    uint8_t slot_index = UINT8_MAX;
    for (uint8_t i = 0; i < SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY; ++i) {
        if (s_writer_slots[i].state == HISTORY_SLOT_FREE) {
            slot_index = i;
            break;
        }
    }
    if (slot_index == UINT8_MAX) {
        xSemaphoreGive(s_writer_lock);
        result->user_status = ESP_ERR_TIMEOUT;
        result->assistant_status = ESP_ERR_TIMEOUT;
        strlcpy(result->error, "history writer queue full",
                sizeof(result->error));
        return ESP_ERR_TIMEOUT;
    }
    history_writer_slot_t *slot = &s_writer_slots[slot_index];
    while (xSemaphoreTake(slot->done, 0) == pdTRUE) {
    }
    slot->generation++;
    if (slot->generation == 0U) {
        slot->generation = 1U;
    }
    uint32_t generation = slot->generation;
    __atomic_store_n(&slot->caller_waiting, true, __ATOMIC_RELEASE);
    slot->state = HISTORY_SLOT_QUEUED;
    history_result_defaults(&slot->result);
    history_copy_turn(&s_writer_turns[slot_index], turn);
    xSemaphoreGive(s_writer_lock);

    if (xQueueSend(s_writer_queue, &slot_index, 0) != pdTRUE) {
        /* No consumer can own an item which xQueueSend rejected. Atomic
         * release avoids an unbounded cleanup-lock wait on the provider. */
        __atomic_store_n(&slot->caller_waiting, false, __ATOMIC_RELEASE);
        __atomic_store_n(&slot->state, HISTORY_SLOT_FREE, __ATOMIC_RELEASE);
        result->user_status = ESP_ERR_TIMEOUT;
        result->assistant_status = ESP_ERR_TIMEOUT;
        strlcpy(result->error, "history writer queue full",
                sizeof(result->error));
        return ESP_ERR_TIMEOUT;
    }

    bool signalled = xSemaphoreTake(slot->done, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
    if (xSemaphoreTake(
            s_writer_lock,
            pdMS_TO_TICKS(HISTORY_WRITER_RESULT_LOCK_WAIT_MS)) != pdTRUE) {
        __atomic_store_n(&slot->caller_waiting, false, __ATOMIC_RELEASE);
        result->pending = true;
        result->user_status = ESP_ERR_TIMEOUT;
        result->assistant_status = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }
    if (slot->generation != generation) {
        xSemaphoreGive(s_writer_lock);
        result->user_status = ESP_ERR_INVALID_STATE;
        result->assistant_status = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    if (slot->state == HISTORY_SLOT_COMPLETE) {
        *result = slot->result;
        __atomic_store_n(&slot->caller_waiting, false, __ATOMIC_RELEASE);
        slot->state = HISTORY_SLOT_FREE;
        xSemaphoreGive(s_writer_lock);
        return result->saved ? ESP_OK :
               (result->assistant_status != ESP_OK ?
                    result->assistant_status : result->user_status);
    }
    (void)signalled;
    __atomic_store_n(&slot->caller_waiting, false, __ATOMIC_RELEASE);
    result->pending = true;
    result->user_status = ESP_ERR_TIMEOUT;
    result->assistant_status = ESP_ERR_TIMEOUT;
    strlcpy(result->error, "history writer completion timeout",
            sizeof(result->error));
    xSemaphoreGive(s_writer_lock);
    return ESP_ERR_TIMEOUT;
#endif
}

size_t si_agent_history_writer_pending_count(void)
{
#ifdef SI_AGENT_HISTORY_WRITER_HOST_TEST
    return s_host_force_queue_full ? SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY : 0U;
#else
    if (!s_writer_lock || !s_writer_turns ||
        xSemaphoreTake(s_writer_lock, 0) != pdTRUE) {
        return 0U;
    }
    size_t pending = 0U;
    for (size_t i = 0; i < SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY; ++i) {
        if (s_writer_slots[i].state != HISTORY_SLOT_FREE) {
            ++pending;
        }
    }
    xSemaphoreGive(s_writer_lock);
    return pending;
#endif
}

#ifdef SI_AGENT_HISTORY_WRITER_HOST_TEST
void si_agent_history_writer_host_reset(void)
{
    s_host_scan = NULL;
    s_host_append = NULL;
    s_host_ctx = NULL;
    s_host_force_queue_full = false;
    s_host_force_pending = false;
}

void si_agent_history_writer_host_set_hooks(
    si_agent_history_writer_host_scan_fn scan_fn,
    si_agent_history_writer_host_append_fn append_fn,
    void *ctx)
{
    s_host_scan = scan_fn;
    s_host_append = append_fn;
    s_host_ctx = ctx;
}

void si_agent_history_writer_host_force_queue_full(bool full)
{
    s_host_force_queue_full = full;
}

void si_agent_history_writer_host_force_pending(bool pending)
{
    s_host_force_pending = pending;
}
#endif
