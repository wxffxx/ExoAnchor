// SSH terminal output transport: frames are queued to the HTTPD task that
// owns the WebSocket, with a byte budget that throttles the SSH reader.
#define SSH_WS_MAX_FRAME 16384
#define SSH_WS_WRITE_CHUNK 1024
#define SSH_WS_WRITE_CHUNK_TIMEOUT_MS 200
typedef struct {
    httpd_handle_t handle;
    int fd;
    si_ssh_shell_t *shell;
    uint32_t auth_generation;
    char session_id[SI_AUTH_SESSION_ID_MAX_LEN + 1];
} ssh_ws_ctx_t;

static SemaphoreHandle_t s_ssh_ws_lock;
static httpd_handle_t s_ssh_ws_handle;
static int s_ssh_ws_fd = -1;
static uint32_t s_ssh_ws_generation;
static char s_ssh_ws_session_id[SI_AUTH_SESSION_ID_MAX_LEN + 1];

#define SSH_WS_CLEANUP_TASK_STACK 4096
#define SSH_WS_REAPER_QUEUE_LEN 4U

static QueueHandle_t s_ssh_ws_reaper_queue;
static bool s_ssh_ws_revoked;

typedef struct {
    httpd_handle_t handle;
    int fd;
    uint32_t epoch;
    bool binary;
    size_t len;
    uint8_t payload[];
} ssh_ws_send_job_t;

/* Bumped on every published connection so a job queued for a closed socket
 * cannot reach whatever connection reuses its fd. */
static uint32_t s_ssh_ws_epoch;

static void ssh_ws_publish_connection(httpd_handle_t handle, int fd)
{
    s_ssh_ws_handle = handle;
    s_ssh_ws_fd = fd;
    s_ssh_ws_revoked = false;
    __atomic_add_fetch(&s_ssh_ws_epoch, 1U, __ATOMIC_ACQ_REL);
}

