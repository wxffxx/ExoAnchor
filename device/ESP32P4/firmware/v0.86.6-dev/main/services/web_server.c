#include "web_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <unistd.h>
#include "action_event.h"
#include "app_config.h"
#include "agent_api_settings.h"
#include "agent_cloud_retry_policy.h"
#if SI_CFG_EMBEDDED_AGENT_ENABLED
#include "agent_http_routes.h"
#include "agent_page_context_checkpoint.h"
#include "agent_prompt_service.h"
#include "agent_request_broker.h"
#include "agent_router.h"
#include "agent_skill_service.h"
#endif
#include "agent_tools_settings.h"
#include "agent_history_writer.h"
#include "agent_repository.h"
#include "auth_service.h"
#include "cJSON.h"
#if SI_CFG_EMBEDDED_AGENT_ENABLED
#include "agent_result_error.h"
#endif
#include "control_lease.h"
#include "console_credentials.h"
#include "device_http.h"
#include "device_observation_service.h"
#include "device_settings.h"
#include "diagnostics_service.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "hid_device.h"
#include "hid_ascii.h"
#include "hid_json.h"
#include "http_api.h"
#include "host_display_http.h"
#include "host_display_manager.h"
#if SI_CFG_EMBEDDED_AGENT_ENABLED
#include "model_provider.h"
#endif
#include "boot_key_http.h"
#include "boot_key_manager.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "esp_rom_crc.h"
#include "ms2109_power.h"
#include "ms2109_test.h"
#include "net_manager.h"
#include "network_http.h"
#include "nvs_flash.h"
#include "power_control.h"
#include "resource_operation_state.h"
#include "sdkconfig.h"
#include "secret_store.h"
#include "ssh_client.h"
#include "storage_path.h"
#include "storage_manager.h"
#include "status_ws.h"
#include "target_uart.h"
#include "target_profile_settings.h"
#include "terminal_control_state.h"
#include "time_utils.h"
#include "utf8_utils.h"
#include "version_utils.h"
#include "video_control.h"
#if SI_CFG_VIDEO_H264_ENABLED
#include "video_h264_stream.h"
#endif
#include "video_stream_metrics.h"
#include "video_input.h"
#include "web_json.h"
static const char *TAG = "si-web";
extern const uint8_t www_index_html_start[] asm("_binary_index_html_start"), www_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t www_kvm_html_start[] asm("_binary_kvm_html_start");
extern const uint8_t www_kvm_html_end[] asm("_binary_kvm_html_end");
#if SI_CFG_EMBEDDED_AGENT_ENABLED
extern const uint8_t www_agent_html_start[] asm("_binary_agent_html_start");
extern const uint8_t www_agent_html_end[] asm("_binary_agent_html_end");
#endif
#if SI_CFG_UART_TERMINAL_ENABLED
extern const uint8_t www_terminal_html_start[] asm("_binary_terminal_html_start");
extern const uint8_t www_terminal_html_end[] asm("_binary_terminal_html_end");
#endif
extern const uint8_t www_ui_core_css_start[] asm("_binary_ui_core_css_start");
extern const uint8_t www_ui_core_css_end[] asm("_binary_ui_core_css_end");
extern const uint8_t www_ui_core_js_start[] asm("_binary_ui_core_js_start");
extern const uint8_t www_ui_core_js_end[] asm("_binary_ui_core_js_end");
extern const uint8_t www_ui_shell_css_start[] asm("_binary_ui_shell_css_start");
extern const uint8_t www_ui_shell_css_end[] asm("_binary_ui_shell_css_end");
extern const uint8_t www_ui_shell_js_start[] asm("_binary_ui_shell_js_start");
extern const uint8_t www_ui_shell_js_end[] asm("_binary_ui_shell_js_end");
extern const uint8_t www_exoanchor_ui_mark_svg_start[] asm("_binary_exoanchor_ui_mark_svg_start");
extern const uint8_t www_exoanchor_ui_mark_svg_end[] asm("_binary_exoanchor_ui_mark_svg_end");
extern const uint8_t www_favicon_svg_start[] asm("_binary_favicon_svg_start");
extern const uint8_t www_favicon_svg_end[] asm("_binary_favicon_svg_end");
#if SI_CFG_UART_TERMINAL_ENABLED
extern const uint8_t www_xterm_css_start[] asm("_binary_xterm_css_start");
extern const uint8_t www_xterm_css_end[] asm("_binary_xterm_css_end");
extern const uint8_t www_xterm_js_start[] asm("_binary_xterm_js_start");
extern const uint8_t www_xterm_js_end[] asm("_binary_xterm_js_end");
extern const uint8_t www_addon_fit_js_start[] asm("_binary_addon_fit_js_start");
extern const uint8_t www_addon_fit_js_end[] asm("_binary_addon_fit_js_end");
#endif
extern const uint8_t www_settings_html_start[] asm("_binary_settings_html_start");
extern const uint8_t www_settings_html_end[] asm("_binary_settings_html_end");
#define agent_sanitize_chat_content si_utf8_sanitize
#define SCRATCH_BUFSIZE 1024
#define SI_MAIN_HTTP_MAX_OPEN_SOCKETS 24
#define SI_MAIN_HTTP_MAX_URI_HANDLERS 160
#define SI_STREAM_HTTP_MAX_OPEN_SOCKETS 4
#define SI_HTTPD_INTERNAL_SOCKETS_PER_SERVER 3
#define SI_HTTPD_SERVER_COUNT 2
#define SI_NETWORK_SOCKET_HEADROOM 8
#define SI_HTTP_SOCKET_BUDGET_REQUIRED                                      \
    (SI_MAIN_HTTP_MAX_OPEN_SOCKETS + SI_STREAM_HTTP_MAX_OPEN_SOCKETS +      \
     SI_HTTPD_INTERNAL_SOCKETS_PER_SERVER * SI_HTTPD_SERVER_COUNT +          \
     SI_NETWORK_SOCKET_HEADROOM)
_Static_assert(CONFIG_LWIP_MAX_SOCKETS >= SI_HTTP_SOCKET_BUDGET_REQUIRED,
               "LWIP socket budget cannot cover both HTTP servers and "
               "outbound Agent/SSH clients");
