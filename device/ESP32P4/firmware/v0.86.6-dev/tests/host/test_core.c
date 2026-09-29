#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "action_event.h"
#include "agent_cloud_retry_policy.h"
#include "agent_router.h"
#include "authorization.h"
#include "boot_key_sequence.h"
#include "device_observation_utils.h"
#include "host_display_mode.h"
#include "hid_ascii.h"
#include "model_provider.h"
#include "network_config.h"
#include "discovery_rate_limit.h"
#include "product_identity.h"
#include "resource_operation_state.h"
#include "storage_path.h"
#include "terminal_control_state.h"
#include "utf8_utils.h"
#include "version_utils.h"

static void assert_route(const char *message, bool screenshot, bool web_search,
                         si_agent_route_kind_t expected)
{
    si_agent_route_t route;
    si_agent_route_detect(message, screenshot, web_search, &route);
    assert(route.kind == expected);
    assert(route.name != NULL);
    assert(si_agent_route_prompt(&route) != NULL);
}

static void test_agent_router(void)
{
    si_agent_route_t route;

    assert_route("你好", false, false, SI_AGENT_ROUTE_CHAT);
    assert_route("继续推进", false, true, SI_AGENT_ROUTE_FOLLOWUP);
    assert_route("Call the wait tool, then observe_status", false, false,
                 SI_AGENT_ROUTE_TOOL_WORKFLOW);
    si_agent_route_detect("调用 wait 工具后执行 observe_status",
                          false, false, &route);
    assert(route.kind == SI_AGENT_ROUTE_TOOL_WORKFLOW);
    assert(route.max_tool_loops >= 2);
    assert_route("之前我们做过什么", false, false, SI_AGENT_ROUTE_MEMORY_RECALL);
    assert_route("通过 SSH 检查机器状态", false, false, SI_AGENT_ROUTE_SSH_OPS);
    assert_route("使用 KVM 检查系统分辨率", false, false,
                 SI_AGENT_ROUTE_KVM_VISUAL);
    assert_route("你现在可以访问 UART 终端吗", false, false,
                 SI_AGENT_ROUTE_UART_OPS);
    assert_route("通过 UART 检查系统分辨率", false, false,
                 SI_AGENT_ROUTE_UART_OPS);
    assert_route("通过 KVM 配置 UART 和 SSH", false, false,
                 SI_AGENT_ROUTE_KVM_VISUAL);
    assert_route("通过 UART 配置 SSH", false, false,
                 SI_AGENT_ROUTE_UART_OPS);
    assert_route("通过 SSH 配置 UART", false, false,
                 SI_AGENT_ROUTE_SSH_OPS);
    assert_route("读取串口登录提示", false, false, SI_AGENT_ROUTE_UART_OPS);
    assert_route("把波特率改成 115200", false, false, SI_AGENT_ROUTE_UART_OPS);
    assert_route("安装并配置 systemd 服务", false, false,
                 SI_AGENT_ROUTE_SERVICE_DEPLOY);
    assert_route("查看屏幕截图", false, false, SI_AGENT_ROUTE_KVM_VISUAL);
    assert_route("使用保存的账户登录系统", false, false,
                 SI_AGENT_ROUTE_KVM_VISUAL);
    assert_route("搜索最新官方文档", false, true, SI_AGENT_ROUTE_WEB_RESEARCH);
    assert_route("修改 API 设置", false, false, SI_AGENT_ROUTE_SETTINGS_HELP);
    assert_route("重启主机", false, false, SI_AGENT_ROUTE_DANGEROUS_ACTION);
    assert_route("普通描述", true, false, SI_AGENT_ROUTE_KVM_VISUAL);

    si_agent_route_detect("使用 KVM 检查系统分辨率", false, false, &route);
    assert(si_agent_route_allows_tool(&route, "observe_video_status"));
    assert(si_agent_route_allows_tool(&route, "console_login"));
    assert(si_agent_route_allows_tool(&route, "host_display"));
    assert(si_agent_route_allows_tool(&route, "ssh_exec"));
    assert(si_agent_route_allows_tool(&route, "ask_user"));
    assert(si_agent_route_allows_tool(&route, "wait"));
    assert(si_agent_route_allows_tool(&route, "power_action"));
    assert(!si_agent_route_allows_tool(&route, "unknown_tool"));
    assert(si_agent_route_allows_hid(&route));

    si_agent_route_detect("通过 KVM 配置 UART 和 SSH", false, false, &route);
    assert(si_agent_route_allows_hid(&route));
    assert(route.max_tool_loops >= 8);

    si_agent_route_detect("通过 SSH 检查机器状态", false, false, &route);
    assert(si_agent_route_allows_tool(&route, "host_display"));
    assert(si_agent_route_allows_tool(&route, "ssh_exec"));
    assert(si_agent_route_allows_tool(&route, "uart_read"));
    assert(!si_agent_route_allows_hid(&route));

    si_agent_route_detect("通过 UART 检查系统分辨率", false, false, &route);
    assert(si_agent_route_allows_tool(&route, "uart_status"));
    assert(si_agent_route_allows_tool(&route, "uart_read"));
    assert(si_agent_route_allows_tool(&route, "host_display"));
    assert(!si_agent_route_allows_hid(&route));

    si_agent_route_detect("你好", false, false, &route);
    assert(si_agent_route_allows_tool(&route, "ask_user"));
    assert(si_agent_route_allows_tool(&route, "wait"));

    si_agent_route_detect("重启主机", false, false, &route);
    assert(si_agent_route_allows_tool(&route, "power_action"));
    assert(si_agent_route_allows_tool(&route, "ask_user"));
}

