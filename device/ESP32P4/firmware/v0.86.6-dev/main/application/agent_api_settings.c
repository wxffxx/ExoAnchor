#include "agent_api_settings.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#ifndef SI_AGENT_API_VALIDATION_HOST_TEST
#include "esp_check.h"
#include "secret_store.h"
#include "settings_store.h"
#endif

#define NAMESPACE "si_agent_api"
#define ACTIVE_KEY "active"
#define LEGACY_PROVIDER_KEY "provider"
#define LEGACY_ENDPOINT_KEY "endpoint"
#define LEGACY_MODEL_KEY "model"
#define LEGACY_SECRET_KEY "api_key"

#ifndef SI_AGENT_API_VALIDATION_HOST_TEST
typedef struct {
    const char *id;
    const char *label;
    const char *provider;
    const char *endpoint;
    const char *model;
} profile_default_t;

static const char *TAG = "si-agent-api";
static const profile_default_t s_defaults[SI_AGENT_API_PROFILE_COUNT] = {
    {"deepseek", "DeepSeek", "deepseek", "https://api.deepseek.com", "deepseek-v4-pro"},
    {"openai", "OpenAI", "openai", "https://api.openai.com/v1", "gpt-4o"},
    {"qwen", "阿里云百炼 Qwen", "qwen", "https://dashscope.aliyuncs.com/compatible-mode/v1", "qwen3-vl-flash"},
    {"custom", "Custom", "custom", "", "gpt-4o"},
    {"kimi", "Kimi K3", "kimi", "https://api.moonshot.cn/v1", "kimi-k3"},
};
#endif

