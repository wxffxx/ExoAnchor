#include "agent_tools_settings.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "secret_store.h"
#include "settings_store.h"

#define NAMESPACE "si_agent_tools"
#define WEB_ENABLED_KEY "web_en"
#define WEB_PROFILE_KEY "web_prof"
#define WEB_ENDPOINT_KEY "web_ep"
#define WEB_MODEL_KEY "web_model"
#define WEB_SECRET_KEY "web_key"
#define WEB_STRATEGY_KEY "web_strategy"
#define WEB_PROFILE_DEFAULT "qwen"
#define WEB_MODEL_DEFAULT "qwen-plus"
#define WEB_STRATEGY_DEFAULT "agent"

static const char *TAG = "si-agent-tools";

/*
 * MCP authorization runs in HTTP worker tasks. Some of those workers need
 * large PSRAM stacks, but NVS temporarily disables the external-memory cache
 * while reading flash. Keep the request-time policy values in internal BSS so
 * authorization never performs flash I/O from a PSRAM stack.
 */
static SemaphoreHandle_t s_tools_cache_lock;
static bool s_tools_cache_ready;
static uint32_t s_tools_policy_revision = 1U;
static char s_tools_mode_cache[17] = "observe";
static char s_agent_access_mode_cache[17] = "manual";
static char s_tools_policy_cache[SI_AGENT_TOOLS_POLICY_MAX_LEN + 1U] =
    "{\"version\":2,\"tools\":{}}";
static char s_tools_skills_cache[SI_AGENT_TOOLS_SKILLS_MAX_LEN + 1U] =
    "{\"version\":1,\"skills\":{}}";

static esp_err_t agent_tools_load_string_from_nvs(const char *key,
                                                   const char *fallback,
                                                   char *out,
                                                   size_t out_size)
{
    strlcpy(out, fallback, out_size);
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, NAMESPACE);
    if (ret == ESP_ERR_NOT_FOUND) return ESP_OK;
    ESP_RETURN_ON_ERROR(ret, TAG, "open settings store");
    ret = si_settings_store_get_string(&store, key, out, out_size);
    si_settings_store_close(&store);
    if (ret == ESP_ERR_NOT_FOUND) {
        strlcpy(out, fallback, out_size);
        return ESP_OK;
    }
    return ret;
}

static const char *agent_tools_cached_value(const char *key)
{
    if (strcmp(key, SI_AGENT_TOOLS_MODE_KEY) == 0) return s_tools_mode_cache;
    if (strcmp(key, SI_AGENT_ACCESS_MODE_KEY) == 0)
        return s_agent_access_mode_cache;
    if (strcmp(key, SI_AGENT_TOOLS_POLICY_KEY) == 0) return s_tools_policy_cache;
    if (strcmp(key, SI_AGENT_TOOLS_SKILLS_KEY) == 0) return s_tools_skills_cache;
    return NULL;
}