static void test_model_provider_capabilities(void)
{
    si_model_capabilities_t capabilities;

    si_model_capabilities_resolve("kimi", "kimi-k3", &capabilities);
    assert(capabilities.text_input);
    assert(capabilities.image_input);
    assert(capabilities.video_input);
    assert(capabilities.base64_image_input);
    assert(!capabilities.public_image_url);
    assert(capabilities.native_tools);
    assert(capabilities.structured_json);
    assert(capabilities.streaming);
    assert(capabilities.reasoning);
    assert(capabilities.preserve_assistant_message);
    assert(capabilities.fixed_sampling_parameters);
    assert(strcmp(capabilities.default_reasoning_effort, "low") == 0);
    assert(si_model_is_kimi_k3("KIMI", "Kimi-K3"));
    assert(!si_model_is_kimi_k3("kimi", "kimi-k2.6"));

    si_model_capabilities_resolve("qwen", "qwen3-vl-flash", &capabilities);
    assert(capabilities.image_input);
    assert(capabilities.public_image_url);
    assert(!capabilities.preserve_assistant_message);
    assert(!capabilities.fixed_sampling_parameters);

    si_model_capabilities_resolve("qwen", "qwen-plus", &capabilities);
    assert(!capabilities.image_input);

    si_model_capabilities_resolve("deepseek", "deepseek-chat", &capabilities);
    assert(!capabilities.image_input);
    assert(capabilities.native_tools);

    si_model_capabilities_resolve("custom", "private-model", &capabilities);
    assert(capabilities.text_input);
    assert(!capabilities.image_input);
    assert(!capabilities.native_tools);
}

static void test_version_compare(void)
{
    assert(si_version_compare("v0.86.0", "0.85.9") > 0);
    assert(si_version_compare("1.10", "1.9") > 0);
    assert(si_version_compare("V2.0-RC1", "v2.0-rc1") == 0);
    assert(si_version_compare("0.85.10x", "0.85.10y") < 0);
    assert(si_version_compare(NULL, NULL) == 0);
}

static void test_utf8_sanitize(void)
{
    const char invalid[] = {'o', 'k', ' ', (char)0x94, ' ', 'x', '\0'};
    char *safe = si_utf8_sanitize(invalid, 0);
    assert(safe != NULL);
    assert(strcmp(safe, "ok ? x") == 0);
    free(safe);

    safe = si_utf8_sanitize("日志\xE2\x82\xAC", 7);
    assert(safe != NULL);
    assert(strstr(safe, "?") != NULL);
    assert(strstr(safe, "[truncated]") != NULL);
    free(safe);
}

static void test_hid_ascii_mapping(void)
{
    si_hid_ascii_key_t key;
    for (unsigned int value = 0x20; value <= 0x7e; value++) {
        assert(si_hid_ascii_map((unsigned char)value, &key));
        assert(key.code != NULL);
        assert(key.code[0] != '\0');
    }
    assert(!si_hid_ascii_map(0x00, &key));
    assert(!si_hid_ascii_map('\n', &key));
    assert(!si_hid_ascii_map(0x7f, &key));
    assert(!si_hid_ascii_map(0xc3, &key));
    assert(!si_hid_ascii_map('A', NULL));

    assert(si_hid_ascii_map('a', &key));
    assert(strcmp(key.code, "KeyA") == 0);
    assert(!key.shift);
    assert(si_hid_ascii_map('A', &key));
    assert(strcmp(key.code, "KeyA") == 0);
    assert(key.shift);
    assert(si_hid_ascii_map('@', &key));
    assert(strcmp(key.code, "Digit2") == 0);
    assert(key.shift);
    assert(si_hid_ascii_map('\\', &key));
    assert(strcmp(key.code, "Backslash") == 0);
    assert(!key.shift);
}

