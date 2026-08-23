#pragma once

#include <stdbool.h>

typedef enum {
    SI_AGENT_ROUTE_CHAT = 0,
    SI_AGENT_ROUTE_FOLLOWUP,
    SI_AGENT_ROUTE_TOOL_WORKFLOW,
    SI_AGENT_ROUTE_MEMORY_RECALL,
    SI_AGENT_ROUTE_KVM_VISUAL,
    SI_AGENT_ROUTE_UART_OPS,
    SI_AGENT_ROUTE_SSH_OPS,
    SI_AGENT_ROUTE_SERVICE_DEPLOY,
    SI_AGENT_ROUTE_WEB_RESEARCH,
    SI_AGENT_ROUTE_SETTINGS_HELP,
    SI_AGENT_ROUTE_DANGEROUS_ACTION,
} si_agent_route_kind_t;

typedef struct {
    si_agent_route_kind_t kind;
    const char *name;
    const char *intent;
    const char *prompt_profile;
    const char *tool_policy;
    const char *final_report_style;
    int confidence;
    int max_tool_loops;
    bool needs_control_lease;
    bool prefer_memory;
    bool prefer_screenshot;
    bool prefer_web_search;
} si_agent_route_t;

void si_agent_route_detect(const char *message, bool include_screenshot,
                           bool allow_web_search, si_agent_route_t *route);

const char *si_agent_route_prompt(const si_agent_route_t *route);
bool si_agent_route_allows_tool(const si_agent_route_t *route,
                                const char *tool_name);
bool si_agent_route_allows_hid(const si_agent_route_t *route);