#define SSH_KEY_NAMESPACE "si_ssh_key"
#define SSH_KEY_PRIVATE_KEY "private"
#define SSH_KEY_PUBLIC_KEY "public"
#define SSH_KEY_PASSPHRASE_KEY "passphrase"
#define SSH_KEY_MAX_BODY 8192
#define SSH_TARGET_NAMESPACE "si_ssh_target"
#define SSH_TARGET_HOST_KEY "host"
#define SSH_TARGET_PORT_KEY "port"
#define SSH_TARGET_USERNAME_KEY "username"
#define SSH_TARGET_PASSWORD_KEY "password"
#define SSH_TARGET_SUDO_PASSWORD_KEY "sudo_password"
#define SSH_TARGET_AUTH_METHOD_KEY "auth"
#define SSH_TARGET_TIMEOUT_MS_KEY "timeout_ms"
#define SSH_TARGET_AUTH_MAX_LEN 16
#define SSH_TARGET_MAX_BODY 1024
#define SSH_TARGET_DEFAULT_PORT 22U
#define SSH_TARGET_DEFAULT_TIMEOUT_MS 30000U
#define SSH_TARGET_MAX_TIMEOUT_MS 600000U
#define AGENT_API_PROVIDER_DEFAULT "deepseek"
#define AGENT_API_MODEL_DEFAULT SI_AGENT_API_MODEL_DEFAULT
#define AGENT_API_PROFILE_COUNT SI_AGENT_API_PROFILE_COUNT
#define AGENT_API_PROFILE_ID_MAX_LEN SI_AGENT_API_PROFILE_ID_MAX_LEN
#define AGENT_API_PROFILE_LABEL_MAX_LEN SI_AGENT_API_PROFILE_LABEL_MAX_LEN
#define AGENT_API_PROVIDER_MAX_LEN SI_AGENT_API_PROVIDER_MAX_LEN
#define AGENT_API_ENDPOINT_MAX_LEN SI_AGENT_API_ENDPOINT_MAX_LEN
#define AGENT_API_MODEL_MAX_LEN SI_AGENT_API_MODEL_MAX_LEN
#define AGENT_API_SECRET_MAX_LEN SI_AGENT_API_SECRET_MAX_LEN
#define agent_api_validate_text si_agent_api_validate_text
#define agent_api_validate_endpoint si_agent_api_validate_endpoint
#define agent_api_get_profile si_agent_api_get_profile
#define agent_api_save_profile si_agent_api_save_profile
#define agent_api_get_secret_for_profile si_agent_api_get_secret
#define agent_api_profile_slot(id) (si_agent_api_profile_id_valid(id) ? 0 : -1)
#define AGENT_PROMPT_MAX_BODY 4096
#define AGENT_TOOLS_MODE_KEY SI_AGENT_TOOLS_MODE_KEY
#define AGENT_TOOLS_POLICY_KEY SI_AGENT_TOOLS_POLICY_KEY
#define AGENT_TOOLS_SKILLS_KEY SI_AGENT_TOOLS_SKILLS_KEY
#define AGENT_TOOLS_POLICY_MAX_LEN SI_AGENT_TOOLS_POLICY_MAX_LEN
#define AGENT_TOOLS_SKILLS_MAX_LEN SI_AGENT_TOOLS_SKILLS_MAX_LEN
#define AGENT_TOOLS_MAX_BODY 12288
#define AGENT_WEB_SEARCH_PROFILE_DEFAULT "qwen"
#define AGENT_WEB_SEARCH_MODEL_DEFAULT "qwen-plus"
#define AGENT_WEB_SEARCH_STRATEGY_DEFAULT "agent"
#define AGENT_WEB_SEARCH_QUERY_MAX_LEN 256
#define AGENT_WEB_SEARCH_STRATEGY_MAX_LEN 16
#define agent_tools_load_string si_agent_tools_load_string
#define agent_web_search_defaults si_agent_web_search_defaults
#define agent_web_search_load si_agent_web_search_load
#define agent_web_search_get_secret si_agent_web_search_get_secret
#define agent_web_search_available si_agent_web_search_available
#define agent_tools_reset_settings si_agent_tools_reset
#define s_agent_history_lock si_agent_repository_lock()
#define s_agent_history_last_error si_agent_repository_last_error()
#define AGENT_RUN_MAX_BODY 8192
#define AGENT_CLOUD_TIMEOUT_MS 60000
#define AGENT_CLOUD_RESPONSE_MAX (64 * 1024)
#define AGENT_HISTORY_MAX_BODY 4096
#define AGENT_HISTORY_MAX_RECORD_BYTES 3072U
#define AGENT_HISTORY_RETURN_MAX 80U
#define AGENT_HISTORY_CONTEXT_MAX 12U
#define AGENT_HISTORY_CONTEXT_TEXT_MAX 700U
#define AGENT_HISTORY_CONTENT_STORE_MAX 1800U
#define AGENT_HISTORY_TF_ROOT SI_STORAGE_ROOT
#define AGENT_HISTORY_TF_DIR SI_STORAGE_AGENT_DIR
#define AGENT_HISTORY_TF_FILE AGENT_HISTORY_TF_DIR "/HIST.LOG"
#define AGENT_MEMORY_TF_FILE AGENT_HISTORY_TF_DIR "/MEMORY.LOG"
#define AGENT_HISTORY_LINE_MAX (AGENT_HISTORY_MAX_RECORD_BYTES + 256U)
#define AGENT_HISTORY_ERROR_MAX 128
#define AGENT_MEMORY_RETURN_MAX 48U
#define AGENT_MEMORY_CONTEXT_MAX 10U
#define AGENT_MEMORY_CONTEXT_TEXT_MAX 520U
#define AGENT_MEMORY_CONTEXT_BYTES_MAX 2400U
#define AGENT_MEMORY_TYPE_MAX_LEN 24
#define AGENT_MEMORY_SEARCH_RETURN_MAX 12U
#define AGENT_HISTORY_SEARCH_RETURN_MAX 16U
#define AGENT_SEARCH_QUERY_MAX_LEN 160
#define AGENT_SEARCH_RECORD_TEXT_MAX 900U
#define AGENT_DATA_CLEAR_CONFIRMATION "DELETE_ALL_AGENT_DATA"
#define STORAGE_LIST_QUERY_MAX 192
#define STORAGE_LIST_PATH_MAX 192
#define STORAGE_LIST_MAX_ITEMS 96U
#define STORAGE_UPLOAD_MAX_BYTES (64U * 1024U * 1024U)
#define STORAGE_VIEW_MAX_BYTES 8192U
#define STORAGE_IO_BUFFER_SIZE 4096U
#define AGENT_TOOL_CALLS_MAX_ITEMS 8
#define AGENT_TOOL_OUTPUT_MAX 4096U
#define AGENT_TOOL_LOOP_MAX 6
#define AGENT_SESSION_ID_MAX_LEN 48
#define AGENT_SESSION_TITLE_MAX_LEN 48
#define AGENT_SESSION_RETURN_MAX 24U
#define AGENT_SESSION_DEFAULT_ID "default"
#define AGENT_RUN_JOB_ID_MAX_LEN 48
#define AGENT_RUN_STAGE_MAX_LEN 40
#define AGENT_RUN_DETAIL_MAX_LEN 160
#define AGENT_RUN_ERROR_MAX_LEN 192
#define AGENT_TURN_ID_MAX_LEN 48
#define AGENT_RUN_GOAL_MAX_LEN 512
#define AGENT_RUN_STEER_MAX_LEN 512
#define AGENT_PAGE_CONTEXT_SCHEMA_VERSION 1
#define AGENT_PAGE_CONTEXT_MAX_LEN 2048
#define AGENT_PAGE_CONTEXT_SUMMARY_MAX_LEN 256
#define AGENT_PAGE_CONTEXT_ITEM_MAX 8
#define AGENT_PAGE_CONTEXT_LABEL_MAX_LEN 64
#define AGENT_PAGE_CONTEXT_VALUE_MAX_LEN 384
#define AGENT_RUN_TASK_STACK 24576
#define AGENT_RUN_TASK_MEMORY_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define HID_WS_MAX_FRAME 1024
#define HID_ACTIONS_MAX_BODY 4096
#define HID_ACTIONS_MAX_ITEMS 64
#define OTA_NAMESPACE "si_ota"
#define OTA_MANIFEST_URL_KEY "manifest_url"
#define OTA_CHANNEL_KEY "channel"
#define OTA_AUTO_CHECK_KEY "auto_check"
#define OTA_MANIFEST_URL_DEFAULT ""
#define OTA_CHANNEL_DEFAULT "stable"
#define OTA_URL_MAX_LEN 256
#define OTA_VERSION_MAX_LEN 32
#define OTA_CHANNEL_MAX_LEN 16
#define OTA_SHA256_HEX_LEN 64
#define OTA_NOTES_MAX_LEN 128
#define OTA_MANIFEST_MAX_SIZE 4096
#define OTA_IO_BUFFER_SIZE 4096
#define OTA_UPLOAD_IDLE_TIMEOUT_MS 30000
#define OTA_UPLOAD_SESSION_TIMEOUT_MS 300000
#define OTA_UPLOAD_SESSION_ID_LEN 16
#define OTA_UPLOAD_CHUNK_MAX_SIZE (64U * 1024U)
#if SI_CFG_EMBEDDED_AGENT_ENABLED
#include "web/agent_runtime_types_module.inc"
#endif
typedef struct {
    bool configured;
    bool password_configured;
    bool sudo_password_configured;
    char host[SI_SSH_HOST_MAX_LEN];
    uint16_t port;
    char username[SI_SSH_USERNAME_MAX_LEN];
    char auth_method[SSH_TARGET_AUTH_MAX_LEN + 1];
    uint32_t timeout_ms;
} ssh_target_settings_t;
typedef struct {
    bool active;
    bool sha_started;
    char session_id[OTA_UPLOAD_SESSION_ID_LEN + 1];
    char expected_sha256[OTA_SHA256_HEX_LEN + 1];
    uint32_t expected_size;
    uint32_t written;
    int64_t last_progress_us;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    mbedtls_sha256_context sha;
} ota_upload_session_t;
typedef si_agent_web_search_settings_t agent_web_search_settings_t;
static const char *AGENT_TOOLS_POLICY_DEFAULT =
    "{\"version\":2,\"tools\":{}}";
