#include "agent_skill_service.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "agent_tools_settings.h"
#include "cJSON.h"

extern const uint8_t configure_uart_cli_skill_start[]
    asm("_binary_configure_uart_cli_skill_start");
extern const uint8_t configure_uart_cli_skill_end[]
    asm("_binary_configure_uart_cli_skill_end");
extern const uint8_t configure_access_via_kvm_skill_start[]
    asm("_binary_configure_access_via_kvm_skill_start");
extern const uint8_t configure_access_via_kvm_skill_end[]
    asm("_binary_configure_access_via_kvm_skill_end");
extern const uint8_t bridge_uart_ssh_access_skill_start[]
    asm("_binary_bridge_uart_ssh_access_skill_start");
extern const uint8_t bridge_uart_ssh_access_skill_end[]
    asm("_binary_bridge_uart_ssh_access_skill_end");

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} si_agent_builtin_skill_t;

static const si_agent_builtin_skill_t BUILTIN_SKILLS[] = {
    {
        .name = SI_AGENT_SKILL_CONFIGURE_UART_CLI,
        .start = configure_uart_cli_skill_start,
        .end = configure_uart_cli_skill_end,
    },
    {
        .name = SI_AGENT_SKILL_CONFIGURE_ACCESS_VIA_KVM,
        .start = configure_access_via_kvm_skill_start,
        .end = configure_access_via_kvm_skill_end,
    },
    {
        .name = SI_AGENT_SKILL_BRIDGE_UART_SSH_ACCESS,
        .start = bridge_uart_ssh_access_skill_start,
        .end = bridge_uart_ssh_access_skill_end,
    },
};

