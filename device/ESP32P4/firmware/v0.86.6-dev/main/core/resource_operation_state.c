#include "resource_operation_state.h"

#include <string.h>

static bool resource_valid(si_resource_operation_resource_t resource)
{
    return resource >= SI_RESOURCE_OPERATION_POWER &&
           resource < SI_RESOURCE_OPERATION_COUNT;
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

void si_resource_operation_state_init(si_resource_operation_state_t *state)
{
    if (state) memset(state, 0, sizeof(*state));
}

bool si_resource_operation_state_begin(
    si_resource_operation_state_t *state,
    si_resource_operation_resource_t resource,
    si_resource_operation_actor_t actor, const char *operation_id,
    const char *run_id, const char *label, bool cancellable,
    uint32_t started_ms, si_resource_operation_token_t *token)
{
    if (!state || !token || !resource_valid(resource) ||
        actor <= SI_RESOURCE_OPERATION_ACTOR_NONE ||
        actor > SI_RESOURCE_OPERATION_ACTOR_SYSTEM) return false;

    si_resource_operation_status_t *maintenance =
        &state->slots[SI_RESOURCE_OPERATION_MAINTENANCE];
    if ((resource != SI_RESOURCE_OPERATION_MAINTENANCE &&
         maintenance->active) ||
        (resource == SI_RESOURCE_OPERATION_MAINTENANCE &&
         si_resource_operation_state_active_count(state) > 0U)) return false;

    si_resource_operation_status_t *slot = &state->slots[resource];
    if (slot->active) return false;
    uint32_t generation = slot->generation + 1U;
    if (generation == 0U) generation = 1U;
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->cancellable = cancellable;
    slot->generation = generation;
    slot->started_ms = started_ms;
    slot->actor = actor;
    copy_string(slot->operation_id, sizeof(slot->operation_id), operation_id);
    copy_string(slot->run_id, sizeof(slot->run_id), run_id);
    copy_string(slot->label, sizeof(slot->label), label);
    token->resource = resource;
    token->generation = generation;
    return true;
}

bool si_resource_operation_state_request_cancel(
    si_resource_operation_state_t *state,
    si_resource_operation_resource_t resource,
    uint32_t expected_generation)
{
    if (!state || !resource_valid(resource) || expected_generation == 0U)
        return false;
    si_resource_operation_status_t *slot = &state->slots[resource];
    if (!slot->active || !slot->cancellable ||
        slot->generation != expected_generation) return false;
    slot->cancel_requested = true;
    return true;
}

bool si_resource_operation_state_cancel_requested(
    const si_resource_operation_state_t *state,
    const si_resource_operation_token_t *token)
{
    if (!state || !token || !resource_valid(token->resource) ||
        token->generation == 0U) return true;
    const si_resource_operation_status_t *slot =
        &state->slots[token->resource];
    return !slot->active || slot->generation != token->generation ||
           slot->cancel_requested;
}

bool si_resource_operation_state_finish(
    si_resource_operation_state_t *state,
    const si_resource_operation_token_t *token)
{
    if (!state || !token || !resource_valid(token->resource) ||
        token->generation == 0U) return false;
    si_resource_operation_status_t *slot = &state->slots[token->resource];
    if (!slot->active || slot->generation != token->generation) return false;
    uint32_t generation = slot->generation;
    memset(slot, 0, sizeof(*slot));
    slot->generation = generation;
    return true;
}

bool si_resource_operation_state_manual_allowed(
    const si_resource_operation_state_t *state,
    si_resource_operation_resource_t resource)
{
    return state && resource_valid(resource) &&
           !state->slots[resource].active &&
           (resource == SI_RESOURCE_OPERATION_MAINTENANCE ||
            !state->slots[SI_RESOURCE_OPERATION_MAINTENANCE].active);
}

bool si_resource_operation_state_get(
    const si_resource_operation_state_t *state,
    si_resource_operation_resource_t resource,
    si_resource_operation_status_t *status)
{
    if (!state || !status || !resource_valid(resource)) return false;
    *status = state->slots[resource];
    return true;
}

size_t si_resource_operation_state_active_count(
    const si_resource_operation_state_t *state)
{
    if (!state) return 0U;
    size_t count = 0U;
    for (size_t i = 0; i < SI_RESOURCE_OPERATION_COUNT; i++) {
        if (state->slots[i].active) count++;
    }
    return count;
}