#if SI_CFG_EMBEDDED_AGENT_ENABLED
static const char *AGENT_TOOLS_SKILLS_DEFAULT =
    "["
    "{\"name\":\"system_checklist\",\"enabled\":true,\"mode\":\"guided\",\"description\":\"系统故障检查清单\",\"tools\":[\"observe_status\",\"observe_hid_status\",\"observe_video_status\",\"uart_status\",\"uart_read\",\"ssh_exec\"]},"
    "{\"name\":\"ssh_system_snapshot\",\"enabled\":true,\"mode\":\"scripted\",\"description\":\"通过 SSH 采集 uname、磁盘、内存和服务状态\",\"tools\":[\"ssh_exec\"]},"
    "{\"name\":\"agent_memory_ops\",\"enabled\":false,\"mode\":\"guided\",\"description\":\"恢复候选禁用同步 TF 记忆与历史工具\",\"tools\":[]},"
    "{\"name\":\"web_research_mvp\",\"enabled\":true,\"mode\":\"guided\",\"description\":\"通过联网搜索获取实时资料后再执行运维判断\",\"tools\":[\"web_search\",\"ask_user\"]},"
    "{\"name\":\"uart_console_diagnostics\",\"enabled\":true,\"mode\":\"guided\",\"description\":\"非破坏读取 UART 状态和输出；经 Request Broker 精确授权后写入串口、提交本地凭据或切换波特率\",\"tools\":[\"uart_status\",\"uart_read\",\"uart_write\",\"uart_auth\",\"uart_baud\"]},"
    "{\"name\":\"configure-uart-cli\",\"version\":\"1.0.0\",\"source\":\"firmware\",\"enabled\":true,\"mode\":\"guided\",\"description\":\"配置并验证 Ubuntu serial-getty UART 登录终端；UART 路由按需加载内置 Skill 正文\",\"tools\":[\"uart_status\",\"uart_read\",\"uart_write\",\"uart_auth\",\"uart_baud\"]},"
    "{\"name\":\"configure-access-via-kvm\",\"version\":\"1.0.0\",\"source\":\"firmware\",\"enabled\":true,\"mode\":\"guided\",\"description\":\"仅有 KVM 可用时，通过目标机图形终端建立并独立验证 UART 与 SSH 维护通道\",\"tools\":[\"observe_video_status\",\"observe_screenshot\",\"observe_hid_status\",\"hid_actions\",\"console_login\",\"uart_status\",\"uart_read\",\"ssh_exec\",\"ask_user\"]},"
    "{\"name\":\"bridge-uart-ssh-access\",\"version\":\"1.0.0\",\"source\":\"firmware\",\"enabled\":true,\"mode\":\"guided\",\"description\":\"保留当前可用通道，通过 UART 或 SSH 建立、恢复并验证另一条维护通道\",\"tools\":[\"uart_status\",\"uart_read\",\"uart_write\",\"uart_auth\",\"uart_baud\",\"ssh_exec\",\"ask_user\"]},"
    "{\"name\":\"ubuntu_host_display\",\"enabled\":true,\"mode\":\"guided\",\"description\":\"观察 Ubuntu DRM、生成精确显示模式计划，经浏览器批准后持久应用、读回验证或回滚\",\"tools\":[\"host_display\",\"observe_video_status\",\"observe_screenshot\"]},"
    "{\"name\":\"host_firmware_entry\",\"enabled\":true,\"mode\":\"guided\",\"description\":\"按 Host profile 提议有界 BIOS/Boot Menu 发键计划；浏览器负责批准、启动和人工画面确认\",\"tools\":[\"boot_key_sequence\",\"observe_video_status\",\"observe_screenshot\"]},"
    "{\"name\":\"kvm_console_login\",\"enabled\":true,\"mode\":\"guided\",\"description\":\"根据 KVM 画面定位登录控件，并通过设备本地凭据引用完成 Console 登录；密码不进入模型上下文\",\"tools\":[\"observe_video_status\",\"observe_screenshot\",\"observe_hid_status\",\"hid_actions\",\"console_login\"]},"
    "{\"name\":\"minecraft_server_mvp\",\"enabled\":true,\"mode\":\"guided\",\"description\":\"通过通用 SSH 工具原语安装并配置 Paper Minecraft Server 的 MVP 验证技能\",\"tools\":[\"ssh_exec\",\"ensure_package\",\"ensure_user\",\"ensure_directory\",\"download_file\",\"write_file\",\"ensure_systemd_service\",\"check_service\",\"verify_port\"]},"
    "{\"name\":\"kvm_os_install\",\"enabled\":false,\"mode\":\"guided\",\"description\":\"通过 HDMI 截图和 HID 完成系统安装流程\",\"tools\":[\"observe_screenshot\",\"hid_actions\",\"power_action\"]}"
    "]";