static bool text_contains_ci(const char *text, const char *needle)
{
    if (!text || !needle || !needle[0]) {
        return false;
    }
    size_t needle_len = strlen(needle);
    for (const char *cursor = text; *cursor; cursor++) {
        if (strncasecmp(cursor, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool text_has_any(const char *text, const char *const *needles,
                         size_t needle_count)
{
    for (size_t index = 0; index < needle_count; index++) {
        if (text_contains_ci(text, needles[index])) {
            return true;
        }
    }
    return false;
}

static const si_agent_builtin_skill_t *skill_find(const char *skill_name)
{
    if (!skill_name) {
        return NULL;
    }
    for (size_t index = 0; index < si_agent_skill_count(); index++) {
        if (strcmp(BUILTIN_SKILLS[index].name, skill_name) == 0) {
            return &BUILTIN_SKILLS[index];
        }
    }
    return NULL;
}

size_t si_agent_skill_count(void)
{
    return sizeof(BUILTIN_SKILLS) / sizeof(BUILTIN_SKILLS[0]);
}

const char *si_agent_skill_name_at(size_t index)
{
    return index < si_agent_skill_count() ? BUILTIN_SKILLS[index].name : NULL;
}

bool si_agent_skill_enabled(const char *skill_name)
{
    if (!skill_find(skill_name)) {
        return false;
    }

    char *settings = calloc(1, SI_AGENT_TOOLS_SKILLS_MAX_LEN + 1U);
    if (!settings) {
        return true;
    }
    cJSON *root = NULL;
    if (si_agent_tools_load_string(
            SI_AGENT_TOOLS_SKILLS_KEY, "[]", settings,
            SI_AGENT_TOOLS_SKILLS_MAX_LEN + 1U) == ESP_OK) {
        root = cJSON_Parse(settings);
    }
    memset(settings, 0, SI_AGENT_TOOLS_SKILLS_MAX_LEN + 1U);
    free(settings);

    bool enabled = true;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        const cJSON *name =
            cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsString(name) || !name->valuestring ||
            strcmp(name->valuestring, skill_name) != 0) {
            continue;
        }
        const cJSON *enabled_item =
            cJSON_GetObjectItemCaseSensitive(item, "enabled");
        enabled = !cJSON_IsFalse(enabled_item);
        break;
    }
    cJSON_Delete(root);
    return enabled;
}

static bool should_activate_uart_cli(const si_agent_route_t *route,
                                     const char *message)
{
    if (route && route->kind == SI_AGENT_ROUTE_UART_OPS) {
        return true;
    }
    static const char *const triggers[] = {
        "ttyacm", "agetty", "serial-getty", "serial getty",
        "uart cli", "uart login", "串口登录", "串口登陆",
        "串口输出", "登录提示", "登陆提示",
    };
    return text_has_any(message, triggers,
                        sizeof(triggers) / sizeof(triggers[0]));
}

static bool should_activate_kvm_access(const si_agent_route_t *route,
                                       const char *message)
{
    if (text_contains_ci(message, SI_AGENT_SKILL_CONFIGURE_ACCESS_VIA_KVM)) {
        return true;
    }
    if (!route || route->kind != SI_AGENT_ROUTE_KVM_VISUAL) {
        return false;
    }
    static const char *const triggers[] = {
        "configure uart", "configure ssh", "enable uart", "enable ssh",
        "setup uart", "setup ssh", "recover uart", "recover ssh",
        "配置uart", "配置 uart", "配置串口", "配置ssh", "配置 ssh",
        "启用uart", "启用 uart", "启用串口", "启用ssh", "启用 ssh",
        "串口和ssh", "串口与ssh", "uart and ssh", "uart 与 ssh",
        "远程维护通道", "远程访问",
    };
    return text_has_any(message, triggers,
                        sizeof(triggers) / sizeof(triggers[0]));
}

static bool should_activate_uart_ssh_bridge(const si_agent_route_t *route,
                                            const char *message)
{
    if (text_contains_ci(message, SI_AGENT_SKILL_BRIDGE_UART_SSH_ACCESS)) {
        return true;
    }
    if (!route ||
        (route->kind != SI_AGENT_ROUTE_UART_OPS &&
         route->kind != SI_AGENT_ROUTE_SSH_OPS)) {
        return false;
    }
    static const char *const triggers[] = {
        "ssh to uart", "uart to ssh", "ssh-to-uart", "uart-to-ssh",
        "from ssh", "from uart", "via ssh", "via uart",
        "通过ssh配置uart", "通过 ssh 配置 uart",
        "通过ssh配置串口", "通过 ssh 配置串口",
        "通过uart配置ssh", "通过 uart 配置 ssh",
        "通过串口配置ssh", "通过串口配置 ssh",
        "从ssh配置", "从 ssh 配置", "从uart配置", "从 uart 配置",
        "从串口配置", "互相配置", "互配", "备用通道", "第二条通道",
    };
    return text_has_any(message, triggers,
                        sizeof(triggers) / sizeof(triggers[0]));
}

bool si_agent_skill_should_activate(const char *skill_name,
                                    const si_agent_route_t *route,
                                    const char *message)
{
    if (!skill_find(skill_name)) {
        return false;
    }
    if (strcmp(skill_name, SI_AGENT_SKILL_CONFIGURE_UART_CLI) == 0) {
        return should_activate_uart_cli(route, message);
    }
    if (strcmp(skill_name, SI_AGENT_SKILL_CONFIGURE_ACCESS_VIA_KVM) == 0) {
        return should_activate_kvm_access(route, message);
    }
    if (strcmp(skill_name, SI_AGENT_SKILL_BRIDGE_UART_SSH_ACCESS) == 0) {
        return should_activate_uart_ssh_bridge(route, message);
    }
    return false;
}

const char *si_agent_skill_content(const char *skill_name,
                                   size_t *content_length)
{
    if (content_length) {
        *content_length = 0;
    }
    const si_agent_builtin_skill_t *skill = skill_find(skill_name);
    if (!skill || skill->end <= skill->start) {
        return NULL;
    }
    size_t length = (size_t)(skill->end - skill->start);
    if (length == 0 || length > 8192U) {
        return NULL;
    }
    if (content_length) {
        *content_length = length;
    }
    return (const char *)skill->start;
}
