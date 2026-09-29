// Serial diagnostics and maintenance shell.
#include "diag_cli.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "app_config.h"
#include "auth_service.h"
#include "diagnostics_service.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#if SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED
#include "ms2109_eeprom_emulator.h"
#endif
#if SI_CFG_MS2109_POWER_ENABLED
#include "ms2109_power.h"
#endif
#if SI_CFG_MS2109_TEST_ENABLED
#include "ms2109_test.h"
#endif
#include "net_manager.h"
#include "power_control.h"
#include "storage_manager.h"
#include "target_uart.h"
#include "video_input.h"
#if SI_CFG_VIDEO_H264_ENABLED
#include "video_h264_stream.h"
#endif

static const char *TAG = "si-diag-cli";

#define DIAG_CLI_LINE_MAX 4096
#define DIAG_CLI_OUT_MAX 8192
#define DIAG_CLI_AGENT_PROMPT_MAX 8192
#define DIAG_CLI_AGENT_B64_MAX ((((DIAG_CLI_AGENT_PROMPT_MAX) + 2) / 3) * 4 + 8)
#define DIAG_CLI_AGENT_RUN_ID_MAX SI_DIAGNOSTICS_AGENT_RUN_ID_MAX
#define DIAG_CLI_UNKNOWN_PREVIEW_MAX 96
#define DIAG_CLI_UNKNOWN_FLUSH_THRESHOLD 48
#define DIAG_CLI_TASK_STACK 16384
#define DIAG_CLI_AGENT_WATCH_MAX_SECONDS 900
#define DIAG_CLI_TF_LS_PATH_MAX 192
#define DIAG_CLI_TF_LS_MAX_ITEMS 80

static TaskHandle_t s_diag_cli_task;
static bool s_input_quarantine;

typedef struct {
    char session[32];
    char profile[32];
    char model[96];
    bool dry_run;
    bool screenshot;
    bool web_search;
} diag_agent_opts_t;

typedef struct {
    bool active;
    bool base64;
    diag_agent_opts_t opts;
    char *text;
    size_t len;
    size_t cap;
} diag_agent_prompt_capture_t;

static diag_agent_prompt_capture_t s_prompt_capture;

static char *trim(char *line)
{
    if (!line) {
        return line;
    }
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    size_t len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1])) {
        line[--len] = '\0';
    }
    return line;
}

static void print_help(void)
{
    printf("\r\nExoAnchor UART CLI commands:\r\n");
    printf("  help                 Show this help\r\n");
    printf("  status               Basic firmware/runtime status\r\n");
    printf("  network show         Show runtime, active, and staged network configuration\r\n");
    printf("  network dhcp [hostname]\r\n");
    printf("                       Stage DHCP + AutoIP fallback configuration\r\n");
    printf("  network static <ip> <mask> <gateway|-> <dns1|-> [dns2|-]\r\n");
    printf("                       Stage static IPv4 configuration\r\n");
    printf("  network hostname <name>\r\n");
    printf("                       Stage a hostname change without changing address mode\r\n");
    printf("  network apply        Apply staged config for 120 seconds pending confirmation\r\n");
    printf("  network commit       Confirm pending config as last-good\r\n");
    printf("  network rollback     Discard staged/pending config and restore last-good\r\n");
    printf("  network reset CONFIRM\r\n");
    printf("                       Restore factory DHCP + AutoIP + derived hostname\r\n");
    printf("  target-uart          Show target UART state and counters\r\n");
    printf("  target-uart-read     Drain up to 512 buffered target UART bytes as base64\r\n");
    printf("  target-uart-write <text>\r\n");
    printf("                       Write text to target UART without an implicit newline\r\n");
    printf("  target-uart-enter    Write one carriage return to target UART\r\n");
    printf("  target-uart-baud <rate>\r\n");
    printf("                       Temporarily change target UART baud rate until reboot\r\n");
    printf("  target-uart-fast     Switch to the configured primary baud rate\r\n");
    printf("  target-uart-fallback Switch to the configured low-speed fallback\r\n");
#if SI_CFG_MS2109_EEPROM_WP_GPIO >= 0
    printf("  eeprom-wp status     Show MS2109 EEPROM write-protect input level\r\n");
    printf("  eeprom-wp write-enable\r\n");
    printf("                       Drive EEPROM WP low until protect or reboot\r\n");
    printf("  eeprom-wp protect    Release WP so the board pull-up protects EEPROM\r\n");
#endif
#if SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED
    printf("  eeprom-emu status    Show virtual EEPROM bus activity and state\r\n");
    printf("  eeprom-emu bias-test Test continuity to the board's 4.7k pull-ups\r\n");
    printf("  eeprom-emu sda-low   Hold SDA low through open drain until reboot\r\n");
    printf("  eeprom-emu scl-low   Hold SCL low through open drain until reboot\r\n");
#endif
#if SI_CFG_MS2109_POWER_ENABLED || SI_CFG_MS2109_TEST_ENABLED
    printf("  ms-switch status|on|off|cycle [off-ms]\r\n");
#if SI_CFG_MS2109_TEST_ENABLED
    printf("  ms-switch cycle-seq 3v3-first|core-pre <delay-ms> [off-ms]\r\n");
#endif
    printf("                       Control the V2.4 TPS22918 MS2109 load switch\r\n");
#endif
#if SI_CFG_MS2109_TEST_ENABLED
    printf("  ms-eeprom status     Probe all AT24C16 block addresses (briefly powers MS off)\r\n");
    printf("  ms-eeprom dump-b64   Read the complete 2 KiB EEPROM as base64\r\n");
    printf("  ms-eeprom program-b64 <base64> CONFIRM\r\n");
    printf("                       Program and verify one complete 2 KiB EEPROM image\r\n");
#endif
    printf("  video                Show UVC mode and latest JPEG evidence\r\n");
    printf("  video-sample [sec]   Sample JPEG SOF dimensions and frame sizes (default 5s)\r\n");
#if SI_CFG_VIDEO_H264_ENABLED
    printf("  video-h264-test      Decode once and encode 120 frames; report sustained H.264 evidence\r\n");
    printf("  video-h264-test-file <path>\r\n");
    printf("                       Run the same codec test with a JPEG under /sdcard/EA\r\n");
#endif
    printf("  video-mode W H FPSx100\r\n");
    printf("                       Temporarily select an exact UVC mode; does not write NVS\r\n");
    printf("  tf                   Show TF card mount and usage\r\n");
    printf("  tf-ls [path]         List /sdcard/EA or a child directory\r\n");
    printf("  tf-rm <file>         Remove one file under /sdcard/EA\r\n");
    printf("  tf-rm <file>         Remove one file under /sdcard/EA\r\n");
    printf("  hist                 Check Agent history file\r\n");
    printf("  hist-append <text>   Append one Agent history test record\r\n");
    printf("  hist-clear           Clear Agent history file on TF card\r\n");
    printf("  mem                  Check Agent working memory file\r\n");
    printf("  mem-append <text>    Append one Agent working memory note\r\n");
    printf("  mem-clear            Clear Agent working memory file on TF card\r\n");
    printf("  agent-data-clear CONFIRM\r\n");
    printf("                       Permanently clear every Agent conversation and memory record\r\n");
    printf("  auth-reset CONFIRM   Generate a new six-digit web bootstrap password\r\n");
    printf("  ssh-target           Show Settings SSH target and key state\r\n");
    printf("  ssh <command>        Execute command on Settings SSH target using local credential\r\n");
    printf("  agent-run [opts] <prompt>\r\n");
    printf("                       Start headless Agent run; default session=default; opts: --execute --dry-run --profile P --model M --session S --screenshot --web-search\r\n");
    printf("  agent-run-b64 [opts] <base64-prompt>\r\n");
    printf("                       Start Agent run with UTF-8 prompt encoded as base64\r\n");
    printf("  agent-prompt-begin [opts]\r\n");
    printf("                       Enter multiline prompt mode; finish with agent-prompt-end\r\n");
    printf("  agent-b64-begin [opts]\r\n");
    printf("                       Enter chunked base64 prompt mode; finish with agent-b64-end\r\n");
    printf("  agent-status [run_id]\r\n");
    printf("                       Show exact run status; no run_id keeps the read-only legacy view\r\n");
    printf("  agent-events <run_id> [after_seq]\r\n");
    printf("                       Print exact-run execution events newer than after_seq\r\n");
    printf("  agent-log [seq]      Legacy read-only current-run event view\r\n");
    printf("  agent-watch [run_id] [sec]\r\n");
    printf("                       Human observation only: blocks UART command input while following\r\n");
    printf("                       Automation must poll agent-events so controls remain interactive\r\n");
    printf("  agent-pause <run_id> Pause an exact active run at its next safe checkpoint\r\n");
    printf("  agent-resume <run_id>\r\n");
    printf("                       Resume an exact paused or reboot-recovered run\r\n");
    printf("  agent-cancel <run_id>\r\n");
    printf("  agent-abort <run_id> Request exact-run fail-stop cancellation (aliases)\r\n");
    printf("  agent-steer <run_id> <message>\r\n");
    printf("                       Add an instruction at the exact run's next safe checkpoint\r\n");
    printf("  agent-result <run_id> [offset] [max-bytes]\r\n");
    printf("                       Read complete result JSON as reconstructable base64 chunks; default 4096, max 5600 raw bytes\r\n");
    printf("  sync                 Clear partial UART input and print sync marker\r\n");
    printf("  abort                Alias for sync; also exits prompt capture\r\n");
    printf("  reboot               Restart MCU\r\n");
}

static void print_tf_status(void)
{
    si_storage_tf_status_t tf;
    si_storage_get_tf_status(&tf);
    printf("tf supported=%d mounted=%d mount=%s name=%s type=%s\r\n",
           tf.supported, tf.mounted, tf.mount_point, tf.card_name, tf.card_type);
    printf("tf mode=%s degraded=%d width=%u freq=%" PRIu32 "kHz fallback=%s\r\n",
           tf.bus_mode, tf.degraded_mode, (unsigned)tf.bus_width,
           tf.bus_frequency_khz,
           tf.fallback_reason[0] ? tf.fallback_reason : "none");
    printf("tf total=%llu used=%llu free=%llu usage=%.2f%% cluster=%" PRIu32 " sector=%" PRIu32 "\r\n",
           (unsigned long long)tf.total_bytes,
           (unsigned long long)tf.used_bytes,
           (unsigned long long)tf.free_bytes,
           tf.usage_percent,
           tf.cluster_size,
           tf.sector_size);
    if (tf.last_error[0]) {
        printf("tf error=%s\r\n", tf.last_error);
    }
}

static const char *diag_file_type(const struct stat *st)
{
    if (!st) {
        return "unknown";
    }
    if (S_ISDIR(st->st_mode)) {
        return "dir";
    }
    if (S_ISREG(st->st_mode)) {
        return "file";
    }
    return "other";
}