esp_err_t si_agent_tools_cache_init(void)
{
    if (s_tools_cache_ready) return ESP_OK;
    if (!s_tools_cache_lock) {
        s_tools_cache_lock = xSemaphoreCreateMutex();
        if (!s_tools_cache_lock) return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = agent_tools_load_string_from_nvs(
        SI_AGENT_TOOLS_MODE_KEY, "observe", s_tools_mode_cache,
        sizeof(s_tools_mode_cache));
    if (ret == ESP_OK) {
        ret = agent_tools_load_string_from_nvs(
            SI_AGENT_ACCESS_MODE_KEY, "manual", s_agent_access_mode_cache,
            sizeof(s_agent_access_mode_cache));
        if (ret == ESP_OK &&
            !si_agent_access_mode_valid(s_agent_access_mode_cache)) {
            strlcpy(s_agent_access_mode_cache, "manual",
                    sizeof(s_agent_access_mode_cache));
        }
    }
    if (ret == ESP_OK) {
        ret = agent_tools_load_string_from_nvs(
            SI_AGENT_TOOLS_POLICY_KEY, "{\"version\":2,\"tools\":{}}",
            s_tools_policy_cache, sizeof(s_tools_policy_cache));
    }
    if (ret == ESP_OK) {
        ret = agent_tools_load_string_from_nvs(
            SI_AGENT_TOOLS_SKILLS_KEY, "{\"version\":1,\"skills\":{}}",
            s_tools_skills_cache, sizeof(s_tools_skills_cache));
    }
    if (ret == ESP_OK) s_tools_cache_ready = true;
    return ret;
}

uint32_t si_agent_tools_policy_revision(void)
{
    return __atomic_load_n(&s_tools_policy_revision, __ATOMIC_ACQUIRE);
}

static void agent_tools_policy_revision_bump(void)
{
    uint32_t next = __atomic_add_fetch(&s_tools_policy_revision, 1U,
                                       __ATOMIC_ACQ_REL);
    if (next == 0U) {
        __atomic_store_n(&s_tools_policy_revision, 1U, __ATOMIC_RELEASE);
    }
}

bool si_agent_access_mode_valid(const char *mode)
{
    return mode && (!strcasecmp(mode, "manual") ||
                    !strcasecmp(mode, "assisted") ||
                    !strcasecmp(mode, "full"));
}

const char *si_agent_access_mode_name(si_agent_access_mode_t mode)
{
    switch (mode) {
    case SI_AGENT_ACCESS_ASSISTED: return "assisted";
    case SI_AGENT_ACCESS_FULL: return "full";
    case SI_AGENT_ACCESS_MANUAL:
    default: return "manual";
    }
}

si_agent_access_mode_t si_agent_access_mode_get(void)
{
    char mode[17] = "manual";
    (void)si_agent_tools_load_string(SI_AGENT_ACCESS_MODE_KEY, "manual",
                                     mode, sizeof(mode));
    if (!strcasecmp(mode, "assisted")) return SI_AGENT_ACCESS_ASSISTED;
    if (!strcasecmp(mode, "full")) return SI_AGENT_ACCESS_FULL;
    return SI_AGENT_ACCESS_MANUAL;
}

esp_err_t si_agent_access_mode_set(si_agent_access_mode_t mode)
{
    if (mode < SI_AGENT_ACCESS_MANUAL || mode > SI_AGENT_ACCESS_FULL)
        return ESP_ERR_INVALID_ARG;
    const char *value = si_agent_access_mode_name(mode);
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(
        si_settings_store_open_write(&store, NAMESPACE), TAG,
        "open settings store");
    esp_err_t ret = si_settings_store_set_string(
        &store, SI_AGENT_ACCESS_MODE_KEY, value);
    if (ret == ESP_OK) ret = si_settings_store_commit(&store);
    si_settings_store_close(&store);
    if (ret == ESP_OK && s_tools_cache_ready && s_tools_cache_lock &&
        xSemaphoreTake(s_tools_cache_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        strlcpy(s_agent_access_mode_cache, value,
                sizeof(s_agent_access_mode_cache));
        xSemaphoreGive(s_tools_cache_lock);
    }
    if (ret == ESP_OK) agent_tools_policy_revision_bump();
    return ret;
}

static bool strategy_valid(const char *strategy)
{
    return strategy && (!strcasecmp(strategy, "agent") || !strcasecmp(strategy, "agent_max") ||
                        !strcasecmp(strategy, "turbo") || !strcasecmp(strategy, "max"));
}

esp_err_t si_agent_tools_load_string(const char *key, const char *fallback,
                                     char *out, size_t out_size)
{
    if (!key || !fallback || !out || !out_size) return ESP_ERR_INVALID_ARG;
    const char *cached = agent_tools_cached_value(key);
    if (cached && s_tools_cache_ready && s_tools_cache_lock &&
        xSemaphoreTake(s_tools_cache_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        strlcpy(out, cached, out_size);
        xSemaphoreGive(s_tools_cache_lock);
        return ESP_OK;
    }
    return agent_tools_load_string_from_nvs(key, fallback, out, out_size);
}

bool si_agent_tool_policy_allows_for(const char *tool_name,
                                     si_agent_tool_audience_t audience,
                                     bool dry_run, char *reason,
                                     size_t reason_size)
{
    if (reason && reason_size) {
        reason[0] = '\0';
    }
    if (!tool_name || !tool_name[0]) {
        return true;
    }
    if (audience != SI_AGENT_TOOL_AUDIENCE_AGENT &&
        audience != SI_AGENT_TOOL_AUDIENCE_MCP) {
        if (reason && reason_size) {
            strlcpy(reason, "invalid tool policy audience", reason_size);
        }
        return false;
    }

    char *policy = calloc(1, SI_AGENT_TOOLS_POLICY_MAX_LEN + 1U);
    if (!policy) {
        if (reason && reason_size) {
            strlcpy(reason, "tool policy allocation failed", reason_size);
        }
        return false;
    }
    esp_err_t ret = si_agent_tools_load_string(
        SI_AGENT_TOOLS_POLICY_KEY, "{\"version\":2,\"tools\":{}}",
        policy, SI_AGENT_TOOLS_POLICY_MAX_LEN + 1U);
    cJSON *root = ret == ESP_OK ? cJSON_Parse(policy) : NULL;
    memset(policy, 0, SI_AGENT_TOOLS_POLICY_MAX_LEN + 1U);
    free(policy);
    if (!root) {
        if (reason && reason_size) {
            strlcpy(reason, "tool policy is invalid", reason_size);
        }
        return false;
    }

    bool allowed = true;
    const cJSON *tools = cJSON_GetObjectItemCaseSensitive(root, "tools");
    const cJSON *item = cJSON_IsObject(tools) ?
                        cJSON_GetObjectItemCaseSensitive(tools, tool_name) : NULL;
    if (cJSON_IsObject(item)) {
        const char *enabled_key =
            audience == SI_AGENT_TOOL_AUDIENCE_MCP ?
            "mcp_enabled" : "agent_enabled";
        const cJSON *enabled =
            cJSON_GetObjectItemCaseSensitive(item, enabled_key);
        if (!cJSON_IsBool(enabled) &&
            audience == SI_AGENT_TOOL_AUDIENCE_AGENT) {
            enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
        }
        if (cJSON_IsFalse(enabled)) {
            allowed = false;
            if (reason && reason_size) {
                strlcpy(
                    reason,
                    audience == SI_AGENT_TOOL_AUDIENCE_MCP ?
                    "tool disabled for MCP by policy" :
                    "tool disabled for local Agent by policy",
                    reason_size);
            }
        }
        const cJSON *permission =
            cJSON_GetObjectItemCaseSensitive(item, "permission");
        if (allowed && !dry_run &&
            audience == SI_AGENT_TOOL_AUDIENCE_AGENT &&
            cJSON_IsString(permission) &&
            permission->valuestring &&
            strcasecmp(permission->valuestring, "autonomous") == 0) {
            char mode[17] = {0};
            (void)si_agent_tools_load_string(
                SI_AGENT_TOOLS_MODE_KEY, "observe", mode, sizeof(mode));
            if (strcasecmp(mode, "autonomous") != 0 &&
                si_agent_access_mode_get() != SI_AGENT_ACCESS_FULL) {
                allowed = false;
                if (reason && reason_size) {
                    strlcpy(reason,
                            "tool requires autonomous policy mode",
                            reason_size);
                }
            }
        }
    }
    cJSON_Delete(root);
    return allowed;
}

bool si_agent_tool_policy_allows(const char *tool_name, bool dry_run,
                                 char *reason, size_t reason_size)
{
    return si_agent_tool_policy_allows_for(
        tool_name, SI_AGENT_TOOL_AUDIENCE_AGENT, dry_run, reason,
        reason_size);
}

bool si_mcp_tool_policy_allows(const char *tool_name, char *reason,
                               size_t reason_size)
{
    return si_agent_tool_policy_allows_for(
        tool_name, SI_AGENT_TOOL_AUDIENCE_MCP, false, reason,
        reason_size);
}

void si_agent_web_search_defaults(si_agent_web_search_settings_t *settings)
{
    if (!settings) return;
    memset(settings, 0, sizeof(*settings));
    strlcpy(settings->profile, WEB_PROFILE_DEFAULT, sizeof(settings->profile));
    strlcpy(settings->model, WEB_MODEL_DEFAULT, sizeof(settings->model));
    strlcpy(settings->strategy, WEB_STRATEGY_DEFAULT, sizeof(settings->strategy));
}

static bool secret_exists(si_settings_store_t *store)
{
    bool configured = false;
    return si_secret_store_is_configured(
               store, WEB_SECRET_KEY, &configured) == ESP_OK &&
           configured;
}

esp_err_t si_agent_web_search_load(si_agent_web_search_settings_t *settings)
{
    if (!settings) return ESP_ERR_INVALID_ARG;
    si_agent_web_search_defaults(settings);
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, NAMESPACE);
    if (ret == ESP_ERR_NOT_FOUND) return ESP_OK;
    ESP_RETURN_ON_ERROR(ret, TAG, "open settings store");
    uint8_t enabled = 0;
    ret = si_settings_store_get_u8(&store, WEB_ENABLED_KEY, &enabled);
    if (ret == ESP_OK) settings->enabled = enabled != 0;
    else if (ret != ESP_ERR_NOT_FOUND) goto done;
    ret = si_settings_store_get_string(
        &store, WEB_PROFILE_KEY, settings->profile, sizeof(settings->profile));
    if (ret == ESP_OK && !si_agent_api_profile_id_valid(settings->profile))
        strlcpy(settings->profile, WEB_PROFILE_DEFAULT, sizeof(settings->profile));
    else if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) goto done;
    ret = si_settings_store_get_string(
        &store, WEB_ENDPOINT_KEY, settings->endpoint,
        sizeof(settings->endpoint));
    if (ret == ESP_OK && si_agent_api_validate_text(settings->endpoint, SI_AGENT_API_ENDPOINT_MAX_LEN, true) != ESP_OK)
        settings->endpoint[0] = 0;
    else if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) goto done;
    ret = si_settings_store_get_string(
        &store, WEB_MODEL_KEY, settings->model, sizeof(settings->model));
    if (ret == ESP_OK && si_agent_api_validate_text(settings->model, SI_AGENT_API_MODEL_MAX_LEN, false) != ESP_OK)
        strlcpy(settings->model, WEB_MODEL_DEFAULT, sizeof(settings->model));
    else if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) goto done;
    ret = si_settings_store_get_string(
        &store, WEB_STRATEGY_KEY, settings->strategy,
        sizeof(settings->strategy));
    if (ret == ESP_OK && !strategy_valid(settings->strategy))
        strlcpy(settings->strategy, WEB_STRATEGY_DEFAULT, sizeof(settings->strategy));
    else if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) goto done;
    settings->api_key_configured = secret_exists(&store);
    ret = ESP_OK;