#else
static const char *AGENT_TOOLS_SKILLS_DEFAULT = "[]";
#endif
#if SI_CFG_EMBEDDED_AGENT_ENABLED
static const char AGENT_RUNTIME_TOOL_PROMPT[] =
    "ExoAnchor runtime tool contract: Return JSON only with keys message, tool_calls, actions. "
    "Use tool_calls for non-HID tools and actions only for USB HID keyboard/mouse. "
    "Optional trace is a short user-visible plan/progress list, not hidden chain-of-thought. "
    "Implemented tool catalog (the runtime context separately lists tools disabled for the local Agent; never call those): observe_status, args={}; "
    "observe_video_status, args={}; observe_hid_status, args={}; "
    "console_login, args={credential_ref:\"console://default\",stage:status|username|password|both|type_username|type_password,between?:tab|enter,key_delay_ms?,inter_field_delay_ms?}; "
    "ssh_exec, args={command, timeout_ms?}; "
    "uart_status, args={}; "
    "uart_read, args={cursor?, max_bytes?, wait_ms?}; "
    "uart_write, args={data, append_enter?, wait_ms?, read_max_bytes?}; "
    "uart_auth, args={credential_ref?:auto://sudo|ssh-sudo://default|console://default, expected_prompt, wait_ms?, read_max_bytes?}; "
    "uart_baud, args={baud_rate}; "
    "power_action, args={action:power|reset|force_off|locator_on|locator_off|locator_toggle,duration_ms?}; "
    "memory_search, history_search, and memory_write are unavailable in this recovery candidate; "
    "web_search, args={query}; "
    "wait, args={ms}; "
    "ask_user, args={question}; "
    "host_display, args={operation:observe|plan|status|apply|verify|rollback,"
    "plan_id?,connector?,width?,height?,refresh_hz?,persistent?}; "
    "boot_key_sequence, args={operation:status|plan,profile_id?,target?,trigger?,"
    "key_code?,start_delay_ms?,interval_ms?,max_attempts?,total_timeout_ms?}; "
    "ensure_package, args={packages:[...]}; ensure_user, args={user,home?,shell?}; "
    "ensure_directory, args={path,owner?,mode?}; download_file, args={url,path,sha256?,owner?}; "
    "write_file, args={path,content,owner?,mode?}; "
    "ensure_systemd_service, args={name,content,enable?,start?}; "
    "check_service, args={name}; verify_port, args={port}. "
    "ssh_exec runs on the Settings SSH target by default; do not include passwords or passphrases in tool args. "
    "Console credentials are represented only by console://default. Never request, infer, repeat, log, or place the username/password in tool args, actions, memory, UART, or messages. "
    "For KVM login, inspect the current screenshot, use ordinary HID actions only to focus the correct login field, then call console_login. "
    "Within one model batch tool_calls run before actions, so never return a focus/click action and console_login together: focus first with actions, wait for the next fresh frame, then call console_login in the next loop. "
    "Use stage=password after selecting an existing user tile; stage=both with between=enter for a two-step text console, or between=tab for a same-page username/password form. "
    "Use type_username/type_password only when the following submit/navigation must be a separately observed HID action. After credential injection, inspect a fresh KVM frame before claiming login succeeded. "
    "UART is independent from SSH. Before claiming UART is unavailable, call uart_status. "
    "Use uart_read with cursor=0 to inspect the retained serial journal without consuming manual Terminal output. "
    "For a bounded console command, call uart_write with append_enter=true and inspect its output; continue uart_read from next_cursor when pending_bytes is nonzero. "
    "UART write and baud changes require an exact Request Broker decision, and fail closed while a manual UART WebSocket is connected because manual control has priority. "
    "Never send passwords, API keys, tokens, or other secrets through uart_write. "
    "When UART shows an exact password prompt, call uart_auth with that expected_prompt and a credential_ref; the device submits the stored secret locally and never returns it. "
    "Do not request memory/history tools: synchronous TF access is disabled until a bounded storage worker exists. "
    "web_search is a runtime tool, not an automatic model feature; use it only when Web search effective=yes and current external information is needed. "
    "Use ask_user only when a concrete piece of missing human information blocks progress; its answer is returned as a tool result bound to the same Run. Do not use it to ask for approval because the Request Broker handles exact action approval. "
    "For Ubuntu display recovery, call host_display observe before plan. A plan must name the observed DRM connector and exact integer refresh. "
    "host_display apply is fail-closed until that exact plan_id is approved by an authenticated browser session at /api/host/display; never substitute ssh_exec to bypass this approval. "
    "Persistent apply only installs the dedicated ExoAnchor GRUB drop-in and backup; after reboot call host_display verify and also inspect ExoAnchor video/KVM evidence. "
    "Use host_display rollback for the approved plan when verification fails. Temporary apply is unsupported until a detected backend is available. "
    "For BIOS or Boot Menu entry, boot_key_sequence may only inspect or propose an exact Host-profile plan. The authenticated browser must approve and start it; only a human viewing KVM may confirm success. Never use raw hid_actions or power_action to bypass this workflow, and never navigate an unknown BIOS menu autonomously. "
    "Use power_action only for the user's explicit target-host power/reset request or locator control; every call remains separately policy and Request Broker gated. "
    "For installation tasks, compose small verified tool calls with these primitives instead of generating one long shell script. "
    "If sudo is needed, use sudo normally in the command; firmware may inject the locally stored sudo password and never sends it to the API. "
    "In execute mode the user's request is approval for the current task; keep making concrete progress with tool_calls instead of asking to continue. "
    "For explicit install, configure, deploy, or service setup tasks, a prerequisite/environment check alone is not completion; continue until the requested artifact/service is installed and verified, or a concrete blocking tool error remains. "
    "For background services, screen/tmux sessions, systemd/docker units, sudo/su, or separate run users, completion requires human operability verification: identify the run owner versus the configured SSH login user, verify the exact operator command to access/control it from that login context, and report reboot/autostart behavior. "
    "Do not claim such a task is done if the service runs but the operator access path was not verified. "
    "Keep each ssh_exec command under 3500 bytes; split into another tool loop when the script would be longer. "
    "If a tool fails, inspect its output and attempt a bounded safe remediation in the next tool loop instead of stopping immediately. "
    "For package-manager mirror/download errors, diagnose the source and switch to an official or reachable mirror before retrying. "
    "If an upstream official API returns 410, sunset, deprecated, schema mismatch, null, or empty data, do not declare the task blocked immediately; call web_search when Web search effective=yes, or test documented successor official endpoints, then continue with the new official source. "
    "Ask the user only before destructive or connectivity-breaking operations such as disk formatting, mass deletion, reboot/poweroff, or network changes that may drop SSH. "
    "If the user asks to check machine status via SSH, call ssh_exec with a bounded read-only command "
    "such as uname -a; uptime; df -h; free -h; systemctl --failed --no-pager. "
    "In dry-run mode still propose tool_calls; firmware will skip execution. "
    "Never say you cannot SSH just because you are a KVM Agent.";
