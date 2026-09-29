#include "agent_router.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

static bool text_contains_ci(const char *text, const char *needle)
{
    if (!text || !needle || !needle[0]) {
        return false;
    }
    size_t needle_len = strlen(needle);
    for (const char *p = text; *p; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool text_has_any(const char *text, const char *const *needles,
                         size_t needle_count)
{
    if (!text || !needles) {
        return false;
    }
    for (size_t i = 0; i < needle_count; i++) {
        if (text_contains_ci(text, needles[i])) {
            return true;
        }
    }
    return false;
}

static bool text_is_short_chat(const char *text)
{
    if (!text) {
        return true;
    }
    if (strlen(text) > 48) {
        return false;
    }
    static const char *const chat_words[] = {
        "你好", "您好", "hello", "hi", "你是谁", "who are you",
        "在吗", "ok", "好的", "谢谢", "thanks",
    };
    return text_has_any(text, chat_words, sizeof(chat_words) / sizeof(chat_words[0]));
}

static void route_set(si_agent_route_t *route, si_agent_route_kind_t kind,
                      const char *name, const char *intent,
                      const char *prompt_profile, const char *tool_policy,
                      const char *final_report_style, int confidence,
                      int max_tool_loops, bool needs_control_lease,
                      bool prefer_memory, bool prefer_screenshot,
                      bool prefer_web_search)
{
    if (!route) {
        return;
    }
    route->kind = kind;
    route->name = name;
    route->intent = intent;
    route->prompt_profile = prompt_profile;
    route->tool_policy = tool_policy;
    route->final_report_style = final_report_style;
    route->confidence = confidence;
    route->max_tool_loops = max_tool_loops;
    route->needs_control_lease = needs_control_lease;
    route->prefer_memory = prefer_memory;
    route->prefer_screenshot = prefer_screenshot;
    route->prefer_web_search = prefer_web_search;
}

static void route_defaults(si_agent_route_t *route)
{
    route_set(route, SI_AGENT_ROUTE_CHAT, "chat", "plain_conversation",
              "chat_light", "no_tools_by_default", "direct",
              65, 1, false, false, false, false);
}

void si_agent_route_detect(const char *message, bool include_screenshot,
                           bool allow_web_search, si_agent_route_t *route)
{
    route_defaults(route);
    const char *text = message ? message : "";

    static const char *const dangerous_words[] = {
        "格式化", "清空磁盘", "擦除", "erase-flash", "erase flash",
        "reset everything", "删除全部", "rm -rf /", "关机", "重启",
        "power off", "shutdown", "reboot", "断网", "修改网络",
    };
    static const char *const followup_words[] = {
        "继续", "执行吧", "开始吧", "go ahead", "continue", "继续执行",
        "按计划", "下一步", "继续推进",
    };
    static const char *const memory_words[] = {
        "历史", "记忆", "之前", "上次", "做过", "待办", "todo",
        "其他会话", "全局记忆", "还剩什么", "记录",
    };
    static const char *const service_words[] = {
        "安装", "部署", "配置服务", "创建服务", "systemd", "docker",
        "minecraft", "paper", "mc服务器", "server", "daemon",
        "后台服务", "开机启动", "service",
    };
    static const char *const ssh_words[] = {
        "ssh", "主机", "机器状态", "命令", "apt",
        "sudo", "journalctl", "systemctl", "日志", "端口", "进程",
        "uname", "df -h", "free -h",
    };
    static const char *const uart_words[] = {
        "uart", "串口", "serial console", "serial terminal", "tty",
        "ttyacm", "agetty", "serial-getty", "serial getty",
        "波特率", "baud", "8n1",
    };
    static const char *const kvm_words[] = {
        "kvm", "屏幕", "截图", "画面", "bios", "uefi", "安装系统", "ubuntu",
        "windows", "proxmox", "点击", "键盘", "鼠标", "hid",
        "上下左右", "enter", "选择", "登录", "登陆", "login",
        "log in", "sign in", "解锁屏幕",
    };
    static const char *const web_words[] = {
        "联网", "搜索", "查一下", "最新", "官方", "文档", "download",
        "release", "版本",
    };
    static const char *const settings_words[] = {
        "设置", "settings", "api", "apikey", "api key", "provider",
        "模型", "prompt", "mcp", "nvs", "tf卡", "tf 卡",
    };
    static const char *const tool_workflow_words[] = {
        "工具调用", "调用工具", "工具契约", "tool call", "tool contract",
        "wait tool", "call wait", "调用 wait",
        "observe_status", "observe_video_status", "observe_hid_status",
        "memory_search", "history_search", "memory_write", "ask_user",
        "host_display", "boot_key_sequence", "web_search", "power_action",
        "ensure_package", "ensure_user", "ensure_directory", "download_file",
        "write_file", "ensure_systemd_service", "check_service", "verify_port",
    };

    bool has_danger = text_has_any(text, dangerous_words,
                                   sizeof(dangerous_words) / sizeof(dangerous_words[0]));
    bool has_followup = text_has_any(text, followup_words,
                                     sizeof(followup_words) / sizeof(followup_words[0]));
    bool has_memory = text_has_any(text, memory_words,
                                   sizeof(memory_words) / sizeof(memory_words[0]));
    bool has_service = text_has_any(text, service_words,
                                    sizeof(service_words) / sizeof(service_words[0]));
    bool has_uart = text_has_any(text, uart_words,
                                 sizeof(uart_words) / sizeof(uart_words[0]));
    bool has_ssh = text_has_any(text, ssh_words,
                                sizeof(ssh_words) / sizeof(ssh_words[0]));
    bool has_kvm = include_screenshot ||
                    text_has_any(text, kvm_words,
                                 sizeof(kvm_words) / sizeof(kvm_words[0]));
    bool has_web = text_has_any(text, web_words,
                                sizeof(web_words) / sizeof(web_words[0]));
    bool has_settings = text_has_any(text, settings_words,
                                     sizeof(settings_words) / sizeof(settings_words[0]));
    bool has_tool_workflow =
        text_has_any(text, tool_workflow_words,
                     sizeof(tool_workflow_words) /
                     sizeof(tool_workflow_words[0]));
    bool says_ssh = text_contains_ci(text, "ssh") || text_contains_ci(text, "通过 SSH");
    bool says_uart = text_contains_ci(text, "uart") ||
                     text_contains_ci(text, "串口") ||
                     text_contains_ci(text, "serial console") ||
                     text_contains_ci(text, "serial terminal");
    bool says_kvm = include_screenshot || text_contains_ci(text, "kvm");
    bool via_kvm = says_kvm &&
                   (text_contains_ci(text, "通过kvm") ||
                    text_contains_ci(text, "通过 kvm") ||
                    text_contains_ci(text, "用kvm") ||
                    text_contains_ci(text, "用 kvm") ||
                    text_contains_ci(text, "via kvm") ||
                    text_contains_ci(text, "through kvm") ||
                    text_contains_ci(text, "from kvm") ||
                    text_contains_ci(text, "kvm配置") ||
                    text_contains_ci(text, "kvm 配置"));
    bool via_uart = says_uart &&
                    (text_contains_ci(text, "通过uart") ||
                     text_contains_ci(text, "通过 uart") ||
                     text_contains_ci(text, "通过串口") ||
                     text_contains_ci(text, "从uart") ||
                     text_contains_ci(text, "从 uart") ||
                     text_contains_ci(text, "从串口") ||
                     text_contains_ci(text, "via uart") ||
                     text_contains_ci(text, "through uart") ||
                     text_contains_ci(text, "from uart"));
    bool via_ssh = says_ssh &&
                   (text_contains_ci(text, "通过ssh") ||
                    text_contains_ci(text, "通过 ssh") ||
                    text_contains_ci(text, "从ssh") ||
                    text_contains_ci(text, "从 ssh") ||
                    text_contains_ci(text, "via ssh") ||
                    text_contains_ci(text, "through ssh") ||
                    text_contains_ci(text, "from ssh"));

    if (has_danger) {
        route_set(route, SI_AGENT_ROUTE_DANGEROUS_ACTION,
                  "dangerous_action", "destructive_or_connectivity_risk",
                  "safety_gate", "ask_before_tools", "confirmation_first",
                  88, 1, false, true, false, false);
    } else if (via_kvm) {
        route_set(route, SI_AGENT_ROUTE_KVM_VISUAL,
                  "kvm_visual", "bootstrap_uart_or_ssh_through_kvm",
                  "kvm_visual_operator", "screenshot_hid_tools", "brief_report",
                  98, 8, true, true, true, allow_web_search && has_web);
    } else if (via_uart && says_ssh) {
        route_set(route, SI_AGENT_ROUTE_UART_OPS,
                  "uart_ops", "bootstrap_ssh_through_target_uart",
                  "uart_operator", "uart_memory_tools", "brief_report",
                  98, 8, false, true, false, false);
    } else if (via_ssh && says_uart) {
        route_set(route, SI_AGENT_ROUTE_SSH_OPS,
                  "ssh_ops", "bootstrap_uart_through_target_ssh",
                  "ssh_operator", "ssh_memory_tools", "brief_report",
                  98, 8, false, true, false, allow_web_search && has_web);
    } else if (says_uart && !says_kvm && !says_ssh) {
        route_set(route, SI_AGENT_ROUTE_UART_OPS,
                  "uart_ops", "inspect_or_operate_target_uart",
                  "uart_operator", "uart_memory_tools", "brief_report",
                  96, 5, false, true, false, false);
    } else if (says_kvm && !says_uart && !says_ssh) {
        route_set(route, SI_AGENT_ROUTE_KVM_VISUAL,
                  "kvm_visual", "inspect_screen_or_drive_hid",
                  "kvm_visual_operator", "screenshot_hid_tools", "brief_report",
                  96, 5, true, true, true, allow_web_search && has_web);
    } else if (says_ssh && !says_uart && !says_kvm) {
        route_set(route, SI_AGENT_ROUTE_SSH_OPS,
                  "ssh_ops", "operate_or_inspect_target_over_ssh",
                  "ssh_operator", "ssh_memory_tools", "brief_report",
                  96, 5, false, true, false, allow_web_search && has_web);
    } else if (has_tool_workflow) {
        route_set(route, SI_AGENT_ROUTE_TOOL_WORKFLOW,
                  "tool_workflow", "execute_explicit_runtime_tool_workflow",
                  "tool_workflow_operator", "runtime_tools", "brief_report",
                  92, 6, false, true, false,
                  allow_web_search && has_web);
    } else if (has_memory && !has_service && !has_uart && !has_ssh && !has_kvm) {
        route_set(route, SI_AGENT_ROUTE_MEMORY_RECALL,
                  "memory_recall", "recall_history_or_working_memory",
                  "memory_reader", "memory_history_first", "direct",
                  86, 2, false, true, false, false);
    } else if (has_followup && strlen(text) <= 80) {
        route_set(route, SI_AGENT_ROUTE_FOLLOWUP,
                  "task_followup", "continue_or_approve_previous_task",
                  "continuation_executor", "history_then_tools", "brief_report",
                  72, 6, false, true, false, allow_web_search);
    } else if (has_service && (has_ssh || says_ssh || (!has_uart && !has_kvm))) {
        route_set(route, SI_AGENT_ROUTE_SERVICE_DEPLOY,
                  "service_deploy", "install_configure_or_verify_service",
                  "ops_service_executor", "ssh_memory_web_tools", "brief_delivery_report",
                  90, 8, false, true, false, allow_web_search && has_web);
    } else if (has_uart) {
        route_set(route, SI_AGENT_ROUTE_UART_OPS,
                  "uart_ops", "inspect_or_operate_target_uart",
                  "uart_operator", "uart_memory_tools", "brief_report",
                  90, 5, false, true, false, false);
    } else if (has_kvm) {
        route_set(route, SI_AGENT_ROUTE_KVM_VISUAL,
                  "kvm_visual", "inspect_screen_or_drive_hid",
                  "kvm_visual_operator", "screenshot_hid_tools", "brief_report",
                  84, 5, true, true, true, allow_web_search && has_web);
    } else if (has_ssh) {
        route_set(route, SI_AGENT_ROUTE_SSH_OPS,
                  "ssh_ops", "operate_or_inspect_target_over_ssh",
                  "ssh_operator", "ssh_memory_tools", "brief_report",
                  86, 5, false, true, false, allow_web_search && has_web);
    } else if (has_web && allow_web_search) {
        route_set(route, SI_AGENT_ROUTE_WEB_RESEARCH,
                  "web_research", "fresh_external_research",
                  "researcher", "web_search_first", "brief_sources_report",
                  78, 4, false, true, false, true);
    } else if (has_settings) {
        route_set(route, SI_AGENT_ROUTE_SETTINGS_HELP,
                  "settings_help", "explain_or_adjust_device_settings",
                  "settings_assistant", "no_tools_unless_needed", "direct",
                  74, 2, false, true, false, false);
    } else if (text_is_short_chat(text)) {
        route_set(route, SI_AGENT_ROUTE_CHAT,
                  "chat", "plain_conversation",
                  "chat_light", "no_tools_by_default", "direct",
                  92, 1, false, false, false, false);
    }
}

static bool tool_name_is(const char *name, const char *expected)
{
    return name && expected && strcmp(name, expected) == 0;
}

static bool tool_is_memory(const char *name)
{
    return tool_name_is(name, "memory_search") ||
           tool_name_is(name, "history_search") ||
           tool_name_is(name, "memory_write");
}

static bool tool_is_observation(const char *name)
{
    return tool_name_is(name, "observe_status") ||
           tool_name_is(name, "observe_video_status") ||
           tool_name_is(name, "observe_hid_status");
}

static bool tool_is_ssh(const char *name)
{
    static const char *const tools[] = {
        "ssh_exec", "ensure_package", "ensure_user", "ensure_directory",
        "download_file", "write_file", "ensure_systemd_service",
        "check_service", "verify_port", "host_display",
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        if (tool_name_is(name, tools[i])) {
            return true;
        }
    }
    return false;
}

static bool tool_is_runtime_capability(const char *name)
{
    return tool_is_memory(name) ||
           tool_is_observation(name) ||
           tool_is_ssh(name) ||
           tool_name_is(name, "ask_user") ||
           tool_name_is(name, "wait") ||
           tool_name_is(name, "web_search") ||
           tool_name_is(name, "console_login") ||
           tool_name_is(name, "boot_key_sequence") ||
           tool_name_is(name, "power_action") ||
           tool_name_is(name, "uart_status") ||
           tool_name_is(name, "uart_read") ||
           tool_name_is(name, "uart_write") ||
           tool_name_is(name, "uart_auth") ||
           tool_name_is(name, "uart_baud");
}

bool si_agent_route_allows_tool(const si_agent_route_t *route,
                                const char *tool_name)
{
    if (!route || !tool_name || !tool_name[0]) {
        return false;
    }
    /*
     * A route selects the prompt profile and recommended first tools; it is
     * not an authorization boundary. Treating keyword routing as a hard
     * whitelist made advertised tools unreachable after a harmless
     * misclassification (notably ask_user, wait, and power_action).
     *
     * Actual authority remains fail-closed in the per-tool enable policy,
     * Request Broker risk decision, control lease, and tool implementation.
     */
    return tool_is_runtime_capability(tool_name);
}

bool si_agent_route_allows_hid(const si_agent_route_t *route)
{
    return route &&
           (route->kind == SI_AGENT_ROUTE_KVM_VISUAL ||
            route->kind == SI_AGENT_ROUTE_FOLLOWUP);
}

const char *si_agent_route_prompt(const si_agent_route_t *route)
{
    const char *name = route && route->name ? route->name : "chat";
    if (strcmp(name, "task_followup") == 0) {
        return "Router profile: task_followup. The user is likely approving or continuing a previous task. "
               "First use restored history/memory context to infer the active task. If the prior task is clear and non-destructive, continue with concrete tool_calls instead of asking again. "
               "If the prior task is ambiguous, ask one concise clarification and keep tool_calls/actions empty.";
    }
    if (strcmp(name, "memory_recall") == 0) {
        return "Router profile: memory_recall. Before answering questions about previous sessions, prior decisions, host facts, deployments, or todos, call memory_search or history_search. "
               "Do not answer only from the current prompt when persistent memory/history is relevant.";
    }
    if (strcmp(name, "tool_workflow") == 0) {
        return "Router profile: tool_workflow. The user explicitly requested one or more runtime tool calls. "
               "Execute the requested tools in dependency order, inspect each result, and continue planning until the requested workflow is complete or a concrete blocker is proven. "
               "Do not treat a successful first tool call as completion when later requested steps remain.";
    }
    if (strcmp(name, "kvm_visual") == 0) {
        return "Router profile: kvm_visual. Prefer screenshot interpretation and small HID actions. The runtime automatically acquires the video lease, starts demand-driven capture, and waits for the first frame; capture_enabled=false or frame_ready=false before that startup is an idle state, not proof that the capture card is physically disconnected. "
               "Use keyboard navigation where possible for installers/BIOS. "
               "For an OS login, focus the correct field with ordinary HID, then use console_login with console://default so no credential enters the model context. Inspect a fresh frame after each login stage. "
               "host_display is SSH-backed and is not a KVM observation tool. Do not use SSH unless the user explicitly asks for SSH. Keep HID batches short and observable.";
    }
    if (strcmp(name, "ssh_ops") == 0) {
        return "Router profile: ssh_ops. Prefer ssh_exec against the Settings target. Use bounded commands, inspect output after every step, and write durable findings to memory when useful. "
               "For read-only checks, execute directly. For ordinary package/config commands in execute mode, do not ask for extra confirmation.";
    }
    if (strcmp(name, "uart_ops") == 0) {
        return "Router profile: uart_ops. UART is a device-local target console and is independent from SSH configuration. "
               "Call uart_status before reporting availability, then use non-destructive uart_read with a cursor for observation. "
               "Use uart_write or uart_baud only when the user's task requires a change and the request broker authorizes it. Use uart_auth with a local credential reference and the exact observed password prompt; never place a password in uart_write. "
               "A connected manual UART terminal has priority over Agent writes and baud changes.";
    }
    if (strcmp(name, "service_deploy") == 0) {
        return "Router profile: service_deploy. This is an install/deploy/configure/verify task. Use multiple small SSH/tool steps, not one huge script. "
               "Checking prerequisites alone is not completion. Continue through install, configuration, service creation/start, logs, port/status checks, operator access/control verification, and reboot/autostart behavior. "
               "If web_search is effective and current official information is needed, call web_search as a tool.";
    }
    if (strcmp(name, "web_research") == 0) {
        return "Router profile: web_research. Use web_search when Web search effective=yes and current external information is needed. Summarize sources briefly in the final message. "
               "If web_search is unavailable, say so and fall back to available SSH/API checks only when appropriate.";
    }
    if (strcmp(name, "settings_help") == 0) {
        return "Router profile: settings_help. Explain the device setting or capability directly. Use tools only when the user asks to inspect or change live device state. "
               "Keep implementation details out of the user-facing answer unless they are relevant.";
    }
    if (strcmp(name, "dangerous_action") == 0) {
        return "Router profile: dangerous_action. Do not execute destructive, connectivity-breaking, power, reboot, disk format, mass deletion, or credential-exposing actions until the user explicitly confirms the specific operation. "
               "Return a concise confirmation request with empty tool_calls/actions.";
    }
    return "Router profile: chat. Answer naturally and briefly. Do not call SSH, HID, screenshot, power, memory, or web_search unless the user explicitly asks for tool-backed work.";
}