static void test_terminal_control_state(void)
{
    si_terminal_control_state_t state;
    si_terminal_control_state_init(&state);
    assert(si_terminal_control_state_manual_input_allowed(
        &state, SI_TERMINAL_CHANNEL_UART));
    assert(si_terminal_control_state_manual_input_allowed(
        &state, SI_TERMINAL_CHANNEL_SSH));

    si_terminal_control_token_t uart = {0};
    assert(si_terminal_control_state_begin(
        &state, SI_TERMINAL_CHANNEL_UART, SI_TERMINAL_ACTOR_AGENT,
        "uart-1", "run-1", "UART command", 100, &uart));
    assert(uart.generation != 0);
    assert(!si_terminal_control_state_manual_input_allowed(
        &state, SI_TERMINAL_CHANNEL_UART));
    assert(si_terminal_control_state_manual_input_allowed(
        &state, SI_TERMINAL_CHANNEL_SSH));
    si_terminal_control_token_t duplicate = {0};
    assert(!si_terminal_control_state_begin(
        &state, SI_TERMINAL_CHANNEL_UART, SI_TERMINAL_ACTOR_MCP,
        "uart-2", NULL, "duplicate", 101, &duplicate));

    si_terminal_control_token_t ssh = {0};
    assert(si_terminal_control_state_begin(
        &state, SI_TERMINAL_CHANNEL_SSH, SI_TERMINAL_ACTOR_MCP,
        "ssh-1", NULL, "SSH command", 102, &ssh));
    assert(!si_terminal_control_state_request_cancel(
        &state, SI_TERMINAL_CHANNEL_SSH, ssh.generation + 1));
    assert(!si_terminal_control_state_cancel_requested(&state, &ssh));
    assert(si_terminal_control_state_request_cancel(
        &state, SI_TERMINAL_CHANNEL_SSH, ssh.generation));
    assert(si_terminal_control_state_cancel_requested(&state, &ssh));
    assert(si_terminal_control_state_finish(&state, &ssh));
    assert(si_terminal_control_state_manual_input_allowed(
        &state, SI_TERMINAL_CHANNEL_SSH));

    si_terminal_control_token_t stale = uart;
    assert(si_terminal_control_state_finish(&state, &uart));
    assert(si_terminal_control_state_begin(
        &state, SI_TERMINAL_CHANNEL_UART, SI_TERMINAL_ACTOR_MCP,
        "uart-3", NULL, "next", 103, &uart));
    assert(!si_terminal_control_state_finish(&state, &stale));
    assert(!si_terminal_control_state_manual_input_allowed(
        &state, SI_TERMINAL_CHANNEL_UART));
    assert(si_terminal_control_state_finish(&state, &uart));
}