static void agent_route_add_json(cJSON *root, const si_agent_route_t *route)
{
    if (!root || !route) {
        return;
    }
    cJSON *obj = cJSON_AddObjectToObject(root, "route");
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "name", route->name ? route->name : "chat");
    cJSON_AddStringToObject(obj, "intent", route->intent ? route->intent : "");
    cJSON_AddStringToObject(obj, "prompt_profile",
                            route->prompt_profile ? route->prompt_profile : "");
    cJSON_AddStringToObject(obj, "tool_policy",
                            route->tool_policy ? route->tool_policy : "");
    cJSON_AddStringToObject(obj, "final_report_style",
                            route->final_report_style ? route->final_report_style : "");
    cJSON_AddNumberToObject(obj, "confidence", route->confidence);
    cJSON_AddNumberToObject(obj, "max_tool_loops", route->max_tool_loops);
    cJSON_AddBoolToObject(obj, "needs_control_lease", route->needs_control_lease);
    cJSON_AddBoolToObject(obj, "prefer_memory", route->prefer_memory);
    cJSON_AddBoolToObject(obj, "prefer_screenshot", route->prefer_screenshot);
    cJSON_AddBoolToObject(obj, "prefer_web_search", route->prefer_web_search);
}
#endif
static httpd_handle_t s_server;
static httpd_handle_t s_stream_server;
#if SI_CFG_EMBEDDED_AGENT_ENABLED
static SemaphoreHandle_t s_agent_run_lock;
static uint32_t s_agent_page_context_generation, s_agent_run_page_context_generation;
#endif
static SemaphoreHandle_t s_ota_lock;
#if SI_CFG_EMBEDDED_AGENT_ENABLED
static EXT_RAM_BSS_ATTR agent_run_job_t s_agent_run_job;
static uint32_t s_agent_run_seq; static bool s_agent_run_initialized;
#endif
static bool s_ota_settings_loaded;
static char s_ota_manifest_url[OTA_URL_MAX_LEN + 1];
static char s_ota_channel[OTA_CHANNEL_MAX_LEN + 1];
static bool s_ota_auto_check;
static bool s_ota_busy;
static bool s_ota_last_checked;
static bool s_ota_last_update_available;
static uint64_t s_server_boot_id;
static char s_ota_last_version[OTA_VERSION_MAX_LEN + 1];
static char s_ota_last_url[OTA_URL_MAX_LEN + 1];
static char s_ota_last_sha256[OTA_SHA256_HEX_LEN + 1];
static char s_ota_last_notes[OTA_NOTES_MAX_LEN + 1];
static uint32_t s_ota_last_size;
static uint32_t s_ota_last_check_sec;
static char s_ota_last_message[128] = "not checked";
static ota_upload_session_t s_ota_upload_session;
#include "web/server_runtime_util_module.inc"
void si_web_log(const char *level, const char *message) { si_device_observation_log_append(level, message); }
static void *agent_alloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ptr ? ptr : malloc(size);
}
static void init_json_memory_policy(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = agent_alloc,
        .free_fn = free,
    };
    cJSON_InitHooks(&hooks);
}
#if SI_CFG_EMBEDDED_AGENT_ENABLED
static bool agent_run_page_context_allowed(void) {
    return __atomic_load_n(&s_agent_run_page_context_generation, __ATOMIC_ACQUIRE) ==
           __atomic_load_n(&s_agent_page_context_generation, __ATOMIC_ACQUIRE);
}
#include "web/agent_skill_registry_module.inc"
static esp_err_t agent_run_revoke_page_context(const si_product_feature_settings_t *settings);
#endif
#include "web/resource_operation_contract.inc"
#include "web/base_settings_http_module.inc"
#include "web/access_mode_http_module.inc"
#include "web/product_features_http_module.inc"
#include "web/power_http_module.inc"
#include "web/ota_settings_module.inc"
#include "web/console_credentials_module.inc"
#include "web/console_login_mcp_module.inc"
#include "web/terminal_control_module.inc"
#include "web/ssh_module.inc"
#include "web/ssh_bootstrap_module.inc"
#include "web/uart_module.inc"
#include "web/ota_runtime_module.inc"
#include "web/storage_http_module.inc"
#include "web/ms2109_test_http_module.inc"
#if SI_CFG_EMBEDDED_AGENT_ENABLED
#include "web/agent_run_forward_contract.inc"
#include "web/agent_request_module.inc"
#include "web/agent_data_http_module.inc"
#include "web/agent_sessions_module.inc"
#include "web/agent_tool_core_module.inc"
#include "web/agent_memory_tools_module.inc"
#include "web/agent_ssh_tool_module.inc"
#include "web/agent_device_tools_module.inc"
#include "web/agent_uart_tool_module.inc"
#include "web/agent_host_display_tool_module.inc"
#include "web/agent_boot_key_tool_module.inc"
#include "web/agent_web_search_tool_module.inc"
#include "web/agent_ssh_primitives_module.inc"
#include "web/agent_broker_http_module.inc"
#include "web/agent_run_status_snapshot_module.inc"
#include "web/agent_run_checkpoint_module.inc"
#include "web/agent_tool_dispatch_module.inc"
#include "web/agent_run_engine_module.inc"
#include "web/agent_run_task_module.inc"
#include "web/agent_run_http_module.inc"
#endif
#include "web/resource_operation_module.inc"
#include "web/system_maintenance_module.inc"
#include "web/video_http_module.inc"
#include "web/async_http_module.inc"
static esp_err_t ws_auth_pre_handshake(httpd_req_t *req)
{ return si_http_check_auth(req) ? ESP_OK : ESP_FAIL; }
static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"error\":\"not found\"}"); }
static void register_uri(httpd_handle_t server, const char *uri, httpd_method_t method,
                         esp_err_t (*handler)(httpd_req_t *), bool websocket) {
    httpd_uri_t cfg = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = NULL,
    };
