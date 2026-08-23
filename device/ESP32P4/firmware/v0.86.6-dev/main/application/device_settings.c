#include "device_settings.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "settings_store.h"

#define DEVICE_NAMESPACE "si_device"
#define DEVICE_LABEL_KEY "label"
#define AGENT_DISPLAY_NAME_KEY "agent_name"
#define SESSION_NAMESPACE "si_session"
#define SESSION_AUTO_LOGOUT_ENABLED_KEY "auto_logout"
#define SESSION_AUTO_LOGOUT_MINUTES_KEY "auto_minutes"
#define MCP_NAMESPACE "si_mcp"
#define MCP_ENABLED_KEY "enabled"
#define FEATURES_NAMESPACE "si_features"
#define LAN_DISCOVERY_KEY "lan_discovery"
#define EMBEDDED_AGENT_KEY "agent_enabled"
#define PAGE_CONTEXT_KEY "page_context"
#define CONVERSATION_HISTORY_KEY "history"
#define LONG_TERM_MEMORY_KEY "memory"

static const char *TAG = "si-settings";

static char s_device_label[SI_DEVICE_LABEL_MAX_LEN + 1];
static bool s_device_label_loaded;
static char s_agent_display_name[SI_AGENT_DISPLAY_NAME_MAX_LEN + 1];
static bool s_agent_display_name_loaded;
static si_session_settings_t s_session_settings = {
    .auto_logout_enabled = SI_SESSION_LOGOUT_DEFAULT_ENABLED,
    .auto_logout_minutes = SI_SESSION_LOGOUT_DEFAULT_MINUTES,
};
static bool s_session_settings_loaded;
static bool s_mcp_enabled = SI_MCP_DEFAULT_ENABLED;
static bool s_mcp_loaded;
/* This published snapshot deliberately starts all-false. Product defaults are
 * not observable until NVS has either been read successfully or confirmed
 * that the feature namespace does not exist. A transient flash/stack error
 * therefore cannot accidentally enable Agent or discovery, and is retried by
 * a later internal-stack preload instead of becoming permanent. */
static si_product_feature_settings_t s_product_features;
static bool s_product_features_loaded;
static atomic_bool s_lan_discovery_enabled_cached;
static esp_err_t load_product_features(void);