esp_err_t si_agent_api_validate_text(const char *value, size_t max_len, bool allow_empty)
{
    if (!value) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(value);
    if ((!allow_empty && len == 0) || len > max_len) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (ch < 32 || ch == 127) return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t si_agent_api_validate_endpoint(const char *value, bool allow_empty)
{
    esp_err_t ret = si_agent_api_validate_text(
        value, SI_AGENT_API_ENDPOINT_MAX_LEN, allow_empty);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!value[0] && allow_empty) {
        return ESP_OK;
    }
    if (strncasecmp(value, "https://", 8) != 0 ||
        strchr(value + 8, '@') != NULL || strchr(value + 8, '?') != NULL ||
        strchr(value + 8, '#') != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *authority = value + 8;
    const char *path = strchr(authority, '/');
    const char *authority_end = path ? path : value + strlen(value);
    if (authority == authority_end) {
        return ESP_ERR_INVALID_ARG;
    }
    for (const char *cursor = authority; *cursor; ++cursor) {
        if (isspace((unsigned char)*cursor) || *cursor == '\\') {
            return ESP_ERR_INVALID_ARG;
        }
    }

    const char *port = NULL;
    if (*authority == '[') {
        const char *closing = memchr(authority + 1, ']',
                                     (size_t)(authority_end - authority - 1));
        if (!closing || closing == authority + 1) {
            return ESP_ERR_INVALID_ARG;
        }
        if (closing + 1 < authority_end) {
            if (closing[1] != ':') {
                return ESP_ERR_INVALID_ARG;
            }
            port = closing + 2;
        }
    } else {
        const char *colon = memchr(authority, ':',
                                   (size_t)(authority_end - authority));
        if (colon) {
            if (memchr(colon + 1, ':',
                       (size_t)(authority_end - colon - 1))) {
                return ESP_ERR_INVALID_ARG;
            }
            if (colon == authority) {
                return ESP_ERR_INVALID_ARG;
            }
            port = colon + 1;
        }
    }
    if (port) {
        if (port == authority_end) {
            return ESP_ERR_INVALID_ARG;
        }
        unsigned port_value = 0;
        for (const char *cursor = port; cursor < authority_end; ++cursor) {
            if (!isdigit((unsigned char)*cursor)) {
                return ESP_ERR_INVALID_ARG;
            }
            unsigned digit = (unsigned)(*cursor - '0');
            if (port_value > (65535U - digit) / 10U) {
                return ESP_ERR_INVALID_ARG;
            }
            port_value = port_value * 10U + digit;
        }
        if (port_value == 0U) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

#ifndef SI_AGENT_API_VALIDATION_HOST_TEST
static int profile_slot(const char *id)
{
    if (!id || !id[0]) return -1;
    for (int i = 0; i < SI_AGENT_API_PROFILE_COUNT; ++i) {
        if (strcasecmp(id, s_defaults[i].id) == 0) return i;
    }
    return -1;
}

static int provider_slot(const char *provider)
{
    if (!provider || !provider[0]) return 0;
    for (int i = 0; i < SI_AGENT_API_PROFILE_COUNT; ++i) {
        if (strcasecmp(provider, s_defaults[i].provider) == 0) return i;
    }
    return 2;
}

static void set_default(int slot, si_agent_api_profile_t *profile)
{
    if (slot < 0 || slot >= SI_AGENT_API_PROFILE_COUNT) slot = 0;
    memset(profile, 0, sizeof(*profile));
    strlcpy(profile->id, s_defaults[slot].id, sizeof(profile->id));
    strlcpy(profile->label, s_defaults[slot].label, sizeof(profile->label));
    strlcpy(profile->provider, s_defaults[slot].provider, sizeof(profile->provider));
    strlcpy(profile->endpoint, s_defaults[slot].endpoint, sizeof(profile->endpoint));
    strlcpy(profile->model, s_defaults[slot].model, sizeof(profile->model));
}

static void profile_key(int slot, const char *field, char *out, size_t out_size)
{
    if (!out || out_size < 4) return;
    out[0] = 'p'; out[1] = (char)('0' + slot); out[2] = '_';
    strlcpy(out + 3, field, out_size - 3);
}

static bool secret_exists(si_settings_store_t *store, const char *key)
{
    bool configured = false;
    return si_secret_store_is_configured(store, key, &configured) == ESP_OK &&
           configured;
}

static bool load_slot(si_settings_store_t *store, int slot,
                      si_agent_api_profile_t *profile)
{
    set_default(slot, profile);
    bool found = false;
    char key[16];
    char endpoint[SI_AGENT_API_ENDPOINT_MAX_LEN + 1] = {0};

    profile_key(slot, "label", key, sizeof(key));
    if (si_settings_store_get_string(
            store, key, profile->label, sizeof(profile->label)) == ESP_OK &&
        si_agent_api_validate_text(profile->label, SI_AGENT_API_PROFILE_LABEL_MAX_LEN, false) == ESP_OK) found = true;
    else strlcpy(profile->label, s_defaults[slot].label, sizeof(profile->label));

    profile_key(slot, "provider", key, sizeof(key));
    if (si_settings_store_get_string(
            store, key, profile->provider, sizeof(profile->provider)) == ESP_OK &&
        si_agent_api_validate_text(profile->provider, SI_AGENT_API_PROVIDER_MAX_LEN, false) == ESP_OK) found = true;
    else strlcpy(profile->provider, s_defaults[slot].provider, sizeof(profile->provider));

    profile_key(slot, "endpoint", key, sizeof(key));
    if (si_settings_store_get_string(
            store, key, endpoint, sizeof(endpoint)) == ESP_OK) {
        if (si_agent_api_validate_endpoint(endpoint, false) == ESP_OK) {
            strlcpy(profile->endpoint, endpoint, sizeof(profile->endpoint));
        } else {
            profile->endpoint[0] = '\0';
        }
        found = true;
    }

    profile_key(slot, "model", key, sizeof(key));
    if (si_settings_store_get_string(
            store, key, profile->model, sizeof(profile->model)) == ESP_OK &&
        si_agent_api_validate_text(profile->model, SI_AGENT_API_MODEL_MAX_LEN, false) == ESP_OK) found = true;
    else strlcpy(profile->model, s_defaults[slot].model, sizeof(profile->model));

    profile_key(slot, "api_key", key, sizeof(key));
    profile->api_key_configured = secret_exists(store, key);
    return found || profile->api_key_configured;
}

static void apply_legacy(si_settings_store_t *store,
                         si_agent_api_profile_t *profile)
{
    char provider[SI_AGENT_API_PROVIDER_MAX_LEN + 1] = {0};
    char endpoint[SI_AGENT_API_ENDPOINT_MAX_LEN + 1] = {0};
    char model[SI_AGENT_API_MODEL_MAX_LEN + 1] = {0};
    if (si_settings_store_get_string(
            store, LEGACY_PROVIDER_KEY, provider, sizeof(provider)) == ESP_OK &&
        si_agent_api_validate_text(provider, SI_AGENT_API_PROVIDER_MAX_LEN, false) == ESP_OK) {
        set_default(provider_slot(provider), profile);
        strlcpy(profile->provider, provider, sizeof(profile->provider));
    }
    if (si_settings_store_get_string(
            store, LEGACY_ENDPOINT_KEY, endpoint, sizeof(endpoint)) == ESP_OK) {
        if (si_agent_api_validate_endpoint(endpoint, false) == ESP_OK) {
            strlcpy(profile->endpoint, endpoint, sizeof(profile->endpoint));
        } else {
            profile->endpoint[0] = '\0';
        }
    }
    if (si_settings_store_get_string(
            store, LEGACY_MODEL_KEY, model, sizeof(model)) == ESP_OK &&
        si_agent_api_validate_text(model, SI_AGENT_API_MODEL_MAX_LEN, false) == ESP_OK)
        strlcpy(profile->model, model, sizeof(profile->model));
    profile->api_key_configured = secret_exists(store, LEGACY_SECRET_KEY);
}

size_t si_agent_api_profile_count(void) { return SI_AGENT_API_PROFILE_COUNT; }
const char *si_agent_api_profile_id_at(size_t index)
{
    return index < SI_AGENT_API_PROFILE_COUNT ? s_defaults[index].id : NULL;
}
bool si_agent_api_profile_id_valid(const char *id) { return profile_slot(id) >= 0; }

esp_err_t si_agent_api_get_profile(const char *requested, si_agent_api_profile_t *profile)
{
    if (!profile) return ESP_ERR_INVALID_ARG;
    char active[SI_AGENT_API_PROFILE_ID_MAX_LEN + 1] = "deepseek";
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, NAMESPACE);
    bool store_open = ret == ESP_OK;
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        return ret;
    }
    if (!requested || !requested[0]) {
        if (store_open) {
            if (si_settings_store_get_string(
                    &store, ACTIVE_KEY, active, sizeof(active)) != ESP_OK ||
                profile_slot(active) < 0) {
                strlcpy(active, "deepseek", sizeof(active));
            }
        }
        requested = active;
    }
    int slot = profile_slot(requested);
    if (slot < 0) {
        si_settings_store_close(&store);
        return ESP_ERR_INVALID_ARG;
    }
    set_default(slot, profile);
    if (store_open) {
        bool found = load_slot(&store, slot, profile);
        if (!found && strcmp(requested, active) == 0) {
            apply_legacy(&store, profile);
        }
    }
    si_settings_store_close(&store);
    return ESP_OK;
}

esp_err_t si_agent_api_save_profile(const char *id, const char *label,
                                    const char *provider, const char *endpoint,
                                    const char *model, const char *api_key,
                                    bool key_present, bool clear_key)
{
    int slot = profile_slot(id);
    if (slot < 0) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(si_agent_api_validate_text(label, SI_AGENT_API_PROFILE_LABEL_MAX_LEN, false), TAG, "label");
    ESP_RETURN_ON_ERROR(si_agent_api_validate_text(provider, SI_AGENT_API_PROVIDER_MAX_LEN, false), TAG, "provider");
    ESP_RETURN_ON_ERROR(si_agent_api_validate_endpoint(endpoint, false), TAG,
                        "endpoint");
    ESP_RETURN_ON_ERROR(si_agent_api_validate_text(model, SI_AGENT_API_MODEL_MAX_LEN, false), TAG, "model");
    if (key_present && !clear_key)
        ESP_RETURN_ON_ERROR(si_agent_api_validate_text(api_key, SI_AGENT_API_SECRET_MAX_LEN, false), TAG, "api key");
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(
        si_settings_store_open_write(&store, NAMESPACE), TAG,
        "open settings store");
    char key[16];
    esp_err_t ret = si_settings_store_set_string(
        &store, ACTIVE_KEY, s_defaults[slot].id);
#define SET_FIELD(field, value) do {                                         \
        profile_key(slot, field, key, sizeof(key));                          \
        if (ret == ESP_OK) {                                                 \
            ret = si_settings_store_set_string(&store, key, value);          \
        }                                                                    \
    } while (0)
    SET_FIELD("label", label); SET_FIELD("provider", provider);
    SET_FIELD("endpoint", endpoint); SET_FIELD("model", model);
#undef SET_FIELD
    profile_key(slot, "api_key", key, sizeof(key));
    if (ret == ESP_OK && clear_key) {
        ret = si_secret_store_erase(&store, key);
    } else if (ret == ESP_OK && key_present) {
        ret = si_secret_store_set_string(&store, key, api_key);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_string(
            &store, LEGACY_PROVIDER_KEY, provider);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_string(
            &store, LEGACY_ENDPOINT_KEY, endpoint);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_string(&store, LEGACY_MODEL_KEY, model);
    }
    if (ret == ESP_OK && clear_key) {
        ret = si_secret_store_erase(&store, LEGACY_SECRET_KEY);
    } else if (ret == ESP_OK && key_present) {
        ret = si_secret_store_set_string(
            &store, LEGACY_SECRET_KEY, api_key);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}

esp_err_t si_agent_api_get_secret(const char *requested, char *out, size_t out_size)
{
    if (!out || !out_size) return ESP_ERR_INVALID_ARG;
    out[0] = 0;
    si_agent_api_profile_t profile;
    ESP_RETURN_ON_ERROR(si_agent_api_get_profile(requested, &profile), TAG, "profile");
    int slot = profile_slot(profile.id);
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(
        si_settings_store_open_read(&store, NAMESPACE), TAG,
        "open settings store");
    char key[16]; profile_key(slot, "api_key", key, sizeof(key));
    esp_err_t ret = si_secret_store_get_string(&store, key, out, out_size);
    if (ret != ESP_OK && slot == 0) {
        ret = si_secret_store_get_string(
            &store, LEGACY_SECRET_KEY, out, out_size);
    }
    si_settings_store_close(&store);
    return ret;
}
#endif
