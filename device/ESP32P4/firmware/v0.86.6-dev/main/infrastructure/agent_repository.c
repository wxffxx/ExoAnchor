#include "agent_repository.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "agent_page_context_checkpoint.h"
#include "storage_manager.h"
#include "utf8_utils.h"

#define AGENT_HISTORY_MAX_RECORD_BYTES 3072U
#define AGENT_HISTORY_FILE_MAX_BYTES (1024U * 1024U)
#define AGENT_HISTORY_RECORD_COUNT_UNKNOWN UINT32_MAX
#define AGENT_HISTORY_TF_ROOT SI_STORAGE_ROOT
#define AGENT_HISTORY_TF_DIR SI_STORAGE_AGENT_DIR
#define AGENT_HISTORY_TF_FILE AGENT_HISTORY_TF_DIR "/HIST.LOG"
#define AGENT_MEMORY_TF_FILE AGENT_HISTORY_TF_DIR "/MEMORY.LOG"
#define AGENT_RUN_CHECKPOINT_TF_FILE AGENT_HISTORY_TF_DIR "/RUN.CHECKPOINT"
#define AGENT_RUN_CHECKPOINT_TF_TMP AGENT_HISTORY_TF_DIR "/RUN.CHECKPOINT.TMP"
#define AGENT_RUN_CHECKPOINT_MAX_BYTES 12288U
#define AGENT_HISTORY_LINE_MAX (AGENT_HISTORY_MAX_RECORD_BYTES + 256U)
#define AGENT_HISTORY_ERROR_MAX 128
#define AGENT_SESSION_ID_MAX_LEN 24
#define AGENT_SESSION_TITLE_MAX_LEN 48
#define AGENT_SESSION_DEFAULT_ID "default"

static const char *TAG = "si-agent-repo";
static SemaphoreHandle_t s_agent_history_lock;
static char s_agent_history_last_error[AGENT_HISTORY_ERROR_MAX];

esp_err_t agent_history_start(void)
{
    if (s_agent_history_lock) {
        return ESP_OK;
    }
    s_agent_history_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_agent_history_lock, ESP_ERR_NO_MEM, TAG, "create agent history mutex");
    return ESP_OK;
}

