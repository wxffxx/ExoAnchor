#include "authorization.h"

static bool principal_valid(si_principal_kind_t principal)
{
    return principal >= SI_PRINCIPAL_ANONYMOUS &&
           principal <= SI_PRINCIPAL_SYSTEM;
}

static bool mode_valid(si_authorization_mode_t mode)
{
    return mode >= SI_AUTHZ_MODE_OBSERVE &&
           mode <= SI_AUTHZ_MODE_AUTONOMOUS;
}

static bool risk_valid(si_authorization_risk_t risk)
{
    return risk >= SI_AUTHZ_RISK_LOW && risk <= SI_AUTHZ_RISK_CRITICAL;
}

static bool capability_valid(si_capability_t capability)
{
    if (capability == SI_CAPABILITY_NONE) {
        return false;
    }
    uint32_t value = (uint32_t)capability;
    return (value & (value - 1U)) == 0U &&
           value <= (uint32_t)SI_CAPABILITY_VIDEO_CONTROL;
}

bool si_capability_set_has(si_capability_set_t set,
                           si_capability_t capability)
{
    return capability_valid(capability) &&
           (set & (si_capability_set_t)capability) != 0U;
}

si_authorization_decision_t
si_authorization_evaluate(const si_authorization_request_t *request)
{
    if (!request || !principal_valid(request->principal) ||
        !capability_valid(request->required_capability) ||
        !mode_valid(request->mode) || !risk_valid(request->risk)) {
        return SI_AUTHZ_DENY_INVALID_REQUEST;
    }
    if (!request->authenticated ||
        request->principal == SI_PRINCIPAL_ANONYMOUS) {
        return SI_AUTHZ_DENY_UNAUTHENTICATED;
    }
    if (!si_capability_set_has(request->capabilities,
                               request->required_capability)) {
        return SI_AUTHZ_DENY_CAPABILITY;
    }
    if (!request->policy_allowed) {
        return SI_AUTHZ_DENY_POLICY;
    }
    if (request->lease_required && request->human_kvm_active) {
        return SI_AUTHZ_DENY_KVM_PREEMPTED;
    }
    if (request->lease_required && !request->lease_held) {
        return SI_AUTHZ_DENY_LEASE;
    }
    if (request->mode == SI_AUTHZ_MODE_OBSERVE &&
        request->risk != SI_AUTHZ_RISK_LOW) {
        return SI_AUTHZ_DENY_OBSERVE_ONLY;
    }
    if (request->approval_required && !request->approved) {
        return SI_AUTHZ_DENY_APPROVAL_REQUIRED;
    }
    return SI_AUTHZ_ALLOW;
}

const char *si_capability_name(si_capability_t capability)
{
    switch (capability) {
    case SI_CAPABILITY_OBSERVE:
        return "observe";
    case SI_CAPABILITY_SSH:
        return "ssh";
    case SI_CAPABILITY_UART:
        return "uart";
    case SI_CAPABILITY_HID:
        return "hid";
    case SI_CAPABILITY_POWER:
        return "power";
    case SI_CAPABILITY_SETTINGS:
        return "settings";
    case SI_CAPABILITY_OTA:
        return "ota";
    case SI_CAPABILITY_AGENT_RUN:
        return "agent_run";
    case SI_CAPABILITY_CONTROL_LEASE:
        return "control_lease";
    case SI_CAPABILITY_STORAGE:
        return "storage";
    case SI_CAPABILITY_VIDEO_CONTROL:
        return "video_control";
    case SI_CAPABILITY_NONE:
    default:
        return "none";
    }
}

const char *si_principal_kind_name(si_principal_kind_t principal)
{
    switch (principal) {
    case SI_PRINCIPAL_BROWSER:
        return "browser";
    case SI_PRINCIPAL_MCP:
        return "mcp";
    case SI_PRINCIPAL_AGENT:
        return "agent";
    case SI_PRINCIPAL_SYSTEM:
        return "system";
    case SI_PRINCIPAL_ANONYMOUS:
    default:
        return "anonymous";
    }
}

const char *si_authorization_decision_name(
    si_authorization_decision_t decision)
{
    switch (decision) {
    case SI_AUTHZ_ALLOW:
        return "allow";
    case SI_AUTHZ_DENY_INVALID_REQUEST:
        return "invalid_request";
    case SI_AUTHZ_DENY_UNAUTHENTICATED:
        return "unauthenticated";
    case SI_AUTHZ_DENY_CAPABILITY:
        return "capability_missing";
    case SI_AUTHZ_DENY_POLICY:
        return "policy_denied";
    case SI_AUTHZ_DENY_KVM_PREEMPTED:
        return "kvm_preempted";
    case SI_AUTHZ_DENY_LEASE:
        return "lease_required";
    case SI_AUTHZ_DENY_OBSERVE_ONLY:
        return "observe_only";
    case SI_AUTHZ_DENY_APPROVAL_REQUIRED:
        return "approval_required";
    default:
        return "invalid_request";
    }
}
