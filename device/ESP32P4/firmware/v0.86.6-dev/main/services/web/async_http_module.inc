// Long HTTP operations run outside the main esp_http_server task so page
// navigation, status probes, and logout remain responsive.
#define SI_LONG_HTTP_MAX_WORKERS 3
#define SI_VIDEO_HTTP_MAX_WORKERS 2
#define SI_LONG_HTTP_TASK_STACK_SIZE 12288

typedef esp_err_t (*si_http_handler_fn_t)(httpd_req_t *req);

typedef struct {
    httpd_req_t *req;
    si_http_handler_fn_t handler;
    const char *name;
    SemaphoreHandle_t slot_pool;
} long_http_task_t;

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

static esp_err_t dispatch_http_worker(httpd_req_t *req,
                                      si_http_handler_fn_t handler,
                                      const char *name,
                                      SemaphoreHandle_t slot_pool)
{
    if (!slot_pool || xSemaphoreTake(slot_pool, 0) != pdTRUE) {
        httpd_resp_set_hdr(req, "Retry-After", "1");
        return si_http_send_text_status(req, "503 Service Unavailable",
                                        "long operation slots busy");
    }

    httpd_req_t *async_req = NULL;
    esp_err_t ret = httpd_req_async_handler_begin(req, &async_req);
    if (ret != ESP_OK) {
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
                            tskIDLE_PRIORITY + 2, NULL,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
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
                                    const char *name)
{
    return dispatch_http_worker(req, handler, name, s_long_http_slots);
}

#define DEFINE_LONG_HTTP_WRAPPER(wrapper_name, target_handler, operation_name) \
    static esp_err_t wrapper_name(httpd_req_t *req)                           \
    {                                                                          \
        return dispatch_long_http(req, target_handler, operation_name);        \
    }

DEFINE_LONG_HTTP_WRAPPER(ssh_exec_async_handler, ssh_exec_handler, "ssh.exec")
DEFINE_LONG_HTTP_WRAPPER(storage_upload_async_handler, storage_upload_handler,
                         "storage.upload")
DEFINE_LONG_HTTP_WRAPPER(storage_download_async_handler,
                         storage_download_handler, "storage.download")
DEFINE_LONG_HTTP_WRAPPER(storage_list_async_handler, storage_list_handler,
                         "storage.list")
DEFINE_LONG_HTTP_WRAPPER(storage_view_async_handler, storage_view_handler,
                         "storage.view")
DEFINE_LONG_HTTP_WRAPPER(hid_actions_async_handler, hid_actions_handler,
                         "hid.actions")
DEFINE_LONG_HTTP_WRAPPER(console_login_mcp_async_handler,
                         console_login_mcp_handler, "console.login")
DEFINE_LONG_HTTP_WRAPPER(power_action_async_handler, power_action_handler,
                         "power.action")
DEFINE_LONG_HTTP_WRAPPER(snapshot_async_handler, snapshot_handler,
                         "video.snapshot")
static esp_err_t stream_async_handler(httpd_req_t *req)
{
    /* A reconnect briefly overlaps the retiring and replacement stream.
     * Keep those long-lived sockets away from SSH/OTA/snapshot workers so
     * video backpressure cannot starve the control plane. */
    return dispatch_http_worker(req, stream_handler, "video.stream",
                                s_video_http_slots);
}
DEFINE_LONG_HTTP_WRAPPER(ota_check_async_handler, ota_check_handler,
                         "ota.check")
DEFINE_LONG_HTTP_WRAPPER(ota_install_async_handler, ota_install_handler,
                         "ota.install")
DEFINE_LONG_HTTP_WRAPPER(ota_upload_async_handler, ota_upload_handler,
                         "ota.upload")
#if SI_CFG_EMBEDDED_AGENT_ENABLED
DEFINE_LONG_HTTP_WRAPPER(agent_sessions_get_async_handler,
                         agent_sessions_get_handler, "agent.sessions.get")
DEFINE_LONG_HTTP_WRAPPER(agent_sessions_post_async_handler,
                         agent_sessions_post_handler, "agent.sessions.post")
DEFINE_LONG_HTTP_WRAPPER(agent_sessions_delete_async_handler,
                         agent_sessions_delete_handler,
                         "agent.sessions.delete")
DEFINE_LONG_HTTP_WRAPPER(agent_history_get_async_handler,
                         agent_history_get_handler, "agent.history.get")
DEFINE_LONG_HTTP_WRAPPER(agent_history_post_async_handler,
                         agent_history_post_handler, "agent.history.post")
DEFINE_LONG_HTTP_WRAPPER(agent_history_clear_async_handler,
                         agent_history_clear_handler, "agent.history.clear")
DEFINE_LONG_HTTP_WRAPPER(agent_memory_get_async_handler,
                         agent_memory_get_handler, "agent.memory.get")
DEFINE_LONG_HTTP_WRAPPER(agent_memory_post_async_handler,
                         agent_memory_post_handler, "agent.memory.post")
DEFINE_LONG_HTTP_WRAPPER(agent_memory_clear_async_handler,
                         agent_memory_clear_handler, "agent.memory.clear")
DEFINE_LONG_HTTP_WRAPPER(agent_data_clear_async_handler,
                         agent_data_clear_handler, "agent.data.clear")
#endif
