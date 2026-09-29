#include "agent_prompt_service.h"

#include <string.h>

#include "esp_check.h"
#include "settings_store.h"

#define AGENT_PROMPT_NAMESPACE "si_agent_prompt"
#define AGENT_PROMPT_SYSTEM_KEY "system"

static const char *TAG = "si-agent-prompt";

static const char s_default_prompt[] =
    "你是 ExoAnchor 的 ESP32-P4 KVM/运维 Agent。默认用用户的语言自然、简洁地回答。"
    "你可以通过工具观察和维护目标主机，包括 SSH、屏幕截图、USB HID 和电源控制。"
    "通过 KVM 登录时，只能引用 console://default 并调用 console_login；绝不能要求模型生成、复述或记录账户密码。"
    "先根据截图聚焦正确的登录字段，设备将在本地把已保存凭据转换为 HID 输入，随后必须用新截图验证登录结果。"
    "用户要求通过 SSH 检查、安装或配置系统时，应优先使用 ssh_exec 工具，而不是声称无法 SSH。"
    "在 execute 模式下，用户已经授权你推进当前明确任务；不要在常规软件安装、用户创建、服务配置、下载校验或状态检查前再次询问确认。"
    "只有格式化磁盘、删除大量数据、重启/关机、修改网络导致可能失联、暴露凭据等高风险动作才需要先询问用户。"
    "涉及后台进程、screen/tmux、systemd/docker、独立运行用户或 sudo/su 的任务，完成前必须验证操作者从当前 SSH 登录身份能否按最终说明实际访问、控制或回滚。"
    "如果运行用户和登录用户不同，必须给出正确的 sudo -u/su 命令，并说明重启后是否自动恢复。"
    "当用户询问此前会话、长期状态、历史决策、待办或主机部署事实时，应优先使用 memory_search/history_search，而不是仅凭当前上下文猜测。"
    "普通聊天、解释、回顾上下文、确认信息时，tool_calls 和 actions 必须是空数组。"
    "可以返回 trace 数组作为给用户看的简短计划/进度摘要，但不要输出隐藏思维链。"
    "必须只返回 JSON，不要 Markdown。Schema: "
    "{\"message\":\"给用户看的自然回复\",\"trace\":[\"可展示步骤\"],\"tool_calls\":[...],\"actions\":[...]}. "
    "SSH tool call: {\"tool\":\"ssh_exec\",\"args\":{\"command\":\"uname -a\"}}. "
    "Allowed action types: wait, keydown, keyup, combo, mousemove, absmove, "
    "absclick, click, wheel, releaseall. Use JavaScript KeyboardEvent code names "
    "such as Enter, Escape, KeyA, ControlLeft. Keep action batches small.";

static esp_err_t validate_prompt(const char *prompt)
{
    if (!prompt) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(prompt);
    if (len == 0 || len > SI_AGENT_PROMPT_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)prompt[i];
        if (ch < 32 && ch != '\n' && ch != '\r' && ch != '\t') {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

const char *si_agent_prompt_default(void)
{
    return s_default_prompt;
}

esp_err_t si_agent_prompt_get(si_agent_prompt_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(settings, 0, sizeof(*settings));
    strlcpy(settings->system_prompt, s_default_prompt, sizeof(settings->system_prompt));
    settings->using_default = true;

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, AGENT_PROMPT_NAMESPACE);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    char stored[SI_AGENT_PROMPT_MAX_LEN + 1] = {0};
    ret = si_settings_store_get_string(&store, AGENT_PROMPT_SYSTEM_KEY,
                                       stored, sizeof(stored));
    si_settings_store_close(&store);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (validate_prompt(stored) == ESP_OK) {
        strlcpy(settings->system_prompt, stored, sizeof(settings->system_prompt));
        settings->using_default = false;
    }
    return ESP_OK;
}

esp_err_t si_agent_prompt_set(const char *prompt)
{
    ESP_RETURN_ON_ERROR(validate_prompt(prompt), TAG, "validate Agent prompt");
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store, AGENT_PROMPT_NAMESPACE),
                        TAG, "open Agent prompt settings");
    esp_err_t ret = si_settings_store_set_string(&store,
                                                 AGENT_PROMPT_SYSTEM_KEY,
                                                 prompt);
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}

esp_err_t si_agent_prompt_reset(void)
{
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(&store, AGENT_PROMPT_NAMESPACE);
    ESP_RETURN_ON_ERROR(ret, TAG, "open Agent prompt settings");
    ret = si_settings_store_erase_key(&store, AGENT_PROMPT_SYSTEM_KEY);
    if (ret == ESP_ERR_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}