static esp_err_t diag_tf_resolve_path(const char *input, char *rel, size_t rel_size,
                                      char *abs, size_t abs_size)
{
    if (!rel || !abs || rel_size == 0 || abs_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    rel[0] = '\0';
    abs[0] = '\0';

    const char *p = input && input[0] ? input : "";
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    while (*p == '/') {
        p++;
    }
    if (strncasecmp(p, "sdcard/", 7) == 0) {
        p += 7;
    }
    if (strncasecmp(p, "EA", 2) == 0 && (p[2] == '\0' || p[2] == '/')) {
        p += 2;
        if (*p == '/') {
            p++;
        }
    }

    size_t len = strlen(p);
    while (len > 0 && isspace((unsigned char)p[len - 1])) {
        len--;
    }
    if (len >= rel_size || memchr(p, '\\', len) || memchr(p, ':', len)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i + 1 < len; i++) {
        if (p[i] == '.' && p[i + 1] == '.') {
            return ESP_ERR_INVALID_ARG;
        }
    }
    memcpy(rel, p, len);
    rel[len] = '\0';

    int written = rel[0] ?
        snprintf(abs, abs_size, "%s/%s", SI_STORAGE_ROOT, rel) :
        snprintf(abs, abs_size, "%s", SI_STORAGE_ROOT);
    if (written < 0 || (size_t)written >= abs_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void print_tf_list(const char *tail)
{
    si_storage_tf_status_t tf;
    si_storage_get_tf_status(&tf);
    if (!tf.mounted) {
        printf("tf not mounted: %s\r\n", tf.last_error[0] ? tf.last_error : "no card");
        return;
    }

    char rel[DIAG_CLI_TF_LS_PATH_MAX] = {0};
    char abs[DIAG_CLI_TF_LS_PATH_MAX + sizeof(SI_STORAGE_ROOT) + 4] = {0};
    esp_err_t ret = diag_tf_resolve_path(tail, rel, sizeof(rel), abs, sizeof(abs));
    if (ret != ESP_OK) {
        printf("tf-ls invalid path: %s\r\n", esp_err_to_name(ret));
        return;
    }

    DIR *dir = opendir(abs);
    if (!dir) {
        printf("tf-ls open failed path=/EA%s%s errno=%d\r\n",
               rel[0] ? "/" : "", rel, errno);
        return;
    }

    printf("tf-ls /EA%s%s\r\n", rel[0] ? "/" : "", rel);
    uint32_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (count >= DIAG_CLI_TF_LS_MAX_ITEMS) {
            printf("... truncated at %u entries\r\n", DIAG_CLI_TF_LS_MAX_ITEMS);
            break;
        }

        char child[DIAG_CLI_TF_LS_PATH_MAX + sizeof(SI_STORAGE_ROOT) + 4] = {0};
        int written = snprintf(child, sizeof(child), "%s/%s", abs, ent->d_name);
        struct stat st;
        bool have_stat = written > 0 && (size_t)written < sizeof(child) &&
                         stat(child, &st) == 0;
        printf("  %-5s %10llu  %s\r\n",
               have_stat ? diag_file_type(&st) : "?",
               (unsigned long long)(have_stat && S_ISREG(st.st_mode) ? st.st_size : 0),
               ent->d_name);
        count++;
    }
    closedir(dir);
    printf("tf-ls count=%" PRIu32 "\r\n", count);
}

static void remove_tf_file(const char *tail)
{
    si_storage_tf_status_t tf;
    si_storage_get_tf_status(&tf);
    if (!tf.mounted) {
        printf("tf-rm not mounted: %s\r\n", tf.last_error[0] ? tf.last_error : "no card");
        return;
    }

    char rel[DIAG_CLI_TF_LS_PATH_MAX] = {0};
    char abs[DIAG_CLI_TF_LS_PATH_MAX + sizeof(SI_STORAGE_ROOT) + 4] = {0};
    esp_err_t ret = diag_tf_resolve_path(tail, rel, sizeof(rel), abs, sizeof(abs));
    if (ret != ESP_OK || !rel[0]) {
        printf("tf-rm invalid path: %s\r\n", esp_err_to_name(ret));
        return;
    }

    struct stat st;
    if (stat(abs, &st) != 0) {
        printf("tf-rm missing path=/EA/%s errno=%d\r\n", rel, errno);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        printf("tf-rm refused non-file path=/EA/%s\r\n", rel);
        return;
    }
    if (remove(abs) != 0) {
        printf("tf-rm failed path=/EA/%s errno=%d\r\n", rel, errno);
        return;
    }
    printf("tf-rm ok path=/EA/%s size=%llu\r\n", rel, (unsigned long long)st.st_size);
}

static void print_status(void)
{
    si_net_status_t net;
    si_power_status_t power;
    si_net_get_status(&net);
    si_power_get_status(&power);

    printf("version=%s board=%s heap=%" PRIu32 " min_heap=%" PRIu32
           " web_clients=%d\r\n",
           SI_BMC_VERSION,
           SI_BOARD_ID,
           esp_get_free_heap_size(),
           esp_get_minimum_free_heap_size(),
           si_diagnostics_web_client_count());
    printf("net configured=%d initialized=%d link=%d connected=%d "
           "mode=%s source=%s state=%s ip=%s speed=%dMbps duplex=%s\r\n",
           net.configured,
           net.initialized,
           net.link_up,
           net.connected,
           net.mode[0] ? net.mode : "none",
           net.address_source[0] ? net.address_source : "none",
           net.config_state[0] ? net.config_state : "none",
           net.ip[0] ? net.ip : "none",
           net.speed_mbps,
           net.full_duplex ? "full" : "half");
    printf("power initialized=%d busy=%d pwr_gpio=%d rst_gpio=%d actions=%" PRIu32
           " last=%s error=%s\r\n",
           power.initialized,
           power.busy,
           power.power_button_gpio,
           power.reset_button_gpio,
           power.action_count,
           power.last_action[0] ? power.last_action : "none",
           power.last_error[0] ? power.last_error : "none");
    printf("detect power_supported=%d power_active=%d power_gpio=%d power_level=%s "
           "standby_supported=%d standby_active=%d standby_gpio=%d standby_level=%s\r\n",
           power.power_detect_supported,
           power.power_on,
           power.power_detect_gpio,
           power.power_detect_active_high ? "high" : "low",
           power.standby_detect_supported,
           power.standby_on,
           power.standby_detect_gpio,
           power.standby_detect_active_high ? "high" : "low");
}

static void print_target_uart_status(void)
{
    si_target_uart_status_t status;
    si_target_uart_get_status(&status);
    printf("target-uart supported=%d initialized=%d port=%d rx_gpio=%d tx_gpio=%d "
           "baud=%d default_baud=%d fallback_baud=%d format=8N1 "
           "rx_bytes=%" PRIu64 " tx_bytes=%" PRIu64
           " buffered=%" PRIu64 " dropped=%" PRIu64 " error=%s\r\n",
           status.supported,
           status.initialized,
           status.port,
           status.rx_gpio,
           status.tx_gpio,
           status.baud_rate,
           status.default_baud_rate,
           status.fallback_baud_rate,
           status.rx_bytes,
           status.tx_bytes,
           status.buffered_bytes,
           status.dropped_bytes,
           status.last_error[0] ? status.last_error : "none");
}

static void read_target_uart_backlog(void)
{
    uint8_t data[512];
    unsigned char encoded[((sizeof(data) + 2) / 3) * 4 + 1];
    size_t len = si_target_uart_read_cli(data, sizeof(data), 0);
    if (len == 0) {
        printf("target-uart-read bytes=0 base64=\r\n");
        return;
    }

    size_t encoded_len = 0;
    int ret = mbedtls_base64_encode(encoded, sizeof(encoded) - 1,
                                    &encoded_len, data, len);
    if (ret != 0) {
        printf("target-uart-read failed base64_error=%d bytes=%u\r\n",
               ret, (unsigned)len);
        return;
    }
    encoded[encoded_len] = '\0';
    printf("target-uart-read bytes=%u base64=%s\r\n",
           (unsigned)len, (const char *)encoded);
}

static void write_target_uart_data(const uint8_t *data, size_t len)
{
    size_t written = 0;
    esp_err_t ret = si_target_uart_write(data, len, &written);
    printf("target-uart-write requested=%u written=%u result=%s\r\n",
           (unsigned)len, (unsigned)written, esp_err_to_name(ret));
}

static bool diag_jpeg_sof_dimensions(const uint8_t *data, size_t len,
                                     uint32_t *out_width, uint32_t *out_height)
{
    if (!data || len < 4 || !out_width || !out_height ||
        data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }

    size_t pos = 2;
    while (pos + 1 < len) {
        if (data[pos] != 0xFF) {
            return false;
        }
        while (pos < len && data[pos] == 0xFF) {
            pos++;
        }
        if (pos >= len) {
            return false;
        }

        uint8_t marker = data[pos++];
        if (marker == 0xD9 || marker == 0xDA) {
            return false;
        }
        if (marker == 0x01 || marker == 0xD8 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (pos + 2 > len) {
            return false;
        }

        uint16_t segment_len = ((uint16_t)data[pos] << 8) | data[pos + 1];
        if (segment_len < 2 || pos + segment_len > len) {
            return false;
        }
        bool is_sof = marker >= 0xC0 && marker <= 0xCF &&
                      marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (is_sof) {
            if (segment_len < 8) {
                return false;
            }
            *out_height = ((uint32_t)data[pos + 3] << 8) | data[pos + 4];
            *out_width = ((uint32_t)data[pos + 5] << 8) | data[pos + 6];
            return *out_width > 0 && *out_height > 0;
        }
        pos += segment_len;
    }
    return false;
}

static void print_video_status(void)
{
    si_video_status_t video;
    si_video_get_status(&video);
    printf("video enabled=%d initialized=%d streaming=%d ready=%d capture=%d owner=%s\r\n",
           video.enabled, video.initialized, video.streaming, video.frame_ready,
           video.capture_enabled, video.capture_owner[0] ? video.capture_owner : "none");
    printf("video active=%" PRIu32 "x%" PRIu32 " %s %.2ffps "
           "target=%" PRIu32 "x%" PRIu32 "@%.2f stride=%" PRIu32 "ms modes=%" PRIu32 "\r\n",
           video.width, video.height, video.pixel_format,
           video.fps_x100 / 100.0,
           video.target_width, video.target_height, video.target_fps_x100 / 100.0,
           video.capture_stride_ms, video.modes_count);
    printf("video frames captured=%" PRIu32 " accepted=%" PRIu32 " dropped=%" PRIu32
           " last_jpeg=%" PRIu32 "B last_frame=%" PRIu32 "ms error=%s\r\n",
           video.frames_captured, video.frames_encoded, video.frames_dropped,
           video.last_jpeg_size, video.last_frame_ms,
           video.last_error[0] ? video.last_error : "none");

    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    uint32_t frame_id = 0;
    esp_err_t ret = si_video_acquire_jpeg(&jpeg, &jpeg_len, &frame_id);
    if (ret != ESP_OK) {
        printf("video jpeg unavailable=%s\r\n", esp_err_to_name(ret));
        return;
    }
    uint32_t jpeg_width = 0;
    uint32_t jpeg_height = 0;
    bool has_sof = diag_jpeg_sof_dimensions(jpeg, jpeg_len, &jpeg_width, &jpeg_height);
    printf("video jpeg frame_id=%" PRIu32 " bytes=%u sof=%s",
           frame_id, (unsigned)jpeg_len, has_sof ? "valid" : "invalid");
    if (has_sof) {
        printf(" dimensions=%" PRIu32 "x%" PRIu32 " matches_active=%d",
               jpeg_width, jpeg_height,
               jpeg_width == video.width && jpeg_height == video.height);
    }
    printf("\r\n");
    free(jpeg);
}

static void sample_video_frames(const char *tail)
{
    int seconds = atoi(tail ? tail : "");
    if (seconds <= 0) {
        seconds = 5;
    }
    if (seconds > 30) {
        seconds = 30;
    }

    si_video_status_t start;
    si_video_get_status(&start);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)seconds * 1000U);
    uint32_t last_frame_id = 0;
    uint32_t first_frame_id = 0;
    uint32_t samples = 0;
    uint32_t sof_invalid = 0;
    uint32_t sof_mismatch = 0;
    uint32_t observed_width = 0;
    uint32_t observed_height = 0;
    size_t size_min = SIZE_MAX;
    size_t size_max = 0;
    uint64_t size_total = 0;

    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        uint8_t *jpeg = NULL;
        size_t jpeg_len = 0;
        uint32_t frame_id = 0;
        esp_err_t ret = si_video_acquire_jpeg(&jpeg, &jpeg_len, &frame_id);
        if (ret == ESP_OK && frame_id != last_frame_id) {
            si_video_status_t current;
            si_video_get_status(&current);
            uint32_t jpeg_width = 0;
            uint32_t jpeg_height = 0;
            bool has_sof = diag_jpeg_sof_dimensions(jpeg, jpeg_len,
                                                    &jpeg_width, &jpeg_height);
            if (!has_sof) {
                sof_invalid++;
            } else {
                if (observed_width == 0 && observed_height == 0) {
                    observed_width = jpeg_width;
                    observed_height = jpeg_height;
                }
                if (jpeg_width != current.width || jpeg_height != current.height) {
                    sof_mismatch++;
                }
            }
            if (samples == 0) {
                first_frame_id = frame_id;
            }
            last_frame_id = frame_id;
            samples++;
            if (jpeg_len < size_min) {
                size_min = jpeg_len;
            }
            if (jpeg_len > size_max) {
                size_max = jpeg_len;
            }
            size_total += jpeg_len;
        }
        free(jpeg);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    si_video_status_t end;
    si_video_get_status(&end);
    printf("video-sample seconds=%d active=%" PRIu32 "x%" PRIu32 " target=%" PRIu32
           "x%" PRIu32 "@%.2f samples=%" PRIu32 " frame_ids=%" PRIu32 "..%" PRIu32 "\r\n",
           seconds, end.width, end.height, end.target_width, end.target_height,
           end.target_fps_x100 / 100.0, samples, first_frame_id, last_frame_id);
    printf("video-sample jpeg_sof=%" PRIu32 "x%" PRIu32
           " invalid=%" PRIu32 " mismatch_active=%" PRIu32
           " size_min=%u size_max=%u size_avg=%u\r\n",
           observed_width, observed_height, sof_invalid, sof_mismatch,
           samples ? (unsigned)size_min : 0,
           samples ? (unsigned)size_max : 0,
           samples ? (unsigned)(size_total / samples) : 0);
    printf("video-sample driver captured_delta=%" PRIu32 " accepted_delta=%" PRIu32
           " dropped_delta=%" PRIu32 " fps=%.2f error=%s\r\n",
           end.frames_captured - start.frames_captured,
           end.frames_encoded - start.frames_encoded,
           end.frames_dropped - start.frames_dropped,
           end.fps_x100 / 100.0,
           end.last_error[0] ? end.last_error : "none");
}

static void set_video_mode(const char *tail)
{
    unsigned width = 0;
    unsigned height = 0;
    unsigned fps_x100 = 0;
    if (!tail || sscanf(tail, "%u %u %u", &width, &height, &fps_x100) != 3 ||
        width == 0 || height == 0 || fps_x100 == 0) {
        printf("usage: video-mode WIDTH HEIGHT FPSx100\r\n");
        return;
    }
    esp_err_t ret = si_video_set_mode(width, height, fps_x100);
    printf("video-mode request=%ux%u@%.2f result=%s persistent=0\r\n",
           width, height, fps_x100 / 100.0, esp_err_to_name(ret));
}

static void print_diag_output(const char *text)
{
    if (!text || !text[0]) {
        printf("(no output)\r\n");
        return;
    }
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            putchar('\r');
        }
        putchar((unsigned char)*p);
    }
    if (text[strlen(text) - 1] != '\n') {
        printf("\r\n");
    }
}