#ifdef CONFIG_HTTPD_WS_SUPPORT
    cfg.is_websocket = websocket;
    if (websocket) {
        cfg.ws_pre_handshake_cb = handler == hid_ws_handler ? hid_ws_pre_handshake : ws_auth_pre_handshake;
        if (handler == ssh_ws_handler) { cfg.ws_pre_handshake_cb = ssh_ws_pre_handshake; cfg.ws_post_handshake_cb = ssh_ws_post_handshake; }
#if SI_CFG_UART_TERMINAL_ENABLED
        if (handler == uart_ws_handler) { cfg.ws_pre_handshake_cb = uart_ws_pre_handshake; cfg.ws_post_handshake_cb = uart_ws_post_handshake; }
#endif
#if SI_CFG_VIDEO_H264_ENABLED
        if (handler == si_h264_stream_ws_handler) cfg.ws_post_handshake_cb = si_h264_stream_ws_post_handshake;
#endif
    }
#else
    (void)websocket;
#endif
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &cfg));
}
#include "web/video_stream_server_module.inc"
#include "web/diagnostics_backend_module.inc"
#if SI_CFG_EMBEDDED_AGENT_ENABLED
static esp_err_t register_agent_http_routes(void)
{
    static const si_agent_http_handlers_t handlers = {
        .settings_api = settings_agent_api_handler,
        .settings_models = agent_api_models_handler,
        .settings_prompt = settings_agent_prompt_handler,
        .settings_tools = settings_agent_tools_handler,
        .sessions_get = agent_sessions_get_async_handler,
        .sessions_post = agent_sessions_post_async_handler,
        .sessions_delete = agent_sessions_delete_async_handler,
        .history_get = agent_history_get_async_handler,
        .history_post = agent_history_post_async_handler,
        .history_clear = agent_history_clear_async_handler,
        .memory_get = agent_memory_get_async_handler,
        .memory_post = agent_memory_post_async_handler,
        .memory_clear = agent_memory_clear_async_handler,
        .data_clear = agent_data_clear_async_handler,
        .run_submit = agent_run_handler,
        .run_status = agent_run_status_handler,
        .run_events = agent_run_events_handler,
        .run_pause = agent_run_pause_handler,
        .run_resume = agent_run_resume_handler,
        .run_abort = agent_run_abort_handler,
        .run_cancel = agent_run_cancel_handler,
        .run_steer = agent_run_steer_handler,
        .requests_get = agent_requests_get_handler,
        .requests_decision = agent_request_decision_handler,
        .requests_cancel = agent_request_cancel_handler,
        .context_catalog = agent_context_catalog_handler,
    };
    return si_agent_http_routes_register(s_server, &handlers);
}
#endif
esp_err_t si_web_server_start(void)
{
    /*
     * JSON is control-plane data, not DMA or interrupt state. Keep its many
     * short-lived nodes in PSRAM so concurrent status/Agent requests cannot
     * fragment the scarce internal heap.
     */
    init_json_memory_policy();
    ESP_RETURN_ON_ERROR(si_agent_tools_cache_init(), TAG,
                        "initialize Agent tool settings cache");
    s_server_boot_id =
        ((uint64_t)esp_random() << 32U) | (uint64_t)esp_random();
    register_diagnostics_backend();
    (void)si_auth_is_enabled();
    if (!s_auth_login_lock) {
        s_auth_login_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_auth_login_lock, ESP_ERR_NO_MEM, TAG,
                            "create auth login mutex");
    }
    ESP_RETURN_ON_ERROR(init_http_worker_slots(), TAG,
                        "create HTTP worker slots");
    ota_settings_load();
    if (!s_ota_lock) {
        s_ota_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_ota_lock, ESP_ERR_NO_MEM, TAG, "create ota mutex");
    }
    ESP_RETURN_ON_ERROR(si_video_control_start(), TAG, "start video control");
    ESP_RETURN_ON_ERROR(si_control_lease_start(), TAG, "start control lease");
    ESP_RETURN_ON_ERROR(si_host_display_manager_start(), TAG,
                        "start Host display manager");
    ESP_RETURN_ON_ERROR(si_boot_key_manager_start(), TAG,
                        "start boot-key manager");
#if SI_CFG_EMBEDDED_AGENT_ENABLED
    ESP_RETURN_ON_ERROR(agent_history_start(), TAG, "start agent history");
    ESP_RETURN_ON_ERROR(si_agent_history_writer_start(), TAG, "start agent history writer");
    ESP_RETURN_ON_ERROR(si_agent_request_broker_start(), TAG, "start agent request broker");
    agent_task_service_prepare_shadow();
    ESP_RETURN_ON_ERROR(agent_run_start(), TAG, "start agent run manager");
