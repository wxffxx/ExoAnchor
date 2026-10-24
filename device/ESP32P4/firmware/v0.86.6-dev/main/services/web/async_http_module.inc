// Long HTTP operations run outside the main esp_http_server task so page
// navigation, status probes, and logout remain responsive.
#define SI_LONG_HTTP_MAX_WORKERS 3
/* A browser reconnect briefly overlaps the retiring and the new multipart
 * stream (Chrome may keep the old <img> connection open for a while); with
 * only two slots the replacement got 503 "slots busy" and the KVM page sat at
 * "connecting". Video workers use PSRAM stacks, so headroom is cheap. */
#define SI_VIDEO_HTTP_MAX_WORKERS 4
#define SI_LONG_HTTP_TASK_STACK_SIZE 12288

typedef esp_err_t (*si_http_handler_fn_t)(httpd_req_t *req);

typedef struct {
    httpd_req_t *req;
    si_http_handler_fn_t handler;
    const char *name;
    SemaphoreHandle_t slot_pool;
} long_http_task_t;

/* Workers whose handlers only touch TF, sockets and RAM run on PSRAM stacks.
 * The Dev H.264 profile keeps code in flash, so any task that may reach NVS or
 * OTA flash still needs an internal stack, and a 12 KiB internal block is
 * often unavailable once both HTTPS servers run. NVS from a PSRAM stack fails
 * closed in settings_store instead of crashing. */
#define SI_HTTP_WORKER_INTERNAL_STACK (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define SI_HTTP_WORKER_PSRAM_STACK (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static SemaphoreHandle_t s_long_http_slots;
static SemaphoreHandle_t s_video_http_slots;

static esp_err_t init_http_worker_slots(void)
{
    if (!s_long_http_slots) {
        s_long_http_slots =
            xSemaphoreCreateCounting(SI_LONG_HTTP_MAX_WORKERS,
                                     SI_LONG_HTTP_MAX_WORKERS);
    }
    if (!s_video_http_slots) {
        s_video_http_slots =
            xSemaphoreCreateCounting(SI_VIDEO_HTTP_MAX_WORKERS,
                                     SI_VIDEO_HTTP_MAX_WORKERS);
    }
    return s_long_http_slots && s_video_http_slots ? ESP_OK : ESP_ERR_NO_MEM;
}

static void long_http_worker(void *arg)
{
    long_http_task_t *task = (long_http_task_t *)arg;
    SemaphoreHandle_t slot_pool = task ? task->slot_pool : NULL;
    if (task && task->handler && task->req) {
        int64_t started_us = esp_timer_get_time();
        esp_err_t ret = task->handler(task->req);
        int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
        if (elapsed_ms > 500) {
            ESP_LOGI(TAG, "async HTTP %s completed in %" PRId64 " ms: %s",
                     task->name ? task->name : "operation", elapsed_ms,
                     esp_err_to_name(ret));
        }
        esp_err_t complete_ret =
            httpd_req_async_handler_complete(task->req);
        if (complete_ret != ESP_OK) {
            ESP_LOGW(TAG, "async HTTP %s completion failed: %s",
                     task->name ? task->name : "operation",
                     esp_err_to_name(complete_ret));
        }
    }
    free(task);
    if (slot_pool) {
        xSemaphoreGive(slot_pool);
    }
    vTaskDeleteWithCaps(NULL);
}

/* A 503 here is invisible to the page; record which operation was refused
 * and why so reconnect stalls can be traced from the device log. */
static void dispatch_http_log_refusal(const char *name, const char *reason,
                                      esp_err_t err)
{
    char line[96];
    snprintf(line, sizeof(line), "%s refused: %s (%s)", name ? name : "?",
             reason, esp_err_to_name(err));
    si_web_log("WARN", line);
}

