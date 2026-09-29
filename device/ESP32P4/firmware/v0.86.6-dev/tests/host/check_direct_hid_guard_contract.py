#!/usr/bin/env python3
"""Keep direct HID browser/MCP-only and continuously bound to its lease."""

from pathlib import Path
import sys
from typing import Optional


PROJECT_DIR = Path(__file__).resolve().parents[2]
DEVICE_HTTP = PROJECT_DIR / "main" / "services" / "device_http.c"
STATUS_WS = PROJECT_DIR / "main" / "services" / "status_ws.c"
AUTH_HEADER = PROJECT_DIR / "main" / "application" / "auth_service.h"
AUTH_SOURCE = PROJECT_DIR / "main" / "application" / "auth_service.c"
CONTROL_HEADER = PROJECT_DIR / "main" / "application" / "control_lease.h"
CONTROL_SOURCE = PROJECT_DIR / "main" / "application" / "control_lease.c"
HTTP_API = PROJECT_DIR / "main" / "adapters" / "http_api.c"
HID_HEADER = PROJECT_DIR / "main" / "drivers" / "hid_device.h"
HID_SOURCE = PROJECT_DIR / "main" / "drivers" / "hid_device.c"
HID_JSON = PROJECT_DIR / "main" / "adapters" / "hid_json.c"
VIDEO_CONTROL = PROJECT_DIR / "main" / "application" / "video_control.c"


def function_slice(
    source: str, signature: str, next_signature: Optional[str] = None
) -> str:
    start = source.find(signature)
    if start < 0:
        return ""
    if next_signature is None:
        return source[start:]
    end = source.find(next_signature, start + len(signature))
    return source[start:] if end < 0 else source[start:end]