static void test_resource_operation_state(void)
{
    si_resource_operation_state_t state;
    si_resource_operation_state_init(&state);
    assert(si_resource_operation_state_active_count(&state) == 0U);
    assert(si_resource_operation_state_manual_allowed(
        &state, SI_RESOURCE_OPERATION_POWER));

    si_resource_operation_token_t power = {0};
    assert(si_resource_operation_state_begin(
        &state, SI_RESOURCE_OPERATION_POWER,
        SI_RESOURCE_OPERATION_ACTOR_AGENT, "power-1", "run-1",
        "Agent power action", true, 100U, &power));
    assert(power.generation != 0U);
    assert(!si_resource_operation_state_manual_allowed(
        &state, SI_RESOURCE_OPERATION_POWER));
    assert(si_resource_operation_state_manual_allowed(
        &state, SI_RESOURCE_OPERATION_VIDEO_CONFIG));
    assert(!si_resource_operation_state_begin(
        &state, SI_RESOURCE_OPERATION_POWER,
        SI_RESOURCE_OPERATION_ACTOR_MCP, "power-2", "",
        "duplicate", true, 101U,
        &(si_resource_operation_token_t){0}));
    assert(!si_resource_operation_state_begin(
        &state, SI_RESOURCE_OPERATION_MAINTENANCE,
        SI_RESOURCE_OPERATION_ACTOR_SYSTEM, "ota", "", "OTA", false,
        101U, &(si_resource_operation_token_t){0}));

    assert(!si_resource_operation_state_request_cancel(
        &state, SI_RESOURCE_OPERATION_POWER, power.generation + 1U));
    assert(!si_resource_operation_state_cancel_requested(&state, &power));
    assert(si_resource_operation_state_request_cancel(
        &state, SI_RESOURCE_OPERATION_POWER, power.generation));
    assert(si_resource_operation_state_cancel_requested(&state, &power));
    si_resource_operation_token_t stale = power;
    assert(si_resource_operation_state_finish(&state, &power));

    si_resource_operation_token_t maintenance = {0};
    assert(si_resource_operation_state_begin(
        &state, SI_RESOURCE_OPERATION_MAINTENANCE,
        SI_RESOURCE_OPERATION_ACTOR_SYSTEM, "ota", "", "OTA", false,
        102U, &maintenance));
    assert(!si_resource_operation_state_manual_allowed(
        &state, SI_RESOURCE_OPERATION_POWER));
    assert(!si_resource_operation_state_begin(
        &state, SI_RESOURCE_OPERATION_VIDEO_HARDWARE,
        SI_RESOURCE_OPERATION_ACTOR_MCP, "video", "", "video", true,
        103U, &(si_resource_operation_token_t){0}));
    assert(!si_resource_operation_state_request_cancel(
        &state, SI_RESOURCE_OPERATION_MAINTENANCE,
        maintenance.generation));
    assert(si_resource_operation_state_finish(&state, &maintenance));

    assert(si_resource_operation_state_begin(
        &state, SI_RESOURCE_OPERATION_POWER,
        SI_RESOURCE_OPERATION_ACTOR_MCP, "power-3", "", "next", true,
        104U, &power));
    assert(power.generation != stale.generation);
    assert(!si_resource_operation_state_finish(&state, &stale));
    assert(si_resource_operation_state_finish(&state, &power));
    assert(si_resource_operation_state_active_count(&state) == 0U);
}

static void test_url_decode(void)
{
    char value[64] = "ASSETS%2Fdocs%2Fhello+world.txt";
    assert(si_storage_url_decode_inplace(value) == SI_STORAGE_PATH_OK);
    assert(strcmp(value, "ASSETS/docs/hello world.txt") == 0);

    char incomplete[] = "bad%2";
    assert(si_storage_url_decode_inplace(incomplete) ==
           SI_STORAGE_PATH_INVALID_ARGUMENT);

    char invalid_hex[] = "bad%GG";
    assert(si_storage_url_decode_inplace(invalid_hex) ==
           SI_STORAGE_PATH_INVALID_ARGUMENT);

    char encoded_nul[] = "safe%00hidden";
    assert(si_storage_url_decode_inplace(encoded_nul) ==
           SI_STORAGE_PATH_INVALID_ARGUMENT);
}

static void test_storage_resolve(void)
{
    char relative[192];
    char absolute[256];

    assert(si_storage_resolve_path("/sdcard/EA/assets/docs/a.txt", "/sdcard/EA",
                                   relative, sizeof(relative),
                                   absolute, sizeof(absolute)) == SI_STORAGE_PATH_OK);
    assert(strcmp(relative, "ASSETS/docs/a.txt") == 0);
    assert(strcmp(absolute, "/sdcard/EA/ASSETS/docs/a.txt") == 0);
    assert(si_storage_path_is_file_target(relative));

    assert(si_storage_resolve_path("/EA/logs/run.log", "/sdcard/EA",
                                   relative, sizeof(relative),
                                   absolute, sizeof(absolute)) == SI_STORAGE_PATH_OK);
    assert(strcmp(relative, "LOGS/run.log") == 0);

    assert(si_storage_resolve_path("", "/sdcard/EA",
                                   relative, sizeof(relative),
                                   absolute, sizeof(absolute)) == SI_STORAGE_PATH_OK);
    assert(relative[0] == '\0');
    assert(strcmp(absolute, "/sdcard/EA") == 0);
    assert(!si_storage_path_is_file_target(relative));

    assert(si_storage_resolve_path("../secret", "/sdcard/EA",
                                   relative, sizeof(relative),
                                   absolute, sizeof(absolute)) ==
           SI_STORAGE_PATH_INVALID_ARGUMENT);
    assert(si_storage_resolve_path("LOGS\\secret", "/sdcard/EA",
                                   relative, sizeof(relative),
                                   absolute, sizeof(absolute)) ==
           SI_STORAGE_PATH_INVALID_ARGUMENT);
}

