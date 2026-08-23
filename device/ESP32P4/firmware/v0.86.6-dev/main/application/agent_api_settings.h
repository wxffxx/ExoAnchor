#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define SI_AGENT_API_PROFILE_COUNT 5
#define SI_AGENT_API_PROFILE_ID_MAX_LEN 16
#define SI_AGENT_API_PROFILE_LABEL_MAX_LEN 24
#define SI_AGENT_API_PROVIDER_MAX_LEN 16
#define SI_AGENT_API_ENDPOINT_MAX_LEN 160
#define SI_AGENT_API_MODEL_MAX_LEN 64
#define SI_AGENT_API_SECRET_MAX_LEN 256
#define SI_AGENT_API_MODEL_DEFAULT "deepseek-v4-pro"

typedef struct {
    char id[SI_AGENT_API_PROFILE_ID_MAX_LEN + 1];
    char label[SI_AGENT_API_PROFILE_LABEL_MAX_LEN + 1];
    char provider[SI_AGENT_API_PROVIDER_MAX_LEN + 1];
    char endpoint[SI_AGENT_API_ENDPOINT_MAX_LEN + 1];
    char model[SI_AGENT_API_MODEL_MAX_LEN + 1];
    bool api_key_configured;
} si_agent_api_profile_t;

size_t si_agent_api_profile_count(void);
const char *si_agent_api_profile_id_at(size_t index);
bool si_agent_api_profile_id_valid(const char *profile_id);
esp_err_t si_agent_api_validate_text(const char *value, size_t max_len, bool allow_empty);
esp_err_t si_agent_api_validate_endpoint(const char *value, bool allow_empty);
esp_err_t si_agent_api_get_profile(const char *profile_id, si_agent_api_profile_t *profile);
esp_err_t si_agent_api_save_profile(const char *profile_id, const char *label,
                                    const char *provider, const char *endpoint,
                                    const char *model, const char *api_key,
                                    bool api_key_present, bool clear_api_key);
esp_err_t si_agent_api_get_secret(const char *profile_id, char *out, size_t out_size);