#endif
    ESP_RETURN_ON_ERROR(terminal_control_start(), TAG, "start terminal control");
    ESP_RETURN_ON_ERROR(resource_operation_start(), TAG,
                        "start resource operation manager");
    ESP_RETURN_ON_ERROR(ssh_ws_bridge_start(), TAG, "start SSH WebSocket bridge");
    ESP_RETURN_ON_ERROR(uart_ws_bridge_start(), TAG,
                        "start target UART WebSocket bridge");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SI_DEFAULT_HTTP_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 12288;
    config.max_uri_handlers = SI_MAIN_HTTP_MAX_URI_HANDLERS;
    config.max_open_sockets = SI_MAIN_HTTP_MAX_OPEN_SOCKETS;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 1;
    // All normal requests share this task. If a browser cancels a large page
    // navigation, release the blocked send quickly for the next page.
    config.send_wait_timeout = 1;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start");
    register_uri(s_server, "/", HTTP_GET, index_handler, false);
    register_uri(s_server, "/kvm", HTTP_GET, kvm_handler, false);
#if SI_CFG_EMBEDDED_AGENT_ENABLED
    agent_task_service_start_shadow_async(); /* TASKS.LOG stays off Web stack. */
    register_uri(s_server, "/agent", HTTP_GET, agent_handler, false);
#endif
#if SI_CFG_UART_TERMINAL_ENABLED
    register_uri(s_server, "/terminal", HTTP_GET, terminal_handler, false);
#endif
    register_uri(s_server, "/assets/ui-core.css", HTTP_GET, ui_core_css_handler, false);
    register_uri(s_server, "/assets/ui-core.js", HTTP_GET, ui_core_js_handler, false);
    register_uri(s_server, "/assets/ui-shell.css", HTTP_GET, ui_shell_css_handler, false);
    register_uri(s_server, "/assets/ui-shell.js", HTTP_GET, ui_shell_js_handler, false);
    register_uri(s_server, "/assets/exoanchor-ui-mark.svg", HTTP_GET, exoanchor_ui_mark_svg_handler, false);
    register_uri(s_server, "/favicon.svg", HTTP_GET, favicon_svg_handler, false);
#if SI_CFG_UART_TERMINAL_ENABLED
    register_uri(s_server, "/assets/xterm/xterm.css", HTTP_GET, xterm_css_handler, false);
    register_uri(s_server, "/assets/xterm/xterm.js", HTTP_GET, xterm_js_handler, false);
    register_uri(s_server, "/assets/xterm/addon-fit.js", HTTP_GET, xterm_fit_js_handler, false);
#endif
#if SI_CFG_EMBEDDED_AGENT_ENABLED
    register_uri(s_server, "/skills", HTTP_GET, skills_handler, false);
#endif
    register_uri(s_server, "/settings", HTTP_GET, settings_handler, false);
    register_uri(s_server, "/api/auth/login", HTTP_POST, auth_login_handler, false);
    register_uri(s_server, "/api/auth/login/status", HTTP_GET, auth_login_status_handler, false);
    register_uri(s_server, "/api/auth/logout", HTTP_POST, auth_logout_handler, false);
    register_uri(s_server, "/api/auth/status", HTTP_GET, auth_status_handler, false);
    register_uri(s_server, "/api/settings/account", HTTP_POST, settings_password_handler, false);
    register_uri(s_server, "/api/settings/password", HTTP_POST, settings_password_handler, false);
    register_uri(s_server, "/api/settings/device", HTTP_GET, settings_device_handler, false);
    register_uri(s_server, "/api/settings/device", HTTP_POST, settings_device_handler, false);
    register_uri(s_server, "/api/settings/target-profile", HTTP_GET, settings_target_profile_handler, false);
    register_uri(s_server, "/api/settings/target-profile", HTTP_POST, settings_target_profile_handler, false);
    register_uri(s_server, "/api/settings/session", HTTP_GET, settings_session_handler, false);
    register_uri(s_server, "/api/settings/session", HTTP_POST, settings_session_handler, false);
    register_uri(s_server, "/api/settings/mcp", HTTP_GET, settings_mcp_handler, false);
    register_uri(s_server, "/api/settings/mcp", HTTP_POST, settings_mcp_handler, false);
    register_uri(s_server, "/api/settings/access-mode", HTTP_GET,
                 settings_access_mode_handler, false);
    register_uri(s_server, "/api/settings/access-mode", HTTP_POST,
                 settings_access_mode_handler, false);
    register_uri(s_server, "/api/settings/product-features", HTTP_GET, settings_product_features_handler, false);
    register_uri(s_server, "/api/settings/product-features", HTTP_POST, settings_product_features_handler, false);
    register_uri(s_server, "/api/settings/system", HTTP_GET, settings_system_handler, false);
    register_uri(s_server, "/api/settings/system", HTTP_POST, settings_system_handler, false);
    register_uri(s_server, "/api/settings/console-credentials", HTTP_GET, settings_console_credentials_handler, false);
    register_uri(s_server, "/api/settings/console-credentials", HTTP_POST, settings_console_credentials_handler, false);
    register_uri(s_server, "/api/console/login", HTTP_POST, console_login_mcp_async_handler, false);
    register_uri(s_server, "/api/settings/ssh-target", HTTP_GET, settings_ssh_target_handler, false);
    register_uri(s_server, "/api/settings/ssh-target", HTTP_POST, settings_ssh_target_handler, false);
    register_uri(s_server, "/api/settings/ssh-key", HTTP_GET, settings_ssh_key_handler, false);
    register_uri(s_server, "/api/settings/ssh-key", HTTP_POST, settings_ssh_key_handler, false);
#if SI_CFG_EMBEDDED_AGENT_ENABLED
    ESP_RETURN_ON_ERROR(register_agent_http_routes(), TAG,
                        "register Agent HTTP routes");
#else
    register_uri(s_server, "/api/settings/agent-tools", HTTP_GET,
                 settings_agent_tools_handler, false);
    register_uri(s_server, "/api/settings/agent-tools", HTTP_POST,
                 settings_agent_tools_handler, false);
