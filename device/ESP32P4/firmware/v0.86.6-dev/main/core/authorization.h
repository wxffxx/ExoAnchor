#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Transport-independent authorization decision.
 *
 * Ownership: callers provide an immutable request snapshot and receive one
 * deterministic decision. This module owns no session, policy, or lease state.
 * Concurrency: pure functions; no locks or tasks.
 * Error semantics: invalid or incomplete requests fail closed.
 */

typedef uint32_t si_capability_set_t;

typedef enum {
    SI_CAPABILITY_NONE = 0U,
    SI_CAPABILITY_OBSERVE = 1U << 0,
    SI_CAPABILITY_SSH = 1U << 1,
    SI_CAPABILITY_UART = 1U << 2,
    SI_CAPABILITY_HID = 1U << 3,
    SI_CAPABILITY_POWER = 1U << 4,
    SI_CAPABILITY_SETTINGS = 1U << 5,
    SI_CAPABILITY_OTA = 1U << 6,
    SI_CAPABILITY_AGENT_RUN = 1U << 7,
    SI_CAPABILITY_CONTROL_LEASE = 1U << 8,
    SI_CAPABILITY_STORAGE = 1U << 9,
    SI_CAPABILITY_VIDEO_CONTROL = 1U << 10,
} si_capability_t;

#define SI_CAPABILITIES_WEB_DEFAULT                                      \
    ((si_capability_set_t)(SI_CAPABILITY_OBSERVE | SI_CAPABILITY_SSH |   \
                           SI_CAPABILITY_UART | SI_CAPABILITY_HID |       \
                           SI_CAPABILITY_POWER | SI_CAPABILITY_SETTINGS | \
                           SI_CAPABILITY_OTA | SI_CAPABILITY_AGENT_RUN |  \
                           SI_CAPABILITY_CONTROL_LEASE |                  \
                           SI_CAPABILITY_STORAGE |                        \
                           SI_CAPABILITY_VIDEO_CONTROL))

#define SI_CAPABILITIES_MCP_DEFAULT                                    \
    ((si_capability_set_t)(SI_CAPABILITY_OBSERVE | SI_CAPABILITY_SSH | \
                           SI_CAPABILITY_UART | SI_CAPABILITY_HID |     \
                           SI_CAPABILITY_CONTROL_LEASE |                \
                           SI_CAPABILITY_VIDEO_CONTROL))

typedef enum {
    SI_PRINCIPAL_ANONYMOUS = 0,
    SI_PRINCIPAL_BROWSER,
    SI_PRINCIPAL_MCP,
    SI_PRINCIPAL_AGENT,
    SI_PRINCIPAL_SYSTEM,
} si_principal_kind_t;

typedef enum {
    SI_AUTHZ_MODE_OBSERVE = 0,
    SI_AUTHZ_MODE_SUPERVISED,
    SI_AUTHZ_MODE_AUTONOMOUS,
} si_authorization_mode_t;

typedef enum {
    SI_AUTHZ_RISK_LOW = 0,
    SI_AUTHZ_RISK_MEDIUM,
    SI_AUTHZ_RISK_HIGH,
    SI_AUTHZ_RISK_CRITICAL,
} si_authorization_risk_t;

typedef enum {
    SI_AUTHZ_ALLOW = 0,
    SI_AUTHZ_DENY_INVALID_REQUEST,
    SI_AUTHZ_DENY_UNAUTHENTICATED,
    SI_AUTHZ_DENY_CAPABILITY,
    SI_AUTHZ_DENY_POLICY,
    SI_AUTHZ_DENY_KVM_PREEMPTED,
    SI_AUTHZ_DENY_LEASE,
    SI_AUTHZ_DENY_OBSERVE_ONLY,
    SI_AUTHZ_DENY_APPROVAL_REQUIRED,
} si_authorization_decision_t;

typedef struct {
    bool authenticated;
    si_principal_kind_t principal;
    si_capability_set_t capabilities;
    si_capability_t required_capability;
    bool policy_allowed;
    bool lease_required;
    bool lease_held;
    bool human_kvm_active;
    si_authorization_mode_t mode;
    si_authorization_risk_t risk;
    bool approval_required;
    bool approved;
} si_authorization_request_t;

si_authorization_decision_t
si_authorization_evaluate(const si_authorization_request_t *request);

bool si_capability_set_has(si_capability_set_t set,
                           si_capability_t capability);
const char *si_capability_name(si_capability_t capability);
const char *si_principal_kind_name(si_principal_kind_t principal);
const char *si_authorization_decision_name(
    si_authorization_decision_t decision);