static bool word_boundary(char ch)
{
    return ch == '\0' || isspace((unsigned char)ch);
}

static bool line_has_eol(const char *line)
{
    return line && (strchr(line, '\n') || strchr(line, '\r'));
}

static void flush_stdin_input(void)
{
    clearerr(stdin);
    s_input_quarantine = true;
}

static bool command_looks_known(const char *line)
{
    char tmp[64];
    const char *cmd = line;
    while (*cmd && isspace((unsigned char)*cmd)) {
        cmd++;
    }
    size_t len = 0;
    while (cmd[len] && !isspace((unsigned char)cmd[len]) && len < sizeof(tmp) - 1) {
        tmp[len] = cmd[len];
        len++;
    }
    tmp[len] = '\0';
    if (!tmp[0]) {
        return true;
    }
    return strcmp(tmp, "?") == 0 ||
           strcmp(tmp, "help") == 0 ||
           strcmp(tmp, "status") == 0 ||
           strcmp(tmp, "network") == 0 ||
           strcmp(tmp, "target-uart") == 0 ||
           strcmp(tmp, "uart-target") == 0 ||
           strcmp(tmp, "target-uart-read") == 0 ||
           strcmp(tmp, "target-uart-write") == 0 ||
           strcmp(tmp, "target-uart-enter") == 0 ||
           strcmp(tmp, "target-uart-baud") == 0 ||
           strcmp(tmp, "target-uart-fast") == 0 ||
           strcmp(tmp, "target-uart-fallback") == 0 ||
#if SI_CFG_MS2109_EEPROM_WP_GPIO >= 0
           strcmp(tmp, "eeprom-wp") == 0 ||
#endif
#if SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED
           strcmp(tmp, "eeprom-emu") == 0 ||
#endif
#if SI_CFG_MS2109_POWER_ENABLED || SI_CFG_MS2109_TEST_ENABLED
           strcmp(tmp, "ms-switch") == 0 ||
#endif
#if SI_CFG_MS2109_TEST_ENABLED
           strcmp(tmp, "ms-eeprom") == 0 ||
#endif
           strcmp(tmp, "video") == 0 ||
           strcmp(tmp, "video-sample") == 0 ||
           strcmp(tmp, "video-mode") == 0 ||
           strcmp(tmp, "tf") == 0 ||
           strcmp(tmp, "tf-ls") == 0 ||
           strcmp(tmp, "tf-rm") == 0 ||
           strcmp(tmp, "hist") == 0 ||
           strcmp(tmp, "history") == 0 ||
           strcmp(tmp, "hist-append") == 0 ||
           strcmp(tmp, "hist-clear") == 0 ||
           strcmp(tmp, "mem") == 0 ||
           strcmp(tmp, "memory") == 0 ||
           strcmp(tmp, "mem-append") == 0 ||
           strcmp(tmp, "mem-clear") == 0 ||
           strcmp(tmp, "agent-data-clear") == 0 ||
           strcmp(tmp, "ssh-target") == 0 ||
           strcmp(tmp, "ssh") == 0 ||
           strcmp(tmp, "agent-run") == 0 ||
           strcmp(tmp, "agent-run-b64") == 0 ||
           strcmp(tmp, "agent-prompt-begin") == 0 ||
           strcmp(tmp, "agent-prompt-end") == 0 ||
           strcmp(tmp, "agent-prompt-cancel") == 0 ||
           strcmp(tmp, "agent-b64-begin") == 0 ||
           strcmp(tmp, "agent-b64-end") == 0 ||
           strcmp(tmp, "agent-b64-cancel") == 0 ||
           strcmp(tmp, "agent-status") == 0 ||
           strcmp(tmp, "agent-events") == 0 ||
           strcmp(tmp, "agent-log") == 0 ||
           strcmp(tmp, "agent-watch") == 0 ||
           strcmp(tmp, "agent-pause") == 0 ||
           strcmp(tmp, "agent-resume") == 0 ||
           strcmp(tmp, "agent-cancel") == 0 ||
           strcmp(tmp, "agent-abort") == 0 ||
           strcmp(tmp, "agent-steer") == 0 ||
           strcmp(tmp, "agent-result") == 0 ||
           strcmp(tmp, "sync") == 0 ||
           strcmp(tmp, "uart-sync") == 0 ||
           strcmp(tmp, "abort") == 0 ||
           strcmp(tmp, "reboot") == 0;
}

static bool line_is_resync_command(const char *line)
{
    if (!line) {
        return false;
    }
    char tmp[32];
    size_t len = 0;
    while (line[len] && line[len] != '\r' && line[len] != '\n' &&
           len < sizeof(tmp) - 1) {
        tmp[len] = line[len];
        len++;
    }
    tmp[len] = '\0';
    char *cmd = trim(tmp);
    if (strcmp(cmd, "sync") == 0 ||
        strcmp(cmd, "uart-sync") == 0 ||
        strcmp(cmd, "abort") == 0) {
        return true;
    }

    // When a host reconnects mid-line, stale bytes can be prepended to the next
    // recovery command. Treat short lines ending in the recovery token as a hard
    // resync so UART automation can always regain the prompt.
    size_t cmd_len = strlen(cmd);
    if (cmd_len > 0 && cmd_len <= 64) {
        if (cmd_len >= 4 && strcmp(cmd + cmd_len - 4, "sync") == 0) {
            return true;
        }
        if (cmd_len >= 9 && strcmp(cmd + cmd_len - 9, "uart-sync") == 0) {
            return true;
        }
        if (cmd_len >= 5 && strcmp(cmd + cmd_len - 5, "abort") == 0) {
            return true;
        }
    }
    return false;
}

static void print_unknown_command(const char *cmd)
{
    char preview[DIAG_CLI_UNKNOWN_PREVIEW_MAX + 1];
    size_t len = 0;
    if (cmd) {
        for (const char *p = cmd; *p && len < DIAG_CLI_UNKNOWN_PREVIEW_MAX; p++) {
            unsigned char ch = (unsigned char)*p;
            preview[len++] = isprint(ch) ? (char)ch : '?';
        }
    }
    preview[len] = '\0';
    printf("unknown command: %s%s\r\n",
           preview,
           cmd && strlen(cmd) > DIAG_CLI_UNKNOWN_PREVIEW_MAX ? "..." : "");
    printf("type 'help' for commands; use agent-run-b64 or agent-prompt-begin for long/UTF-8 prompts\r\n");
    if (cmd && strlen(cmd) >= DIAG_CLI_UNKNOWN_FLUSH_THRESHOLD) {
        flush_stdin_input();
        printf("input quarantine enabled after oversized unknown command\r\n");
    }
}