static bool ssh_ws_job_is_current(const ssh_ws_send_job_t *job)
{
    bool current = false;
    if (s_ssh_ws_lock &&
        xSemaphoreTake(s_ssh_ws_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        current = s_ssh_ws_handle == job->handle && s_ssh_ws_fd == job->fd &&
                  s_ssh_ws_epoch == job->epoch && !s_ssh_ws_revoked;
        xSemaphoreGive(s_ssh_ws_lock);
    }
    return current;
}

/* Terminal output is queued to the main HTTPD task one job per SSH read.
 * httpd's control queue has only CONFIG_LWIP_UDP_RECVMBOX_SIZE (6) slots,
 * shared with the UART bridge and httpd's own session-close messages, so cap
 * SSH at a few outstanding jobs. A flood (cat of a large file) then pauses
 * the SSH reader instead of overflowing the queue and dropping output. */
#define SSH_WS_OUTPUT_MAX_JOBS 3U
#define SSH_WS_OUTPUT_BACKPRESSURE_MS 3000U

static uint32_t s_ssh_ws_inflight_jobs;

/* httpd closes a session whose handler fails, so an oversized terminal frame
 * is read and dropped instead of returning ESP_ERR_INVALID_SIZE. */
#define TERMINAL_WS_DISCARD_MAX (256U * 1024U)

static esp_err_t terminal_ws_discard_frame(httpd_req_t *req,
                                           httpd_ws_frame_t *frame)
{
    if (frame->len > TERMINAL_WS_DISCARD_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *sink = heap_caps_malloc(frame->len,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sink) {
        return ESP_ERR_NO_MEM;
    }
    frame->payload = sink;
    esp_err_t ret = httpd_ws_recv_frame(req, frame, frame->len);
    heap_caps_free(sink);
    return ret;
}

static void ssh_ws_send_work(void *arg)
{
    ssh_ws_send_job_t *job = (ssh_ws_send_job_t *)arg;
    if (!job) {
        return;
    }
    httpd_ws_frame_t frame = {
        .type = job->binary ? HTTPD_WS_TYPE_BINARY : HTTPD_WS_TYPE_TEXT,
        .payload = job->payload,
        .len = job->len,
    };
    if (ssh_ws_job_is_current(job)) {
        (void)httpd_ws_send_frame_async(job->handle, job->fd, &frame);
    }
    __atomic_sub_fetch(&s_ssh_ws_inflight_jobs, 1U, __ATOMIC_RELEASE);
    heap_caps_free(job);
}

static esp_err_t ssh_ws_queue_job(ssh_ws_send_job_t *job)
{
    __atomic_add_fetch(&s_ssh_ws_inflight_jobs, 1U, __ATOMIC_ACQ_REL);
    esp_err_t ret = httpd_queue_work(job->handle, ssh_ws_send_work, job);
    if (ret != ESP_OK) {
        __atomic_sub_fetch(&s_ssh_ws_inflight_jobs, 1U, __ATOMIC_RELEASE);
    }
    return ret;
}

static ssh_ws_send_job_t *ssh_ws_job_create(httpd_handle_t handle, int fd,
                                            const void *data, size_t len,
                                            bool binary)
{
    if (!handle || fd < 0 || !data || len == 0) {
        return NULL;
    }
    ssh_ws_send_job_t *job = heap_caps_malloc(sizeof(*job) + len,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job) {
        job->handle = handle;
        job->fd = fd;
        job->epoch = __atomic_load_n(&s_ssh_ws_epoch, __ATOMIC_ACQUIRE);
        job->binary = binary;
        job->len = len;
        memcpy(job->payload, data, len);
    }
    return job;
}

/* Non-blocking: also called from HTTPD handlers. */
static esp_err_t ssh_ws_send_async(httpd_handle_t handle, int fd,
                                   const void *data, size_t len, bool binary)
{
    ssh_ws_send_job_t *job = ssh_ws_job_create(handle, fd, data, len, binary);
    if (!job) {
        return handle && fd >= 0 && data && len ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ssh_ws_queue_job(job);
    if (ret != ESP_OK) {
        heap_caps_free(job);
    }
    return ret;
}

/* SSH shell task only: waits for a job slot and retries a full control queue,
 * so the remote side is throttled by TCP instead of output being dropped. */
static esp_err_t ssh_ws_send_output_blocking(httpd_handle_t handle, int fd,
                                             const void *data, size_t len,
                                             bool binary)
{
    ssh_ws_send_job_t *job = ssh_ws_job_create(handle, fd, data, len, binary);
    if (!job) {
        return handle && fd >= 0 && data && len ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    const int64_t deadline_us = esp_timer_get_time() +
                                (int64_t)SSH_WS_OUTPUT_BACKPRESSURE_MS * 1000;
    esp_err_t ret = ESP_ERR_TIMEOUT;
    while (esp_timer_get_time() < deadline_us) {
        if (__atomic_load_n(&s_ssh_ws_inflight_jobs, __ATOMIC_ACQUIRE) <
                SSH_WS_OUTPUT_MAX_JOBS &&
            (ret = ssh_ws_queue_job(job)) == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    heap_caps_free(job);
    return ret;
}

static esp_err_t ssh_ws_send_terminal_frame(const void *data, size_t len,
                                            bool binary)
{
    httpd_handle_t handle = NULL;
    int fd = -1;
    if (!data || len == 0 || !s_ssh_ws_lock ||
        xSemaphoreTake(s_ssh_ws_lock, pdMS_TO_TICKS(20)) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    handle = s_ssh_ws_handle;
    fd = s_ssh_ws_fd;
    xSemaphoreGive(s_ssh_ws_lock);
    return handle && fd >= 0 ? ssh_ws_send_async(handle, fd, data, len, binary) :
                              ESP_ERR_NOT_FOUND;
}

static esp_err_t ssh_ws_send_terminal_json(const char *json, size_t len)
{
    return ssh_ws_send_terminal_frame(json, len, false);
}

static esp_err_t ssh_ws_send_terminal_output(const void *data, size_t len)
{
    return ssh_ws_send_terminal_frame(data, len, true);
}

static esp_err_t ssh_ws_send_str(ssh_ws_ctx_t *ctx, const char *text)
{
    return text ? ssh_ws_send_async(ctx->handle, ctx->fd, text, strlen(text), false) : ESP_ERR_INVALID_ARG;
}

static esp_err_t ssh_ws_output_cb(const uint8_t *data, size_t len, bool remote,
                                  void *user_ctx)
{
    ssh_ws_ctx_t *ctx = (ssh_ws_ctx_t *)user_ctx;
    if (!ctx || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Runs on the SSH shell task: waiting here stops reading the channel. */
    return ssh_ws_send_output_blocking(ctx->handle, ctx->fd, data, len, remote);
}
