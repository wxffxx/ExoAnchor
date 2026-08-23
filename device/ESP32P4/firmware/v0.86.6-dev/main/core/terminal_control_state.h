#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SI_TERMINAL_CONTROL_OPERATION_ID_MAX_LEN 48
#define SI_TERMINAL_CONTROL_RUN_ID_MAX_LEN 64
#define SI_TERMINAL_CONTROL_LABEL_MAX_LEN 96

typedef enum {
    SI_TERMINAL_CHANNEL_UART = 0,
    SI_TERMINAL_CHANNEL_SSH,
    SI_TERMINAL_CHANNEL_COUNT,
} si_terminal_channel_t;

typedef enum {
    SI_TERMINAL_ACTOR_NONE = 0,
    SI_TERMINAL_ACTOR_AGENT,
    SI_TERMINAL_ACTOR_MCP,
} si_terminal_actor_t;

typedef struct {
    si_terminal_channel_t channel;
    uint32_t generation;
} si_terminal_control_token_t;

typedef struct {
    bool active;
    bool cancel_requested;
    uint32_t generation;
    uint32_t started_ms;
    si_terminal_actor_t actor;
    char operation_id[SI_TERMINAL_CONTROL_OPERATION_ID_MAX_LEN + 1];
    char run_id[SI_TERMINAL_CONTROL_RUN_ID_MAX_LEN + 1];
    char label[SI_TERMINAL_CONTROL_LABEL_MAX_LEN + 1];
} si_terminal_control_status_t;

typedef struct {
    si_terminal_control_status_t channels[SI_TERMINAL_CHANNEL_COUNT];
} si_terminal_control_state_t;

void si_terminal_control_state_init(si_terminal_control_state_t *state);
bool si_terminal_control_state_begin(
    si_terminal_control_state_t *state, si_terminal_channel_t channel,
    si_terminal_actor_t actor, const char *operation_id, const char *run_id,
    const char *label, uint32_t started_ms,
    si_terminal_control_token_t *token);
bool si_terminal_control_state_request_cancel(
    si_terminal_control_state_t *state, si_terminal_channel_t channel,
    uint32_t expected_generation);
bool si_terminal_control_state_cancel_requested(
    const si_terminal_control_state_t *state,
    const si_terminal_control_token_t *token);
bool si_terminal_control_state_finish(
    si_terminal_control_state_t *state,
    const si_terminal_control_token_t *token);
bool si_terminal_control_state_manual_input_allowed(
    const si_terminal_control_state_t *state,
    si_terminal_channel_t channel);
bool si_terminal_control_state_get(
    const si_terminal_control_state_t *state, si_terminal_channel_t channel,
    si_terminal_control_status_t *status);