static char *consume_token(char *p, char *out, size_t out_size)
{
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    p = trim(p);
    size_t len = 0;
    while (p[len] && !isspace((unsigned char)p[len])) {
        len++;
    }
    if (out && out_size > 0 && len > 0) {
        size_t copy_len = len < out_size ? len : out_size - 1;
        memcpy(out, p, copy_len);
        out[copy_len] = '\0';
    }
    return trim(p + len);
}

static void print_network_config_line(const char *label,
                                      const si_network_config_t *config)
{
    if (!config) {
        return;
    }
    printf("%s generation=%" PRIu32 " mode=%s hostname=%s autoip=%d",
           label, config->generation, si_network_mode_name(config->mode),
           config->hostname, config->autoip_fallback);
    if (config->mode == SI_NETWORK_MODE_STATIC) {
        printf(" ip=%s mask=%s gateway=%s dns1=%s dns2=%s",
               config->address,
               config->netmask,
               config->gateway[0] ? config->gateway : "none",
               config->dns_primary[0] ? config->dns_primary : "none",
               config->dns_secondary[0] ? config->dns_secondary : "none");
    }
    printf("\r\n");
}

static bool optional_network_value(const char *token, char *out,
                                   size_t out_size)
{
    if (!token || !out || out_size == 0) {
        return false;
    }
    if (strcmp(token, "-") == 0) {
        out[0] = '\0';
        return true;
    }
    if (strlen(token) >= out_size) {
        return false;
    }
    strlcpy(out, token, out_size);
    return true;
}

static esp_err_t network_edit_base(si_network_config_t *config)
{
    esp_err_t ret = si_net_get_staged_config(config);
    if (ret == ESP_ERR_NOT_FOUND) {
        ret = si_net_get_active_config(config);
    }
    return ret;
}

static void run_network_command(char *tail)
{
    char action[24];
    char *rest = consume_token(tail, action, sizeof(action));
    if (!action[0] || strcmp(action, "show") == 0) {
        si_net_status_t status;
        si_net_get_status(&status);
        printf("network initialized=%d link=%d connected=%d state=%s "
               "source=%s ip=%s remaining=%" PRIu32 "s error=%s\r\n",
               status.initialized, status.link_up, status.connected,
               status.config_state[0] ? status.config_state : "unavailable",
               status.address_source[0] ? status.address_source : "none",
               status.ip[0] ? status.ip : "none",
               status.confirm_remaining_seconds,
               status.last_error[0] ? status.last_error : "none");
        printf("identity device_id=%s hostname=%s mac=%s\r\n",
               status.device_id[0] ? status.device_id : "unknown",
               status.hostname[0] ? status.hostname : "unknown",
               status.mac[0] ? status.mac : "unknown");
        si_network_config_t config;
        esp_err_t ret = si_net_get_active_config(&config);
        if (ret == ESP_OK) {
            print_network_config_line("active", &config);
        } else {
            printf("active unavailable result=%s\r\n", esp_err_to_name(ret));
        }
        ret = si_net_get_staged_config(&config);
        if (ret == ESP_OK) {
            print_network_config_line("staged", &config);
        } else if (ret != ESP_ERR_NOT_FOUND) {
            printf("staged unavailable result=%s\r\n", esp_err_to_name(ret));
        }
        return;
    }

    if (strcmp(action, "apply") == 0 ||
        strcmp(action, "commit") == 0 ||
        strcmp(action, "rollback") == 0) {
        if (rest[0]) {
            printf("network %s takes no arguments\r\n", action);
            return;
        }
        esp_err_t ret = strcmp(action, "apply") == 0
                            ? si_net_apply_staged()
                            : strcmp(action, "commit") == 0
                                  ? si_net_commit_pending()
                                  : si_net_rollback_pending();
        printf("network %s result=%s\r\n", action, esp_err_to_name(ret));
        return;
    }
    if (strcmp(action, "reset") == 0) {
        if (strcmp(rest, "CONFIRM") != 0) {
            printf("refused: use exactly 'network reset CONFIRM'\r\n");
            return;
        }
        esp_err_t ret = si_net_reset_config();
        printf("network reset result=%s\r\n", esp_err_to_name(ret));
        return;
    }

    si_network_config_t config;
    esp_err_t ret = network_edit_base(&config);
    if (ret != ESP_OK) {
        printf("network settings unavailable result=%s\r\n",
               esp_err_to_name(ret));
        return;
    }
    if (strcmp(action, "dhcp") == 0) {
        char hostname[SI_PRODUCT_HOSTNAME_MAX_LEN + 1];
        rest = consume_token(rest, hostname, sizeof(hostname));
        if (rest[0]) {
            printf("usage: network dhcp [hostname]\r\n");
            return;
        }
        config.mode = SI_NETWORK_MODE_DHCP;
        config.autoip_fallback = true;
        config.address[0] = '\0';
        config.netmask[0] = '\0';
        config.gateway[0] = '\0';
        config.dns_primary[0] = '\0';
        config.dns_secondary[0] = '\0';
        if (hostname[0]) {
            strlcpy(config.hostname, hostname, sizeof(config.hostname));
        }
    } else if (strcmp(action, "hostname") == 0) {
        char hostname[SI_PRODUCT_HOSTNAME_MAX_LEN + 1];
        rest = consume_token(rest, hostname, sizeof(hostname));
        if (!hostname[0] || rest[0]) {
            printf("usage: network hostname <name>\r\n");
            return;
        }
        strlcpy(config.hostname, hostname, sizeof(config.hostname));
    } else if (strcmp(action, "static") == 0) {
        char address[16], netmask[16], gateway[16], dns1[16], dns2[16];
        rest = consume_token(rest, address, sizeof(address));
        rest = consume_token(rest, netmask, sizeof(netmask));
        rest = consume_token(rest, gateway, sizeof(gateway));
        rest = consume_token(rest, dns1, sizeof(dns1));
        rest = consume_token(rest, dns2, sizeof(dns2));
        if (!address[0] || !netmask[0] || !gateway[0] || !dns1[0] ||
            rest[0]) {
            printf("usage: network static <ip> <mask> <gateway|-> "
                   "<dns1|-> [dns2|-]\r\n");
            return;
        }
        config.mode = SI_NETWORK_MODE_STATIC;
        config.autoip_fallback = false;
        strlcpy(config.address, address, sizeof(config.address));
        strlcpy(config.netmask, netmask, sizeof(config.netmask));
        if (!optional_network_value(
                gateway, config.gateway, sizeof(config.gateway)) ||
            !optional_network_value(
                dns1, config.dns_primary, sizeof(config.dns_primary)) ||
            !optional_network_value(
                dns2[0] ? dns2 : "-", config.dns_secondary,
                sizeof(config.dns_secondary))) {
            printf("network static value too long\r\n");
            return;
        }
    } else {
        printf("usage: network show|dhcp|static|hostname|apply|commit|"
               "rollback|reset\r\n");
        return;
    }
    ret = si_net_stage_config(&config);
    printf("network stage mode=%s hostname=%s result=%s\r\n",
           si_network_mode_name(config.mode), config.hostname,
           esp_err_to_name(ret));
}

static void agent_opts_init(diag_agent_opts_t *opts)
{
    if (!opts) {
        return;
    }
    strlcpy(opts->session, "default", sizeof(opts->session));
    opts->profile[0] = '\0';
    opts->model[0] = '\0';
    opts->dry_run = true;
    opts->screenshot = false;
    opts->web_search = false;
}

static char *parse_agent_opts(char *tail, diag_agent_opts_t *opts)
{
    char *p = trim(tail);
    while (p && *p) {
        if (strncmp(p, "--execute", 9) == 0 && word_boundary(p[9])) {
            opts->dry_run = false;
            p = trim(p + 9);
        } else if (strncmp(p, "--dry-run", 9) == 0 && word_boundary(p[9])) {
            opts->dry_run = true;
            p = trim(p + 9);
        } else if (strncmp(p, "--screenshot", 12) == 0 && word_boundary(p[12])) {
            opts->screenshot = true;
            p = trim(p + 12);
        } else if (strncmp(p, "--web-search", 12) == 0 && word_boundary(p[12])) {
            opts->web_search = true;
            p = trim(p + 12);
        } else if (strncmp(p, "--no-web-search", 15) == 0 && word_boundary(p[15])) {
            opts->web_search = false;
            p = trim(p + 15);
        } else if (strncmp(p, "--profile", 9) == 0 && word_boundary(p[9])) {
            p = consume_token(p + 9, opts->profile, sizeof(opts->profile));
        } else if (strncmp(p, "--model", 7) == 0 && word_boundary(p[7])) {
            p = consume_token(p + 7, opts->model, sizeof(opts->model));
        } else if (strncmp(p, "--session", 9) == 0 && word_boundary(p[9])) {
            p = consume_token(p + 9, opts->session, sizeof(opts->session));
        } else {
            break;
        }
    }
    return trim(p);
}

static void start_agent_with_opts(const diag_agent_opts_t *opts,
                                  const char *message,
                                  char *out,
                                  size_t out_size)
{
    if (!message || !message[0]) {
        snprintf(out, out_size,
                 "agent prompt is empty\n");
        return;
    }

    si_diagnostics_agent_run_start(opts->session, message,
                                   opts->profile[0] ? opts->profile : NULL,
                                   opts->model[0] ? opts->model : NULL,
                                   opts->dry_run, opts->screenshot,
                                   opts->web_search,
                                   out, out_size);
}

static void run_agent_start_command(char *tail, char *out, size_t out_size)
{
    diag_agent_opts_t opts;
    agent_opts_init(&opts);
    char *message = parse_agent_opts(tail, &opts);
    if (!message || !message[0]) {
        snprintf(out, out_size,
                 "usage: agent-run [--execute] [--profile P] [--model M] [--session S] [--screenshot] [--web-search] <prompt>\n");
        return;
    }
    start_agent_with_opts(&opts, message, out, out_size);
}

static bool base64_prompt_char_valid(char ch)
{
    return isalnum((unsigned char)ch) || ch == '+' || ch == '/' || ch == '=' ||
           ch == '-' || ch == '_';
}

