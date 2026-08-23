#include "hid_owner.h"

#include <string.h>

static void next_generation(si_hid_owner_state_t *state)
{
    if (++state->next_generation == 0U) {
        state->next_generation = 1U;
    }
}

bool si_hid_owner_claim_valid(const si_hid_owner_claim_t *claim)
{
    if (!claim || claim->kind <= SI_HID_OWNER_NONE ||
        claim->kind > SI_HID_OWNER_KVM_STREAM ||
        !claim->session_id[0] || claim->authority_epoch == 0U) {
        return false;
    }
    return claim->kind != SI_HID_OWNER_KVM_STREAM ||
           claim->resource_id != 0U;
}

bool si_hid_owner_claim_equal(const si_hid_owner_claim_t *left,
                              const si_hid_owner_claim_t *right)
{
    return left && right && left->kind == right->kind &&
           left->auth_generation == right->auth_generation &&
           left->resource_id == right->resource_id &&
           left->authority_epoch == right->authority_epoch &&
           strcmp(left->session_id, right->session_id) == 0;
}

bool si_hid_owner_token_equal(const si_hid_owner_token_t *left,
                              const si_hid_owner_token_t *right)
{
    return left && right && left->generation != 0U &&
           left->generation == right->generation &&
           si_hid_owner_claim_equal(&left->claim, &right->claim);
}

bool si_hid_owner_state_is_current(const si_hid_owner_state_t *state,
                                   const si_hid_owner_token_t *token)
{
    return state && token &&
           si_hid_owner_token_equal(&state->current, token);
}

bool si_hid_owner_state_install(si_hid_owner_state_t *state,
                                const si_hid_owner_claim_t *claim,
                                bool replace,
                                si_hid_owner_token_t *token)
{
    if (!state || !si_hid_owner_claim_valid(claim)) {
        return false;
    }
    if (!replace && state->current.generation != 0U &&
        si_hid_owner_claim_equal(&state->current.claim, claim)) {
        if (token) {
            *token = state->current;
        }
        return true;
    }
    next_generation(state);
    memset(&state->current, 0, sizeof(state->current));
    state->current.claim = *claim;
    state->current.generation = state->next_generation;
    if (token) {
        *token = state->current;
    }
    return true;
}

bool si_hid_owner_state_begin_report(si_hid_owner_state_t *state,
                                     const si_hid_owner_token_t *token)
{
    if (!si_hid_owner_state_is_current(state, token) ||
        state->neutral_pending) {
        return false;
    }
    state->report_owner = *token;
    return true;
}

bool si_hid_owner_state_revoke_current(si_hid_owner_state_t *state,
                                       const si_hid_owner_token_t *expected,
                                       si_hid_owner_token_t *revoked)
{
    if (!state || (expected &&
                   !si_hid_owner_state_is_current(state, expected)) ||
        state->current.generation == 0U) {
        return false;
    }
    if (revoked) {
        *revoked = state->current;
    }
    memset(&state->current, 0, sizeof(state->current));
    next_generation(state);
    return true;
}

bool si_hid_owner_state_revoke_session(si_hid_owner_state_t *state,
                                       const char *session_id,
                                       uint32_t auth_generation,
                                       si_hid_owner_token_t *revoked)
{
    if (!state || !session_id || !session_id[0] ||
        state->current.generation == 0U ||
        strcmp(state->current.claim.session_id, session_id) != 0 ||
        (auth_generation != 0U &&
         state->current.claim.auth_generation != auth_generation)) {
        return false;
    }
    return si_hid_owner_state_revoke_current(state, &state->current, revoked);
}

void si_hid_owner_state_clear_report(si_hid_owner_state_t *state,
                                     const si_hid_owner_token_t *expected)
{
    if (!state || !expected ||
        !si_hid_owner_token_equal(&state->report_owner, expected)) {
        return;
    }
    memset(&state->report_owner, 0, sizeof(state->report_owner));
}

void si_hid_owner_state_note_report(si_hid_owner_state_t *state,
                                    uint8_t report_mask, bool neutral)
{
    if (!state || report_mask == 0U ||
        state->report_owner.generation == 0U) {
        return;
    }
    if (neutral) {
        state->dirty_reports &= (uint8_t)~report_mask;
    } else {
        state->dirty_reports |= report_mask;
    }
}

void si_hid_owner_state_host_reset(si_hid_owner_state_t *state)
{
    if (!state) {
        return;
    }
    memset(&state->report_owner, 0, sizeof(state->report_owner));
    state->dirty_reports = 0U;
    state->neutral_pending = false;
}

static bool embedded_token_matches_domain(
    const si_hid_owner_token_t *token, uint32_t lease_resource_id,
    uint32_t lease_epoch)
{
    return token && token->generation != 0U &&
           token->claim.kind == SI_HID_OWNER_CONTROL_LEASE &&
           token->claim.auth_generation == 0U &&
           strcmp(token->claim.session_id, "embedded-agent") == 0 &&
           token->claim.resource_id == lease_resource_id &&
           token->claim.authority_epoch == lease_epoch;
}

si_hid_embedded_touch_decision_t si_hid_embedded_touch_decide(
    bool lease_active, const si_hid_owner_token_t *current,
    const si_hid_owner_token_t *expected, uint32_t lease_resource_id,
    uint32_t lease_epoch)
{
    if (lease_active) {
        return embedded_token_matches_domain(
                   expected, lease_resource_id, lease_epoch) &&
               si_hid_owner_token_equal(current, expected) ?
                   SI_HID_EMBEDDED_TOUCH_RENEW :
                   SI_HID_EMBEDDED_TOUCH_DENY;
    }
    /* A token that was already synchronously released cannot mint a successor.
     * A still-current token may cross the lazy TTL observation boundary; its
     * new claim will replace+neutralize it in the same authority transaction. */
    if (expected && expected->generation != 0U &&
        !si_hid_owner_token_equal(current, expected)) {
        return SI_HID_EMBEDDED_TOUCH_DENY;
    }
    return SI_HID_EMBEDDED_TOUCH_NEW;
}

bool si_hid_embedded_release_matches(
    bool lease_active, const si_hid_owner_token_t *expected,
    uint32_t lease_resource_id, uint32_t lease_epoch)
{
    return lease_active && embedded_token_matches_domain(
        expected, lease_resource_id, lease_epoch);
}
