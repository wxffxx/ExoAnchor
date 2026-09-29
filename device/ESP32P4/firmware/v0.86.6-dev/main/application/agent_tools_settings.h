#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agent_api_settings.h"
#include "esp_err.h"

#define SI_AGENT_TOOLS_MODE_KEY "mode"
#define SI_AGENT_ACCESS_MODE_KEY "access_mode"
#define SI_AGENT_TOOLS_POLICY_KEY "policy_json"
#define SI_AGENT_TOOLS_SKILLS_KEY "skills_json"
#define SI_AGENT_TOOLS_POLICY_MAX_LEN 4096
#define SI_AGENT_TOOLS_SKILLS_MAX_LEN 4096

typedef enum {
    SI_AGENT_TOOL_AUDIENCE_AGENT = 0,
    SI_AGENT_TOOL_AUDIENCE_MCP,
} si_agent_tool_audience_t;

typedef enum {
    SI_AGENT_ACCESS_MANUAL = 0,
    SI_AGENT_ACCESS_ASSISTED,
    SI_AGENT_ACCESS_FULL,
} si_agent_access_mode_t;

typedef struct {
    bool enabled;
    bool api_key_configured;
    char profile[SI_AGENT_API_PROFILE_ID_MAX_LEN + 1];
    char endpoint[SI_AGENT_API_ENDPOINT_MAX_LEN + 1];
    char model[SI_AGENT_API_MODEL_MAX_LEN + 1];
    char strategy[17];
} si_agent_web_search_settings_t;

typedef struct {
    const char *mode;
    const char *policy;
    const char *skills;
    bool web_present;
    bool web_enabled_present;
    bool web_enabled;
    const char *web_profile;
    const char *web_endpoint;
    const char *web_model;
    const char *web_strategy;
    const char *web_api_key;
    bool clear_web_api_key;
} si_agent_tools_update_t;

esp_err_t si_agent_tools_load_string(const char *key, const char *fallback,
                                     char *out, size_t out_size);
esp_err_t si_agent_tools_cache_init(void);
uint32_t si_agent_tools_policy_revision(void);
bool si_agent_access_mode_valid(const char *mode);
const char *si_agent_access_mode_name(si_agent_access_mode_t mode);
si_agent_access_mode_t si_agent_access_mode_get(void);
esp_err_t si_agent_access_mode_set(si_agent_access_mode_t mode);
bool si_agent_tool_policy_allows(const char *tool_name, bool dry_run,
                                 char *reason, size_t reason_size);
bool si_agent_tool_policy_allows_for(const char *tool_name,
                                     si_agent_tool_audience_t audience,
                                     bool dry_run, char *reason,
                                     size_t reason_size);
bool si_mcp_tool_policy_allows(const char *tool_name, char *reason,
                               size_t reason_size);
void si_agent_web_search_defaults(si_agent_web_search_settings_t *settings);
esp_err_t si_agent_web_search_load(si_agent_web_search_settings_t *settings);
esp_err_t si_agent_web_search_get_secret(const si_agent_web_search_settings_t *settings,
                                         char *out, size_t out_size,
                                         const char **source_out);
bool si_agent_web_search_available(const si_agent_web_search_settings_t *settings);
esp_err_t si_agent_tools_update(const si_agent_tools_update_t *update);
esp_err_t si_agent_tools_reset(void);
