#include "agent_execution_guard.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static si_agent_execution_intent_t allowed(void)
{
    return (si_agent_execution_intent_t){
        .contract_version = 1,
        .thread_id = "thread-1",
        .turn_id = "turn-1",
        .run_id = "run-1",
        .step_id = "step-1",
        .action_id = "action-1",
        .idempotency_key = "idem-1",
        .tool_name = "hid_actions",
        .normalized_args_hash =
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        .expected_target_identity = "board-a/host-a",
        .current_target_identity = "board-a/host-a",
        .now_ms = 100,
        .expires_at_ms = 200,
        .expected_policy_revision = 2,
        .current_policy_revision = 2,
        .expected_lease_epoch = 7,
        .current_lease_epoch = 7,
        .expected_auth_generation = 11,
        .current_auth_generation = 11,
        .authenticated = true,
        .principal = SI_PRINCIPAL_AGENT,
        .principal_capabilities = SI_CAPABILITY_HID | SI_CAPABILITY_OBSERVE,
        .origin_authority_ceiling = SI_CAPABILITY_HID | SI_CAPABILITY_OBSERVE,
        .required_capability = SI_CAPABILITY_HID,
        .policy_allowed = true,
        .lease_required = true,
        .lease_held = true,
        .human_kvm_active = false,
        .mode = SI_AUTHZ_MODE_SUPERVISED,
        .risk = SI_AUTHZ_RISK_HIGH,
        .approval_required = true,
        .grant_consumed = true,
    };
}

int main(void)
{
    si_agent_execution_intent_t intent = allowed();
    si_agent_execution_guard_result_t result =
        si_agent_execution_guard_evaluate(&intent);
    assert(result.decision == SI_AGENT_EXECUTION_ALLOW);

    intent.origin_authority_ceiling = SI_CAPABILITY_OBSERVE;
    result = si_agent_execution_guard_evaluate(&intent);
    assert(result.decision == SI_AGENT_EXECUTION_DENY_AUTHORIZATION);
    assert(result.authorization_decision == SI_AUTHZ_DENY_CAPABILITY);

    intent = allowed();
    intent.current_target_identity = "board-b/host-a";
    result = si_agent_execution_guard_evaluate(&intent);
    assert(result.decision == SI_AGENT_EXECUTION_DENY_TARGET_MISMATCH);

    intent = allowed();
    intent.now_ms = intent.expires_at_ms;
    result = si_agent_execution_guard_evaluate(&intent);
    assert(result.decision == SI_AGENT_EXECUTION_DENY_EXPIRED);

    intent = allowed();
    intent.current_auth_generation++;
    result = si_agent_execution_guard_evaluate(&intent);
    assert(result.decision == SI_AGENT_EXECUTION_DENY_AUTH_CHANGED);
    assert(result.authorization_decision == SI_AUTHZ_DENY_UNAUTHENTICATED);

    intent = allowed();
    intent.current_lease_epoch++;
    result = si_agent_execution_guard_evaluate(&intent);
    assert(result.decision == SI_AGENT_EXECUTION_DENY_LEASE_CHANGED);

    intent = allowed();
    intent.grant_consumed = false;
    result = si_agent_execution_guard_evaluate(&intent);
    assert(result.decision == SI_AGENT_EXECUTION_DENY_GRANT);

    intent = allowed();
    intent.human_kvm_active = true;
    result = si_agent_execution_guard_evaluate(&intent);
    assert(result.decision == SI_AGENT_EXECUTION_DENY_AUTHORIZATION);
    assert(result.authorization_decision == SI_AUTHZ_DENY_KVM_PREEMPTED);

    assert(strcmp(si_agent_execution_decision_name(
                      SI_AGENT_EXECUTION_DENY_TARGET_MISMATCH),
                  "target_mismatch") == 0);
    assert(strcmp(si_agent_execution_decision_name(
                      SI_AGENT_EXECUTION_DENY_AUTH_CHANGED),
                  "auth_changed") == 0);
    puts("agent execution guard tests: PASS");
    return 0;
}
