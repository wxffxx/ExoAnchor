#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SI_DEVICE_LABEL_DEFAULT "ExoAnchor"
#define SI_DEVICE_LABEL_MAX_LEN 48
#define SI_AGENT_DISPLAY_NAME_DEFAULT "Agent"
#define SI_AGENT_DISPLAY_NAME_MAX_LEN 32
#define SI_SESSION_LOGOUT_DEFAULT_ENABLED false
#define SI_SESSION_LOGOUT_DEFAULT_MINUTES 15U
#define SI_SESSION_LOGOUT_MIN_MINUTES 1U
#define SI_SESSION_LOGOUT_MAX_MINUTES 1440U
#define SI_MCP_DEFAULT_ENABLED true
#define SI_LAN_DISCOVERY_DEFAULT_ENABLED true
#define SI_EMBEDDED_AGENT_DEFAULT_ENABLED true
#define SI_PAGE_CONTEXT_DEFAULT_ENABLED false
#define SI_CONVERSATION_HISTORY_DEFAULT_ENABLED true
#define SI_LONG_TERM_MEMORY_DEFAULT_ENABLED false

typedef struct {
    bool auto_logout_enabled;
    uint32_t auto_logout_minutes;
} si_session_settings_t;

typedef struct {
    bool lan_discovery_enabled;
    bool embedded_agent_enabled;
    bool page_context_enabled;
    bool conversation_history_enabled;
    bool long_term_memory_enabled;
} si_product_feature_settings_t;

void si_device_label_get(char *label, size_t label_size);
esp_err_t si_device_label_set(const char *label);
void si_agent_display_name_get(char *name, size_t name_size);
esp_err_t si_agent_display_name_set(const char *name);
esp_err_t si_device_identity_set(const char *label, const char *agent_name);

void si_session_settings_get(si_session_settings_t *settings);
esp_err_t si_session_settings_set(const si_session_settings_t *settings);

bool si_mcp_is_enabled(void);
esp_err_t si_mcp_set_enabled(bool enabled);

void si_product_feature_settings_get(si_product_feature_settings_t *settings);
esp_err_t si_product_feature_settings_set(
    const si_product_feature_settings_t *settings);
/* Load the NVS-backed feature snapshot while the caller has an internal
 * stack. Until this succeeds all feature gates remain fail-closed. */
esp_err_t si_product_feature_settings_preload(void);
bool si_lan_discovery_is_enabled_cached(void);
bool si_page_context_is_enabled(void);
bool si_conversation_history_is_enabled(void);
bool si_long_term_memory_is_enabled(void);
