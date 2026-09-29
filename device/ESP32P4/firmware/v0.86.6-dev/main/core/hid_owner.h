#pragma once

/*
 * Transport-neutral HID report ownership.
 *
 * A token is minted by the HID manager and is intentionally not derivable
 * from a client supplied stream/session id.  The generation is the release
 * capability: only the exact current token may emit or neutralize reports.
 */

#include <stdbool.h>
#include <stdint.h>

#define SI_HID_OWNER_SESSION_ID_MAX_LEN 20

typedef bool (*si_hid_live_guard_fn)(void *context);

typedef enum {
    SI_HID_OWNER_NONE = 0,
    SI_HID_OWNER_CONTROL_LEASE,
    SI_HID_OWNER_KVM_STREAM,
} si_hid_owner_kind_t;

typedef struct {
    si_hid_owner_kind_t kind;
    char session_id[SI_HID_OWNER_SESSION_ID_MAX_LEN + 1];
    uint32_t auth_generation;
    uint32_t resource_id;
    uint32_t authority_epoch;
} si_hid_owner_claim_t;

typedef struct {
    si_hid_owner_claim_t claim;
    uint32_t generation;
} si_hid_owner_token_t;

typedef struct {
    si_hid_owner_token_t current;
    si_hid_owner_token_t report_owner;
    uint32_t next_generation;
    uint8_t dirty_reports;
    bool neutral_pending;
} si_hid_owner_state_t;

enum {
    SI_HID_REPORT_DIRTY_KEYBOARD = 1U << 0,
    SI_HID_REPORT_DIRTY_MOUSE = 1U << 1,
    SI_HID_REPORT_DIRTY_ABS_POINTER = 1U << 2,
};

typedef enum {
    SI_HID_EMBEDDED_TOUCH_DENY = 0,
    SI_HID_EMBEDDED_TOUCH_NEW,
    SI_HID_EMBEDDED_TOUCH_RENEW,
} si_hid_embedded_touch_decision_t;

#define SI_HID_OWNER_STATE_INITIALIZER {0}

bool si_hid_owner_claim_valid(const si_hid_owner_claim_t *claim);
bool si_hid_owner_claim_equal(const si_hid_owner_claim_t *left,
                              const si_hid_owner_claim_t *right);
bool si_hid_owner_token_equal(const si_hid_owner_token_t *left,
                              const si_hid_owner_token_t *right);
bool si_hid_owner_state_is_current(const si_hid_owner_state_t *state,
                                   const si_hid_owner_token_t *token);
bool si_hid_owner_state_install(si_hid_owner_state_t *state,
                                const si_hid_owner_claim_t *claim,
                                bool replace,
                                si_hid_owner_token_t *token);
bool si_hid_owner_state_begin_report(si_hid_owner_state_t *state,
                                     const si_hid_owner_token_t *token);
bool si_hid_owner_state_revoke_current(si_hid_owner_state_t *state,
                                       const si_hid_owner_token_t *expected,
                                       si_hid_owner_token_t *revoked);
bool si_hid_owner_state_revoke_session(si_hid_owner_state_t *state,
                                       const char *session_id,
                                       uint32_t auth_generation,
                                       si_hid_owner_token_t *revoked);
void si_hid_owner_state_clear_report(si_hid_owner_state_t *state,
                                     const si_hid_owner_token_t *expected);
void si_hid_owner_state_note_report(si_hid_owner_state_t *state,
                                    uint8_t report_mask, bool neutral);
void si_hid_owner_state_host_reset(si_hid_owner_state_t *state);
si_hid_embedded_touch_decision_t si_hid_embedded_touch_decide(
    bool lease_active, const si_hid_owner_token_t *current,
    const si_hid_owner_token_t *expected, uint32_t lease_resource_id,
    uint32_t lease_epoch);
bool si_hid_embedded_release_matches(
    bool lease_active, const si_hid_owner_token_t *expected,
    uint32_t lease_resource_id, uint32_t lease_epoch);
