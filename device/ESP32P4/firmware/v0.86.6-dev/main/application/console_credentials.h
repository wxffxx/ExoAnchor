#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define SI_CONSOLE_USERNAME_MAX_LEN 64
#define SI_CONSOLE_PASSWORD_MAX_LEN 128

typedef struct {
    bool configured;
    bool password_configured;
    char username[SI_CONSOLE_USERNAME_MAX_LEN];
} si_console_credentials_status_t;

/*
 * Target-host Console credentials are a local execution secret. Status reads
 * never return the password; callers that need the secret must use the
 * explicit local getter and clear their buffer immediately after use.
 */
esp_err_t si_console_credentials_get_status(
    si_console_credentials_status_t *status);
esp_err_t si_console_credentials_save(const char *username,
                                      const char *password,
                                      bool password_present,
                                      bool clear_password);
esp_err_t si_console_credentials_clear(void);
esp_err_t si_console_credentials_get_password(char *out, size_t out_size);
