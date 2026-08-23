#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hid_owner.h"

#define SI_CONTROL_LEASE_GRACE_MS 10000U
#define SI_CONTROL_LEASE_MODE_MAX_LEN 16
#define SI_CONTROL_LEASE_OWNER_MAX_LEN 16
#define SI_CONTROL_LEASE_SESSION_ID_MAX_LEN 20
#define SI_CONTROL_LEASE_REASON_MAX_LEN 96

typedef enum {
    SI_CONTROL_LEASE_UPDATE_OK = 0,
    SI_CONTROL_LEASE_UPDATE_INVALID,
    SI_CONTROL_LEASE_UPDATE_HELD_BY_OTHER,
    SI_CONTROL_LEASE_UPDATE_UNAVAILABLE,
} si_control_lease_update_result_t;

typedef struct {
    uint32_t epoch;
    bool active;
    bool kvm_view_active;
    bool kvm_active;
    bool agent_active;
    bool input_control_active;
    bool agent_takeover;
    bool can_request;
    uint32_t kvm_remaining_ms;
    uint32_t agent_remaining_ms;
    uint32_t expires_in_ms;
    char owner[SI_CONTROL_LEASE_OWNER_MAX_LEN + 1];
    char mode[SI_CONTROL_LEASE_MODE_MAX_LEN + 1];
    char agent_owner[SI_CONTROL_LEASE_OWNER_MAX_LEN + 1];
    char session_id[SI_CONTROL_LEASE_SESSION_ID_MAX_LEN + 1];
    char reason[SI_CONTROL_LEASE_REASON_MAX_LEN + 1];
} si_control_lease_status_t;

esp_err_t si_control_lease_start(void);
bool si_control_lease_mode_valid(const char *mode);
bool si_control_lease_owner_valid(const char *owner);

esp_err_t si_control_lease_touch_agent_owned(
    const char *mode, const char *reason, si_hid_owner_token_t *owner);
esp_err_t si_control_lease_touch_boot_sequence_owned(
    si_hid_owner_token_t *owner);
bool si_control_lease_release_embedded_owner(
    const si_hid_owner_token_t *owner);
/* Transitional cleanup for the existing Agent task epilogue.  It resolves the
 * exact token bound to the calling producer task; it never fetches current. */
void si_control_lease_release_agent(void);
bool si_control_lease_claim_kvm_hid_owner(
    uint32_t stream_id, const char *session_id, uint32_t auth_generation,
    si_hid_live_guard_fn live_guard, void *guard_context,
    si_hid_owner_token_t *owner);
bool si_control_lease_kvm_hid_owner_is_current(
    uint32_t stream_id, uint32_t owner_epoch, const char *session_id,
    uint32_t auth_generation);
bool si_control_lease_agent_active(void);
bool si_control_lease_agent_allows_actions(void);
bool si_control_lease_owner_matches(const char *owner);
bool si_control_lease_owner_session_matches(const char *owner,
                                            const char *session_id);
bool si_control_lease_revoke_auth_session(const char *session_id,
                                          uint32_t auth_generation);
bool si_control_lease_get_hid_owner(const char *owner,
                                    const char *session_id,
                                    uint32_t auth_generation,
                                    uint32_t expected_epoch,
                                    si_hid_owner_token_t *token);

si_control_lease_update_result_t si_control_lease_update_for_auth_session(
    const char *owner, const char *session_id, uint32_t auth_generation,
    const char *mode, const char *reason, bool active, bool force,
    si_hid_live_guard_fn live_guard, void *guard_context);

void si_control_lease_get_status(si_control_lease_status_t *status);
