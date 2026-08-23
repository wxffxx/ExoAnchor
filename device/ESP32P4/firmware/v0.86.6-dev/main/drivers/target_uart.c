#include "target_uart.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define TARGET_UART_DRIVER_RX_BYTES 4096
#define TARGET_UART_BACKLOG_BYTES 8192
#define TARGET_UART_TASK_STACK 4096
#define TARGET_UART_TASK_PRIORITY 10

static const char *TAG = "si-target-uart";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static StreamBufferHandle_t s_http_backlog;
static StreamBufferHandle_t s_cli_backlog;
static TaskHandle_t s_rx_task;
static bool s_initialized;
static int s_baud_rate = SI_CFG_TARGET_UART_BAUD_RATE;
static uint64_t s_rx_bytes;
static uint64_t s_tx_bytes;
static uint64_t s_dropped_bytes;
static uint64_t s_boot_id;
static uint64_t s_generation;
static EXT_RAM_BSS_ATTR uint8_t s_journal[TARGET_UART_BACKLOG_BYTES];
static size_t s_journal_write_pos;
static size_t s_journal_count;
static uint64_t s_journal_next_seq = 1;
static char s_last_error[96];
static si_target_uart_rx_callback_t s_rx_callback;
static void *s_rx_callback_ctx;

static uart_port_t target_uart_port(void)
{
    return (uart_port_t)SI_CFG_TARGET_UART_PORT;
}

static void set_last_error(const char *operation, esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    if (error == ESP_OK) {
        s_last_error[0] = '\0';
    } else {
        snprintf(s_last_error, sizeof(s_last_error), "%s: %s",
                 operation ? operation : "uart", esp_err_to_name(error));
    }
    portEXIT_CRITICAL(&s_lock);
}

static void target_uart_rx_task(void *arg)
{
    (void)arg;
    uint8_t data[256];

    while (true) {
        int received = uart_read_bytes(target_uart_port(), data, sizeof(data),
                                       pdMS_TO_TICKS(100));
        if (received <= 0) {
            continue;
        }

        size_t len = (size_t)received;
        size_t http_buffered =
            xStreamBufferSend(s_http_backlog, data, len, 0);
        size_t cli_buffered =
            xStreamBufferSend(s_cli_backlog, data, len, 0);
        si_target_uart_rx_callback_t callback = NULL;
        void *callback_ctx = NULL;

        portENTER_CRITICAL(&s_lock);
        for (size_t i = 0; i < len; i++) {
            s_journal[s_journal_write_pos] = data[i];
            s_journal_write_pos =
                (s_journal_write_pos + 1U) % sizeof(s_journal);
            if (s_journal_count < sizeof(s_journal)) {
                s_journal_count++;
            }
            s_journal_next_seq++;
        }
        s_rx_bytes += len;
        s_dropped_bytes += (len - http_buffered) + (len - cli_buffered);
        callback = s_rx_callback;
        callback_ctx = s_rx_callback_ctx;
        portEXIT_CRITICAL(&s_lock);

        if (callback) {
            callback(data, len, callback_ctx);
        }
    }
}

