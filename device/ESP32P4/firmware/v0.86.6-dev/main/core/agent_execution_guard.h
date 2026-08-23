#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "authorization.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SI_AGENT_EXECUTION_ID_MAX_LEN 64U
#define SI_AGENT_EXECUTION_TOOL_MAX_LEN 48U
#define SI_AGENT_EXECUTION_TARGET_MAX_LEN 96U
#define SI_AGENT_EXECUTION_HASH_MAX_LEN 64U

typedef enum {
    SI_AGENT_EXECUTION_ALLOW = 0,
    SI_AGENT_EXECUTION_DENY_INVALID,
    SI_AGENT_EXECUTION_DENY_EXPIRED,
    SI_AGENT_EXECUTION_DENY_TARGET_MISMATCH,
    SI_AGENT_EXECUTION_DENY_AUTH_CHANGED,
    SI_AGENT_EXECUTION_DENY_POLICY_CHANGED,
    SI_AGENT_EXECUTION_DENY_LEASE_CHANGED,
    SI_AGENT_EXECUTION_DENY_GRANT,
    SI_AGENT_EXECUTION_DENY_AUTHORIZATION,
} si_agent_execution_decision_t;

/*
 * Immutable snapshot consumed immediately before an external side effect.
 * Every identity/authority value is supplied by trusted adapters/services,
 * never copied from model arguments.
 */
typedef struct {
    uint32_t contract_version;
    const char *thread_id;
    const char *turn_id;
    const char *run_id;
    const char *step_id;
    const char *action_id;
    const char *idempotency_key;
    const char *tool_name;
    const char *normalized_args_hash;
    const char *expected_target_identity;
    const char *current_target_identity;

    uint64_t now_ms;
    uint64_t expires_at_ms;
    uint32_t expected_policy_revision;
    uint32_t current_policy_revision;
    uint32_t expected_lease_epoch;
    uint32_t current_lease_epoch;
    uint32_t expected_auth_generation;
    uint32_t current_auth_generation;

    bool authenticated;
    si_principal_kind_t principal;
    si_capability_set_t principal_capabilities;
    si_capability_set_t origin_authority_ceiling;
    si_capability_t required_capability;
    bool policy_allowed;
    bool lease_required;
    bool lease_held;
    bool human_kvm_active;
    si_authorization_mode_t mode;
    si_authorization_risk_t risk;
    bool approval_required;
    bool grant_consumed;
} si_agent_execution_intent_t;

typedef struct {
    si_agent_execution_decision_t decision;
    si_authorization_decision_t authorization_decision;
    si_capability_set_t effective_capabilities;
    const char *reason;
} si_agent_execution_guard_result_t;

si_agent_execution_guard_result_t si_agent_execution_guard_evaluate(
    const si_agent_execution_intent_t *intent);

const char *si_agent_execution_decision_name(
    si_agent_execution_decision_t decision);

#ifdef __cplusplus
}
#endif
