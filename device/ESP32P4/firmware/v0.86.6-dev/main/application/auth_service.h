#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "authorization.h"
#include "esp_err.h"

#define SI_AUTH_USERNAME_MAX_LEN 32
#define SI_AUTH_PASSWORD_MAX_LEN 64
#define SI_AUTH_PASSWORD_MIN_LEN 6
#define SI_AUTH_BOOTSTRAP_PASSWORD_LEN 6
#define SI_AUTH_TOKEN_LEN 64
#define SI_AUTH_SESSION_ID_MAX_LEN 20
#define SI_AUTH_DISABLED_SESSION_GENERATION 1U

typedef struct {
    bool authenticated;
    si_principal_kind_t principal;
    si_capability_set_t capabilities;
    uint32_t generation;
    char session_id[SI_AUTH_SESSION_ID_MAX_LEN + 1];
} si_auth_session_context_t;

typedef struct {
    bool enabled;
    bool using_default;
    bool legacy_password;
    uint32_t login_count;
    char username[SI_AUTH_USERNAME_MAX_LEN + 1];
} si_auth_status_t;

typedef struct {
    uint32_t browser_active;
    uint32_t mcp_active;
    uint32_t restored;
    uint32_t evictions;
    uint32_t expirations;
    uint32_t validation_misses;
    uint32_t lock_timeouts;
} si_auth_runtime_status_t;

const char *si_auth_default_username(void);
esp_err_t si_auth_initialize(void);
esp_err_t si_auth_reset_bootstrap(char *bootstrap_password_out,
                                  size_t out_size);
bool si_auth_is_enabled(void);
bool si_auth_credentials_match(const char *username, const char *password);
bool si_auth_token_matches(const char *token);
bool si_auth_token_get_context(const char *token, bool touch,
                               si_auth_session_context_t *context);
bool si_auth_session_get_live_context(
    const char *session_id, uint32_t expected_generation,
    si_auth_session_context_t *context);
esp_err_t si_auth_create_session(char out_token[SI_AUTH_TOKEN_LEN + 1]);
esp_err_t si_auth_create_session_for_client(
    const char *client, char out_token[SI_AUTH_TOKEN_LEN + 1]);
bool si_auth_token_is_mcp(const char *token);
void si_auth_revoke_session(const char *token);
void si_auth_revoke_all_sessions(void);
uint32_t si_auth_login_retry_after_ms(void);
void si_auth_record_login_failure(void);
void si_auth_clear_login_failures(void);
uint32_t si_auth_login_count(void);
void si_auth_record_login(void);

esp_err_t si_auth_validate_username(const char *username);
esp_err_t si_auth_validate_password(const char *password);
esp_err_t si_auth_set_credentials(const char *username, const char *password);

void si_auth_get_status(si_auth_status_t *status);
void si_auth_get_runtime_status(si_auth_runtime_status_t *status);