bool agent_session_id_valid(const char *session_id)
{
    if (!session_id || session_id[0] == '\0') {
        return false;
    }
    size_t len = strlen(session_id);
    if (len > AGENT_SESSION_ID_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)session_id[i];
        if (!isalnum(ch) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

void agent_session_normalize(const char *session_id, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (agent_session_id_valid(session_id)) {
        strlcpy(out, session_id, out_size);
    } else {
        strlcpy(out, AGENT_SESSION_DEFAULT_ID, out_size);
    }
}

void agent_session_title_from_content(const char *content, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!content || content[0] == '\0') {
        strlcpy(out, "新会话", out_size);
        return;
    }
    size_t n = 0;
    while (content[n] && content[n] != '\n' && n + 1 < out_size && n < 36) {
        out[n] = content[n];
        n++;
    }
    out[n] = '\0';
}

const char *agent_history_record_session_id(const cJSON *record)
{
    cJSON *session = cJSON_GetObjectItemCaseSensitive(record, "session_id");
    if (cJSON_IsString(session) && agent_session_id_valid(session->valuestring)) {
        return session->valuestring;
    }
    session = cJSON_GetObjectItemCaseSensitive(record, "thread_id");
    if (cJSON_IsString(session) && agent_session_id_valid(session->valuestring)) {
        return session->valuestring;
    }
    return AGENT_SESSION_DEFAULT_ID;
}

bool agent_history_record_is_session_control(const cJSON *record)
{
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(record, "kind");
    return cJSON_IsString(kind) && kind->valuestring &&
           strcasecmp(kind->valuestring, "session") == 0;
}

bool agent_history_record_matches_session(const cJSON *record, const char *session_id)
{
    char normalized[AGENT_SESSION_ID_MAX_LEN + 1];
    agent_session_normalize(session_id, normalized, sizeof(normalized));
    return strcmp(agent_history_record_session_id(record), normalized) == 0;
}

bool agent_history_record_is_visible(const cJSON *record)
{
    if (!record || agent_history_record_is_session_control(record)) {
        return false;
    }
    return cJSON_GetObjectItemCaseSensitive(record, "content") ||
           cJSON_GetObjectItemCaseSensitive(record, "message") ||
           cJSON_GetObjectItemCaseSensitive(record, "text");
}

const char *agent_memory_type_normalize(const char *type)
{
    if (!type || !type[0]) {
        return NULL;
    }
    if (strcasecmp(type, "fact") == 0 ||
        strcasecmp(type, "facts") == 0) {
        return "fact";
    }
    if (strcasecmp(type, "host_state") == 0 ||
        strcasecmp(type, "host-state") == 0 ||
        strcasecmp(type, "host") == 0 ||
        strcasecmp(type, "system_state") == 0) {
        return "host_state";
    }
    if (strcasecmp(type, "service_deployment") == 0 ||
        strcasecmp(type, "service-deployment") == 0 ||
        strcasecmp(type, "deployment") == 0 ||
        strcasecmp(type, "service") == 0) {
        return "service_deployment";
    }
    if (strcasecmp(type, "user_decision") == 0 ||
        strcasecmp(type, "user-decision") == 0 ||
        strcasecmp(type, "decision") == 0) {
        return "user_decision";
    }
    if (strcasecmp(type, "todo") == 0 ||
        strcasecmp(type, "todos") == 0 ||
        strcasecmp(type, "task") == 0) {
        return "todo";
    }
    if (strcasecmp(type, "task_summary") == 0 ||
        strcasecmp(type, "task-summary") == 0 ||
        strcasecmp(type, "summary") == 0 ||
        strcasecmp(type, "agent_run") == 0) {
        return "task_summary";
    }
    if (strcasecmp(type, "manual_note") == 0 ||
        strcasecmp(type, "manual-note") == 0 ||
        strcasecmp(type, "note") == 0 ||
        strcasecmp(type, "memory") == 0) {
        return "manual_note";
    }
    return NULL;
}

const char *agent_memory_record_type(const cJSON *record)
{
    if (!record) {
        return "manual_note";
    }
    cJSON *type = cJSON_GetObjectItemCaseSensitive(record, "type");
    if (cJSON_IsString(type) && type->valuestring) {
        const char *normalized = agent_memory_type_normalize(type->valuestring);
        if (normalized) {
            return normalized;
        }
    }
    type = cJSON_GetObjectItemCaseSensitive(record, "scope");
    if (cJSON_IsString(type) && type->valuestring) {
        const char *normalized = agent_memory_type_normalize(type->valuestring);
        if (normalized) {
            return normalized;
        }
    }
    type = cJSON_GetObjectItemCaseSensitive(record, "kind");
    if (cJSON_IsString(type) && type->valuestring) {
        const char *normalized = agent_memory_type_normalize(type->valuestring);
        if (normalized) {
            return normalized;
        }
    }
    return "manual_note";
}

bool agent_history_tf_mounted(void)
{
    si_storage_tf_status_t tf_status;
    si_storage_get_tf_status(&tf_status);
    return tf_status.mounted;
}

void agent_history_set_error(const char *fmt, ...)
{
    if (!fmt) {
        s_agent_history_last_error[0] = '\0';
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_agent_history_last_error, sizeof(s_agent_history_last_error), fmt, args);
    va_end(args);
}

static esp_err_t agent_history_mkdir_if_needed(const char *path)
{
    if (!path || !path[0]) {
        agent_history_set_error("mkdir path empty");
        return ESP_ERR_INVALID_ARG;
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        }
        agent_history_set_error("%s exists but is not a directory", path);
        return ESP_FAIL;
    }
    errno = 0;
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    agent_history_set_error("mkdir %s failed errno=%d", path, errno);
    ESP_LOGW(TAG, "Failed to create %s: errno=%d", path, errno);
    return ESP_FAIL;
}

esp_err_t agent_history_file_ensure_ready(bool *formatted)
{
    if (formatted) {
        *formatted = false;
    }

    si_storage_tf_status_t tf_status;
    si_storage_get_tf_status(&tf_status);
    if (!tf_status.mounted) {
        agent_history_set_error("%s", tf_status.last_error[0] ?
                                tf_status.last_error : "TF card not mounted");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(agent_history_mkdir_if_needed(AGENT_HISTORY_TF_ROOT),
                        TAG, "create ExoAnchor TF dir");
    ESP_RETURN_ON_ERROR(agent_history_mkdir_if_needed(AGENT_HISTORY_TF_DIR),
                        TAG, "create Agent TF dir");

    struct stat st;
    bool existed = stat(AGENT_HISTORY_TF_FILE, &st) == 0;
    if (existed && (st.st_size < 0 ||
                    (uint64_t)st.st_size > AGENT_HISTORY_FILE_MAX_BYTES)) {
        agent_history_set_error("HIST.LOG exceeds bounded runtime limit");
        return ESP_ERR_INVALID_SIZE;
    }
    errno = 0;
    FILE *file = fopen(AGENT_HISTORY_TF_FILE, "a");
    if (!file) {
        agent_history_set_error("open %s failed errno=%d", AGENT_HISTORY_TF_FILE, errno);
        ESP_LOGW(TAG, "Failed to open %s: errno=%d", AGENT_HISTORY_TF_FILE, errno);
        return ESP_FAIL;
    }
    fclose(file);
    if (!existed && formatted) {
        *formatted = true;
    }
    if (!existed) {
        ESP_LOGI(TAG, "Agent history TF file initialized");
    }
    agent_history_set_error(NULL);
    return ESP_OK;
}

esp_err_t agent_history_file_stats_locked(size_t *used_bytes_out,
                                                 uint32_t *record_count_out)
{
    if (used_bytes_out) {
        *used_bytes_out = 0;
    }
    if (record_count_out) {
        *record_count_out = 0;
    }

    struct stat st;
    if (stat(AGENT_HISTORY_TF_FILE, &st) == 0 && used_bytes_out) {
        *used_bytes_out = st.st_size > 0 ? (size_t)st.st_size : 0;
    }

    if (!record_count_out) {
        return ESP_OK;
    }

    FILE *file = fopen(AGENT_HISTORY_TF_FILE, "r");
    if (!file) {
        if (errno != ENOENT) {
            agent_history_set_error("read %s failed errno=%d", AGENT_HISTORY_TF_FILE, errno);
        }
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }

    char *line = malloc(AGENT_HISTORY_LINE_MAX);
    if (!line) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    uint32_t count = 0;
    while (fgets(line, AGENT_HISTORY_LINE_MAX, file)) {
        size_t len = strlen(line);
        bool complete = len == 0 || line[len - 1] == '\n';
        if (!complete) {
            int ch;
            while ((ch = fgetc(file)) != EOF && ch != '\n') {
            }
        }
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len > 0 && complete) {
            count++;
        }
    }

    free(line);
    fclose(file);
    *record_count_out = count;
    return ESP_OK;
}