static uint32_t clamp_u32(uint32_t value, uint32_t minimum, uint32_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static esp_err_t validate_device_label(const char *label)
{
    if (!label) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(label);
    if (len < 1 || len > SI_DEVICE_LABEL_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)label[i];
        if (ch < 32 || ch == 127) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static esp_err_t validate_agent_display_name(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(name);
    if (len < 1 || len > SI_AGENT_DISPLAY_NAME_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 32 || ch == 127) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static void load_device_label(void)
{
    if (s_device_label_loaded) {
        return;
    }

    strlcpy(s_device_label, SI_DEVICE_LABEL_DEFAULT, sizeof(s_device_label));
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t err = si_settings_store_open_read(&store, DEVICE_NAMESPACE);
    if (err == ESP_OK) {
        char label[sizeof(s_device_label)] = {0};
        err = si_settings_store_get_string(&store, DEVICE_LABEL_KEY,
                                           label, sizeof(label));
        si_settings_store_close(&store);
        if (err == ESP_OK && validate_device_label(label) == ESP_OK) {
            strlcpy(s_device_label,
                    strcmp(label, "ESP32-P4") == 0 ?
                    SI_DEVICE_LABEL_DEFAULT : label,
                    sizeof(s_device_label));
        }
    }
    s_device_label_loaded = true;
}

void si_device_label_get(char *label, size_t label_size)
{
    if (!label || label_size == 0) {
        return;
    }
    load_device_label();
    strlcpy(label, s_device_label, label_size);
}

esp_err_t si_device_label_set(const char *label)
{
    return si_device_identity_set(label, NULL);
}

esp_err_t si_device_identity_set(const char *label, const char *agent_name)
{
    if (!label && !agent_name) {
        return ESP_ERR_INVALID_ARG;
    }
    if (label) {
        ESP_RETURN_ON_ERROR(validate_device_label(label), TAG,
                            "validate device label");
    }
    if (agent_name) {
        ESP_RETURN_ON_ERROR(validate_agent_display_name(agent_name), TAG,
                            "validate Agent display name");
    }
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store, DEVICE_NAMESPACE),
                        TAG, "open device settings");
    esp_err_t ret = ESP_OK;
    if (label) {
        ret = si_settings_store_set_string(&store, DEVICE_LABEL_KEY, label);
    }
    if (ret == ESP_OK && agent_name) {
        ret = si_settings_store_set_string(&store, AGENT_DISPLAY_NAME_KEY,
                                           agent_name);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    ESP_RETURN_ON_ERROR(ret, TAG, "save device settings");

    if (label) {
        strlcpy(s_device_label, label, sizeof(s_device_label));
        s_device_label_loaded = true;
    }
    if (agent_name) {
        strlcpy(s_agent_display_name, agent_name,
                sizeof(s_agent_display_name));
        s_agent_display_name_loaded = true;
    }
    return ESP_OK;
}

static void load_agent_display_name(void)
{
    if (s_agent_display_name_loaded) {
        return;
    }
    strlcpy(s_agent_display_name, SI_AGENT_DISPLAY_NAME_DEFAULT,
            sizeof(s_agent_display_name));
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t err = si_settings_store_open_read(&store, DEVICE_NAMESPACE);
    if (err == ESP_OK) {
        char name[sizeof(s_agent_display_name)] = {0};
        err = si_settings_store_get_string(&store, AGENT_DISPLAY_NAME_KEY,
                                           name, sizeof(name));
        si_settings_store_close(&store);
        if (err == ESP_OK && validate_agent_display_name(name) == ESP_OK) {
            strlcpy(s_agent_display_name, name,
                    sizeof(s_agent_display_name));
        }
    }
    s_agent_display_name_loaded = true;
}

void si_agent_display_name_get(char *name, size_t name_size)
{
    if (!name || name_size == 0) {
        return;
    }
    load_agent_display_name();
    strlcpy(name, s_agent_display_name, name_size);
}

esp_err_t si_agent_display_name_set(const char *name)
{
    return si_device_identity_set(NULL, name);
}

static void load_session_settings(void)
{
    if (s_session_settings_loaded) {
        return;
    }

    s_session_settings.auto_logout_enabled = SI_SESSION_LOGOUT_DEFAULT_ENABLED;
    s_session_settings.auto_logout_minutes = SI_SESSION_LOGOUT_DEFAULT_MINUTES;

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t err = si_settings_store_open_read(&store, SESSION_NAMESPACE);
    if (err == ESP_OK) {
        uint8_t enabled = 0;
        if (si_settings_store_get_u8(&store, SESSION_AUTO_LOGOUT_ENABLED_KEY,
                                     &enabled) == ESP_OK) {
            s_session_settings.auto_logout_enabled = enabled != 0;
        }
        uint32_t minutes = 0;
        if (si_settings_store_get_u32(&store, SESSION_AUTO_LOGOUT_MINUTES_KEY,
                                      &minutes) == ESP_OK) {
            s_session_settings.auto_logout_minutes =
                clamp_u32(minutes, SI_SESSION_LOGOUT_MIN_MINUTES,
                          SI_SESSION_LOGOUT_MAX_MINUTES);
        }
        si_settings_store_close(&store);
    }
    s_session_settings_loaded = true;
}

void si_session_settings_get(si_session_settings_t *settings)
{
    if (!settings) {
        return;
    }
    load_session_settings();
    *settings = s_session_settings;
}

esp_err_t si_session_settings_set(const si_session_settings_t *settings)
{
    if (!settings || settings->auto_logout_minutes < SI_SESSION_LOGOUT_MIN_MINUTES ||
        settings->auto_logout_minutes > SI_SESSION_LOGOUT_MAX_MINUTES) {
        return ESP_ERR_INVALID_ARG;
    }

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store, SESSION_NAMESPACE),
                        TAG, "open session settings");
    esp_err_t ret = si_settings_store_set_u8(
        &store, SESSION_AUTO_LOGOUT_ENABLED_KEY,
        settings->auto_logout_enabled ? 1 : 0);
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u32(&store, SESSION_AUTO_LOGOUT_MINUTES_KEY,
                                        settings->auto_logout_minutes);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    ESP_RETURN_ON_ERROR(ret, TAG, "save session settings");

    s_session_settings = *settings;
    s_session_settings_loaded = true;
    return ESP_OK;
}

static void load_mcp_enabled(void)
{
    if (s_mcp_loaded) {
        return;
    }
    s_mcp_enabled = SI_MCP_DEFAULT_ENABLED;
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, MCP_NAMESPACE);
    if (ret == ESP_OK) {
        uint8_t enabled = SI_MCP_DEFAULT_ENABLED ? 1 : 0;
        if (si_settings_store_get_u8(&store, MCP_ENABLED_KEY, &enabled) == ESP_OK) {
            s_mcp_enabled = enabled != 0;
        }
        si_settings_store_close(&store);
    }
    s_mcp_loaded = true;
}

bool si_mcp_is_enabled(void)
{
    load_mcp_enabled();
    return s_mcp_enabled;
}