done:
    si_settings_store_close(&store);
    return ret == ESP_ERR_NOT_FOUND ? ESP_OK : ret;
}

esp_err_t si_agent_web_search_get_secret(const si_agent_web_search_settings_t *settings,
                                         char *out, size_t out_size, const char **source_out)
{
    if (!settings || !out || !out_size) return ESP_ERR_INVALID_ARG;
    out[0] = 0; if (source_out) *source_out = "none";
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, NAMESPACE);
    if (ret == ESP_OK) {
        ret = si_secret_store_get_string(
            &store, WEB_SECRET_KEY, out, out_size);
        si_settings_store_close(&store);
        if (ret == ESP_OK && out[0]) { if (source_out) *source_out = "web_search"; return ESP_OK; }
        si_secret_store_clear(out, out_size);
    } else if (ret != ESP_ERR_NOT_FOUND) return ret;
    ret = si_agent_api_get_secret(settings->profile, out, out_size);
    if (ret == ESP_OK && out[0]) { if (source_out) *source_out = "agent_profile"; return ESP_OK; }
    si_secret_store_clear(out, out_size);
    return ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;
}

bool si_agent_web_search_available(const si_agent_web_search_settings_t *settings)
{
    if (!settings || !settings->enabled) return false;
    char secret[SI_AGENT_API_SECRET_MAX_LEN + 1] = {0};
    bool ok = si_agent_web_search_get_secret(settings, secret, sizeof(secret), NULL) == ESP_OK && secret[0];
    si_secret_store_clear(secret, sizeof(secret));
    return ok;
}

