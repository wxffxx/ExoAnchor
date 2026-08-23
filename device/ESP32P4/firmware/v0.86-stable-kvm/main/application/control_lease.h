#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SI_CONTROL_LEASE_GRACE_MS 10000U
#define SI_CONTROL_LEASE_MODE_MAX_LEN 16
#define SI_CONTROL_LEASE_OWNER_MAX_LEN 16
#define SI_CONTROL_LEASE_REASON_MAX_LEN 96

typedef enum {
    SI_CONTROL_LEASE_UPDATE_OK = 0,
    SI_CONTROL_LEASE_UPDATE_INVALID,
    SI_CONTROL_LEASE_UPDATE_KVM_ACTIVE,
    SI_CONTROL_LEASE_UPDATE_HELD_BY_OTHER,
    SI_CONTROL_LEASE_UPDATE_UNAVAILABLE,
} si_control_lease_update_result_t;

typedef struct {
    bool active;
    bool kvm_active;
    bool agent_active;
    bool agent_takeover;
    bool can_request;
    uint32_t kvm_remaining_ms;
    uint32_t agent_remaining_ms;
    uint32_t expires_in_ms;
    char owner[SI_CONTROL_LEASE_OWNER_MAX_LEN + 1];
    char mode[SI_CONTROL_LEASE_MODE_MAX_LEN + 1];
    char agent_owner[SI_CONTROL_LEASE_OWNER_MAX_LEN + 1];
    char reason[SI_CONTROL_LEASE_REASON_MAX_LEN + 1];
} si_control_lease_status_t;

esp_err_t si_control_lease_start(void);
bool si_control_lease_mode_valid(const char *mode);
bool si_control_lease_owner_valid(const char *owner);

esp_err_t si_control_lease_touch_agent(const char *mode, const char *reason);
bool si_control_lease_agent_active(void);
bool si_control_lease_agent_allows_actions(void);
bool si_control_lease_owner_matches(const char *owner);

si_control_lease_update_result_t si_control_lease_update(const char *owner,
                                                         const char *mode,
                                                         const char *reason,
                                                         bool active,
                                                         bool force);

void si_control_lease_get_status(si_control_lease_status_t *status);