esp_err_t agent_history_file_for_each_locked(agent_history_file_record_cb_t cb,
                                                   void *user_ctx,
                                                   uint32_t *record_count_out)
{
    if (record_count_out) {
        *record_count_out = 0;
    }

    FILE *file = fopen(AGENT_HISTORY_TF_FILE, "r");
    if (!file) {
        if (errno != ENOENT) {
            agent_history_set_error("iterate %s failed errno=%d", AGENT_HISTORY_TF_FILE, errno);
        }
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }

    char *line = malloc(AGENT_HISTORY_LINE_MAX);
    if (!line) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    uint32_t count = 0;
    esp_err_t ret = ESP_OK;
    while (fgets(line, AGENT_HISTORY_LINE_MAX, file)) {
        size_t len = strlen(line);
        bool complete = len == 0 || line[len - 1] == '\n';
        if (!complete) {
            int ch;
            while ((ch = fgetc(file)) != EOF && ch != '\n') {
            }
        }
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || !complete) {
            continue;
        }

        count++;
        if (!cb) {
            continue;
        }

        char *safe_line = si_utf8_sanitize(line, 0);
        if (!safe_line) {
            ret = ESP_ERR_NO_MEM;
            break;
        }
        cJSON *entry = cJSON_Parse(safe_line);
        if (!entry) {
            entry = cJSON_CreateObject();
            if (entry) {
                cJSON_AddStringToObject(entry, "kind", "raw");
                cJSON_AddStringToObject(entry, "content", safe_line);
            }
        }
        free(safe_line);
        if (!entry) {
            ret = ESP_ERR_NO_MEM;
            break;
        }
        ret = cb(entry, user_ctx);
        cJSON_Delete(entry);
        if (ret != ESP_OK) {
            break;
        }
    }

    free(line);
    fclose(file);
    if (record_count_out) {
        *record_count_out = count;
    }
    return ret;
}