esp_err_t si_agent_tools_update(const si_agent_tools_update_t *u)
{
    if (!u) return ESP_ERR_INVALID_ARG;
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(
        si_settings_store_open_write(&store, NAMESPACE), TAG,
        "open settings store");
    esp_err_t ret = ESP_OK;
#define SET_STR(key, value) do {                                             \
        if (ret == ESP_OK && (value)) {                                      \
            ret = si_settings_store_set_string(&store, key, value);          \
        }                                                                    \
    } while (0)
    SET_STR(SI_AGENT_TOOLS_MODE_KEY, u->mode); SET_STR(SI_AGENT_TOOLS_POLICY_KEY, u->policy);
    SET_STR(SI_AGENT_TOOLS_SKILLS_KEY, u->skills);
    if (ret == ESP_OK && u->web_present && u->web_enabled_present) {
        ret = si_settings_store_set_u8(
            &store, WEB_ENABLED_KEY, u->web_enabled);
    }
    if (u->web_present) {
        SET_STR(WEB_PROFILE_KEY, u->web_profile); SET_STR(WEB_ENDPOINT_KEY, u->web_endpoint);
        SET_STR(WEB_MODEL_KEY, u->web_model); SET_STR(WEB_STRATEGY_KEY, u->web_strategy);
        if (ret == ESP_OK && u->clear_web_api_key) {
            ret = si_secret_store_erase(&store, WEB_SECRET_KEY);
        } else if (ret == ESP_OK && u->web_api_key &&
                   u->web_api_key[0]) {
            ret = si_secret_store_set_string(
                &store, WEB_SECRET_KEY, u->web_api_key);
        }
    }
