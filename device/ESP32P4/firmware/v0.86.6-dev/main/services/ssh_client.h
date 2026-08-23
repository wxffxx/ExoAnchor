#pragma once

// SSH service public API.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SI_SSH_HOST_MAX_LEN 64
#define SI_SSH_USERNAME_MAX_LEN 64
#define SI_SSH_PASSWORD_MAX_LEN 128
#define SI_SSH_COMMAND_MAX_LEN 4096
#define SI_SSH_STDIN_MAX_LEN 256
#define SI_SSH_ERROR_MAX_LEN 160
#define SI_SSH_PUBLIC_KEY_MAX_LEN 1024
#define SI_SSH_PRIVATE_KEY_MAX_LEN 4096
#define SI_SSH_KEY_PASSPHRASE_MAX_LEN 128

typedef void (*si_ssh_exec_output_cb_t)(const char *data, size_t len, void *user_ctx);
typedef bool (*si_ssh_exec_cancel_cb_t)(void *user_ctx);

typedef struct {
    char host[SI_SSH_HOST_MAX_LEN];
    uint16_t port;
    char username[SI_SSH_USERNAME_MAX_LEN];
    char password[SI_SSH_PASSWORD_MAX_LEN];
    bool use_private_key;
    char public_key[SI_SSH_PUBLIC_KEY_MAX_LEN];
    char private_key[SI_SSH_PRIVATE_KEY_MAX_LEN];
    char key_passphrase[SI_SSH_KEY_PASSPHRASE_MAX_LEN];
    char command[SI_SSH_COMMAND_MAX_LEN];
    char stdin_data[SI_SSH_STDIN_MAX_LEN];
    bool close_stdin_after_write;
    uint32_t timeout_ms;
    si_ssh_exec_output_cb_t output_cb;
    void *output_user_ctx;
    si_ssh_exec_cancel_cb_t cancel_cb;
    void *cancel_user_ctx;
} si_ssh_exec_config_t;

typedef struct {
    bool ok;
    int exit_status;
    int ssh_rc;
    int64_t elapsed_ms;
    char error[SI_SSH_ERROR_MAX_LEN];
    char *output;
    size_t output_size;
    size_t output_len;
    bool truncated;
} si_ssh_exec_result_t;

esp_err_t si_ssh_exec(const si_ssh_exec_config_t *config, si_ssh_exec_result_t *result);

typedef struct si_ssh_shell si_ssh_shell_t;

typedef esp_err_t (*si_ssh_shell_output_cb_t)(const uint8_t *data, size_t len, void *user_ctx);

typedef struct {
    char host[SI_SSH_HOST_MAX_LEN];
    uint16_t port;
    char username[SI_SSH_USERNAME_MAX_LEN];
    char password[SI_SSH_PASSWORD_MAX_LEN];
    bool use_private_key;
    char public_key[SI_SSH_PUBLIC_KEY_MAX_LEN];
    char private_key[SI_SSH_PRIVATE_KEY_MAX_LEN];
    char key_passphrase[SI_SSH_KEY_PASSPHRASE_MAX_LEN];
    uint16_t cols;
    uint16_t rows;
    uint32_t timeout_ms;
} si_ssh_shell_config_t;

esp_err_t si_ssh_shell_start(const si_ssh_shell_config_t *config,
                             si_ssh_shell_output_cb_t output_cb,
                             void *user_ctx,
                             si_ssh_shell_t **out_shell);
esp_err_t si_ssh_shell_write(si_ssh_shell_t *shell, const uint8_t *data, size_t len,
                             uint32_t timeout_ms);
void si_ssh_shell_stop(si_ssh_shell_t *shell);
bool si_ssh_shell_is_running(si_ssh_shell_t *shell);