esp_err_t si_mcp_set_enabled(bool enabled)
{
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store, MCP_NAMESPACE),
                        TAG, "open MCP settings");
    esp_err_t ret = si_settings_store_set_u8(&store, MCP_ENABLED_KEY,
                                             enabled ? 1 : 0);
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    if (ret == ESP_OK) {
        s_mcp_enabled = enabled;
        s_mcp_loaded = true;
    }
    return ret;
}

bool si_conversation_history_is_enabled(void)
{
    (void)load_product_features();
    return s_product_features.conversation_history_enabled;
}

bool si_page_context_is_enabled(void)
{
    (void)load_product_features();
    return s_product_features.page_context_enabled;
}

bool si_long_term_memory_is_enabled(void)
{
    (void)load_product_features();
    return s_product_features.long_term_memory_enabled;
}

static esp_err_t load_product_features(void)
{
    if (s_product_features_loaded) {
        return ESP_OK;
    }

    si_product_feature_settings_t candidate = {
        .lan_discovery_enabled = SI_LAN_DISCOVERY_DEFAULT_ENABLED,
        .embedded_agent_enabled = SI_EMBEDDED_AGENT_DEFAULT_ENABLED,
        .page_context_enabled = SI_PAGE_CONTEXT_DEFAULT_ENABLED,
        .conversation_history_enabled = SI_CONVERSATION_HISTORY_DEFAULT_ENABLED,
        .long_term_memory_enabled = SI_LONG_TERM_MEMORY_DEFAULT_ENABLED,
    };
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, FEATURES_NAMESPACE);
    if (ret == ESP_ERR_NOT_FOUND) {
        /* A missing namespace is the normal first-boot state. */
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        uint8_t value = 0;
#define LOAD_FEATURE(key, field)                                             \
        do {                                                                 \
            if (ret == ESP_OK) {                                             \
                esp_err_t read_ret =                                         \
                    si_settings_store_get_u8(&store, key, &value);            \
                if (read_ret == ESP_OK) {                                    \
                    candidate.field = value != 0;                            \
                } else if (read_ret != ESP_ERR_NOT_FOUND) {                  \
                    ret = read_ret;                                          \
                }                                                            \
            }                                                                \
        } while (0)
        LOAD_FEATURE(LAN_DISCOVERY_KEY, lan_discovery_enabled);
        LOAD_FEATURE(EMBEDDED_AGENT_KEY, embedded_agent_enabled);
        LOAD_FEATURE(PAGE_CONTEXT_KEY, page_context_enabled);
        LOAD_FEATURE(CONVERSATION_HISTORY_KEY, conversation_history_enabled);
        LOAD_FEATURE(LONG_TERM_MEMORY_KEY, long_term_memory_enabled);
#undef LOAD_FEATURE
        si_settings_store_close(&store);
    }

    if (ret != ESP_OK) {
        return ret;
    }
    s_product_features = candidate;
    atomic_store_explicit(&s_lan_discovery_enabled_cached,
                          candidate.lan_discovery_enabled,
                          memory_order_release);
    s_product_features_loaded = true;
    return ESP_OK;
}

esp_err_t si_product_feature_settings_preload(void)
{
    return load_product_features();
}

bool si_lan_discovery_is_enabled_cached(void)
{
    return atomic_load_explicit(&s_lan_discovery_enabled_cached,
                                memory_order_acquire);
}

void si_product_feature_settings_get(si_product_feature_settings_t *settings)
{
    if (!settings) {
        return;
    }
    (void)load_product_features();
    *settings = s_product_features;
}

esp_err_t si_product_feature_settings_set(
    const si_product_feature_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store, FEATURES_NAMESPACE),
                        TAG, "open product feature settings");
    esp_err_t ret = ESP_OK;
#define SAVE_FEATURE(key, value)                                             \
    do {                                                                     \
        if (ret == ESP_OK) {                                                 \
            ret = si_settings_store_set_u8(&store, key, (value) ? 1 : 0);    \
        }                                                                    \
    } while (0)
    SAVE_FEATURE(LAN_DISCOVERY_KEY, settings->lan_discovery_enabled);
    SAVE_FEATURE(EMBEDDED_AGENT_KEY, settings->embedded_agent_enabled);
    SAVE_FEATURE(PAGE_CONTEXT_KEY, settings->page_context_enabled);
    SAVE_FEATURE(CONVERSATION_HISTORY_KEY,
                 settings->conversation_history_enabled);
    SAVE_FEATURE(LONG_TERM_MEMORY_KEY, settings->long_term_memory_enabled);
#undef SAVE_FEATURE
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    if (ret == ESP_OK) {
        s_product_features = *settings;
        atomic_store_explicit(&s_lan_discovery_enabled_cached,
                              settings->lan_discovery_enabled,
                              memory_order_release);
        s_product_features_loaded = true;
    }
    return ret;
}