static void test_device_observation_mapping(void)
{
    assert(si_observation_used_percent(1000, 250) == 75.0);
    assert(si_observation_used_percent(1000, 1500) == 0.0);
    assert(si_observation_used_percent(0, 0) == 0.0);

    char uptime[32];
    si_observation_format_uptime(90060, uptime, sizeof(uptime));
    assert(strcmp(uptime, "1d 1h 1m") == 0);

    size_t start = 99;
    assert(si_observation_recent_window(3, 8, 8, 5, &start) == 5);
    assert(start == 6);
    assert(si_observation_recent_window(2, 2, 8, 5, &start) == 2);
    assert(start == 0);
    assert(si_observation_recent_window(0, 0, 8, 5, &start) == 0);
    assert(start == 0);
}

static void test_action_event_buffer(void)
{
    si_action_event_buffer_t buffer;
    si_action_event_buffer_init(&buffer);
    assert(buffer.count == 0);
    assert(buffer.latest_seq == 0);

    si_action_event_input_t input = {
        .run_id = "r42",
        .action_id = "a1",
        .actor = SI_ACTION_ACTOR_AGENT,
        .channel = SI_ACTION_CHANNEL_SSH,
        .phase = SI_ACTION_PHASE_INPUT_SENT,
        .summary = "systemctl status serial-getty@ttyACM0",
        .redacted = true,
        .background = true,
    };
    si_action_event_t appended;
    assert(si_action_event_buffer_append(&buffer, 1200, &input, &appended));
    assert(appended.schema_version == SI_ACTION_EVENT_SCHEMA_VERSION);
    assert(appended.seq == 1);
    assert(appended.timestamp_ms == 1200);
    assert(strcmp(appended.run_id, "r42") == 0);
    assert(strcmp(appended.action_id, "a1") == 0);
    assert(strcmp(appended.summary,
                  "systemctl status serial-getty@ttyACM0") == 0);
    assert(appended.redacted);
    assert(appended.background);
    assert(strcmp(si_action_actor_name(appended.actor), "agent") == 0);
    assert(strcmp(si_action_channel_name(appended.channel), "ssh") == 0);
    assert(strcmp(si_action_phase_name(appended.phase), "input_sent") == 0);
    assert(strcmp(si_action_phase_legacy_kind(appended.phase), "cmd") == 0);

    si_action_event_t events[SI_ACTION_EVENT_CAPACITY];
    uint32_t latest = 0;
    bool history_lost = true;
    assert(si_action_event_buffer_read_after(&buffer, 0, events,
                                             SI_ACTION_EVENT_CAPACITY,
                                             &latest, &history_lost) == 1);
    assert(latest == 1);
    assert(!history_lost);

    input.redacted = false;
    input.background = false;
    input.phase = SI_ACTION_PHASE_OUTPUT_RECEIVED;
    for (uint32_t i = 0; i < SI_ACTION_EVENT_CAPACITY + 3U; i++) {
        assert(si_action_event_buffer_append(&buffer, 1300U + i, &input, NULL));
    }
    assert(buffer.count == SI_ACTION_EVENT_CAPACITY);
    assert(buffer.dropped == 4U);
    size_t count = si_action_event_buffer_read_after(
        &buffer, 1, events, SI_ACTION_EVENT_CAPACITY, &latest, &history_lost);
    assert(count == SI_ACTION_EVENT_CAPACITY);
    assert(history_lost);
    assert(events[0].seq == 5U);
    assert(events[count - 1U].seq == latest);

    history_lost = true;
    count = si_action_event_buffer_read_after(
        &buffer, latest - 1U, events, SI_ACTION_EVENT_CAPACITY,
        &latest, &history_lost);
    assert(count == 1U);
    assert(!history_lost);
    assert(!si_action_event_buffer_append(NULL, 0, &input, NULL));
    assert(!si_action_event_buffer_append(&buffer, 0, NULL, NULL));
}

static si_authorization_request_t allowed_hid_request(void)
{
    si_authorization_request_t request = {
        .authenticated = true,
        .principal = SI_PRINCIPAL_MCP,
        .capabilities = SI_CAPABILITIES_MCP_DEFAULT,
        .required_capability = SI_CAPABILITY_HID,
        .policy_allowed = true,
        .lease_required = true,
        .lease_held = true,
        .mode = SI_AUTHZ_MODE_SUPERVISED,
        .risk = SI_AUTHZ_RISK_MEDIUM,
    };
    return request;
}

