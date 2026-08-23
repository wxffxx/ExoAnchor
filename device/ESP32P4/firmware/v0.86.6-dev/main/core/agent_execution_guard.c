#include "agent_execution_guard.h"

#include <stddef.h>
#include <string.h>

static bool bounded_nonempty(const char *value, size_t limit)
{
    if (!value || !value[0]) {
        return false;
    }
    for (size_t index = 0; index <= limit; ++index) {
        if (value[index] == '\0') {
            return true;
        }
    }
    return false;
}

static si_agent_execution_guard_result_t result(
    si_agent_execution_decision_t decision,
    si_authorization_decision_t authorization,
    si_capability_set_t effective,
    const char *reason)
{
    return (si_agent_execution_guard_result_t){
        .decision = decision,
        .authorization_decision = authorization,
        .effective_capabilities = effective,
        .reason = reason,
    };
}

si_agent_execution_guard_result_t si_agent_execution_guard_evaluate(
    const si_agent_execution_intent_t *intent)
{
    if (!intent || intent->contract_version != 1U ||
        !bounded_nonempty(intent->thread_id, SI_AGENT_EXECUTION_ID_MAX_LEN) ||
        !bounded_nonempty(intent->turn_id, SI_AGENT_EXECUTION_ID_MAX_LEN) ||
        !bounded_nonempty(intent->run_id, SI_AGENT_EXECUTION_ID_MAX_LEN) ||
        !bounded_nonempty(intent->step_id, SI_AGENT_EXECUTION_ID_MAX_LEN) ||
        !bounded_nonempty(intent->action_id, SI_AGENT_EXECUTION_ID_MAX_LEN) ||
        !bounded_nonempty(intent->idempotency_key,
                          SI_AGENT_EXECUTION_ID_MAX_LEN) ||
        !bounded_nonempty(intent->tool_name,
                          SI_AGENT_EXECUTION_TOOL_MAX_LEN) ||
        !bounded_nonempty(intent->normalized_args_hash,
                          SI_AGENT_EXECUTION_HASH_MAX_LEN) ||
        !bounded_nonempty(intent->expected_target_identity,
                          SI_AGENT_EXECUTION_TARGET_MAX_LEN) ||
        !bounded_nonempty(intent->current_target_identity,
                          SI_AGENT_EXECUTION_TARGET_MAX_LEN) ||
        intent->now_ms == 0U || intent->expires_at_ms == 0U ||
        intent->expected_policy_revision == 0U ||
        intent->current_policy_revision == 0U ||
        intent->expected_auth_generation == 0U ||
        intent->current_auth_generation == 0U) {
        return result(SI_AGENT_EXECUTION_DENY_INVALID,
                      SI_AUTHZ_DENY_INVALID_REQUEST, 0U,
                      "invalid execution intent");
    }

    si_capability_set_t effective =
        intent->principal_capabilities & intent->origin_authority_ceiling;
    if (intent->now_ms >= intent->expires_at_ms) {
        return result(SI_AGENT_EXECUTION_DENY_EXPIRED,
                      SI_AUTHZ_DENY_INVALID_REQUEST, effective,
                      "execution intent expired");
    }
    if (strcmp(intent->expected_target_identity,
               intent->current_target_identity) != 0) {
        return result(SI_AGENT_EXECUTION_DENY_TARGET_MISMATCH,
                      SI_AUTHZ_DENY_INVALID_REQUEST, effective,
                      "target identity changed");
    }
    if (intent->expected_auth_generation !=
        intent->current_auth_generation) {
        return result(SI_AGENT_EXECUTION_DENY_AUTH_CHANGED,
                      SI_AUTHZ_DENY_UNAUTHENTICATED, effective,
                      "source authorization generation changed");
    }
    if (intent->expected_policy_revision != intent->current_policy_revision) {
        return result(SI_AGENT_EXECUTION_DENY_POLICY_CHANGED,
                      SI_AUTHZ_DENY_POLICY, effective,
                      "policy revision changed");
    }
    if (intent->lease_required &&
        (intent->expected_lease_epoch == 0U ||
         intent->current_lease_epoch == 0U ||
         intent->expected_lease_epoch != intent->current_lease_epoch)) {
        return result(SI_AGENT_EXECUTION_DENY_LEASE_CHANGED,
                      SI_AUTHZ_DENY_LEASE, effective,
                      "control lease epoch changed");
    }
    if (intent->approval_required && !intent->grant_consumed) {
        return result(SI_AGENT_EXECUTION_DENY_GRANT,
                      SI_AUTHZ_DENY_APPROVAL_REQUIRED, effective,
                      "single-use grant not consumed");
    }

    si_authorization_request_t request = {
        .authenticated = intent->authenticated,
        .principal = intent->principal,
        .capabilities = effective,
        .required_capability = intent->required_capability,
        .policy_allowed = intent->policy_allowed,
        .lease_required = intent->lease_required,
        .lease_held = intent->lease_held,
        .human_kvm_active = intent->human_kvm_active,
        .mode = intent->mode,
        .risk = intent->risk,
        .approval_required = intent->approval_required,
        .approved = !intent->approval_required || intent->grant_consumed,
    };
    si_authorization_decision_t authorization =
        si_authorization_evaluate(&request);
    if (authorization != SI_AUTHZ_ALLOW) {
        return result(SI_AGENT_EXECUTION_DENY_AUTHORIZATION,
                      authorization, effective,
                      si_authorization_decision_name(authorization));
    }
    return result(SI_AGENT_EXECUTION_ALLOW, authorization, effective,
                  "allow");
}

const char *si_agent_execution_decision_name(
    si_agent_execution_decision_t decision)
{
    switch (decision) {
    case SI_AGENT_EXECUTION_ALLOW:
        return "allow";
    case SI_AGENT_EXECUTION_DENY_INVALID:
        return "invalid";
    case SI_AGENT_EXECUTION_DENY_EXPIRED:
        return "expired";
    case SI_AGENT_EXECUTION_DENY_TARGET_MISMATCH:
        return "target_mismatch";
    case SI_AGENT_EXECUTION_DENY_AUTH_CHANGED:
        return "auth_changed";
    case SI_AGENT_EXECUTION_DENY_POLICY_CHANGED:
        return "policy_changed";
    case SI_AGENT_EXECUTION_DENY_LEASE_CHANGED:
        return "lease_changed";
    case SI_AGENT_EXECUTION_DENY_GRANT:
        return "grant_required";
    case SI_AGENT_EXECUTION_DENY_AUTHORIZATION:
    default:
        return "authorization_denied";
    }
}