static char *decode_base64_prompt(const char *encoded, char *err, size_t err_size)
{
    if (!encoded || !encoded[0]) {
        snprintf(err, err_size, "base64 prompt is empty");
        return NULL;
    }

    size_t raw_len = strlen(encoded);
    size_t compact_cap = raw_len + 1;
    char *compact = heap_caps_malloc(compact_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!compact) {
        compact = malloc(compact_cap);
    }
    if (!compact) {
        snprintf(err, err_size, "base64 compact alloc failed");
        return NULL;
    }

    size_t compact_len = 0;
    for (size_t i = 0; i < raw_len; i++) {
        char ch = encoded[i];
        if (isspace((unsigned char)ch)) {
            continue;
        }
        if (!base64_prompt_char_valid(ch)) {
            free(compact);
            snprintf(err, err_size, "invalid base64 char");
            return NULL;
        }
        if (ch == '-') {
            ch = '+';
        } else if (ch == '_') {
            ch = '/';
        }
        compact[compact_len++] = ch;
    }
    compact[compact_len] = '\0';
    if (compact_len == 0 || compact_len > DIAG_CLI_AGENT_B64_MAX) {
        free(compact);
        snprintf(err, err_size, "base64 prompt too large");
        return NULL;
    }

    size_t decoded_cap = DIAG_CLI_AGENT_PROMPT_MAX + 1;
    char *decoded = heap_caps_malloc(decoded_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!decoded) {
        decoded = malloc(decoded_cap);
    }
    if (!decoded) {
        free(compact);
        snprintf(err, err_size, "base64 decode alloc failed");
        return NULL;
    }

    size_t written = 0;
    int ret = mbedtls_base64_decode((unsigned char *)decoded, decoded_cap - 1, &written,
                                    (const unsigned char *)compact, compact_len);
    free(compact);
    if (ret != 0 || written == 0 || written > DIAG_CLI_AGENT_PROMPT_MAX ||
        memchr(decoded, '\0', written)) {
        free(decoded);
        snprintf(err, err_size, "base64 decode failed ret=%d", ret);
        return NULL;
    }
    decoded[written] = '\0';
    return decoded;
}

static void run_agent_b64_command(char *tail, char *out, size_t out_size)
{
    diag_agent_opts_t opts;
    agent_opts_init(&opts);
    char *encoded = parse_agent_opts(tail, &opts);
    if (!encoded || !encoded[0]) {
        snprintf(out, out_size,
                 "usage: agent-run-b64 [--execute] [--profile P] [--model M] [--session S] [--screenshot] [--web-search] <base64-prompt>\n");
        return;
    }

    char err[96] = {0};
    char *decoded = decode_base64_prompt(encoded, err, sizeof(err));
    if (!decoded) {
        snprintf(out, out_size, "agent-run-b64 %s\n", err[0] ? err : "decode failed");
        return;
    }
    start_agent_with_opts(&opts, decoded, out, out_size);
    free(decoded);
}

static void agent_prompt_capture_clear(void)
{
    free(s_prompt_capture.text);
    memset(&s_prompt_capture, 0, sizeof(s_prompt_capture));
}

static void run_agent_prompt_begin_command(char *tail, char *out, size_t out_size)
{
    if (s_prompt_capture.active) {
        snprintf(out, out_size,
                 "agent prompt capture already active; finish with agent-prompt-end/agent-b64-end or cancel with agent-prompt-cancel\n");
        return;
    }

    diag_agent_opts_t opts;
    agent_opts_init(&opts);
    char *rest = parse_agent_opts(tail, &opts);
    if (rest && rest[0]) {
        snprintf(out, out_size,
                 "usage: agent-prompt-begin [--execute] [--profile P] [--model M] [--session S] [--screenshot] [--web-search]\n");
        return;
    }

    char *text = heap_caps_calloc(1, DIAG_CLI_AGENT_PROMPT_MAX + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) {
        text = calloc(1, DIAG_CLI_AGENT_PROMPT_MAX + 1);
    }
    if (!text) {
        snprintf(out, out_size, "agent prompt capture alloc failed\n");
        return;
    }

    s_prompt_capture.active = true;
    s_prompt_capture.base64 = false;
    s_prompt_capture.opts = opts;
    s_prompt_capture.text = text;
    s_prompt_capture.len = 0;
    s_prompt_capture.cap = DIAG_CLI_AGENT_PROMPT_MAX + 1;
    snprintf(out, out_size,
             "agent prompt capture started; send prompt lines, then agent-prompt-end; cancel with agent-prompt-cancel\n");
}

static void run_agent_b64_begin_command(char *tail, char *out, size_t out_size)
{
    if (s_prompt_capture.active) {
        snprintf(out, out_size,
                 "agent prompt capture already active; finish with agent-prompt-end/agent-b64-end or cancel with agent-prompt-cancel\n");
        return;
    }

    diag_agent_opts_t opts;
    agent_opts_init(&opts);
    char *rest = parse_agent_opts(tail, &opts);
    if (rest && rest[0]) {
        snprintf(out, out_size,
                 "usage: agent-b64-begin [--execute] [--profile P] [--model M] [--session S] [--screenshot] [--web-search]\n");
        return;
    }

    char *text = heap_caps_calloc(1, DIAG_CLI_AGENT_B64_MAX + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) {
        text = calloc(1, DIAG_CLI_AGENT_B64_MAX + 1);
    }
    if (!text) {
        snprintf(out, out_size, "agent b64 capture alloc failed\n");
        return;
    }

    s_prompt_capture.active = true;
    s_prompt_capture.base64 = true;
    s_prompt_capture.opts = opts;
    s_prompt_capture.text = text;
    s_prompt_capture.len = 0;
    s_prompt_capture.cap = DIAG_CLI_AGENT_B64_MAX + 1;
    snprintf(out, out_size,
             "agent b64 capture started; send base64 chunks, then agent-b64-end; cancel with agent-prompt-cancel\n");
}

static bool handle_agent_prompt_capture_line(char *line)
{
    if (!s_prompt_capture.active) {
        return false;
    }

    char *content = line;
    size_t len = strlen(content);
    while (len > 0 && (content[len - 1] == '\n' || content[len - 1] == '\r')) {
        content[--len] = '\0';
    }

    char *cmd = trim(content);
    len = strlen(content);
    bool finish_prompt = !s_prompt_capture.base64 &&
                         (strcmp(cmd, "agent-prompt-end") == 0 ||
                          strcmp(cmd, ".") == 0);
    bool finish_b64 = s_prompt_capture.base64 &&
                      (strcmp(cmd, "agent-b64-end") == 0 ||
                       strcmp(cmd, ".") == 0);
    if (finish_prompt || finish_b64) {
        char *out = heap_caps_calloc(1, DIAG_CLI_OUT_MAX,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) {
            out = calloc(1, DIAG_CLI_OUT_MAX);
        }
        if (!out) {
            printf("agent prompt finish alloc failed\r\n");
            agent_prompt_capture_clear();
            return true;
        }
        if (s_prompt_capture.base64) {
            char err[96] = {0};
            char *decoded = decode_base64_prompt(s_prompt_capture.text,
                                                 err, sizeof(err));
            if (decoded) {
                start_agent_with_opts(&s_prompt_capture.opts, decoded,
                                      out, DIAG_CLI_OUT_MAX);
                free(decoded);
            } else {
                snprintf(out, DIAG_CLI_OUT_MAX, "agent-b64-end %s\n",
                         err[0] ? err : "decode failed");
            }
        } else {
            start_agent_with_opts(&s_prompt_capture.opts, s_prompt_capture.text,
                                  out, DIAG_CLI_OUT_MAX);
        }
        print_diag_output(out);
        free(out);
        agent_prompt_capture_clear();
        return true;
    }
    if (strcmp(cmd, "agent-prompt-cancel") == 0 ||
        strcmp(cmd, "agent-b64-cancel") == 0) {
        agent_prompt_capture_clear();
        printf("agent prompt capture cancelled\r\n");
        return true;
    }

    if (s_prompt_capture.base64) {
        for (size_t i = 0; i < len; i++) {
            char ch = content[i];
            if (isspace((unsigned char)ch)) {
                continue;
            }
            if (!base64_prompt_char_valid(ch)) {
                agent_prompt_capture_clear();
                printf("agent b64 invalid char; capture cancelled\r\n");
                return true;
            }
            if (ch == '-') {
                ch = '+';
            } else if (ch == '_') {
                ch = '/';
            }
            if (s_prompt_capture.len + 1 >= s_prompt_capture.cap) {
                agent_prompt_capture_clear();
                printf("agent b64 prompt too large; capture cancelled\r\n");
                return true;
            }
            s_prompt_capture.text[s_prompt_capture.len++] = ch;
        }
        s_prompt_capture.text[s_prompt_capture.len] = '\0';
        printf("... b64 %" PRIu32 "/%" PRIu32 " bytes\r\n",
               (uint32_t)s_prompt_capture.len,
               (uint32_t)(s_prompt_capture.cap - 1));
    } else {
        if (s_prompt_capture.len + len + 2 >= s_prompt_capture.cap) {
            agent_prompt_capture_clear();
            printf("agent prompt too large; capture cancelled\r\n");
            return true;
        }
        memcpy(s_prompt_capture.text + s_prompt_capture.len, content, len);
        s_prompt_capture.len += len;
        s_prompt_capture.text[s_prompt_capture.len++] = '\n';
        s_prompt_capture.text[s_prompt_capture.len] = '\0';
        printf("... %" PRIu32 "/%" PRIu32 " bytes\r\n",
               (uint32_t)s_prompt_capture.len,
               (uint32_t)(s_prompt_capture.cap - 1));
    }
    return true;
}

static bool diag_agent_run_id_token(char *tail,
                                    char *run_id,
                                    size_t run_id_size,
                                    char **rest_out)
{
    if (!run_id || run_id_size == 0) {
        return false;
    }
    run_id[0] = '\0';
    char *p = trim(tail);
    size_t len = 0;
    while (p[len] && !isspace((unsigned char)p[len])) {
        len++;
    }
    if (len == 0 || len > DIAG_CLI_AGENT_RUN_ID_MAX || len >= run_id_size) {
        return false;
    }
    memcpy(run_id, p, len);
    run_id[len] = '\0';
    if (!si_diagnostics_run_id_valid(run_id)) {
        run_id[0] = '\0';
        return false;
    }
    if (rest_out) {
        *rest_out = trim(p + len);
    }
    return true;
}

static bool diag_parse_size_token(char *tail, size_t *value, char **rest_out)
{
    char token[32] = {0};
    char *rest = consume_token(tail, token, sizeof(token));
    if (!token[0]) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(token, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed > SIZE_MAX) {
        return false;
    }
    if (value) {
        *value = (size_t)parsed;
    }
    if (rest_out) {
        *rest_out = rest;
    }
    return true;
}