#endif
    register_uri(s_server, "/api/ssh/status", HTTP_GET, ssh_status_handler, false);
    register_uri(s_server, "/api/ssh/configure-from-console", HTTP_POST,
                 ssh_target_from_console_handler, false);
    register_uri(s_server, "/api/ssh/exec", HTTP_POST, ssh_exec_async_handler, false);
    register_uri(s_server, "/api/terminal/control/stop", HTTP_POST, terminal_control_stop_handler, false);
    register_uri(s_server, "/api/automation/status", HTTP_GET,
                 automation_status_handler, false);
    register_uri(s_server, "/api/automation/stop", HTTP_POST,
                 automation_stop_handler, false);
    register_uri(s_server, "/api/host/display", HTTP_GET, si_host_display_http_handler, false);
    register_uri(s_server, "/api/host/display", HTTP_POST, si_host_display_http_handler, false);
    register_uri(s_server, "/api/host/boot-key", HTTP_GET, si_boot_key_http_handler, false);
    register_uri(s_server, "/api/host/boot-key", HTTP_POST, si_boot_key_http_handler, false);
    register_uri(s_server, "/api/uart/status", HTTP_GET, uart_status_handler, false);
    register_uri(s_server, "/api/uart/baud", HTTP_POST, uart_baud_handler, false);
    register_uri(s_server, "/api/uart/read", HTTP_GET, uart_read_handler, false);
    register_uri(s_server, "/api/uart/write", HTTP_POST, uart_write_handler, false);
    register_uri(s_server, "/api/uart/authenticate", HTTP_POST,
                 uart_authenticate_handler, false);
    register_uri(s_server, "/api/settings/gpio-map", HTTP_GET, power_gpio_map_handler, false);
    register_uri(s_server, "/api/settings/gpio-map", HTTP_POST, power_gpio_map_handler, false);
    register_uri(s_server, "/api/capabilities", HTTP_GET, capabilities_handler, false);
    register_uri(s_server, "/api/status", HTTP_GET, overall_status_handler, false);
    register_uri(s_server, "/api/system/info", HTTP_GET, system_info_handler, false);
    register_uri(s_server, "/api/v1/network/status", HTTP_GET,
                 si_network_status_http_handler, false);
    register_uri(s_server, "/api/v1/network/config", HTTP_GET,
                 si_network_config_http_handler, false);
    register_uri(s_server, "/api/v1/network/config", HTTP_POST,
                 si_network_config_http_handler, false);
    register_uri(s_server, "/api/system/logs", HTTP_GET, logs_handler, false);
    register_uri(s_server, "/api/system/logs/download", HTTP_GET, logs_download_handler, false);
    register_uri(s_server, "/api/storage/list", HTTP_GET, storage_list_async_handler, false);
    register_uri(s_server, "/api/storage/upload", HTTP_POST, storage_upload_async_handler, false);
    register_uri(s_server, "/api/storage/download", HTTP_GET, storage_download_async_handler, false);
    register_uri(s_server, "/api/storage/view", HTTP_GET, storage_view_async_handler, false);
    register_uri(s_server, "/api/hid/status", HTTP_GET, hid_status_handler, false);
    register_uri(s_server, "/api/hid/actions", HTTP_POST, hid_actions_async_handler, false);
    register_uri(s_server, "/api/control/lease", HTTP_GET, control_lease_handler, false);
    register_uri(s_server, "/api/control/lease", HTTP_POST, control_lease_handler, false);
    register_uri(s_server, "/api/power/status", HTTP_GET, power_status_handler, false);
    register_uri(s_server, "/api/power/action", HTTP_POST,
                 power_action_async_handler, false);
#if SI_CFG_MS2109_POWER_ENABLED || SI_CFG_MS2109_TEST_ENABLED
    register_uri(s_server, "/api/ms2109/status", HTTP_GET,
                 ms2109_status_handler, false);
    register_uri(s_server, "/api/ms2109/power", HTTP_POST,
                 ms2109_power_handler, false);
#endif
#if SI_CFG_MS2109_TEST_ENABLED
    register_uri(s_server, "/api/ms2109/eeprom/probe", HTTP_POST,
                 ms2109_eeprom_probe_handler, false);
    register_uri(s_server, "/api/ms2109/eeprom/read", HTTP_GET,
                 ms2109_eeprom_read_handler, false);
    register_uri(s_server, "/api/ms2109/eeprom/program", HTTP_POST,
                 ms2109_eeprom_program_handler, false);
#endif
    register_uri(s_server, "/api/ota/status", HTTP_GET, ota_status_handler, false);
    register_uri(s_server, "/api/ota/settings", HTTP_POST, ota_settings_handler, false);
    register_uri(s_server, "/api/ota/check", HTTP_POST, ota_check_async_handler, false);
    register_uri(s_server, "/api/ota/install", HTTP_POST, ota_install_async_handler, false);
    register_uri(s_server, "/api/ota/upload", HTTP_POST,
                 ota_upload_async_handler, false);
    register_uri(s_server, "/api/ota/upload/start", HTTP_POST,
                 ota_upload_start_handler, false);
    register_uri(s_server, "/api/ota/upload/chunk", HTTP_POST,
                 ota_upload_chunk_handler, false);
    register_uri(s_server, "/api/ota/upload/status", HTTP_GET,
                 ota_upload_status_handler, false);
    register_uri(s_server, "/api/ota/upload/finish", HTTP_POST,
                 ota_upload_finish_handler, false);
    register_uri(s_server, "/api/ota/upload/abort", HTTP_POST,
                 ota_upload_abort_handler, false);
    register_uri(s_server, "/api/ota/reboot", HTTP_POST, ota_reboot_handler, false);
#ifdef CONFIG_HTTPD_WS_SUPPORT
    register_uri(s_server, "/api/ws/hid", HTTP_GET, hid_ws_handler, true);
    register_uri(s_server, "/api/ws/ssh", HTTP_GET, ssh_ws_handler, true);
#if SI_CFG_UART_TERMINAL_ENABLED
    register_uri(s_server, "/api/ws/uart", HTTP_GET, uart_ws_handler, true);
#endif
#endif
    register_uri(s_server, "/api/video/status", HTTP_GET, video_status_handler, false);
    register_uri(s_server, "/api/video/settings", HTTP_POST, video_settings_handler, false);
    register_uri(s_server, "/api/video/quality", HTTP_POST, video_quality_handler, false);
    register_uri(s_server, "/api/video/resolution", HTTP_POST, video_resolution_handler, false);
    register_uri(s_server, "/api/video/lease", HTTP_POST, video_lease_handler, false);
    /* Keep MJPEG on the authenticated page origin.  Its asynchronous copy
     * prevents a long-lived multipart response from occupying the main HTTP
     * server task; H.264 uses the dedicated video listener on ordinary HTTP. */
    register_uri(s_server, "/api/stream", HTTP_GET,
                 stream_async_handler, false);
    register_uri(s_server, "/api/snapshot", HTTP_GET,
                 snapshot_async_handler, false);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler);
    ESP_RETURN_ON_ERROR(start_stream_server(), TAG, "start stream server");
    register_uri(s_stream_server, "/stream", HTTP_GET, stream_handler, false);
    si_web_log("INFO", "HTTP server started");
    ESP_LOGI(TAG, "HTTP server started on port %d", SI_DEFAULT_HTTP_PORT);
    return ESP_OK;
}