static void test_authorization(void)
{
    si_authorization_request_t request = allowed_hid_request();
    assert(si_authorization_evaluate(&request) == SI_AUTHZ_ALLOW);
    assert(si_capability_set_has(request.capabilities, SI_CAPABILITY_HID));
    assert(si_capability_set_has(request.capabilities,
                                 SI_CAPABILITY_VIDEO_CONTROL));
    assert(!si_capability_set_has(request.capabilities, SI_CAPABILITY_OTA));
    assert(strcmp(si_capability_name(SI_CAPABILITY_CONTROL_LEASE),
                  "control_lease") == 0);
    assert(strcmp(si_principal_kind_name(SI_PRINCIPAL_MCP), "mcp") == 0);

    request.authenticated = false;
    assert(si_authorization_evaluate(&request) ==
           SI_AUTHZ_DENY_UNAUTHENTICATED);
    request = allowed_hid_request();
    request.capabilities = SI_CAPABILITY_OBSERVE;
    assert(si_authorization_evaluate(&request) ==
           SI_AUTHZ_DENY_CAPABILITY);
    request = allowed_hid_request();
    request.policy_allowed = false;
    assert(si_authorization_evaluate(&request) == SI_AUTHZ_DENY_POLICY);
    request = allowed_hid_request();
    request.human_kvm_active = true;
    request.lease_held = false;
    assert(si_authorization_evaluate(&request) ==
           SI_AUTHZ_DENY_KVM_PREEMPTED);
    request = allowed_hid_request();
    request.lease_held = false;
    assert(si_authorization_evaluate(&request) == SI_AUTHZ_DENY_LEASE);
    request = allowed_hid_request();
    request.mode = SI_AUTHZ_MODE_OBSERVE;
    assert(si_authorization_evaluate(&request) ==
           SI_AUTHZ_DENY_OBSERVE_ONLY);
    request.lease_required = false;
    request.lease_held = false;
    assert(si_authorization_evaluate(&request) ==
           SI_AUTHZ_DENY_OBSERVE_ONLY);
    request.risk = SI_AUTHZ_RISK_LOW;
    assert(si_authorization_evaluate(&request) == SI_AUTHZ_ALLOW);
    request = allowed_hid_request();
    request.approval_required = true;
    assert(si_authorization_evaluate(&request) ==
           SI_AUTHZ_DENY_APPROVAL_REQUIRED);
    request.approved = true;
    assert(si_authorization_evaluate(&request) == SI_AUTHZ_ALLOW);
    assert(si_authorization_evaluate(NULL) ==
           SI_AUTHZ_DENY_INVALID_REQUEST);
}

static void test_host_display_mode(void)
{
    si_host_display_request_t request = {
        .connector = "HDMI-A-1",
        .width = 1280,
        .height = 720,
        .refresh_millihz = 60000,
        .persistent = true,
    };
    assert(si_host_display_validate_request(&request) == SI_HOST_DISPLAY_OK);

    char mode[SI_HOST_DISPLAY_MODE_MAX + 1U];
    assert(si_host_display_format_mode(&request, mode, sizeof(mode)) ==
           SI_HOST_DISPLAY_OK);
    assert(strcmp(mode, "HDMI-A-1:1280x720@60") == 0);

    char command[SI_HOST_DISPLAY_COMMAND_MAX + 1U];
    assert(si_host_display_build_observe_command(command, sizeof(command)) ==
           SI_HOST_DISPLAY_OK);
    assert(strstr(command, "exoanchor.host_display.v1") != NULL);
    assert(strstr(command, "/sys/class/drm/card*-*") != NULL);

    assert(si_host_display_build_apply_command(
               "hd-42", &request, command, sizeof(command)) ==
           SI_HOST_DISPLAY_OK);
    assert(strstr(command, "99-exoanchor-display.cfg") != NULL);
    assert(strstr(command, "video=HDMI-A-1:1280x720@60e") != NULL);
    assert(strstr(command, "/var/lib/exoanchor/display/hd-42") != NULL);
    assert(strstr(command, "update-grub") != NULL);

    assert(si_host_display_build_verify_command(
               "hd-42", &request, command, sizeof(command)) ==
           SI_HOST_DISPLAY_OK);
    assert(strstr(command, "runtime_token") != NULL);
    assert(strstr(command, "fb_virtual_size") != NULL);

    assert(si_host_display_build_rollback_command(
               "hd-42", command, sizeof(command)) == SI_HOST_DISPLAY_OK);
    assert(strstr(command, "previous_present") != NULL);
    assert(strstr(command, "rollback=restored") != NULL);

    request.persistent = false;
    assert(si_host_display_build_apply_command(
               "hd-42", &request, command, sizeof(command)) ==
           SI_HOST_DISPLAY_UNSUPPORTED);

    strlcpy(request.connector, "HDMI-A-1;reboot", sizeof(request.connector));
    assert(si_host_display_validate_request(&request) ==
           SI_HOST_DISPLAY_INVALID_ARGUMENT);
    strlcpy(request.connector, "HDMI-A-1", sizeof(request.connector));
    request.refresh_millihz = 59940;
    assert(si_host_display_validate_request(&request) ==
           SI_HOST_DISPLAY_INVALID_ARGUMENT);
    request.refresh_millihz = 60000;
    assert(si_host_display_build_apply_command(
               "../escape", &request, command, sizeof(command)) ==
           SI_HOST_DISPLAY_INVALID_ARGUMENT);

    char tiny[16];
    assert(si_host_display_build_observe_command(tiny, sizeof(tiny)) ==
           SI_HOST_DISPLAY_OUTPUT_TOO_SMALL);
    assert(tiny[0] == '\0');
    assert(strcmp(si_host_display_result_name(SI_HOST_DISPLAY_UNSUPPORTED),
                  "unsupported") == 0);
}