esp_err_t si_target_uart_init(void)
{
#if !SI_CFG_TARGET_UART_ENABLED
    set_last_error("disabled", ESP_ERR_NOT_SUPPORTED);
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(SI_CFG_TARGET_UART_PORT > 0 &&
                            SI_CFG_TARGET_UART_PORT < UART_NUM_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART port");
    ESP_RETURN_ON_FALSE(SI_CFG_TARGET_UART_RX_GPIO >= 0 &&
                            SI_CFG_TARGET_UART_TX_GPIO >= 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART pins");

    s_http_backlog = xStreamBufferCreate(TARGET_UART_BACKLOG_BYTES, 1);
    s_cli_backlog = xStreamBufferCreate(TARGET_UART_BACKLOG_BYTES, 1);
    if (!s_http_backlog || !s_cli_backlog) {
        if (s_http_backlog) {
            vStreamBufferDelete(s_http_backlog);
        }
        if (s_cli_backlog) {
            vStreamBufferDelete(s_cli_backlog);
        }
        s_http_backlog = NULL;
        s_cli_backlog = NULL;
        set_last_error("create consumer backlogs", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    uart_config_t config = {
        .baud_rate = SI_CFG_TARGET_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {
            .allow_pd = 0,
            .backup_before_sleep = 0,
        },
    };

    esp_err_t ret = uart_driver_install(target_uart_port(),
                                        TARGET_UART_DRIVER_RX_BYTES, 0,
                                        0, NULL, 0);
    if (ret != ESP_OK) {
        vStreamBufferDelete(s_http_backlog);
        vStreamBufferDelete(s_cli_backlog);
        s_http_backlog = NULL;
        s_cli_backlog = NULL;
        set_last_error("install driver", ret);
        return ret;
    }

    ret = uart_param_config(target_uart_port(), &config);
    if (ret == ESP_OK) {
        ret = uart_set_pin(target_uart_port(),
                           SI_CFG_TARGET_UART_TX_GPIO,
                           SI_CFG_TARGET_UART_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (ret == ESP_OK) {
        ret = uart_set_rx_timeout(target_uart_port(), 2);
    }
    if (ret != ESP_OK) {
        (void)uart_driver_delete(target_uart_port());
        vStreamBufferDelete(s_http_backlog);
        vStreamBufferDelete(s_cli_backlog);
        s_http_backlog = NULL;
        s_cli_backlog = NULL;
        set_last_error("configure driver", ret);
        return ret;
    }

    BaseType_t task_ret = xTaskCreate(target_uart_rx_task, "target-uart-rx",
                                      TARGET_UART_TASK_STACK, NULL,
                                      TARGET_UART_TASK_PRIORITY, &s_rx_task);
    if (task_ret != pdPASS) {
        (void)uart_driver_delete(target_uart_port());
        vStreamBufferDelete(s_http_backlog);
        vStreamBufferDelete(s_cli_backlog);
        s_http_backlog = NULL;
        s_cli_backlog = NULL;
        s_rx_task = NULL;
        set_last_error("create RX task", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_lock);
    s_initialized = true;
    s_baud_rate = SI_CFG_TARGET_UART_BAUD_RATE;
    s_boot_id = ((uint64_t)esp_random() << 32U) | esp_random();
    if (s_boot_id == 0) {
        s_boot_id = 1;
    }
    s_generation = 1;
    s_journal_write_pos = 0;
    s_journal_count = 0;
    s_journal_next_seq = 1;
    s_last_error[0] = '\0';
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "Target UART ready: UART%d RX=GPIO%d TX=GPIO%d %d 8N1",
             SI_CFG_TARGET_UART_PORT, SI_CFG_TARGET_UART_RX_GPIO,
             SI_CFG_TARGET_UART_TX_GPIO, SI_CFG_TARGET_UART_BAUD_RATE);
    return ESP_OK;
#endif
}

esp_err_t si_target_uart_set_baud_rate(int baud_rate)
{
    if (baud_rate < 300 || baud_rate > 3000000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = uart_set_baudrate(target_uart_port(), baud_rate);
    if (ret != ESP_OK) {
        set_last_error("set baud", ret);
        return ret;
    }

    uint8_t stale[64];
    if (s_http_backlog) {
        while (xStreamBufferReceive(s_http_backlog, stale,
                                    sizeof(stale), 0) > 0) {
        }
    }
    if (s_cli_backlog) {
        while (xStreamBufferReceive(s_cli_backlog, stale,
                                    sizeof(stale), 0) > 0) {
        }
    }

    portENTER_CRITICAL(&s_lock);
    s_baud_rate = baud_rate;
    s_generation++;
    if (s_generation == 0) {
        s_generation = 1;
    }
    s_journal_write_pos = 0;
    s_journal_count = 0;
    s_journal_next_seq = 1;
    s_last_error[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void si_target_uart_get_status(si_target_uart_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    status->supported = SI_CFG_TARGET_UART_ENABLED;
    status->port = SI_CFG_TARGET_UART_PORT;
    status->rx_gpio = SI_CFG_TARGET_UART_RX_GPIO;
    status->tx_gpio = SI_CFG_TARGET_UART_TX_GPIO;
    status->default_baud_rate = SI_CFG_TARGET_UART_BAUD_RATE;
    status->fallback_baud_rate = SI_CFG_TARGET_UART_FALLBACK_BAUD_RATE;

    portENTER_CRITICAL(&s_lock);
    status->initialized = s_initialized;
    status->baud_rate = s_baud_rate;
    status->rx_bytes = s_rx_bytes;
    status->tx_bytes = s_tx_bytes;
    status->dropped_bytes = s_dropped_bytes;
    status->boot_id = s_boot_id;
    status->generation = s_generation;
    status->journal_start_seq = s_journal_next_seq - s_journal_count;
    status->journal_next_seq = s_journal_next_seq;
    status->journal_bytes = s_journal_count;
    status->journal_history_lost =
        s_journal_next_seq > (uint64_t)s_journal_count + 1U;
    strlcpy(status->last_error, s_last_error, sizeof(status->last_error));
    portEXIT_CRITICAL(&s_lock);

    status->buffered_bytes = s_http_backlog ?
        xStreamBufferBytesAvailable(s_http_backlog) : 0;
}

size_t si_target_uart_journal_read(uint8_t *data, size_t len,
                                   si_target_uart_journal_t *journal)
{
    if (!journal) {
        return 0;
    }
    memset(journal, 0, sizeof(*journal));
    if (!s_initialized) {
        return 0;
    }

    portENTER_CRITICAL(&s_lock);
    size_t available = s_journal_count;
    size_t copy_len =
        data && len > 0 ? (available < len ? available : len) : 0;
    size_t skip = available - copy_len;
    size_t oldest =
        (s_journal_write_pos + sizeof(s_journal) - available) %
        sizeof(s_journal);
    size_t start = (oldest + skip) % sizeof(s_journal);
    if (data) {
        for (size_t i = 0; i < copy_len; i++) {
            data[i] = s_journal[(start + i) % sizeof(s_journal)];
        }
    }
    journal->boot_id = s_boot_id;
    journal->generation = s_generation;
    journal->next_seq = s_journal_next_seq;
    journal->start_seq = s_journal_next_seq - copy_len;
    journal->bytes = copy_len;
    journal->history_lost =
        s_journal_next_seq > (uint64_t)available + 1U || skip > 0;
    portEXIT_CRITICAL(&s_lock);
    return copy_len;
}

size_t si_target_uart_journal_read_from(uint64_t cursor, uint8_t *data,
                                        size_t len,
                                        si_target_uart_journal_t *journal)
{
    if (!journal) {
        return 0;
    }
    memset(journal, 0, sizeof(*journal));
    if (!s_initialized) {
        return 0;
    }

    portENTER_CRITICAL(&s_lock);
    uint64_t global_next = s_journal_next_seq;
    uint64_t oldest_seq = global_next - s_journal_count;
    bool history_lost = false;
    if (cursor == 0) {
        cursor = oldest_seq;
    } else if (cursor < oldest_seq) {
        cursor = oldest_seq;
        history_lost = true;
    } else if (cursor > global_next) {
        // A future cursor normally belongs to an earlier boot/generation.
        // Replay the retained window and make the discontinuity explicit
        // instead of silently returning an empty result.
        cursor = oldest_seq;
        history_lost = true;
    }

    uint64_t available_u64 = global_next - cursor;
    size_t available =
        available_u64 > SIZE_MAX ? SIZE_MAX : (size_t)available_u64;
    size_t copy_len = data && len > 0 ?
        (available < len ? available : len) : 0;
    size_t oldest =
        (s_journal_write_pos + sizeof(s_journal) - s_journal_count) %
        sizeof(s_journal);
    size_t offset = (size_t)(cursor - oldest_seq);
    size_t start = (oldest + offset) % sizeof(s_journal);
    if (data) {
        for (size_t i = 0; i < copy_len; i++) {
            data[i] = s_journal[(start + i) % sizeof(s_journal)];
        }
    }
    journal->boot_id = s_boot_id;
    journal->generation = s_generation;
    journal->start_seq = cursor;
    journal->next_seq = global_next;
    journal->bytes = copy_len;
    journal->history_lost = history_lost;
    portEXIT_CRITICAL(&s_lock);
    return copy_len;
}

void si_target_uart_set_rx_callback(si_target_uart_rx_callback_t callback,
                                    void *user_ctx)
{
    portENTER_CRITICAL(&s_lock);
    s_rx_callback = callback;
    s_rx_callback_ctx = user_ctx;
    portEXIT_CRITICAL(&s_lock);
}

size_t si_target_uart_read(uint8_t *data, size_t len, TickType_t wait_ticks)
{
    if (!data || len == 0 || !s_initialized || !s_http_backlog) {
        return 0;
    }
    return xStreamBufferReceive(s_http_backlog, data, len, wait_ticks);
}

size_t si_target_uart_read_cli(uint8_t *data, size_t len,
                               TickType_t wait_ticks)
{
    if (!data || len == 0 || !s_initialized || !s_cli_backlog) {
        return 0;
    }
    return xStreamBufferReceive(s_cli_backlog, data, len, wait_ticks);
}

esp_err_t si_target_uart_write(const uint8_t *data, size_t len,
                               size_t *written)
{
    if (written) {
        *written = 0;
    }
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int result = uart_write_bytes(target_uart_port(), data, len);
    if (result < 0) {
        set_last_error("write", ESP_FAIL);
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_lock);
    s_tx_bytes += (size_t)result;
    portEXIT_CRITICAL(&s_lock);
    if (written) {
        *written = (size_t)result;
    }
    return (size_t)result == len ? ESP_OK : ESP_ERR_TIMEOUT;
}