#undef SET_STR
    if (ret == ESP_OK) ret = si_settings_store_commit(&store);
    si_settings_store_close(&store);
    if (ret == ESP_OK && s_tools_cache_ready && s_tools_cache_lock &&
        xSemaphoreTake(s_tools_cache_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (u->mode) strlcpy(s_tools_mode_cache, u->mode,
                             sizeof(s_tools_mode_cache));
        if (u->policy) strlcpy(s_tools_policy_cache, u->policy,
                               sizeof(s_tools_policy_cache));
        if (u->skills) strlcpy(s_tools_skills_cache, u->skills,
                               sizeof(s_tools_skills_cache));
        xSemaphoreGive(s_tools_cache_lock);
    }
    if (ret == ESP_OK) {
        agent_tools_policy_revision_bump();
    }
    return ret;
}

esp_err_t si_agent_tools_reset(void)
{
    static const char *keys[] = {SI_AGENT_TOOLS_MODE_KEY, SI_AGENT_ACCESS_MODE_KEY,
        SI_AGENT_TOOLS_POLICY_KEY,
        SI_AGENT_TOOLS_SKILLS_KEY, WEB_ENABLED_KEY, WEB_PROFILE_KEY, WEB_ENDPOINT_KEY,
        WEB_MODEL_KEY, WEB_SECRET_KEY, WEB_STRATEGY_KEY};
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(
        si_settings_store_open_write(&store, NAMESPACE), TAG,
        "open settings store");
    esp_err_t first = ESP_OK;
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) {
        esp_err_t ret = strcmp(keys[i], WEB_SECRET_KEY) == 0 ?
                        si_secret_store_erase(&store, keys[i]) :
                        si_settings_store_erase_key(&store, keys[i]);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND && first == ESP_OK) {
            first = ret;
        }
    }
    if (first == ESP_OK) first = si_settings_store_commit(&store);
    si_settings_store_close(&store);
    if (first == ESP_OK && s_tools_cache_ready && s_tools_cache_lock &&
        xSemaphoreTake(s_tools_cache_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        strlcpy(s_tools_mode_cache, "observe", sizeof(s_tools_mode_cache));
        strlcpy(s_agent_access_mode_cache, "manual",
                sizeof(s_agent_access_mode_cache));
        strlcpy(s_tools_policy_cache, "{\"version\":2,\"tools\":{}}",
                sizeof(s_tools_policy_cache));
        strlcpy(s_tools_skills_cache, "{\"version\":1,\"skills\":{}}",
                sizeof(s_tools_skills_cache));
        xSemaphoreGive(s_tools_cache_lock);
    }
    if (first == ESP_OK) {
        agent_tools_policy_revision_bump();
    }
    return first;
}