static void test_boot_key_sequence(void)
{
    si_boot_key_sequence_config_t config = {
        .profile_id = "lab-host-1",
        .target = SI_BOOT_KEY_TARGET_BOOT_MENU,
        .trigger = SI_BOOT_KEY_TRIGGER_RESET,
        .key_code = "F11",
        .start_delay_ms = 500,
        .interval_ms = 250,
        .max_attempts = 8,
        .total_timeout_ms = 5000,
    };
    assert(si_boot_key_sequence_validate(&config));
    assert(si_boot_key_sequence_decide(
               &config, 0, 0, false, false, true, true) ==
           SI_BOOT_KEY_DECISION_WAIT);
    assert(si_boot_key_sequence_decide(
               &config, 500, 0, false, false, true, true) ==
           SI_BOOT_KEY_DECISION_SEND);
    assert(si_boot_key_sequence_decide(
               &config, 600, 1, false, false, true, true) ==
           SI_BOOT_KEY_DECISION_WAIT);
    assert(si_boot_key_sequence_decide(
               &config, 750, 1, false, false, true, true) ==
           SI_BOOT_KEY_DECISION_SEND);
    assert(si_boot_key_sequence_decide(
               &config, 800, 1, false, false, true, false) ==
           SI_BOOT_KEY_DECISION_WAIT_HID);
    assert(si_boot_key_sequence_decide(
               &config, 800, 1, false, true, true, true) ==
           SI_BOOT_KEY_DECISION_PREEMPT);
    assert(si_boot_key_sequence_decide(
               &config, 800, 1, false, false, false, true) ==
           SI_BOOT_KEY_DECISION_PREEMPT);
    assert(si_boot_key_sequence_decide(
               &config, 800, 1, true, false, true, true) ==
           SI_BOOT_KEY_DECISION_CANCEL);
    assert(si_boot_key_sequence_decide(
               &config, 3000, 8, false, false, true, true) ==
           SI_BOOT_KEY_DECISION_COMPLETE);

    config.total_timeout_ms = 2000;
    assert(!si_boot_key_sequence_validate(&config));
    config.total_timeout_ms = 5000;
    strlcpy(config.key_code, "Enter", sizeof(config.key_code));
    assert(!si_boot_key_sequence_validate(&config));
    strlcpy(config.key_code, "F11;Power", sizeof(config.key_code));
    assert(!si_boot_key_sequence_validate(&config));
    strlcpy(config.key_code, "F11", sizeof(config.key_code));
    strlcpy(config.profile_id, "../escape", sizeof(config.profile_id));
    assert(!si_boot_key_sequence_validate(&config));
}

static void test_product_identity(void)
{
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x12, 0x34, 0x56};
    si_product_identity_t identity;
    si_product_identity_default_from_mac(mac, &identity);
    assert(strcmp(identity.suffix, "123456") == 0);
    assert(strcmp(identity.device_id, "ea-p4-123456") == 0);
    assert(strcmp(identity.hostname, "exoanchor-123456") == 0);
    assert(si_product_device_id_valid(identity.device_id));
    assert(si_product_hostname_valid(identity.hostname));
    assert(si_product_hostname_valid("rack-12"));
    assert(!si_product_hostname_valid("Rack-12"));
    assert(!si_product_hostname_valid("-rack"));
    assert(!si_product_hostname_valid("rack-"));
    assert(!si_product_hostname_valid("rack.local"));
    si_product_identity_default_from_mac(NULL, &identity);
    assert(identity.device_id[0] == '\0');
}

