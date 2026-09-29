#!/usr/bin/env python3
"""Static wiring contract for the unified HID report owner."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    owner = read("main/core/hid_owner.h") + read("main/core/hid_owner.c")
    driver = read("main/drivers/hid_device.c")
    lease = read("main/application/control_lease.c")
    auth = read("main/application/auth_service.c")
    video = read("main/application/video_control.c")
    status = read("main/services/status_ws.c")
    video_http = read("main/services/web/video_http_module.inc")
    boot = read("main/application/boot_key_manager.c")
    agent_engine = read("main/services/web/agent_run_engine_module.inc")
    agent_dispatch = read("main/services/web/agent_tool_dispatch_module.inc")
    agent_devices = read("main/services/web/agent_device_tools_module.inc")
    failures: list[str] = []

    contracts = {
        "owner state": (owner, (
            "SI_HID_OWNER_CONTROL_LEASE", "SI_HID_OWNER_KVM_STREAM",
            "auth_generation", "authority_epoch", "generation",
            "report_owner", "dirty_reports", "neutral_pending",
            "SI_HID_REPORT_DIRTY_KEYBOARD", "SI_HID_REPORT_DIRTY_MOUSE",
            "SI_HID_REPORT_DIRTY_ABS_POINTER",
            "si_hid_owner_state_note_report",
            "si_hid_owner_state_host_reset",
            "si_hid_owner_state_revoke_current",
            "si_hid_owner_state_revoke_session",
        )),
        "driver authority": (driver, (
            "si_hid_authority_transition", "owner_transition_locked",
            "si_hid_owner_token_equal(&s_owner_state.report_owner",
            "drain_neutral_pending_locked", "s_mount_reset_pending",
            "s_owner_state.dirty_reports & SI_HID_REPORT_DIRTY_KEYBOARD",
            "s_owner_state.dirty_reports & SI_HID_REPORT_DIRTY_MOUSE",
            "s_owner_state.dirty_reports & SI_HID_REPORT_DIRTY_ABS_POINTER",
            "si_hid_execute_owned", "si_hid_owner_state_is_current",
            "si_hid_owner_release_if_current",
            "si_hid_set_embedded_authority_guard",
            "si_hid_bind_embedded_producer",
            "si_hid_get_bound_embedded_owner",
        )),
        "lease transitions": (lease, (
            "LEASE_OP_RELEASE_EMBEDDED", "LEASE_OP_UPDATE",
            "LEASE_OP_REVOKE_SESSION", "LEASE_OP_MAINTAIN",
            "materialize_expiry_locked", "pending_revoke_add_under_hid_gate",
            "s_pending_revoke_fail_closed", "si_hid_owner_revoke_session",
            "LEASE_MAINTENANCE_MS",
            "embedded_lease_live_under_hid_gate",
            "context->live_guard(context->guard_context)",
            "si_hid_embedded_touch_decide(",
            "si_control_lease_release_embedded_owner(",
        )),
        "auth exact revoke": (auth, (
            "revoked->sessions[revoked->count].generation",
            "si_control_lease_revoke_auth_session(",
            "revoked_sessions_apply(&revoked)",
        )),
        "KVM transitions": (video, (
            "KVM_OP_ACQUIRE", "KVM_OP_RELEASE", "KVM_OP_WS_CLAIM",
            "KVM_OP_AGENT", "KVM_OP_EXPIRE", "kvm_epoch_bump_locked",
            "si_hid_authority_transition(",
            "strcmp(s_kvm_session_id, context->session_id) == 0",
        )),
        "WS exact owner": (status, (
            "uint32_t stream_owner_epoch;", "uint32_t auth_generation;",
            "char session_id[SI_AUTH_SESSION_ID_MAX_LEN + 1];",
            "si_hid_owner_token_t owner;",
            "si_auth_session_get_live_context(",
            "si_control_lease_kvm_hid_owner_is_current(",
            "si_hid_json_execute_owned(",
            "si_hid_owner_release_if_current(&ctx->owner)",
            "if (!stream_id_present)",
            "if (hid_ret == ESP_ERR_INVALID_STATE)",
        )),
        "KVM HTTP auth binding": (video_http, (
            "si_http_require_capability(req, SI_CAPABILITY_VIDEO_CONTROL, &session)",
            "si_video_control_acquire_kvm_stream_for_session(",
            '"stream_id is required for revocable KVM ownership"',
            '"stream_id is required for revocable KVM video"',
        )),
        "embedded exact producer": (
            boot + agent_engine + agent_dispatch + agent_devices, (
                "si_control_lease_touch_boot_sequence_owned(&hid_owner)",
                "si_control_lease_touch_agent_owned(",
                "agent_run_hid_owner_snapshot(",
                "si_hid_json_execute_owned(",
                "si_hid_execute_owned(",
                "si_control_lease_release_embedded_owner(",
            ),
        ),
    }
    for name, (source, markers) in contracts.items():
        for marker in markers:
            if marker not in source:
                failures.append(f"{name} missing {marker}")

    for forbidden in (
        "si_hid_revoke_and_release(",
        "si_hid_release_all_guarded(",
        "si_hid_abort_pending_commands(",
        "si_control_lease_invalidate_session_under_hid_gate(",
    ):
        if forbidden in "\n".join((driver, lease, auth, video, status, video_http)):
            failures.append(f"obsolete global HID authority path remains: {forbidden}")
    if "si_hid_json_execute(root)" in status:
        failures.append("KVM WS still has unowned HID execution")
    if "si_hid_release_all()" in status or "si_hid_release_all()" in video_http:
        failures.append("KVM transport still has a naked global release")
    if "si_video_control_touch_kvm(" in video or \
            "si_video_control_touch_kvm(" in video_http:
        failures.append("identityless video transport can renew KVM HID authority")
    acquire_start = video.find(
        "bool si_video_control_acquire_kvm_stream_for_session("
    )
    acquire_end = video.find(
        "bool si_video_control_claim_kvm_hid_owner(", acquire_start
    )
    acquire = video[acquire_start:acquire_end]
    for marker in (
        "const bool hid_handoff_ready = run_kvm_transition(&context, NULL);",
        "if (!hid_handoff_ready && context.accepted && claim)",
        "return context.accepted;",
    ):
        if marker not in acquire:
            failures.append(
                f"KVM video lease still depends on HID neutral readiness: {marker}"
            )
    hid_claim_start = video.find(
        "bool si_video_control_claim_kvm_hid_owner("
    )
    hid_claim_end = video.find(
        "bool si_video_control_kvm_hid_owner_is_current(", hid_claim_start
    )
    hid_claim = video[hid_claim_start:hid_claim_end]
    if "return run_kvm_transition(&context, owner) && context.accepted;" not in hid_claim:
        failures.append(
            "KVM HID WebSocket no longer fails closed when owner handoff is pending"
        )
    release_all = driver[driver.find("void si_hid_release_all(void)"):]
    if "si_hid_get_bound_embedded_owner(&owner)" not in release_all or \
            "si_hid_owner_get_current(&owner)" in release_all:
        failures.append(
            "Agent task epilogue release must use its producer binding, never current owner"
        )
    for name, source in (
        ("boot-key", boot), ("Agent engine", agent_engine),
        ("Agent dispatch", agent_dispatch), ("Agent device tools", agent_devices),
    ):
        for forbidden in ("si_hid_release_all();", "si_hid_execute(&",
                          "si_hid_json_execute(action)"):
            if forbidden in source:
                failures.append(
                    f"{name} retains ambient embedded HID authority: {forbidden}"
                )

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print("Unified HID owner contract: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