def main() -> int:
    source = DEVICE_HTTP.read_text(encoding="utf-8")
    status_ws = STATUS_WS.read_text(encoding="utf-8")
    auth_header = AUTH_HEADER.read_text(encoding="utf-8")
    auth_source = AUTH_SOURCE.read_text(encoding="utf-8")
    control_header = CONTROL_HEADER.read_text(encoding="utf-8")
    control_source = CONTROL_SOURCE.read_text(encoding="utf-8")
    http_api = HTTP_API.read_text(encoding="utf-8")
    hid_header = HID_HEADER.read_text(encoding="utf-8")
    hid_source = HID_SOURCE.read_text(encoding="utf-8")
    hid_json = HID_JSON.read_text(encoding="utf-8")
    video_control = VIDEO_CONTROL.read_text(encoding="utf-8")
    handler = function_slice(source, "esp_err_t hid_actions_handler(")
    checkpoint = function_slice(
        source,
        "static bool hid_actions_control_checkpoint(",
        "static bool hid_actions_guarded_wait(",
    )
    guarded_wait = function_slice(
        source,
        "static bool hid_actions_guarded_wait(",
        "static void hid_actions_record_control_abort(",
    )
    failures: list[str] = []

    handler_markers = (
        "direct_hid_principal_allowed(session.principal)",
        "embedded Agent must use the Request Broker/Execution Gateway",
        ".risk = SI_AUTHZ_RISK_HIGH",
        "const uint32_t expected_lease_epoch = lease.epoch",
        "hid_actions_control_checkpoint(",
        "hid_actions_guarded_wait(",
        "hid_actions_record_control_abort(",
        "si_hid_json_execute_owned(",
    )
    for marker in handler_markers:
        if marker not in handler:
            failures.append(f"direct HID handler missing {marker}")
    if "SI_AUTHZ_RISK_MEDIUM" in handler:
        failures.append("direct HID handler still classifies HID as medium risk")
    principal_gate = function_slice(
        source,
        "static bool direct_hid_principal_allowed(",
        "static bool hid_actions_control_checkpoint(",
    )
    for marker in (
        "principal == SI_PRINCIPAL_BROWSER",
        "principal == SI_PRINCIPAL_MCP",
    ):
        if marker not in principal_gate:
            failures.append(f"direct HID principal gate missing {marker}")
    if "SI_PRINCIPAL_AGENT" in principal_gate:
        failures.append("embedded Agent may still bypass the execution gateway")

    checkpoint_markers = (
        "direct_hid_principal_allowed(session->principal)",
        "session->generation == 0U",
        "si_auth_session_get_live_context(",
        "session->session_id, session->generation",
        "direct_hid_principal_allowed(live_session.principal)",
        "live_session.principal != session->principal",
        "live_session.generation != session->generation",
        "si_capability_set_has(live_session.capabilities",
        "SI_CAPABILITY_HID",
        "lease.kvm_active",
        "lease.epoch != expected_lease_epoch",
        "lease.agent_active",
        "lease.agent_owner",
        "lease.session_id",
        "si_control_lease_owner_session_matches(",
        "SI_AUTHZ_MODE_OBSERVE",
    )
    for marker in checkpoint_markers:
        if marker not in checkpoint:
            failures.append(f"direct HID control checkpoint missing {marker}")

    wait_markers = (
        "while (remaining_ms > 0U)",
        "hid_actions_control_checkpoint(",
        "HID_ACTIONS_WAIT_SLICE_MS",
        "vTaskDelay(pdMS_TO_TICKS(slice_ms))",
    )
    for marker in wait_markers:
        if marker not in guarded_wait:
            failures.append(f"guarded HID wait missing {marker}")

    release_helper = function_slice(
        source,
        "static bool hid_actions_release_all(",
        "static void hid_actions_record_control_abort(",
    )
    for marker in (
        "expected_lease_epoch",
        "si_control_lease_get_hid_owner(",
        "si_hid_execute_owned(",
        "hid_actions_authority_guard",
    ):
        if marker not in release_helper:
            failures.append(f"direct HID fenced release missing {marker}")

    abort_helper = function_slice(
        source,
        "static void hid_actions_record_control_abort(",
        "esp_err_t control_lease_handler(",
    )
    for marker in ("hid_actions_release_all(", '"aborted"', '"abort_reason"'):
        if marker not in abort_helper:
            failures.append(f"direct HID abort path missing {marker}")

    for marker in (
        "uint32_t generation;",
        "bool si_auth_session_get_live_context(",
        "const char *session_id, uint32_t expected_generation",
    ):
        if marker not in auth_header:
            failures.append(f"auth liveness API contract missing {marker}")

    live_lookup = function_slice(
        auth_source,
        "bool si_auth_session_get_live_context(",
        "bool si_auth_token_matches(",
    )
    for marker in (
        "expected_generation == 0U",
        'strcmp(session_id, "auth-disabled")',
        "s_sessions[i].generation != expected_generation",
        "session_expired(&s_sessions[i]",
        "context->capabilities = s_sessions[i].capabilities",
        "context->generation = s_sessions[i].generation",
    ):
        if marker not in live_lookup:
            failures.append(f"live auth session lookup missing {marker}")
    if "last_seen_us = now_us" in live_lookup:
        failures.append("live auth checkpoint must not extend the idle timeout")

    revoke_one = function_slice(
        auth_source,
        "void si_auth_revoke_session(",
        "void si_auth_revoke_all_sessions(",
    )
    revoke_all = function_slice(
        auth_source,
        "void si_auth_revoke_all_sessions(",
        "uint32_t si_auth_login_retry_after_ms(",
    )
    for name, body in (("single", revoke_one), ("all", revoke_all)):
        if "revoked_sessions_apply(" not in body:
            failures.append(
                f"{name}-session auth revoke does not apply exact-generation revocation"
            )
        if "xSemaphoreTake(s_lock, portMAX_DELAY)" not in body:
            failures.append(
                f"{name}-session explicit revoke may return before acquiring the auth boundary"
            )

    session_clear = function_slice(
        auth_source,
        "static void session_clear_locked(",
        "static void revoked_sessions_apply(",
    )
    if "si_hid_abort_pending_commands(" in session_clear:
        failures.append(
            "clearing an unrelated auth session must not globally abort HID"
        )

    for marker in (
        "bool si_control_lease_revoke_auth_session(const char *session_id,",
    ):
        if marker not in control_header:
            failures.append(f"control lease revoke API missing {marker}")
    lease_revoke = function_slice(
        control_source, "bool si_control_lease_revoke_auth_session(",
        "si_control_lease_update_result_t si_control_lease_update_for_auth_session(",
    )
    for marker in (
        "si_hid_owner_revoke_session(",
        "LEASE_OP_REVOKE_SESSION",
        "context.state_changed",
    ):
        if marker not in lease_revoke:
            failures.append(f"control lease revoke path missing {marker}")

    for marker in (
        "si_hid_execute_owned(",
        "si_hid_authority_transition(",
        "si_hid_owner_release_if_current(",
        "si_hid_owner_revoke_session(",
        "si_hid_set_embedded_authority_guard(",
    ):
        if marker not in hid_header:
            failures.append(f"HID execution manager API missing {marker}")
    for marker in (
        "xSemaphoreCreateRecursiveMutex()",
        "xSemaphoreTakeRecursive(s_authority_gate",
        "si_hid_owner_state_is_current(&s_owner_state, owner)",
        "guard && !guard(guard_context)",
        "si_hid_abort_fence_snapshot(&s_abort_fence)",
        "si_hid_abort_fence_is_current(",
        "vTaskDelay(pdMS_TO_TICKS(60))",
        "neutralize_aborted_command_locked()",
        "release_all_locked();",
    ):
        if marker not in hid_source:
            failures.append(f"HID linearization/fence implementation missing {marker}")
    if "owner.claim.kind != SI_HID_OWNER_CONTROL_LEASE" not in hid_source:
        failures.append("ordinary HID execute bypasses the manager gate")
    for marker in (
        "si_hid_get_bound_embedded_owner",
        "embedded_owner &&",
        "s_embedded_authority_guard",
    ):
        if marker not in hid_source:
            failures.append(
                f"embedded HID execution lacks live lease admission: {marker}"
            )
    lease_update = function_slice(
        control_source,
        "si_control_lease_update_result_t si_control_lease_update_for_auth_session(",
        "void si_control_lease_get_status(",
    )
    if ".live_guard = live_guard" not in lease_update:
        failures.append("control lease update does not carry a live auth guard")
    lease_transition = function_slice(
        control_source, "static bool lease_transition_under_hid_gate(",
        "static bool run_transition(",
    )
    if "context->live_guard(context->guard_context)" not in lease_transition:
        failures.append(
            "control lease update does not revalidate auth under the HID authority gate"
        )
    if "si_hid_execute_owned(command, owner, guard, guard_context)" not in hid_json:
        failures.append("HID JSON adapter does not preserve exact owner admission")
    for marker in (
        "KVM_OP_ACQUIRE",
        "KVM_OP_EXPIRE",
        "si_hid_authority_transition(",
    ):
        if marker not in video_control:
            failures.append(f"manual KVM HID preemption missing {marker}")

    if "context->generation = SI_AUTH_DISABLED_SESSION_GENERATION;" not in http_api:
        failures.append("auth-disabled HTTP context lacks a stable generation")

    # Direct batch hardening must not replace or reroute the human KVM socket.
    for marker in (
        "si_control_lease_claim_kvm_hid_owner(",
        "esp_err_t hid_ws_handler(httpd_req_t *req)",
        "si_hid_json_execute_owned(",
        "si_auth_session_get_live_context(",
        "si_hid_owner_release_if_current(&ctx->owner)",
    ):
        if marker not in status_ws:
            failures.append(f"human KVM WebSocket contract missing {marker}")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print("Direct HID execution guard contract: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
