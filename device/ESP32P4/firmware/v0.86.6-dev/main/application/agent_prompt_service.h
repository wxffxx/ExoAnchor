#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define SI_AGENT_PROMPT_MAX_LEN 2048

typedef struct {
    char system_prompt[SI_AGENT_PROMPT_MAX_LEN + 1];
    bool using_default;
} si_agent_prompt_settings_t;

// Owns validation and NVS persistence for the firmware Agent system prompt.
esp_err_t si_agent_prompt_get(si_agent_prompt_settings_t *settings);
esp_err_t si_agent_prompt_set(const char *prompt);
esp_err_t si_agent_prompt_reset(void);
const char *si_agent_prompt_default(void);
