#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SI_AUTH_USERNAME_MAX_LEN 32
#define SI_AUTH_PASSWORD_MAX_LEN 64
#define SI_AUTH_PASSWORD_MIN_LEN 6
#define SI_AUTH_TOKEN_LEN 64

typedef struct {
    bool enabled;
    bool using_default;
    uint32_t login_count;
    char username[SI_AUTH_USERNAME_MAX_LEN + 1];
    char token[SI_AUTH_TOKEN_LEN + 1];
} si_auth_status_t;

const char *si_auth_default_username(void);
bool si_auth_is_enabled(void);
bool si_auth_credentials_match(const char *username, const char *password);
bool si_auth_token_matches(const char *token);
uint32_t si_auth_login_count(void);
void si_auth_record_login(void);

esp_err_t si_auth_validate_username(const char *username);
esp_err_t si_auth_validate_password(const char *password);
esp_err_t si_auth_set_credentials(const char *username, const char *password);

void si_auth_get_status(si_auth_status_t *status);
