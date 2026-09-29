// Small serial diagnostics and maintenance shell for Stable KVM.
#include "diag_cli.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_manager.h"
#include "power_control.h"
#include "video_input.h"
#include "web_server.h"

static const char *TAG = "si-diag-cli";

#define DIAG_CLI_LINE_MAX 256
// Newlib stdio needs more than 4 KiB on ESP32-P4, even for the initial prompt.
#define DIAG_CLI_TASK_STACK 8192

static TaskHandle_t s_diag_cli_task;

static char *trim(char *line)
{
    while (line && *line && isspace((unsigned char)*line)) {
        line++;
    }
    if (!line) {
        return NULL;
    }
    size_t len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1])) {
        line[--len] = '\0';
    }
    return line;
}

static void print_help(void)
{
    printf("\r\nExoAnchor Stable KVM UART commands:\r\n");
    printf("  help     Show this help\r\n");
    printf("  status   Show KVM runtime status\r\n");
    printf("  sync     Clear a partial command\r\n");
    printf("  reboot   Restart the controller\r\n");
}

static void print_status(void)
{
    si_net_status_t net;
    si_power_status_t power;
    si_video_status_t video;
    si_net_get_status(&net);
    si_power_get_status(&power);
    si_video_get_status(&video);

    printf("version=%s heap=%" PRIu32 " min_heap=%" PRIu32
           " web_clients=%d\r\n",
           SI_BMC_VERSION, esp_get_free_heap_size(),
           esp_get_minimum_free_heap_size(), si_web_client_count());
    printf("net link=%d connected=%d phy=%s ip=%s speed=%dMbps\r\n",
           net.link_up, net.connected, net.phy,
           net.ip[0] ? net.ip : "none", net.speed_mbps);
    printf("video initialized=%d ready=%d %" PRIu32 "x%" PRIu32
           " fps=%.2f error=%s\r\n",
           video.initialized, video.frame_ready, video.width, video.height,
           video.fps_x100 / 100.0,
           video.last_error[0] ? video.last_error : "none");
    printf("power initialized=%d busy=%d pwr_gpio=%d rst_gpio=%d error=%s\r\n",
           power.initialized, power.busy, power.power_button_gpio,
           power.reset_button_gpio,
           power.last_error[0] ? power.last_error : "none");
}

static void run_command(char *line)
{
    char *cmd = trim(line);
    if (!cmd || !cmd[0]) {
        return;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_help();
    } else if (strcmp(cmd, "status") == 0) {
        print_status();
    } else if (strcmp(cmd, "sync") == 0 || strcmp(cmd, "abort") == 0) {
        printf("sync ok\r\n");
    } else if (strcmp(cmd, "reboot") == 0) {
        printf("rebooting...\r\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        printf("unknown command: %s\r\ntype 'help' for commands\r\n", cmd);
    }
}

static void diag_cli_task(void *arg)
{
    (void)arg;
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\r\nExoAnchor Stable KVM UART CLI ready. Type 'help'.\r\n");
    printf("kvm> ");
    fflush(stdout);

    char line[DIAG_CLI_LINE_MAX];
    while (true) {
        if (!fgets(line, sizeof(line), stdin)) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!strchr(line, '\n') && !strchr(line, '\r')) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != '\r' && ch != EOF) {
            }
            printf("input line too long\r\n");
            printf("kvm> ");
            fflush(stdout);
            continue;
        }
        run_command(line);
        printf("kvm> ");
        fflush(stdout);
    }
}

esp_err_t si_diag_cli_start(void)
{
    if (s_diag_cli_task) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(diag_cli_task, "si_diag_cli",
                                DIAG_CLI_TASK_STACK, NULL,
                                tskIDLE_PRIORITY + 1, &s_diag_cli_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create diagnostic CLI task");
    return ESP_OK;
}
