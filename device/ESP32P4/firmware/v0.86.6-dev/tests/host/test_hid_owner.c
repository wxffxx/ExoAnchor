#include <assert.h>
#include <stdio.h>

#include "hid_owner.h"

static si_hid_owner_claim_t claim(si_hid_owner_kind_t kind,
                                  const char *session_id,
                                  uint32_t auth_generation,
                                  uint32_t resource_id,
                                  uint32_t epoch)
{
    si_hid_owner_claim_t value = {
        .kind = kind,
        .auth_generation = auth_generation,
        .resource_id = resource_id,
        .authority_epoch = epoch,
    };
    snprintf(value.session_id, sizeof(value.session_id), "%s", session_id);
    return value;
}

int main(void)
{
    si_hid_owner_state_t state = SI_HID_OWNER_STATE_INITIALIZER;
    si_hid_owner_claim_t a_claim = claim(
        SI_HID_OWNER_KVM_STREAM, "browser-a", 7U, 41U, 2U);
    si_hid_owner_token_t a = {0};
    assert(si_hid_owner_state_install(&state, &a_claim, true, &a));
    assert(a.generation != 0U);
    assert(si_hid_owner_state_begin_report(&state, &a));

    si_hid_owner_token_t revoked = {0};
    assert(si_hid_owner_state_revoke_current(&state, &a, &revoked));
    assert(si_hid_owner_token_equal(&a, &revoked));
    si_hid_owner_state_clear_report(&state, &a);
    si_hid_owner_token_t b = {0};
    assert(si_hid_owner_state_install(&state, &a_claim, true, &b));
    assert(b.generation != a.generation);
    assert(si_hid_owner_state_begin_report(&state, &b));

    /* Same visible stream, but stale socket A cannot revoke/release B. */
    assert(!si_hid_owner_state_revoke_current(&state, &a, NULL));
    si_hid_owner_state_clear_report(&state, &a);
    assert(si_hid_owner_state_is_current(&state, &b));
    assert(si_hid_owner_token_equal(&state.report_owner, &b));

    /* Session-id reuse does not authorize an old auth generation. */
    assert(!si_hid_owner_state_revoke_session(
        &state, "browser-a", 6U, NULL));
    assert(si_hid_owner_state_revoke_session(
        &state, "browser-a", 7U, &revoked));
    si_hid_owner_state_clear_report(&state, &b);

    si_hid_owner_claim_t lease_claim = claim(
        SI_HID_OWNER_CONTROL_LEASE, "agent-a", 11U, 0U, 9U);
    si_hid_owner_token_t lease = {0};
    assert(si_hid_owner_state_install(&state, &lease_claim, false, &lease));
    state.neutral_pending = true;
    assert(!si_hid_owner_state_begin_report(&state, &lease));
    state.neutral_pending = false;
    assert(si_hid_owner_state_begin_report(&state, &lease));
    si_hid_owner_state_note_report(
        &state, SI_HID_REPORT_DIRTY_KEYBOARD, false);
    assert(state.dirty_reports == SI_HID_REPORT_DIRTY_KEYBOARD);
    si_hid_owner_state_note_report(
        &state, SI_HID_REPORT_DIRTY_MOUSE, false);
    assert(state.dirty_reports ==
           (SI_HID_REPORT_DIRTY_KEYBOARD | SI_HID_REPORT_DIRTY_MOUSE));
    si_hid_owner_state_note_report(
        &state, SI_HID_REPORT_DIRTY_KEYBOARD, true);
    assert(state.dirty_reports == SI_HID_REPORT_DIRTY_MOUSE);
    si_hid_owner_state_host_reset(&state);
    assert(state.dirty_reports == 0U);
    assert(!state.neutral_pending);
    assert(state.report_owner.generation == 0U);

    /* Expiry/revoke is exact-once. */
    assert(si_hid_owner_state_revoke_current(&state, &lease, &revoked));
    assert(!si_hid_owner_state_revoke_current(&state, &lease, NULL));
    si_hid_owner_state_clear_report(&state, &lease);

    /* Embedded Agent A and boot-key B are distinct producer capabilities.
     * B cannot interleave while A is live; after an explicit A transition B
     * may acquire, and A's delayed cleanup cannot match or release B. */
    si_hid_owner_claim_t agent_claim = claim(
        SI_HID_OWNER_CONTROL_LEASE, "embedded-agent", 0U, 101U, 20U);
    si_hid_owner_token_t agent = {0};
    assert(si_hid_owner_state_install(
        &state, &agent_claim, true, &agent));
    assert(si_hid_owner_state_begin_report(&state, &agent));
    assert(si_hid_embedded_touch_decide(
               true, &state.current, &agent, 101U, 20U) ==
           SI_HID_EMBEDDED_TOUCH_RENEW);
    assert(si_hid_embedded_touch_decide(
               true, &state.current, NULL, 101U, 20U) ==
           SI_HID_EMBEDDED_TOUCH_DENY);
    assert(si_hid_embedded_release_matches(true, &agent, 101U, 20U));
    assert(si_hid_owner_state_revoke_current(&state, &agent, NULL));
    si_hid_owner_state_clear_report(&state, &agent);

    si_hid_owner_claim_t boot_claim = claim(
        SI_HID_OWNER_CONTROL_LEASE, "embedded-agent", 0U, 102U, 21U);
    si_hid_owner_token_t boot = {0};
    assert(si_hid_owner_state_install(&state, &boot_claim, true, &boot));
    assert(si_hid_owner_state_begin_report(&state, &boot));
    assert(!si_hid_embedded_release_matches(true, &agent, 102U, 21U));
    assert(!si_hid_owner_state_revoke_current(&state, &agent, NULL));
    si_hid_owner_state_clear_report(&state, &agent);
    assert(si_hid_owner_state_is_current(&state, &boot));
    assert(si_hid_owner_token_equal(&state.report_owner, &boot));
    assert(si_hid_embedded_touch_decide(
               true, &state.current, &agent, 102U, 21U) ==
           SI_HID_EMBEDDED_TOUCH_DENY);

    puts("hid owner token tests: PASS");
    return 0;
}