static void test_network_config(void)
{
    char error[SI_NETWORK_VALIDATION_ERROR_MAX_LEN];
    si_network_config_t config;
    si_network_config_default(&config, "exoanchor-123456");
    assert(config.schema_version == SI_NETWORK_CONFIG_SCHEMA_VERSION);
    assert(config.mode == SI_NETWORK_MODE_DHCP);
    assert(config.autoip_fallback);
    assert(si_network_config_validate(&config, error, sizeof(error)));
    assert(strcmp(si_network_mode_name(config.mode), "dhcp") == 0);

    config.mode = SI_NETWORK_MODE_STATIC;
    snprintf(config.address, sizeof(config.address), "192.0.2.223");
    snprintf(config.netmask, sizeof(config.netmask), "255.255.255.0");
    snprintf(config.gateway, sizeof(config.gateway), "192.0.2.1");
    snprintf(config.dns_primary, sizeof(config.dns_primary), "1.1.1.1");
    snprintf(config.dns_secondary, sizeof(config.dns_secondary), "8.8.8.8");
    assert(si_network_config_validate(&config, error, sizeof(error)));
    assert(strcmp(si_network_mode_name(config.mode), "static") == 0);

    snprintf(config.netmask, sizeof(config.netmask), "255.0.255.0");
    assert(!si_network_config_validate(&config, error, sizeof(error)));
    snprintf(config.netmask, sizeof(config.netmask), "255.255.255.0");
    snprintf(config.gateway, sizeof(config.gateway), "198.51.100.1");
    assert(!si_network_config_validate(&config, error, sizeof(error)));
    snprintf(config.gateway, sizeof(config.gateway), "192.0.2.1");
    snprintf(config.address, sizeof(config.address), "192.0.2.0");
    assert(!si_network_config_validate(&config, error, sizeof(error)));
    snprintf(config.address, sizeof(config.address), "169.254.1.12");
    assert(!si_network_config_validate(&config, error, sizeof(error)));
    snprintf(config.address, sizeof(config.address), "192.0.2.223");
    snprintf(config.hostname, sizeof(config.hostname), "Bad.Host");
    assert(!si_network_config_validate(&config, error, sizeof(error)));
    assert(!si_network_ipv4_parse("192.0.2.999", NULL));
    assert(!si_network_ipv4_parse("192.168.1", NULL));
}

static void test_discovery_rate_limit(void)
{
    si_discovery_rate_limiter_t limiter = {0};
    const uint32_t source = 0x01020304U;
    assert(!si_discovery_rate_limit_allow(NULL, source, 0));
    assert(!si_discovery_rate_limit_allow(&limiter, 0, 0));
    for (uint32_t i = 0; i < SI_DISCOVERY_RATE_PER_SOURCE; ++i) {
        assert(si_discovery_rate_limit_allow(&limiter, source, i));
    }
    assert(!si_discovery_rate_limit_allow(&limiter, source, 10));
    assert(si_discovery_rate_limit_allow(
        &limiter, source, SI_DISCOVERY_RATE_WINDOW_MS));

    limiter = (si_discovery_rate_limiter_t){0};
    for (uint32_t i = 0; i < SI_DISCOVERY_RATE_GLOBAL; ++i) {
        uint32_t varied_source = 0x0a000001U + i;
        assert(si_discovery_rate_limit_allow(
            &limiter, varied_source, 100));
    }
    assert(!si_discovery_rate_limit_allow(
        &limiter, 0x0b000001U, 100));
    assert(si_discovery_rate_limit_allow(
        &limiter, 0x0b000001U,
        100 + SI_DISCOVERY_RATE_WINDOW_MS));
}

static void test_agent_cloud_retry_policy(void)
{
    assert(!si_agent_cloud_async_error_is_fatal(0, 0, 0));
    assert(!si_agent_cloud_async_error_is_fatal(EAGAIN, 0, 0));
    assert(!si_agent_cloud_async_error_is_fatal(EWOULDBLOCK, 0, 0));
    assert(!si_agent_cloud_async_error_is_fatal(EINPROGRESS, 0, 0));
    assert(si_agent_cloud_async_error_is_fatal(ECONNRESET, 0, 0));
    assert(si_agent_cloud_async_error_is_fatal(EAGAIN, 0x8018, 1));
    assert(si_agent_cloud_async_error_is_fatal(0, 0, 1));
}

int main(void)
{
    test_agent_router();
    test_model_provider_capabilities();
    test_version_compare();
    test_utf8_sanitize();
    test_hid_ascii_mapping();
    test_terminal_control_state();
    test_resource_operation_state();
    test_url_decode();
    test_storage_resolve();
    test_device_observation_mapping();
    test_action_event_buffer();
    test_authorization();
    test_host_display_mode();
    test_boot_key_sequence();
    test_product_identity();
    test_network_config();
    test_discovery_rate_limit();
    test_agent_cloud_retry_policy();
    puts("host core tests: PASS");
    return 0;
}