esp_err_t agent_history_file_append_locked(const char *json, size_t json_len,
                                                  size_t *used_bytes_out,
                                                  uint32_t *record_count_out)
{
    if (!json || json_len == 0 || json_len > AGENT_HISTORY_MAX_RECORD_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat before = {0};
    if (stat(AGENT_HISTORY_TF_FILE, &before) != 0 || before.st_size < 0) {
        agent_history_set_error("stat %s before append failed errno=%d",
                                AGENT_HISTORY_TF_FILE, errno);
        return ESP_FAIL;
    }
    size_t prior_bytes = (size_t)before.st_size;
    if (prior_bytes > AGENT_HISTORY_FILE_MAX_BYTES ||
        json_len + 1U > AGENT_HISTORY_FILE_MAX_BYTES - prior_bytes) {
        agent_history_set_error("HIST.LOG bounded size limit exceeded");
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(AGENT_HISTORY_TF_FILE, "a");
    if (!file) {
        agent_history_set_error("append %s failed errno=%d", AGENT_HISTORY_TF_FILE, errno);
        ESP_LOGW(TAG, "Failed to append %s: errno=%d", AGENT_HISTORY_TF_FILE, errno);
        return ESP_FAIL;
    }
    errno = 0;
    size_t written = fwrite(json, 1, json_len, file);
    bool ok = written == json_len && fputc('\n', file) != EOF;
    if (ok) {
        ok = fflush(file) == 0;
    }
    if (ok) {
        int fd = fileno(file);
        ok = fd >= 0 && fsync(fd) == 0;
    }
    int write_errno = errno;
    if (fclose(file) != 0) {
        ok = false;
        if (write_errno == 0) {
            write_errno = errno;
        }
    }
    if (!ok) {
        errno = 0;
        int rollback_ret = truncate(AGENT_HISTORY_TF_FILE, (off_t)prior_bytes);
        int rollback_errno = errno;
        if (rollback_ret == 0) {
            agent_history_set_error(
                "durable write %s failed errno=%d rollback=ok",
                AGENT_HISTORY_TF_FILE, write_errno);
        } else {
            agent_history_set_error(
                "durable write %s failed errno=%d rollback_errno=%d",
                AGENT_HISTORY_TF_FILE, write_errno, rollback_errno);
        }
        return ESP_FAIL;
    }
    if (used_bytes_out) {
        *used_bytes_out = prior_bytes + json_len + 1U;
    }
    if (record_count_out) {
        *record_count_out = AGENT_HISTORY_RECORD_COUNT_UNKNOWN;
    }
    return ESP_OK;
}

esp_err_t agent_memory_file_ensure_ready(bool *formatted)
{
    if (formatted) {
        *formatted = false;
    }

    si_storage_tf_status_t tf_status;
    si_storage_get_tf_status(&tf_status);
    if (!tf_status.mounted) {
        agent_history_set_error("%s", tf_status.last_error[0] ?
                                tf_status.last_error : "TF card not mounted");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(agent_history_mkdir_if_needed(AGENT_HISTORY_TF_ROOT),
                        TAG, "create ExoAnchor TF dir");
    ESP_RETURN_ON_ERROR(agent_history_mkdir_if_needed(AGENT_HISTORY_TF_DIR),
                        TAG, "create Agent TF dir");

    struct stat st;
    bool existed = stat(AGENT_MEMORY_TF_FILE, &st) == 0;
    errno = 0;
    FILE *file = fopen(AGENT_MEMORY_TF_FILE, "a");
    if (!file) {
        agent_history_set_error("open %s failed errno=%d", AGENT_MEMORY_TF_FILE, errno);
        ESP_LOGW(TAG, "Failed to open %s: errno=%d", AGENT_MEMORY_TF_FILE, errno);
        return ESP_FAIL;
    }
    fclose(file);
    if (!existed && formatted) {
        *formatted = true;
    }
    if (!existed) {
        ESP_LOGI(TAG, "Agent memory TF file initialized");
    }
    agent_history_set_error(NULL);
    return ESP_OK;
}

esp_err_t agent_memory_file_stats_locked(size_t *used_bytes_out,
                                                uint32_t *record_count_out)
{
    if (used_bytes_out) {
        *used_bytes_out = 0;
    }
    if (record_count_out) {
        *record_count_out = 0;
    }

    struct stat st;
    if (stat(AGENT_MEMORY_TF_FILE, &st) == 0 && used_bytes_out) {
        *used_bytes_out = st.st_size > 0 ? (size_t)st.st_size : 0;
    }

    if (!record_count_out) {
        return ESP_OK;
    }

    FILE *file = fopen(AGENT_MEMORY_TF_FILE, "r");
    if (!file) {
        if (errno != ENOENT) {
            agent_history_set_error("read %s failed errno=%d", AGENT_MEMORY_TF_FILE, errno);
        }
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }

    char *line = malloc(AGENT_HISTORY_LINE_MAX);
    if (!line) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    uint32_t count = 0;
    while (fgets(line, AGENT_HISTORY_LINE_MAX, file)) {
        size_t len = strlen(line);
        bool complete = len == 0 || line[len - 1] == '\n';
        if (!complete) {
            int ch;
            while ((ch = fgetc(file)) != EOF && ch != '\n') {
            }
        }
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len > 0 && complete) {
            count++;
        }
    }

    free(line);
    fclose(file);
    *record_count_out = count;
    return ESP_OK;
}

esp_err_t agent_memory_file_for_each_locked(agent_history_file_record_cb_t cb,
                                                  void *user_ctx,
                                                  uint32_t *record_count_out)
{
    if (record_count_out) {
        *record_count_out = 0;
    }

    FILE *file = fopen(AGENT_MEMORY_TF_FILE, "r");
    if (!file) {
        if (errno != ENOENT) {
            agent_history_set_error("iterate %s failed errno=%d", AGENT_MEMORY_TF_FILE, errno);
        }
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }

    char *line = malloc(AGENT_HISTORY_LINE_MAX);
    if (!line) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    uint32_t count = 0;
    esp_err_t ret = ESP_OK;
    while (fgets(line, AGENT_HISTORY_LINE_MAX, file)) {
        size_t len = strlen(line);
        bool complete = len == 0 || line[len - 1] == '\n';
        if (!complete) {
            int ch;
            while ((ch = fgetc(file)) != EOF && ch != '\n') {
            }
        }
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || !complete) {
            continue;
        }

        count++;
        if (!cb) {
            continue;
        }

        char *safe_line = si_utf8_sanitize(line, 0);
        if (!safe_line) {
            ret = ESP_ERR_NO_MEM;
            break;
        }
        cJSON *entry = cJSON_Parse(safe_line);
        if (!entry) {
            entry = cJSON_CreateObject();
            if (entry) {
                cJSON_AddStringToObject(entry, "kind", "raw");
                cJSON_AddStringToObject(entry, "content", safe_line);
            }
        }
        free(safe_line);
        if (!entry) {
            ret = ESP_ERR_NO_MEM;
            break;
        }
        ret = cb(entry, user_ctx);
        cJSON_Delete(entry);
        if (ret != ESP_OK) {
            break;
        }
    }

    free(line);
    fclose(file);
    if (record_count_out) {
        *record_count_out = count;
    }
    return ret;
}

esp_err_t agent_memory_file_append_locked(const char *json, size_t json_len,
                                                 size_t *used_bytes_out,
                                                 uint32_t *record_count_out)
{
    if (!json || json_len == 0 || json_len > AGENT_HISTORY_MAX_RECORD_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(AGENT_MEMORY_TF_FILE, "a");
    if (!file) {
        agent_history_set_error("append %s failed errno=%d", AGENT_MEMORY_TF_FILE, errno);
        ESP_LOGW(TAG, "Failed to append %s: errno=%d", AGENT_MEMORY_TF_FILE, errno);
        return ESP_FAIL;
    }
    size_t written = fwrite(json, 1, json_len, file);
    bool ok = written == json_len && fputc('\n', file) != EOF && fflush(file) == 0;
    fclose(file);
    if (!ok) {
        agent_history_set_error("write %s failed errno=%d", AGENT_MEMORY_TF_FILE, errno);
        return ESP_FAIL;
    }
    return agent_memory_file_stats_locked(used_bytes_out, record_count_out);
}

static esp_err_t agent_repository_truncate_locked(const char *path)
{
    errno = 0;
    FILE *file = fopen(path, "w");
    if (!file) {
        agent_history_set_error("clear %s failed errno=%d", path, errno);
        ESP_LOGW(TAG, "Failed to clear %s: errno=%d", path, errno);
        return ESP_FAIL;
    }
    bool ok = fflush(file) == 0;
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        agent_history_set_error("flush %s failed errno=%d", path, errno);
        ESP_LOGW(TAG, "Failed to flush cleared %s: errno=%d", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t agent_repository_ensure_tf_dir_locked(void)
{
    si_storage_tf_status_t tf_status;
    si_storage_get_tf_status(&tf_status);
    if (!tf_status.mounted) {
        agent_history_set_error("%s", tf_status.last_error[0] ?
                                tf_status.last_error : "TF card not mounted");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(agent_history_mkdir_if_needed(AGENT_HISTORY_TF_ROOT),
                        TAG, "create ExoAnchor TF dir");
    ESP_RETURN_ON_ERROR(agent_history_mkdir_if_needed(AGENT_HISTORY_TF_DIR),
                        TAG, "create Agent TF dir");
    return ESP_OK;
}

static esp_err_t agent_repository_run_checkpoint_write_locked(
    const char *json, size_t json_len)
{
    esp_err_t ret = agent_repository_ensure_tf_dir_locked();
    if (ret == ESP_OK) {
        errno = 0;
        FILE *file = fopen(AGENT_RUN_CHECKPOINT_TF_TMP, "w");
        if (!file) {
            agent_history_set_error("open %s failed errno=%d",
                                    AGENT_RUN_CHECKPOINT_TF_TMP, errno);
            ret = ESP_FAIL;
        } else {
            size_t written = fwrite(json, 1, json_len, file);
            bool ok = written == json_len && fflush(file) == 0;
            if (fclose(file) != 0) {
                ok = false;
            }
            if (!ok) {
                agent_history_set_error("write %s failed errno=%d",
                                        AGENT_RUN_CHECKPOINT_TF_TMP, errno);
                ret = ESP_FAIL;
            } else if (rename(AGENT_RUN_CHECKPOINT_TF_TMP,
                              AGENT_RUN_CHECKPOINT_TF_FILE) != 0) {
                agent_history_set_error("rename Run checkpoint failed errno=%d",
                                        errno);
                ret = ESP_FAIL;
            }
        }
    }
    if (ret == ESP_OK) {
        agent_history_set_error(NULL);
    }
    return ret;
}

esp_err_t si_agent_repository_run_checkpoint_write(const char *json,
                                                   size_t json_len)
{
    if (!json || json_len == 0 || json_len > AGENT_RUN_CHECKPOINT_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(agent_history_start(), TAG,
                        "start Agent repository for Run checkpoint");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(2000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "Agent Run checkpoint lock timeout");

    esp_err_t ret = agent_repository_run_checkpoint_write_locked(json, json_len);
    xSemaphoreGive(s_agent_history_lock);
    return ret;
}

static esp_err_t agent_repository_run_checkpoint_read_file_locked(
    char **json_out)
{
    *json_out = NULL;
    esp_err_t ret = ESP_OK;
    struct stat st;
    if (stat(AGENT_RUN_CHECKPOINT_TF_FILE, &st) != 0) {
        ret = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (ret == ESP_OK &&
        (st.st_size <= 0 ||
         (size_t)st.st_size > AGENT_RUN_CHECKPOINT_MAX_BYTES)) {
        agent_history_set_error("Run checkpoint size invalid: %ld",
                                (long)st.st_size);
        ret = ESP_ERR_INVALID_SIZE;
    }
    char *json = NULL;
    if (ret == ESP_OK) {
        json = malloc((size_t)st.st_size + 1U);
        if (!json) {
            ret = ESP_ERR_NO_MEM;
        }
    }
    if (ret == ESP_OK) {
        errno = 0;
        FILE *file = fopen(AGENT_RUN_CHECKPOINT_TF_FILE, "r");
        if (!file) {
            agent_history_set_error("read %s failed errno=%d",
                                    AGENT_RUN_CHECKPOINT_TF_FILE, errno);
            ret = ESP_FAIL;
        } else {
            size_t read_len = fread(json, 1, (size_t)st.st_size, file);
            bool ok = read_len == (size_t)st.st_size && ferror(file) == 0;
            fclose(file);
            if (!ok) {
                agent_history_set_error("read %s incomplete",
                                        AGENT_RUN_CHECKPOINT_TF_FILE);
                ret = ESP_FAIL;
            } else {
                json[read_len] = '\0';
            }
        }
    }
    if (ret == ESP_OK) {
        *json_out = json;
        json = NULL;
        agent_history_set_error(NULL);
    }
    free(json);
    return ret;
}

static esp_err_t agent_repository_run_checkpoint_read_locked(char **json_out)
{
    *json_out = NULL;
    esp_err_t ret = agent_repository_ensure_tf_dir_locked();
    return ret == ESP_OK ?
        agent_repository_run_checkpoint_read_file_locked(json_out) : ret;
}

esp_err_t si_agent_repository_run_checkpoint_read(char **json_out)
{
    if (!json_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *json_out = NULL;
    ESP_RETURN_ON_ERROR(agent_history_start(), TAG,
                        "start Agent repository for Run checkpoint");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(2000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "Agent Run checkpoint lock timeout");

    esp_err_t ret = agent_repository_run_checkpoint_read_locked(json_out);
    xSemaphoreGive(s_agent_history_lock);
    return ret;
}

esp_err_t si_agent_repository_run_checkpoint_revoke_page_context(void)
{
    ESP_RETURN_ON_ERROR(agent_history_start(), TAG,
                        "start Agent repository for page-context revocation");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(2000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "Agent page-context revocation lock timeout");

    char *checkpoint = NULL;
    esp_err_t ret = agent_repository_ensure_tf_dir_locked();
    if (ret == ESP_OK) {
        ret = agent_repository_run_checkpoint_read_file_locked(&checkpoint);
        if (ret == ESP_ERR_NOT_FOUND) {
            ret = ESP_OK;
        }
    }
    char *sanitized = NULL;
    bool changed = false;
    if (ret == ESP_OK && checkpoint) {
        ret = si_agent_page_context_strip_checkpoint(
            checkpoint, &sanitized, &changed);
    }
    free(checkpoint);
    if (ret == ESP_OK && changed) {
        ret = agent_repository_run_checkpoint_write_locked(
            sanitized, strlen(sanitized));
    }
    free(sanitized);
    xSemaphoreGive(s_agent_history_lock);
    return ret;
}

esp_err_t si_agent_repository_run_checkpoint_clear_if_job_id(
    const char *expected_job_id, bool *cleared)
{
    if (!expected_job_id || !expected_job_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cleared) {
        *cleared = false;
    }
    ESP_RETURN_ON_ERROR(agent_history_start(), TAG,
                        "start Agent repository for conditional Run checkpoint clear");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(2000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "Agent Run checkpoint lock timeout");

    /* Read, compare, and unlink under the same repository lock. A completed or
     * recovered Run must never clear a checkpoint committed by a newer Run. */
    char *checkpoint = NULL;
    esp_err_t ret = agent_repository_ensure_tf_dir_locked();
    if (ret == ESP_OK) {
        ret = agent_repository_run_checkpoint_read_file_locked(&checkpoint);
        if (ret == ESP_ERR_NOT_FOUND) {
            ret = ESP_OK;
        }
    }
    cJSON *root = NULL;
    if (ret == ESP_OK && checkpoint) {
        root = cJSON_Parse(checkpoint);
        cJSON *job_id = root ?
            cJSON_GetObjectItemCaseSensitive(root, "job_id") : NULL;
        if (!cJSON_IsObject(root) || !cJSON_IsString(job_id) ||
            !job_id->valuestring || !job_id->valuestring[0]) {
            agent_history_set_error("Run checkpoint job identity invalid");
            ret = ESP_ERR_INVALID_RESPONSE;
        } else if (strcmp(job_id->valuestring, expected_job_id) == 0) {
            errno = 0;
            if (remove(AGENT_RUN_CHECKPOINT_TF_FILE) != 0) {
                if (errno != ENOENT) {
                    agent_history_set_error("clear %s failed errno=%d",
                                            AGENT_RUN_CHECKPOINT_TF_FILE,
                                            errno);
                    ret = ESP_FAIL;
                }
            } else {
                (void)remove(AGENT_RUN_CHECKPOINT_TF_TMP);
                if (cleared) {
                    *cleared = true;
                }
            }
        }
    }
    cJSON_Delete(root);
    free(checkpoint);
    if (ret == ESP_OK) {
        agent_history_set_error(NULL);
    }
    xSemaphoreGive(s_agent_history_lock);
    return ret;
}

esp_err_t si_agent_repository_run_checkpoint_clear(bool *cleared)
{
    if (cleared) {
        *cleared = false;
    }
    ESP_RETURN_ON_ERROR(agent_history_start(), TAG,
                        "start Agent repository for Run checkpoint clear");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(2000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "Agent Run checkpoint lock timeout");

    esp_err_t ret = agent_repository_ensure_tf_dir_locked();
    if (ret == ESP_OK) {
        errno = 0;
        if (remove(AGENT_RUN_CHECKPOINT_TF_FILE) != 0 && errno != ENOENT) {
            agent_history_set_error("clear %s failed errno=%d",
                                    AGENT_RUN_CHECKPOINT_TF_FILE, errno);
            ret = ESP_FAIL;
        } else if (cleared) {
            *cleared = true;
        }
        (void)remove(AGENT_RUN_CHECKPOINT_TF_TMP);
    }
    if (ret == ESP_OK) {
        agent_history_set_error(NULL);
    }
    xSemaphoreGive(s_agent_history_lock);
    return ret;
}

static esp_err_t agent_repository_clear_locked(bool clear_history,
                                               bool clear_memory,
                                               bool clear_run_checkpoint,
                                               si_agent_data_clear_result_t *result)
{
    si_agent_data_clear_result_t local = {0};
    esp_err_t ret = ESP_OK;
    bool formatted = false;

    if (clear_history) {
        ret = agent_history_file_ensure_ready(&formatted);
    }
    if (ret == ESP_OK && clear_memory) {
        ret = agent_memory_file_ensure_ready(&formatted);
    }
    if (ret == ESP_OK && clear_history) {
        ret = agent_repository_truncate_locked(AGENT_HISTORY_TF_FILE);
        local.history_cleared = ret == ESP_OK;
    }
    if (ret == ESP_OK && clear_memory) {
        ret = agent_repository_truncate_locked(AGENT_MEMORY_TF_FILE);
        local.memory_cleared = ret == ESP_OK;
    }
    if (ret == ESP_OK && clear_run_checkpoint) {
        errno = 0;
        if (remove(AGENT_RUN_CHECKPOINT_TF_FILE) != 0 && errno != ENOENT) {
            agent_history_set_error("clear %s failed errno=%d",
                                    AGENT_RUN_CHECKPOINT_TF_FILE, errno);
            ret = ESP_FAIL;
        } else {
            local.run_checkpoint_cleared = true;
        }
        (void)remove(AGENT_RUN_CHECKPOINT_TF_TMP);
    }
    if (ret == ESP_OK) {
        agent_history_set_error(NULL);
    }
    if (result) {
        *result = local;
    }
    return ret;
}

static esp_err_t agent_repository_clear(bool clear_history, bool clear_memory,
                                        bool clear_run_checkpoint,
                                        si_agent_data_clear_result_t *result)
{
    if (result) {
        memset(result, 0, sizeof(*result));
    }
    ESP_RETURN_ON_ERROR(agent_history_start(), TAG,
                        "start Agent repository for clear");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_agent_history_lock, pdMS_TO_TICKS(2000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "Agent repository clear lock timeout");
    esp_err_t ret =
        agent_repository_clear_locked(clear_history, clear_memory,
                                      clear_run_checkpoint, result);
    xSemaphoreGive(s_agent_history_lock);
    return ret;
}

esp_err_t si_agent_repository_clear_history(bool *cleared)
{
    si_agent_data_clear_result_t result = {0};
    esp_err_t ret = agent_repository_clear(true, false, false, &result);
    if (cleared) {
        *cleared = result.history_cleared;
    }
    return ret;
}

esp_err_t si_agent_repository_clear_memory(bool *cleared)
{
    si_agent_data_clear_result_t result = {0};
    esp_err_t ret = agent_repository_clear(false, true, false, &result);
    if (cleared) {
        *cleared = result.memory_cleared;
    }
    return ret;
}

esp_err_t si_agent_repository_clear_all(si_agent_data_clear_result_t *result)
{
    return agent_repository_clear(true, true, true, result);
}

typedef struct {
    const char *session_id;
    uint32_t count;
} agent_history_count_ctx_t;

static esp_err_t agent_history_count_cb(cJSON *entry, void *user_ctx)
{
    agent_history_count_ctx_t *ctx = (agent_history_count_ctx_t *)user_ctx;
    if (ctx &&
        agent_history_record_matches_session(entry, ctx->session_id) &&
        agent_history_record_is_visible(entry)) {
        ctx->count++;
    }
    return ESP_OK;
}

esp_err_t agent_history_file_count_visible_locked(const char *session_id,
                                                        uint32_t *record_count)
{
    if (!record_count) {
        return ESP_ERR_INVALID_ARG;
    }
    agent_history_count_ctx_t ctx = {
        .session_id = session_id,
        .count = 0,
    };
    esp_err_t ret = agent_history_file_for_each_locked(agent_history_count_cb, &ctx, NULL);
    if (ret == ESP_OK) {
        *record_count = ctx.count;
    }
    return ret;
}

typedef struct {
    cJSON *records;
    const char *session_id;
    uint32_t skip_count;
    uint32_t visible_index;
    uint32_t returned;
} agent_history_add_ctx_t;

static esp_err_t agent_history_add_file_cb(cJSON *entry, void *user_ctx)
{
    agent_history_add_ctx_t *ctx = (agent_history_add_ctx_t *)user_ctx;
    if (!ctx || !ctx->records) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!agent_history_record_matches_session(entry, ctx->session_id) ||
        !agent_history_record_is_visible(entry)) {
        return ESP_OK;
    }
    if (ctx->visible_index++ < ctx->skip_count) {
        return ESP_OK;
    }
    cJSON *copy = cJSON_Duplicate(entry, true);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToArray(ctx->records, copy);
    ctx->returned++;
    return ESP_OK;
}

esp_err_t agent_history_file_add_records_json_locked(cJSON *records,
                                                           uint32_t skip_count,
                                                           const char *session_id,
                                                           uint32_t *returned_out)
{
    if (!records) {
        return ESP_ERR_INVALID_ARG;
    }
    agent_history_add_ctx_t ctx = {
        .records = records,
        .session_id = session_id,
        .skip_count = skip_count,
        .visible_index = 0,
        .returned = 0,
    };
    esp_err_t ret = agent_history_file_for_each_locked(agent_history_add_file_cb, &ctx, NULL);
    if (returned_out) {
        *returned_out = ctx.returned;
    }
    return ret;
}

typedef struct {
    cJSON *records;
    uint32_t skip_count;
    uint32_t visible_index;
    uint32_t returned;
} agent_memory_add_ctx_t;

bool agent_memory_record_is_visible(const cJSON *entry)
{
    if (!entry) {
        return false;
    }
    cJSON *deleted = cJSON_GetObjectItemCaseSensitive(entry, "deleted");
    return !cJSON_IsTrue(deleted);
}

static esp_err_t agent_memory_add_file_cb(cJSON *entry, void *user_ctx)
{
    agent_memory_add_ctx_t *ctx = (agent_memory_add_ctx_t *)user_ctx;
    if (!ctx || !ctx->records) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!agent_memory_record_is_visible(entry)) {
        return ESP_OK;
    }
    if (ctx->visible_index++ < ctx->skip_count) {
        return ESP_OK;
    }
    cJSON *copy = cJSON_Duplicate(entry, true);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToArray(ctx->records, copy);
    ctx->returned++;
    return ESP_OK;
}

esp_err_t agent_memory_file_add_records_json_locked(cJSON *records,
                                                           uint32_t skip_count,
                                                           uint32_t *returned_out)
{
    if (!records) {
        return ESP_ERR_INVALID_ARG;
    }
    agent_memory_add_ctx_t ctx = {
        .records = records,
        .skip_count = skip_count,
        .visible_index = 0,
        .returned = 0,
    };
    esp_err_t ret = agent_memory_file_for_each_locked(agent_memory_add_file_cb, &ctx, NULL);
    if (returned_out) {
        *returned_out = ctx.returned;
    }
    return ret;
}


SemaphoreHandle_t si_agent_repository_lock(void) { return s_agent_history_lock; }
const char *si_agent_repository_last_error(void) { return s_agent_history_last_error; }
