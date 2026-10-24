// SSH terminal output transport: frames are queued to the HTTPD task that
// owns the WebSocket, with a byte budget that throttles the SSH reader.
typedef struct {
    httpd_handle_t handle;
    int fd;
    size_t len;
    uint8_t payload[];
} ssh_ws_send_job_t;

/* Terminal output is queued to the main HTTPD task one job per SSH read.
 * httpd's control queue has only CONFIG_LWIP_UDP_RECVMBOX_SIZE (6) slots,
 * shared with the UART bridge and httpd's own session-close messages, so cap
 * SSH at a few outstanding jobs. A flood (cat of a large file) then pauses
 * the SSH reader instead of overflowing the queue and dropping output. */
#define SSH_WS_OUTPUT_MAX_JOBS 3U
#define SSH_WS_OUTPUT_BACKPRESSURE_MS 3000U

static uint32_t s_ssh_ws_inflight_jobs;

static void ssh_ws_send_work(void *arg)
{
    ssh_ws_send_job_t *job = (ssh_ws_send_job_t *)arg;
    if (!job) {
        return;
    }
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = job->payload,
        .len = job->len,
    };
    (void)httpd_ws_send_frame_async(job->handle, job->fd, &frame);
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
                                            const void *data, size_t len)
{
    if (!handle || fd < 0 || !data || len == 0) {
        return NULL;
    }
    ssh_ws_send_job_t *job = heap_caps_malloc(sizeof(*job) + len,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job) {
        job->handle = handle;
        job->fd = fd;
        job->len = len;
        memcpy(job->payload, data, len);
    }
    return job;
}

/* Non-blocking: also called from HTTPD handlers. */
static esp_err_t ssh_ws_send_async(httpd_handle_t handle, int fd, const void *data, size_t len)
{
    ssh_ws_send_job_t *job = ssh_ws_job_create(handle, fd, data, len);
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
                                             const void *data, size_t len)
{
    ssh_ws_send_job_t *job = ssh_ws_job_create(handle, fd, data, len);
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
