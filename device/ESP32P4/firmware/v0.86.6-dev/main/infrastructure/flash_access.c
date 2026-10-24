#include "flash_access.h"

#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define FLASH_PROXY_STACK_BYTES 4096U
#define FLASH_PROXY_WAIT_MS 10000U

static const char *TAG = "si-flash";

/* Static internal stack: the proxy must exist even when the heap has no
 * contiguous internal block left, which is exactly when callers need it. */
static StackType_t s_proxy_stack[FLASH_PROXY_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_proxy_tcb;
static StaticSemaphore_t s_proxy_lock_buf;
static StaticSemaphore_t s_proxy_done_buf;
static SemaphoreHandle_t s_proxy_lock;
static SemaphoreHandle_t s_proxy_done;
static TaskHandle_t s_proxy_task;
static si_flash_access_fn_t s_pending_fn;
static void *s_pending_ctx;
static esp_err_t s_pending_result;

bool si_flash_access_direct(void)
{
    volatile uint8_t stack_probe = 0;
    return esp_ptr_in_dram((const void *)&stack_probe)
#if CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP
           || esp_ptr_in_rtc_dram_fast((const void *)&stack_probe)
#endif
        ;
}

static void flash_proxy_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_pending_result = s_pending_fn ? s_pending_fn(s_pending_ctx)
                                        : ESP_ERR_INVALID_ARG;
        xSemaphoreGive(s_proxy_done);
    }
}

esp_err_t si_flash_access_init(void)
{
    if (s_proxy_task) {
        return ESP_OK;
    }
    s_proxy_lock = xSemaphoreCreateMutexStatic(&s_proxy_lock_buf);
    s_proxy_done = xSemaphoreCreateBinaryStatic(&s_proxy_done_buf);
    s_proxy_task = xTaskCreateStatic(flash_proxy_task, "si_flash_proxy",
                                     FLASH_PROXY_STACK_BYTES / sizeof(StackType_t),
                                     NULL, tskIDLE_PRIORITY + 4,
                                     s_proxy_stack, &s_proxy_tcb);
    return s_proxy_task ? ESP_OK : ESP_FAIL;
}

esp_err_t si_flash_access_call(si_flash_access_fn_t fn, void *ctx)
{
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (si_flash_access_direct()) {
        return fn(ctx);
    }
    if (!s_proxy_task) {
        ESP_LOGW(TAG, "flash access refused on external-stack task %s",
                 pcTaskGetName(NULL));
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_proxy_lock, pdMS_TO_TICKS(FLASH_PROXY_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_pending_fn = fn;
    s_pending_ctx = ctx;
    xTaskNotifyGive(s_proxy_task);
    /* Wait without a timeout: the operation borrows this stack's buffers and
     * must finish before the caller may return. */
    (void)xSemaphoreTake(s_proxy_done, portMAX_DELAY);
    esp_err_t result = s_pending_result;
    s_pending_fn = NULL;
    s_pending_ctx = NULL;
    xSemaphoreGive(s_proxy_lock);
    return result;
}

#else

esp_err_t si_flash_access_init(void)
{
    return ESP_OK;
}

bool si_flash_access_direct(void)
{
    return true;
}

esp_err_t si_flash_access_call(si_flash_access_fn_t fn, void *ctx)
{
    return fn ? fn(ctx) : ESP_ERR_INVALID_ARG;
}

#endif
