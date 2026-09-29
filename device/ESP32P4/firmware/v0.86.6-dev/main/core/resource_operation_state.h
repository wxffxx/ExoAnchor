#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SI_RESOURCE_OPERATION_ID_MAX_LEN 48
#define SI_RESOURCE_OPERATION_RUN_ID_MAX_LEN 64
#define SI_RESOURCE_OPERATION_LABEL_MAX_LEN 96

typedef enum {
    SI_RESOURCE_OPERATION_POWER = 0,
    SI_RESOURCE_OPERATION_VIDEO_CONFIG,
    SI_RESOURCE_OPERATION_VIDEO_HARDWARE,
    SI_RESOURCE_OPERATION_MAINTENANCE,
    SI_RESOURCE_OPERATION_AGENT_DATA,
    SI_RESOURCE_OPERATION_COUNT,
} si_resource_operation_resource_t;

typedef enum {
    SI_RESOURCE_OPERATION_ACTOR_NONE = 0,
    SI_RESOURCE_OPERATION_ACTOR_AGENT,
    SI_RESOURCE_OPERATION_ACTOR_MCP,
    SI_RESOURCE_OPERATION_ACTOR_SYSTEM,
} si_resource_operation_actor_t;

typedef struct {
    si_resource_operation_resource_t resource;
    uint32_t generation;
} si_resource_operation_token_t;

typedef struct {
    bool active;
    bool cancel_requested;
    bool cancellable;
    uint32_t generation;
    uint32_t started_ms;
    si_resource_operation_actor_t actor;
    char operation_id[SI_RESOURCE_OPERATION_ID_MAX_LEN + 1];
    char run_id[SI_RESOURCE_OPERATION_RUN_ID_MAX_LEN + 1];
    char label[SI_RESOURCE_OPERATION_LABEL_MAX_LEN + 1];
} si_resource_operation_status_t;

typedef struct {
    si_resource_operation_status_t slots[SI_RESOURCE_OPERATION_COUNT];
} si_resource_operation_state_t;

void si_resource_operation_state_init(si_resource_operation_state_t *state);
bool si_resource_operation_state_begin(
    si_resource_operation_state_t *state,
    si_resource_operation_resource_t resource,
    si_resource_operation_actor_t actor, const char *operation_id,
    const char *run_id, const char *label, bool cancellable,
    uint32_t started_ms, si_resource_operation_token_t *token);
bool si_resource_operation_state_request_cancel(
    si_resource_operation_state_t *state,
    si_resource_operation_resource_t resource,
    uint32_t expected_generation);
bool si_resource_operation_state_cancel_requested(
    const si_resource_operation_state_t *state,
    const si_resource_operation_token_t *token);
bool si_resource_operation_state_finish(
    si_resource_operation_state_t *state,
    const si_resource_operation_token_t *token);
bool si_resource_operation_state_manual_allowed(
    const si_resource_operation_state_t *state,
    si_resource_operation_resource_t resource);
bool si_resource_operation_state_get(
    const si_resource_operation_state_t *state,
    si_resource_operation_resource_t resource,
    si_resource_operation_status_t *status);
size_t si_resource_operation_state_active_count(
    const si_resource_operation_state_t *state);