static bool diag_token_is_decimal(const char *token)
{
    if (!token || !token[0]) {
        return false;
    }
    for (const char *p = token; *p; ++p) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static void run_agent_status_command(char *tail, char *out, size_t out_size)
{
    char *args = trim(tail);
    if (!args[0]) {
        /* Preserve the legacy read-only snapshot; all mutations below require
         * an exact, caller-supplied run ID. */
        si_diagnostics_agent_run_status(NULL, out, out_size);
        return;
    }
    char run_id[DIAG_CLI_AGENT_RUN_ID_MAX + 1] = {0};
    char *rest = NULL;
    if (!diag_agent_run_id_token(args, run_id, sizeof(run_id), &rest) ||
        (rest && rest[0])) {
        snprintf(out, out_size, "usage: agent-status [run_id]\n");
        return;
    }
    si_diagnostics_agent_run_status(run_id, out, out_size);
}

static void run_agent_events_command(char *tail, char *out, size_t out_size)
{
    char run_id[DIAG_CLI_AGENT_RUN_ID_MAX + 1] = {0};
    char *rest = NULL;
    if (!diag_agent_run_id_token(tail, run_id, sizeof(run_id), &rest)) {
        snprintf(out, out_size,
                 "usage: agent-events <run_id> [after_seq]\n");
        return;
    }
    size_t parsed_seq = 0;
    if (rest && rest[0]) {
        char *extra = NULL;
        if (!diag_parse_size_token(rest, &parsed_seq, &extra) ||
            parsed_seq > UINT32_MAX || (extra && extra[0])) {
            snprintf(out, out_size,
                     "usage: agent-events <run_id> [after_seq]\n");
            return;
        }
    }
    uint32_t after_seq = (uint32_t)parsed_seq;
    uint32_t latest = after_seq;
    bool running = false;
    si_diagnostics_agent_run_events(run_id, after_seq, &latest, &running,
                                    out, out_size);
    if (!out[0]) {
        snprintf(out, out_size,
                 "no new agent events; run_id=%s latest_seq=%" PRIu32
                 " running=%d\n",
                 run_id, latest, running);
    }
}

static void run_agent_watch_command(char *tail, char *out, size_t out_size)
{
    char run_id[DIAG_CLI_AGENT_RUN_ID_MAX + 1] = {0};
    char first[64] = {0};
    char *args = trim(tail);
    char *rest = consume_token(args, first, sizeof(first));
    int seconds = DIAG_CLI_AGENT_WATCH_MAX_SECONDS;
    if (first[0] && diag_token_is_decimal(first) && !rest[0]) {
        /* Backward-compatible read-only `agent-watch <seconds>`. */
        seconds = atoi(first);
    } else if (first[0]) {
        if (!si_diagnostics_run_id_valid(first)) {
            snprintf(out, out_size,
                     "usage: agent-watch [run_id] [seconds]\n");
            print_diag_output(out);
            return;
        }
        strlcpy(run_id, first, sizeof(run_id));
        if (rest[0]) {
            size_t parsed_seconds = 0;
            char *extra = NULL;
            if (!diag_parse_size_token(rest, &parsed_seconds, &extra) ||
                parsed_seconds > INT_MAX || (extra && extra[0])) {
                snprintf(out, out_size,
                         "usage: agent-watch [run_id] [seconds]\n");
                print_diag_output(out);
                return;
            }
            seconds = (int)parsed_seconds;
        }
    }
    if (seconds <= 0 || seconds > DIAG_CLI_AGENT_WATCH_MAX_SECONDS) {
        seconds = DIAG_CLI_AGENT_WATCH_MAX_SECONDS;
    }

    uint32_t seq = 0;
    bool running = true;
    printf("following Agent execution log run_id=%s for up to %d seconds...\r\n",
           run_id[0] ? run_id : "(legacy-current)", seconds);
    for (int i = 0; i < seconds; i++) {
        out[0] = '\0';
        si_diagnostics_agent_run_events(run_id[0] ? run_id : NULL,
                                        seq, &seq, &running, out, out_size);
        if (out[0]) {
            print_diag_output(out);
        }
        if (!running) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    out[0] = '\0';
    si_diagnostics_agent_run_status(run_id[0] ? run_id : NULL,
                                    out, out_size);
    print_diag_output(out);
}

typedef void (*diag_agent_control_fn_t)(const char *run_id,
                                        char *out,
                                        size_t out_size);

static void run_agent_control_command(const char *command,
                                      char *tail,
                                      diag_agent_control_fn_t control,
                                      char *out,
                                      size_t out_size)
{
    char run_id[DIAG_CLI_AGENT_RUN_ID_MAX + 1] = {0};
    char *rest = NULL;
    if (!diag_agent_run_id_token(tail, run_id, sizeof(run_id), &rest) ||
        (rest && rest[0])) {
        snprintf(out, out_size, "usage: %s <run_id>\n", command);
        return;
    }
    control(run_id, out, out_size);
}

static void run_agent_steer_command(char *tail, char *out, size_t out_size)
{
    char run_id[DIAG_CLI_AGENT_RUN_ID_MAX + 1] = {0};
    char *message = NULL;
    if (!diag_agent_run_id_token(tail, run_id, sizeof(run_id), &message) ||
        !message || !message[0]) {
        snprintf(out, out_size,
                 "usage: agent-steer <run_id> <message>\n");
        return;
    }
    si_diagnostics_agent_run_steer(run_id, message, out, out_size);
}

static void run_agent_result_command(char *tail, char *out, size_t out_size)
{
    char run_id[DIAG_CLI_AGENT_RUN_ID_MAX + 1] = {0};
    char *rest = NULL;
    if (!diag_agent_run_id_token(tail, run_id, sizeof(run_id), &rest)) {
        snprintf(out, out_size,
                 "usage: agent-result <run_id> [offset] [max-bytes]\n");
        return;
    }
    size_t offset = 0;
    size_t max_bytes = 0;
    if (rest && rest[0]) {
        char *next = NULL;
        if (!diag_parse_size_token(rest, &offset, &next)) {
            snprintf(out, out_size,
                     "usage: agent-result <run_id> [offset] [max-bytes]\n");
            return;
        }
        if (next && next[0]) {
            char *extra = NULL;
            if (!diag_parse_size_token(next, &max_bytes, &extra) ||
                (extra && extra[0])) {
                snprintf(out, out_size,
                         "usage: agent-result <run_id> [offset] [max-bytes]\n");
                return;
            }
        }
    }
    si_diagnostics_agent_run_result(run_id, offset, max_bytes,
                                    out, out_size);
}

#if SI_CFG_MS2109_EEPROM_WP_GPIO >= 0
static void run_eeprom_wp_command(char *tail)
{
    char *action = trim(tail);
    const gpio_num_t gpio = (gpio_num_t)SI_CFG_MS2109_EEPROM_WP_GPIO;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        printf("eeprom-wp unavailable gpio=%d\r\n", (int)gpio);
        return;
    }

    esp_err_t ret = ESP_OK;
    int expected_level = -1;
    if (strcmp(action, "write-enable") == 0 ||
        strcmp(action, "protect") == 0) {
        const int output_level =
            strcmp(action, "write-enable") == 0 ? 0 : 1;
        expected_level = output_level;
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << gpio,
            .mode = GPIO_MODE_INPUT_OUTPUT_OD,
            .pull_up_en = output_level ?
                GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ret = gpio_set_level(gpio, output_level);
        if (ret == ESP_OK) {
            ret = gpio_config(&config);
        }
    } else if (strcmp(action, "status") == 0) {
        /* A reset pin may have its input path disabled until it is configured. */
        ret = gpio_input_enable(gpio);
    } else {
        printf("usage: eeprom-wp status|write-enable|protect\r\n");
        return;
    }

    const int level = gpio_get_level(gpio);
    if (ret == ESP_OK && expected_level >= 0 && level != expected_level) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    printf("eeprom-wp gpio=%d level=%d write=%s result=%s\r\n",
           (int)gpio,
           level,
           level == 0 ? "enabled" : "protected",
           esp_err_to_name(ret));
}
#endif

#if SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED
static void print_eeprom_emulator_status(void)
{
    si_ms2109_eeprom_emulator_status_t status;
    si_ms2109_eeprom_emulator_get_status(&status);
    printf("eeprom-emu enabled=%d initialized=%d addr=0x50 size=256 "
           "scl=GPIO%d level=%d falling=%" PRIu32 " "
           "sda=GPIO%d level=%d falling=%" PRIu32 " "
           "h3-even-gpio2-level=%d falling=%" PRIu32 " "
           "h3-even-gpio3-level=%d falling=%" PRIu32 " "
           "rx=%" PRIu32 " requests=%" PRIu32 " pointer=0x%02x\r\n",
           status.enabled,
           status.initialized,
           status.scl_gpio,
           status.scl_level,
           status.scl_falling_edges,
           status.sda_gpio,
           status.sda_level,
           status.sda_falling_edges,
           status.h3_gpio2_level,
           status.h3_gpio2_falling_edges,
           status.h3_gpio3_level,
           status.h3_gpio3_falling_edges,
           status.receive_count,
           status.request_count,
           status.pointer);
}

static void run_eeprom_emulator_command(char *tail)
{
    char *action = trim(tail);
    if (!action[0] || strcmp(action, "status") == 0) {
        print_eeprom_emulator_status();
        return;
    }

    if (strcmp(action, "bias-test") == 0) {
        int scl_level = -1;
        int sda_level = -1;
        esp_err_t ret = si_ms2109_eeprom_emulator_test_external_pullups(
            &scl_level, &sda_level);
        printf("eeprom-emu bias-test weak-pulldown scl-level=%d "
               "sda-level=%d external-pullups=%s result=%s\r\n",
               scl_level,
               sda_level,
               scl_level == 1 && sda_level == 1 ? "present" : "not-confirmed",
               esp_err_to_name(ret));
        return;
    }

    bool scl;
    if (strcmp(action, "sda-low") == 0) {
        scl = false;
    } else if (strcmp(action, "scl-low") == 0) {
        scl = true;
    } else {
        printf("usage: eeprom-emu status|bias-test|sda-low|scl-low\r\n");
        return;
    }

    esp_err_t ret = si_ms2109_eeprom_emulator_pull_line_low(scl);
    printf("eeprom-emu continuity line=%s state=low result=%s "
           "(reboot required to restore I2C)\r\n",
           scl ? "SCL" : "SDA", esp_err_to_name(ret));
}
#endif

#if SI_CFG_MS2109_POWER_ENABLED || SI_CFG_MS2109_TEST_ENABLED
static void print_ms_switch_status(void)
{
#if SI_CFG_MS2109_POWER_ENABLED
    si_ms2109_power_status_t status;
    esp_err_t status_ret = si_ms2109_power_get_status(&status);
    printf("ms-switch enabled=%d initialized=%d switch_gpio=%d switch_active_high=%d "
           "switch_level=%d core_gpio=%d core_active_high=%d core_level=%d "
           "command=%s sequence=3v3-first delay_ms=%d "
           "rail_feedback=unavailable operations=%" PRIu32 " last=%s status=%s\r\n",
           status.enabled,
           status.initialized,
           status.switch_gpio,
           SI_CFG_MS2109_SWITCH_ACTIVE_HIGH,
           status.switch_output_level,
           status.core_enable_gpio,
           SI_CFG_MS2109_CORE_ENABLE_ACTIVE_HIGH,
           status.core_enable_output_level,
           status.power_on ? "on" : "off",
           SI_CFG_MS2109_POWER_SEQUENCE_DELAY_MS,
           status.operation_count,
           esp_err_to_name(status.last_result),
           esp_err_to_name(status_ret));
#else
    si_ms2109_test_status_t status;
    si_ms2109_test_get_status(&status);
    printf("ms-switch enabled=%d initialized=%d switch_gpio=%d switch_active_high=%d "
           "switch_level=%d core_gpio=%d core_active_high=%d core_level=%d "
           "command=%s sequence=%s delay_ms=%" PRIu32
           " rail_feedback=unavailable operations=%" PRIu32 " last=%s\r\n",
           status.enabled,
           status.initialized,
           status.switch_gpio,
           SI_CFG_MS2109_SWITCH_ACTIVE_HIGH,
           status.switch_output_level,
           status.core_enable_gpio,
           SI_CFG_MS2109_CORE_ENABLE_ACTIVE_HIGH,
           status.core_enable_output_level,
           status.power_on ? "on" : "off",
           status.last_power_sequence == SI_MS2109_POWER_SEQUENCE_CORE_PRE_ENABLE ?
               "core-pre" : "3v3-first",
           status.last_sequence_delay_ms,
           status.operation_count,
           esp_err_to_name(status.last_result));
#endif
}

static void run_ms_switch_command(char *tail)
{
    char *action = trim(tail);
    if (!action[0] || strcmp(action, "status") == 0) {
        print_ms_switch_status();
        return;
    }

    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (strcmp(action, "on") == 0) {
#if SI_CFG_MS2109_POWER_ENABLED
        ret = si_ms2109_power_set(true);
#else
        ret = si_ms2109_test_set_power(true);
#endif
    } else if (strcmp(action, "off") == 0) {
#if SI_CFG_MS2109_POWER_ENABLED
        ret = si_ms2109_power_set(false);
#else
        ret = si_ms2109_test_set_power(false);
#endif
#if SI_CFG_MS2109_TEST_ENABLED
    } else if (strncmp(action, "cycle-seq", 9) == 0 &&
               word_boundary(action[9])) {
        char sequence_text[24];
        char delay_text[16];
        char off_text[16];
        char *rest = consume_token(action + 9, sequence_text,
                                   sizeof(sequence_text));
        rest = consume_token(rest, delay_text, sizeof(delay_text));
        (void)consume_token(rest, off_text, sizeof(off_text));
        si_ms2109_power_sequence_t sequence;
        if (strcmp(sequence_text, "3v3-first") == 0) {
            sequence = SI_MS2109_POWER_SEQUENCE_3V3_FIRST;
        } else if (strcmp(sequence_text, "core-pre") == 0) {
            sequence = SI_MS2109_POWER_SEQUENCE_CORE_PRE_ENABLE;
        } else {
            printf("usage: ms-switch cycle-seq 3v3-first|core-pre "
                   "<delay-ms> [off-ms]\r\n");
            return;
        }
        if (!delay_text[0]) {
            printf("usage: ms-switch cycle-seq 3v3-first|core-pre "
                   "<delay-ms> [off-ms]\r\n");
            return;
        }
        uint32_t delay_ms = (uint32_t)strtoul(delay_text, NULL, 10);
        uint32_t off_ms = off_text[0] ?
            (uint32_t)strtoul(off_text, NULL, 10) : 1000U;
        ret = si_ms2109_test_cycle_power_sequence(
            sequence, delay_ms, off_ms);
#endif
    } else if (strncmp(action, "cycle", 5) == 0 && word_boundary(action[5])) {
        char *duration_text = trim(action + 5);
        uint32_t duration_ms = duration_text[0] ?
            (uint32_t)strtoul(duration_text, NULL, 10) : 250U;
#if SI_CFG_MS2109_POWER_ENABLED
        ret = si_ms2109_power_cycle(duration_ms);
#else
        ret = si_ms2109_test_cycle_power(duration_ms);
#endif
    } else {
#if SI_CFG_MS2109_TEST_ENABLED
        printf("usage: ms-switch status|on|off|cycle [off-ms]|"
               "cycle-seq 3v3-first|core-pre <delay-ms> [off-ms]\r\n");
#else
        printf("usage: ms-switch status|on|off|cycle [off-ms]\r\n");
#endif
        return;
    }
    printf("ms-switch action=%s result=%s\r\n", action,
           esp_err_to_name(ret));
    print_ms_switch_status();
}
#endif

#if SI_CFG_MS2109_TEST_ENABLED
static void print_ms_eeprom_status(void)
{
    si_ms2109_test_status_t status;
    uint8_t address_mask = 0;
    esp_err_t probe_ret = si_ms2109_test_probe_eeprom(&address_mask);
    si_ms2109_test_get_status(&status);
    printf("ms-eeprom enabled=%d initialized=%d type=AT24C16 size=%u "
           "page=%u wp_gpio=%d wp=%s scl_gpio=%d sda_gpio=%d "
           "scl_level=%d sda_level=%d "
           "address_mask=0x%02x last_bytes=%u last_crc32=%08" PRIx32
           " last_verified=%d probe=%s\r\n",
           status.enabled,
           status.initialized,
           (unsigned)SI_MS2109_AT24C16_SIZE,
           (unsigned)SI_MS2109_AT24C16_PAGE_SIZE,
           status.wp_gpio,
           status.write_protected ? "protected" : "write-enabled",
           status.scl_gpio,
           status.sda_gpio,
           status.last_scl_level,
           status.last_sda_level,
           address_mask,
           (unsigned)status.last_programmed_bytes,
           status.last_crc32,
           status.last_verified,
           esp_err_to_name(probe_ret));
}

static void dump_ms_eeprom_base64(void)
{
    uint8_t *image = heap_caps_malloc(
        SI_MS2109_AT24C16_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t encoded_capacity =
        ((SI_MS2109_AT24C16_SIZE + 2U) / 3U) * 4U + 1U;
    uint8_t *encoded = heap_caps_malloc(
        encoded_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!image || !encoded) {
        free(image);
        free(encoded);
        printf("ms-eeprom dump result=%s\r\n", esp_err_to_name(ESP_ERR_NO_MEM));
        return;
    }

    esp_err_t ret = si_ms2109_test_read_eeprom(
        0, image, SI_MS2109_AT24C16_SIZE);
    size_t encoded_size = 0;
    if (ret == ESP_OK) {
        int base64_ret = mbedtls_base64_encode(
            encoded, encoded_capacity, &encoded_size,
            image, SI_MS2109_AT24C16_SIZE);
        if (base64_ret != 0) {
            ret = ESP_FAIL;
        }
    }
    if (ret == ESP_OK) {
        encoded[encoded_size] = '\0';
        printf("ms-eeprom dump bytes=%u encoding=base64 data=%s result=ESP_OK\r\n",
               (unsigned)SI_MS2109_AT24C16_SIZE, (char *)encoded);
    } else {
        printf("ms-eeprom dump result=%s\r\n", esp_err_to_name(ret));
    }
    memset(image, 0, SI_MS2109_AT24C16_SIZE);
    free(image);
    free(encoded);
}

static void program_ms_eeprom_base64(char *tail)
{
    char *payload = trim(tail);
    static const char confirmation[] = " CONFIRM";
    size_t payload_size = strlen(payload);
    size_t confirmation_size = sizeof(confirmation) - 1U;
    if (payload_size <= confirmation_size ||
        strcmp(payload + payload_size - confirmation_size, confirmation) != 0) {
        printf("refused: use 'ms-eeprom program-b64 <2048-byte-base64> CONFIRM'\r\n");
        return;
    }
    payload[payload_size - confirmation_size] = '\0';
    payload = trim(payload);

    uint8_t *image = heap_caps_malloc(
        SI_MS2109_AT24C16_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!image) {
        printf("ms-eeprom program result=%s\r\n", esp_err_to_name(ESP_ERR_NO_MEM));
        return;
    }
    size_t decoded_size = 0;
    int base64_ret = mbedtls_base64_decode(
        image, SI_MS2109_AT24C16_SIZE, &decoded_size,
        (const unsigned char *)payload, strlen(payload));
    if (base64_ret != 0 || decoded_size != SI_MS2109_AT24C16_SIZE) {
        memset(image, 0, SI_MS2109_AT24C16_SIZE);
        free(image);
        printf("ms-eeprom program invalid-image decoded=%u required=%u\r\n",
               (unsigned)decoded_size, (unsigned)SI_MS2109_AT24C16_SIZE);
        return;
    }

    uint32_t crc32 = 0;
    bool verified = false;
    esp_err_t ret = si_ms2109_test_program_eeprom(
        image, decoded_size, &crc32, &verified);
    memset(image, 0, SI_MS2109_AT24C16_SIZE);
    free(image);
    printf("ms-eeprom program bytes=%u crc32=%08" PRIx32
           " verified=%d result=%s\r\n",
           (unsigned)decoded_size, crc32, verified, esp_err_to_name(ret));
}

static void run_ms_eeprom_command(char *tail)
{
    char *action = trim(tail);
    if (!action[0] || strcmp(action, "status") == 0) {
        print_ms_eeprom_status();
    } else if (strcmp(action, "dump-b64") == 0) {
        dump_ms_eeprom_base64();
    } else if (strncmp(action, "program-b64", 11) == 0 &&
               word_boundary(action[11])) {
        program_ms_eeprom_base64(action + 11);
    } else {
        printf("usage: ms-eeprom status|dump-b64|program-b64 <base64> CONFIRM\r\n");
    }
}
#endif

static void run_diag_command(char *line)
{
    char *cmd = trim(line);
    if (!cmd || !cmd[0]) {
        return;
    }

    char *out = heap_caps_calloc(1, DIAG_CLI_OUT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        out = calloc(1, DIAG_CLI_OUT_MAX);
    }
    if (!out) {
        printf("diag output alloc failed\r\n");
        return;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_help();
    } else if (strcmp(cmd, "status") == 0) {
        print_status();
    } else if (strncmp(cmd, "network", 7) == 0 &&
               word_boundary(cmd[7])) {
        run_network_command(cmd + 7);
    } else if (strcmp(cmd, "target-uart") == 0 ||
               strcmp(cmd, "uart-target") == 0) {
        print_target_uart_status();
    } else if (strcmp(cmd, "target-uart-read") == 0) {
        read_target_uart_backlog();
    } else if (strncmp(cmd, "target-uart-write", 17) == 0 &&
               word_boundary(cmd[17])) {
        char *text = trim(cmd + 17);
        if (!text[0]) {
            printf("target-uart-write requires text\r\n");
        } else {
            write_target_uart_data((const uint8_t *)text, strlen(text));
        }
    } else if (strcmp(cmd, "target-uart-enter") == 0) {
        const uint8_t carriage_return = '\r';
        write_target_uart_data(&carriage_return, 1);
    } else if (strncmp(cmd, "target-uart-baud", 16) == 0 &&
               word_boundary(cmd[16])) {
        char *tail = trim(cmd + 16);
        char *end = NULL;
        long baud_rate = strtol(tail, &end, 10);
        if (!tail[0] || !end || *trim(end) != '\0' ||
            baud_rate < 300 || baud_rate > 3000000) {
            printf("target-uart-baud invalid rate\r\n");
        } else {
            esp_err_t ret = si_target_uart_set_baud_rate((int)baud_rate);
            printf("target-uart-baud rate=%ld result=%s\r\n",
                   baud_rate, esp_err_to_name(ret));
        }
    } else if (strcmp(cmd, "target-uart-fast") == 0 ||
               strcmp(cmd, "target-uart-fallback") == 0) {
        si_target_uart_status_t status;
        si_target_uart_get_status(&status);
        bool fallback = strcmp(cmd, "target-uart-fallback") == 0;
        int baud_rate = fallback ?
            status.fallback_baud_rate : status.default_baud_rate;
        esp_err_t ret = si_target_uart_set_baud_rate(baud_rate);
        printf("target-uart-mode mode=%s rate=%d result=%s\r\n",
               fallback ? "fallback" : "fast",
               baud_rate, esp_err_to_name(ret));
#if SI_CFG_MS2109_EEPROM_WP_GPIO >= 0
    } else if (strncmp(cmd, "eeprom-wp", 9) == 0 &&
               word_boundary(cmd[9])) {
        run_eeprom_wp_command(cmd + 9);
#endif
#if SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED
    } else if (strncmp(cmd, "eeprom-emu", 10) == 0 &&
               word_boundary(cmd[10])) {
        run_eeprom_emulator_command(cmd + 10);
#endif
#if SI_CFG_MS2109_POWER_ENABLED || SI_CFG_MS2109_TEST_ENABLED
    } else if (strncmp(cmd, "ms-switch", 9) == 0 &&
               word_boundary(cmd[9])) {
        run_ms_switch_command(cmd + 9);
#endif
#if SI_CFG_MS2109_TEST_ENABLED
    } else if (strncmp(cmd, "ms-eeprom", 9) == 0 &&
               word_boundary(cmd[9])) {
        run_ms_eeprom_command(cmd + 9);
#endif
    } else if (strcmp(cmd, "video") == 0) {
        print_video_status();
    } else if (strncmp(cmd, "video-sample", 12) == 0 && word_boundary(cmd[12])) {
        sample_video_frames(cmd + 12);
    } else if (strncmp(cmd, "video-mode", 10) == 0 && word_boundary(cmd[10])) {
        set_video_mode(cmd + 10);
#if SI_CFG_VIDEO_H264_ENABLED
    } else if (strcmp(cmd, "video-h264-test") == 0) {
        esp_err_t ret = si_h264_stream_self_test();
        printf("video-h264-test result=%s\r\n", esp_err_to_name(ret));
    } else if (strncmp(cmd, "video-h264-test-file", 20) == 0 &&
               word_boundary(cmd[20])) {
        char rel[DIAG_CLI_TF_LS_PATH_MAX] = {0};
        char abs[DIAG_CLI_TF_LS_PATH_MAX + sizeof(SI_STORAGE_ROOT) + 4] = {0};
        esp_err_t ret = diag_tf_resolve_path(
            cmd + 20, rel, sizeof(rel), abs, sizeof(abs));
        if (ret == ESP_OK && rel[0]) {
            ret = si_h264_stream_self_test_file(abs);
        } else if (ret == ESP_OK) {
            ret = ESP_ERR_INVALID_ARG;
        }
        printf("video-h264-test-file path=/EA/%s result=%s\r\n",
               rel, esp_err_to_name(ret));
#endif
    } else if (strcmp(cmd, "tf") == 0) {
        print_tf_status();
    } else if (strncmp(cmd, "tf-ls", 5) == 0 && word_boundary(cmd[5])) {
        print_tf_list(cmd + 5);
    } else if (strncmp(cmd, "tf-rm", 5) == 0 && word_boundary(cmd[5])) {
        remove_tf_file(cmd + 5);
    } else if (strcmp(cmd, "hist") == 0 || strcmp(cmd, "history") == 0) {
        si_diagnostics_agent_history(out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "hist-append", 11) == 0) {
        char *text = trim(cmd + 11);
        si_diagnostics_agent_history_append(text, out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strcmp(cmd, "hist-clear") == 0) {
        si_diagnostics_agent_history_clear(out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strcmp(cmd, "mem") == 0 || strcmp(cmd, "memory") == 0) {
        si_diagnostics_agent_memory(out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "mem-append", 10) == 0) {
        char *text = trim(cmd + 10);
        si_diagnostics_agent_memory_append(text, out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strcmp(cmd, "mem-clear") == 0) {
        si_diagnostics_agent_memory_clear(out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strcmp(cmd, "agent-data-clear CONFIRM") == 0) {
        si_diagnostics_agent_data_clear(out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "agent-data-clear", 16) == 0 &&
               word_boundary(cmd[16])) {
        printf("refused: use exactly 'agent-data-clear CONFIRM'\r\n");
    } else if (strcmp(cmd, "auth-reset CONFIRM") == 0) {
        char password[SI_AUTH_BOOTSTRAP_PASSWORD_LEN + 1] = {0};
        esp_err_t ret = si_auth_reset_bootstrap(password, sizeof(password));
        if (ret == ESP_OK) {
            printf("auth reset ok username=%s temporary_password=%s\r\n",
                   si_auth_default_username(), password);
            printf("change the temporary password through the account settings before using device controls\r\n");
        } else {
            printf("auth reset failed: %s\r\n", esp_err_to_name(ret));
        }
        memset(password, 0, sizeof(password));
    } else if (strcmp(cmd, "ssh-target") == 0) {
        si_diagnostics_ssh_target(out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "ssh ", 4) == 0) {
        char *ssh_cmd = trim(cmd + 4);
        si_diagnostics_ssh_exec(ssh_cmd, out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "agent-run-b64", 13) == 0 && word_boundary(cmd[13])) {
        run_agent_b64_command(cmd + 13, out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "agent-run", 9) == 0 && word_boundary(cmd[9])) {
        run_agent_start_command(cmd + 9, out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "agent-prompt-begin", 18) == 0 && word_boundary(cmd[18])) {
        run_agent_prompt_begin_command(cmd + 18, out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "agent-b64-begin", 15) == 0 && word_boundary(cmd[15])) {
        run_agent_b64_begin_command(cmd + 15, out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strcmp(cmd, "agent-prompt-end") == 0 ||
               strcmp(cmd, "agent-prompt-cancel") == 0 ||
               strcmp(cmd, "agent-b64-end") == 0 ||
               strcmp(cmd, "agent-b64-cancel") == 0) {
        printf("no active agent prompt capture\r\n");
    } else if (strncmp(cmd, "agent-status", 12) == 0 &&
               word_boundary(cmd[12])) {
        run_agent_status_command(cmd + 12, out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "agent-events", 12) == 0 &&
               word_boundary(cmd[12])) {
        run_agent_events_command(cmd + 12, out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "agent-log", 9) == 0 && word_boundary(cmd[9])) {
        uint32_t seq = (uint32_t)strtoul(trim(cmd + 9), NULL, 10);
        uint32_t latest = seq;
        bool running = false;
        si_diagnostics_agent_run_events(NULL, seq, &latest, &running,
                                        out, DIAG_CLI_OUT_MAX);
        if (out[0]) {
            print_diag_output(out);
        } else {
            printf("no new agent events; latest_seq=%" PRIu32 " running=%d\r\n",
                   latest, running);
        }
    } else if (strncmp(cmd, "agent-watch", 11) == 0 && word_boundary(cmd[11])) {
        run_agent_watch_command(cmd + 11, out, DIAG_CLI_OUT_MAX);
    } else if (strncmp(cmd, "agent-pause", 11) == 0 &&
               word_boundary(cmd[11])) {
        run_agent_control_command("agent-pause", cmd + 11,
                                  si_diagnostics_agent_run_pause,
                                  out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "agent-resume", 12) == 0 &&
               word_boundary(cmd[12])) {
        run_agent_control_command("agent-resume", cmd + 12,
                                  si_diagnostics_agent_run_resume,
                                  out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "agent-cancel", 12) == 0 &&
               word_boundary(cmd[12])) {
        run_agent_control_command("agent-cancel", cmd + 12,
                                  si_diagnostics_agent_run_cancel,
                                  out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "agent-abort", 11) == 0 &&
               word_boundary(cmd[11])) {
        run_agent_control_command("agent-abort", cmd + 11,
                                  si_diagnostics_agent_run_abort,
                                  out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "agent-steer", 11) == 0 &&
               word_boundary(cmd[11])) {
        run_agent_steer_command(cmd + 11, out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strncmp(cmd, "agent-result", 12) == 0 &&
               word_boundary(cmd[12])) {
        run_agent_result_command(cmd + 12, out, DIAG_CLI_OUT_MAX);
        print_diag_output(out);
    } else if (strcmp(cmd, "sync") == 0 || strcmp(cmd, "uart-sync") == 0 ||
               strcmp(cmd, "abort") == 0) {
        s_input_quarantine = false;
        printf("sync ok\r\n");
    } else if (strcmp(cmd, "reboot") == 0) {
        printf("rebooting...\r\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        print_unknown_command(cmd);
    }

    free(out);
}

static void diag_cli_task(void *arg)
{
    (void)arg;
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\r\nExoAnchor UART CLI ready. Type 'help'.\r\n");
    char line[DIAG_CLI_LINE_MAX];
    char command[DIAG_CLI_LINE_MAX];
    size_t command_len = 0;
    bool need_prompt = true;
    bool discard_input = false;
    command[0] = '\0';
    while (true) {
        if (need_prompt) {
            printf("%s", s_prompt_capture.active ? "agent> " : "exo> ");
            fflush(stdout);
            need_prompt = false;
        }
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            clearerr(stdin);
            continue;
        }
        bool has_eol = line_has_eol(line);
        size_t frag_len = strlen(line);
        if (has_eol && line_is_resync_command(line)) {
            if (s_prompt_capture.active) {
                agent_prompt_capture_clear();
            }
            // fgets() returns a full fragment without EOL when a physical input
            // line exceeds the buffer. Ignore every remaining fragment until
            // that same line terminates; otherwise its tail is executed as a
            // new command and contaminates the following transaction.
            discard_input = false;
            command_len = 0;
            command[0] = '\0';
            s_input_quarantine = false;
            clearerr(stdin);
            printf("sync ok\r\n");
            need_prompt = true;
            continue;
        }
        if (discard_input) {
            if (has_eol) {
                discard_input = false;
                command_len = 0;
                command[0] = '\0';
                printf("input resynced\r\n");
                need_prompt = true;
            }
            continue;
        }
        if (command_len + frag_len >= sizeof(command)) {
            if (s_prompt_capture.active) {
                agent_prompt_capture_clear();
                printf("input line too long; prompt capture cancelled; discarding until newline\r\n");
            } else {
                printf("input line too long; discarding until newline\r\n");
            }
            command_len = 0;
            command[0] = '\0';
            flush_stdin_input();
            discard_input = !has_eol;
            printf("input quarantine enabled\r\n");
            need_prompt = true;
            continue;
        }

        memcpy(command + command_len, line, frag_len + 1);
        command_len += frag_len;
        if (!has_eol) {
            continue;
        }

        if (s_input_quarantine && !command_looks_known(command)) {
            command_len = 0;
            command[0] = '\0';
            need_prompt = true;
            continue;
        }
        if (s_input_quarantine && command_looks_known(command)) {
            s_input_quarantine = false;
        }

        if (handle_agent_prompt_capture_line(command)) {
            command_len = 0;
            command[0] = '\0';
            need_prompt = true;
            continue;
        }
        run_diag_command(command);
        command_len = 0;
        command[0] = '\0';
        need_prompt = true;
    }
}

esp_err_t si_diag_cli_start(void)
{
    if (s_diag_cli_task) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreateWithCaps(
        diag_cli_task, "si_diag_cli", DIAG_CLI_TASK_STACK, NULL,
        tskIDLE_PRIORITY + 1, &s_diag_cli_task,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create diag cli task");
    return ESP_OK;
}