static esp_err_t dispatch_http_worker(httpd_req_t *req,
                                      si_http_handler_fn_t handler,
                                      const char *name,
                                      SemaphoreHandle_t slot_pool,
                                      UBaseType_t stack_caps)
{
    if (!slot_pool || xSemaphoreTake(slot_pool, 0) != pdTRUE) {
        dispatch_http_log_refusal(name, "slots busy", ESP_ERR_NO_MEM);
        httpd_resp_set_hdr(req, "Retry-After", "1");
        return si_http_send_text_status(req, "503 Service Unavailable",
                                        "long operation slots busy");
    }

    httpd_req_t *async_req = NULL;
    esp_err_t ret = httpd_req_async_handler_begin(req, &async_req);
    if (ret != ESP_OK) {
        dispatch_http_log_refusal(name, "async request unavailable", ret);
        xSemaphoreGive(slot_pool);
        return si_http_send_text_status(req, "503 Service Unavailable",
                                        "async request unavailable");
    }

    long_http_task_t *task = calloc(1, sizeof(*task));
    if (!task) {
        (void)httpd_resp_send_err(async_req,
                                  HTTPD_500_INTERNAL_SERVER_ERROR,
                                  "async request alloc failed");
        (void)httpd_req_async_handler_complete(async_req);
        xSemaphoreGive(slot_pool);
        return ESP_OK;
    }
    task->req = async_req;
    task->handler = handler;
    task->name = name;
    task->slot_pool = slot_pool;

    BaseType_t task_ok =
        xTaskCreateWithCaps(long_http_worker, "si_http_long",
                            SI_LONG_HTTP_TASK_STACK_SIZE, task,
                            tskIDLE_PRIORITY + 2, NULL, stack_caps);
    if (task_ok != pdPASS) {
        dispatch_http_log_refusal(name, "worker unavailable", ESP_ERR_NO_MEM);
        free(task);
        httpd_resp_set_status(async_req, "503 Service Unavailable");
        (void)httpd_resp_sendstr(async_req,
                                 "async worker unavailable");
        (void)httpd_req_async_handler_complete(async_req);
        xSemaphoreGive(slot_pool);
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t dispatch_long_http(httpd_req_t *req,
                                    si_http_handler_fn_t handler,
                                    const char *name, UBaseType_t stack_caps)
{
    return dispatch_http_worker(req, handler, name, s_long_http_slots,
                                stack_caps);
}

#define DEFINE_LONG_HTTP_WRAPPER(wrapper_name, target_handler, operation_name) \
    static esp_err_t wrapper_name(httpd_req_t *req)                           \
    {                                                                          \
        return dispatch_long_http(req, target_handler, operation_name,         \
                                  SI_HTTP_WORKER_INTERNAL_STACK);              \
    }

#define DEFINE_LONG_HTTP_PSRAM_WRAPPER(wrapper_name, target_handler,          \
                                       operation_name)                         \
    static esp_err_t wrapper_name(httpd_req_t *req)                           \
    {                                                                          \
        return dispatch_long_http(req, target_handler, operation_name,         \
                                  SI_HTTP_WORKER_PSRAM_STACK);                 \
    }

DEFINE_LONG_HTTP_WRAPPER(ssh_exec_async_handler, ssh_exec_handler, "ssh.exec")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(storage_upload_async_handler, storage_upload_handler,
                         "storage.upload")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(storage_download_async_handler,
                         storage_download_handler, "storage.download")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(storage_list_async_handler, storage_list_handler,
                         "storage.list")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(storage_view_async_handler, storage_view_handler,
                         "storage.view")
DEFINE_LONG_HTTP_WRAPPER(hid_actions_async_handler, hid_actions_handler,
                         "hid.actions")
DEFINE_LONG_HTTP_WRAPPER(console_login_mcp_async_handler,
                         console_login_mcp_handler, "console.login")
/* Waits up to 5 s for the target's password prompt; keep it off the main
 * HTTPS task. Console credentials come through settings_store's flash proxy,
 * so a PSRAM stack is fine. */
DEFINE_LONG_HTTP_PSRAM_WRAPPER(uart_authenticate_async_handler,
                               uart_authenticate_handler, "uart.authenticate")
#if SI_CFG_MS2109_POWER_ENABLED || SI_CFG_MS2109_TEST_ENABLED
/* A power cycle sleeps for the client-chosen off time (up to 5 s); keep it
 * off the main HTTPS task. GPIO only, so a PSRAM stack is fine. */
DEFINE_LONG_HTTP_PSRAM_WRAPPER(ms2109_power_async_handler, ms2109_power_handler,
                               "ms2109.power")
#endif
DEFINE_LONG_HTTP_WRAPPER(power_action_async_handler, power_action_handler,
                         "power.action")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(snapshot_async_handler, snapshot_handler,
                         "video.snapshot")
static esp_err_t stream_async_handler(httpd_req_t *req)
{
    /* A reconnect briefly overlaps the retiring and replacement stream.
     * Keep those long-lived sockets away from SSH/OTA/snapshot workers so
     * video backpressure cannot starve the control plane. */
    return dispatch_http_worker(req, stream_handler, "video.stream",
                                s_video_http_slots,
                                SI_HTTP_WORKER_PSRAM_STACK);
}
DEFINE_LONG_HTTP_WRAPPER(ota_check_async_handler, ota_check_handler,
                         "ota.check")
DEFINE_LONG_HTTP_WRAPPER(ota_install_async_handler, ota_install_handler,
                         "ota.install")
DEFINE_LONG_HTTP_WRAPPER(ota_upload_async_handler, ota_upload_handler,
                         "ota.upload")
#if SI_CFG_EMBEDDED_AGENT_ENABLED
DEFINE_LONG_HTTP_PSRAM_WRAPPER(agent_sessions_get_async_handler,
                         agent_sessions_get_handler, "agent.sessions.get")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(agent_sessions_post_async_handler,
                         agent_sessions_post_handler, "agent.sessions.post")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(agent_sessions_delete_async_handler,
                         agent_sessions_delete_handler,
                         "agent.sessions.delete")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(agent_history_get_async_handler,
                         agent_history_get_handler, "agent.history.get")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(agent_history_post_async_handler,
                         agent_history_post_handler, "agent.history.post")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(agent_history_clear_async_handler,
                         agent_history_clear_handler, "agent.history.clear")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(agent_memory_get_async_handler,
                         agent_memory_get_handler, "agent.memory.get")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(agent_memory_post_async_handler,
                         agent_memory_post_handler, "agent.memory.post")
DEFINE_LONG_HTTP_PSRAM_WRAPPER(agent_memory_clear_async_handler,
                         agent_memory_clear_handler, "agent.memory.clear")
DEFINE_LONG_HTTP_WRAPPER(agent_data_clear_async_handler,
                         agent_data_clear_handler, "agent.data.clear")
#endif
