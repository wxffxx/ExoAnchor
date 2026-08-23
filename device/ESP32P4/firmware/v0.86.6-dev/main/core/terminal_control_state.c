#include "terminal_control_state.h"

#include <stddef.h>
#include <string.h>

static bool channel_valid(si_terminal_channel_t channel)
{
    return channel >= SI_TERMINAL_CHANNEL_UART &&
           channel < SI_TERMINAL_CHANNEL_COUNT;
}

static void copy_string(char *out, size_t out_size, const char *value)
{
    if (!out || out_size == 0) return;
    if (!value) value = "";
    size_t len = strlen(value);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, value, len);
    out[len] = '\0';
}

void si_terminal_control_state_init(si_terminal_control_state_t *state)
{
    if (state) memset(state, 0, sizeof(*state));
}

bool si_terminal_control_state_begin(
    si_terminal_control_state_t *state, si_terminal_channel_t channel,
    si_terminal_actor_t actor, const char *operation_id, const char *run_id,
    const char *label, uint32_t started_ms,
    si_terminal_control_token_t *token)
{
    if (!state || !token || !channel_valid(channel) ||
        (actor != SI_TERMINAL_ACTOR_AGENT &&
         actor != SI_TERMINAL_ACTOR_MCP)) return false;
    si_terminal_control_status_t *slot = &state->channels[channel];
    if (slot->active) return false;
    uint32_t generation = slot->generation + 1U;
    if (generation == 0U) generation = 1U;
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->generation = generation;
    slot->started_ms = started_ms;
    slot->actor = actor;
    copy_string(slot->operation_id, sizeof(slot->operation_id), operation_id);
    copy_string(slot->run_id, sizeof(slot->run_id), run_id);
    copy_string(slot->label, sizeof(slot->label), label);
    token->channel = channel;
    token->generation = generation;
    return true;
}

bool si_terminal_control_state_request_cancel(
    si_terminal_control_state_t *state, si_terminal_channel_t channel,
    uint32_t expected_generation)
{
    if (!state || !channel_valid(channel) || expected_generation == 0U)
        return false;
    si_terminal_control_status_t *slot = &state->channels[channel];
    if (!slot->active || slot->generation != expected_generation) return false;
    slot->cancel_requested = true;
    return true;
}

bool si_terminal_control_state_cancel_requested(
    const si_terminal_control_state_t *state,
    const si_terminal_control_token_t *token)
{
    if (!state || !token || !channel_valid(token->channel) ||
        token->generation == 0U) return true;
    const si_terminal_control_status_t *slot =
        &state->channels[token->channel];
    return !slot->active || slot->generation != token->generation ||
           slot->cancel_requested;
}

bool si_terminal_control_state_finish(
    si_terminal_control_state_t *state,
    const si_terminal_control_token_t *token)
{
    if (!state || !token || !channel_valid(token->channel) ||
        token->generation == 0U) return false;
    si_terminal_control_status_t *slot = &state->channels[token->channel];
    if (!slot->active || slot->generation != token->generation) return false;
    uint32_t generation = slot->generation;
    memset(slot, 0, sizeof(*slot));
    slot->generation = generation;
    return true;
}

bool si_terminal_control_state_manual_input_allowed(
    const si_terminal_control_state_t *state,
    si_terminal_channel_t channel)
{
    return state && channel_valid(channel) &&
           !state->channels[channel].active;
}

bool si_terminal_control_state_get(
    const si_terminal_control_state_t *state, si_terminal_channel_t channel,
    si_terminal_control_status_t *status)
{
    if (!state || !status || !channel_valid(channel)) return false;
    *status = state->channels[channel];
    return true;
}
