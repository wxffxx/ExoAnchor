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
    bool setup_required;
    uint32_t login_count;
    char username[SI_AUTH_USERNAME_MAX_LEN + 1];
} si_auth_status_t;

const char *si_auth_default_username(void);
esp_err_t si_auth_initialize(void);
esp_err_t si_auth_create_session(char out_token[SI_AUTH_TOKEN_LEN + 1]);
esp_err_t si_auth_create_session_for_client(const char *client, char out_token[SI_AUTH_TOKEN_LEN + 1]);
void si_auth_revoke_session(const char *token);
bool si_auth_token_is_recent(const char *token);
uint32_t si_auth_login_retry_after_ms(void);
void si_auth_record_login_failure(void);
void si_auth_clear_login_failures(void);
bool si_auth_is_enabled(void);
bool si_auth_credentials_match(const char *username, const char *password);
bool si_auth_token_matches(const char *token);
uint32_t si_auth_login_count(void);
void si_auth_record_login(void);

esp_err_t si_auth_validate_username(const char *username);
esp_err_t si_auth_validate_password(const char *password);
esp_err_t si_auth_set_credentials(const char *username, const char *password);

void si_auth_get_status(si_auth_status_t *status);

uint32_t si_auth_credential_generation(void);
esp_err_t si_auth_create_session_for_generation(const char *client, uint32_t expected_generation, char out_token[SI_AUTH_TOKEN_LEN + 1]);

bool si_auth_setup_required(void);
esp_err_t si_auth_setup_credentials(const char *username, const char *password, uint32_t expected_generation);
