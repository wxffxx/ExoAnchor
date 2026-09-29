#!/usr/bin/env python3
"""P0 contract and mock behavior for the exact Agent HTTP control plane."""

from __future__ import annotations

import importlib.util
import json
import threading
from http.server import ThreadingHTTPServer
from pathlib import Path
from urllib import error, request


ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[start : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


ENGINE = read("main/services/web/agent_run_engine_module.inc")
HTTP = read("main/services/web/agent_run_http_module.inc")
SNAPSHOT = read("main/services/web/agent_run_status_snapshot_module.inc")
STATUS = read("main/services/web/agent_status_v2_projection_module.inc")
TASK = read("main/services/web/agent_run_task_module.inc")
TYPES = read("main/services/web/agent_runtime_types_module.inc")
MOCK = read("tools/serve-ui.py")


# Exact status selection and snapshot copying are one atomic critical section.
snapshot = function_body(
    SNAPSHOT, "static agent_run_job_t *agent_run_status_snapshot_take("
)
if not (
    snapshot.index("strcmp(expected_run_id, s_agent_run_job.job_id)")
    < snapshot.index("memcpy(snapshot, &s_agent_run_job")
):
    raise AssertionError("exact run comparison must happen before snapshot copy")
for marker in (
    'query, "run_id", requested_run_id',
    "exact ? expected_run_id : NULL",
    'httpd_resp_set_status(req, "404 Not Found")',
    'cJSON_AddStringToObject(resp, "reason", "run_id_mismatch")',
):
    if marker not in ENGINE:
        raise AssertionError(f"exact status contract missing: {marker}")
for marker in (
    "candidate_active_turn && snapshot->job_id[0]",
    "strcmp(candidate_active_turn->run_attempt.run_attempt_id,",
    "snapshot->job_id) == 0 ? candidate_active_turn : NULL",
):
    if marker not in STATUS:
        raise AssertionError(f"cross-run Task projection guard missing: {marker}")


# Steer has no current-run wildcard and mismatch responses do not append the
# unrelated current Run projection.
steer = function_body(HTTP, "static esp_err_t agent_run_steer_handler(")
for marker in (
    '"exact run_id required"',
    "strcmp(expected_run, s_agent_run_job.job_id) == 0",
    'httpd_resp_set_status(req, "409 Conflict")',
    'cJSON_AddStringToObject(resp, "run_id", expected_run)',
):
    if marker not in steer:
        raise AssertionError(f"exact steer contract missing: {marker}")
if "!requested_run || !requested_run[0] ||\n            strcmp" in steer:
    raise AssertionError("steer current-run wildcard returned")


# A busy receipt is a rejection, never a fake queued submission. Successful
# receipts preserve the lock-time busy sample separately from the newer live
# status projection, which may already be terminal or no longer retained.
submit_http = function_body(HTTP, "static esp_err_t agent_run_handler(")
busy_branch = submit_http[
    submit_http.index("if (busy && !deduplicated)") :
    submit_http.index("cJSON_AddBoolToObject(resp, \"accepted\", accepted)")
]
for marker in (
    'httpd_resp_set_status(req, "409 Conflict")',
    'cJSON_AddBoolToObject(resp, "queued", false)',
    'cJSON_AddStringToObject(resp, "submitted_run_id", "")',
    'cJSON_AddStringToObject(resp, "active_run_id", submitted_run_id)',
):
    if marker not in busy_branch:
        raise AssertionError(f"busy receipt contract missing: {marker}")
normal_receipt = submit_http[
    submit_http.index('cJSON_AddBoolToObject(resp, "accepted", accepted)') :
]
for marker in (
    'cJSON_AddBoolToObject(resp, "receipt_busy", busy)',
    'cJSON_AddStringToObject(resp, "active_run_id", busy ? submitted_run_id : "")',
    "agent_run_add_status_json(resp, false, submitted_run_id,",
    'cJSON_AddBoolToObject(resp, "status_retained", false)',
    'if (!cJSON_GetObjectItemCaseSensitive(resp, "busy"))',
    'cJSON_AddBoolToObject(resp, "busy", busy)',
):
    if marker not in normal_receipt:
        raise AssertionError(f"stable receipt/live projection contract missing: {marker}")
if not (
    normal_receipt.index('cJSON_AddBoolToObject(resp, "receipt_busy", busy)')
    < normal_receipt.index("agent_run_add_status_json(resp, false, submitted_run_id,")
    < normal_receipt.index('if (!cJSON_GetObjectItemCaseSensitive(resp, "busy"))')
):
    raise AssertionError("receipt must precede live projection and busy fallback")
if 'httpd_resp_set_status(req, "202 Accepted")' not in normal_receipt:
    raise AssertionError("newly accepted Agent runs must return HTTP 202")


# Event cursors acknowledge only complete JSON items. The ring's available
# latest sequence is informational and must never skip an unserialized event.
event_json = function_body(HTTP, "static bool agent_run_add_event_json(")
for marker in (
    "if (!events || !event)",
    "if (!item)",
    "if (!complete || !cJSON_AddItemToArray(events, item))",
    "return false;",
    "return true;",
):
    if marker not in event_json:
        raise AssertionError(f"event serialization contract missing: {marker}")
event_http = function_body(HTTP, "static esp_err_t agent_run_events_handler(")
for marker in (
    "uint32_t available_latest_seq = 0;",
    'cJSON_AddNumberToObject(resp, "latest_seq", available_latest_seq)',
    "uint32_t next_after_seq = after_seq;",
    "if (!agent_run_add_event_json(items, &events[i]))",
    "truncated = true;",
    "next_after_seq = events[i].seq;",
    'cJSON_AddNumberToObject(resp, "next_after_seq", next_after_seq)',
    'cJSON_AddBoolToObject(resp, "truncated", truncated)',
    '"current_run_ram_ring_no_tf_scan"',
):
    if marker not in event_http:
        raise AssertionError(f"event cursor contract missing: {marker}")
if not (
    event_http.index("if (!agent_run_add_event_json(items, &events[i]))")
    < event_http.index("next_after_seq = events[i].seq;")
):
    raise AssertionError("event cursor advanced before complete serialization")


# Dedupe is exact over trusted source/thread/key and every semantic input;
# reusing a key with changed input is an explicit 409 conflict.
for marker in (
    "char idempotency_key[SI_AGENT_TASK_IDEMPOTENCY_MAX_LEN + 1U];",
    "char idempotency_fingerprint[65];",
):
    if marker not in TYPES:
        raise AssertionError(f"volatile idempotency binding missing: {marker}")
payload_match = function_body(
    TASK, "static bool agent_run_idempotency_fingerprint("
)
for marker in (
    '"message"',
    '"completion_criteria"',
    '"page_context_json"',
    '"profile"',
    '"model"',
    '"dry_run"',
    '"include_screenshot"',
    '"allow_web_search"',
    "agent_text_sha256(serialized, out)",
):
    if marker not in payload_match:
        raise AssertionError(f"idempotency payload dimension missing: {marker}")
for marker in (
    'strlcpy(err, "idempotency_conflict", err_size)',
    "if (deduplicated_out) *deduplicated_out = true;",
    "SI_AGENT_TASK_IDEMPOTENCY_MAX_LEN + 1U",
):
    if marker not in TASK:
        raise AssertionError(f"legacy idempotency guard missing: {marker}")
for marker in (
    'strcmp(err, "idempotency_conflict") == 0',
    'cJSON_AddStringToObject(conflict, "reason",',
    '"idempotency_conflict")',
):
    if marker not in submit_http:
        raise AssertionError(f"HTTP idempotency conflict missing: {marker}")


# Keep the local UI mock executable as a behavioral oracle for clients.
for marker in (
    "agent_run_request_fingerprint",
    '"receipt_busy": receipt_busy',
    '"receipt_busy": False',
    '"status_retained": False',
    '"submitted_run_id": ""',
    '"active_run_id": retained_run_id',
    '"reason": "run_id_mismatch"',
    '"reason": "idempotency_conflict"',
    '"truncated": truncated',
    '"replay_scope": "current_run_ram_ring_no_tf_scan"',
):
    if marker not in MOCK:
        raise AssertionError(f"mock exact-control behavior missing: {marker}")


def load_mock():
    path = ROOT / "tools" / "serve-ui.py"
    spec = importlib.util.spec_from_file_location("exoanchor_serve_ui", path)
    if spec is None or spec.loader is None:
        raise AssertionError("cannot load local UI mock")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mock = load_mock()
server = ThreadingHTTPServer(("127.0.0.1", 0), mock.Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f"http://127.0.0.1:{server.server_port}"


def call(method: str, path: str, payload: dict | None = None) -> tuple[int, dict]:
    body = None if payload is None else json.dumps(payload).encode("utf-8")
    req = request.Request(
        base + path,
        data=body,
        method=method,
        headers={"Content-Type": "application/json"} if body else {},
    )
    try:
        with request.urlopen(req, timeout=3) as response:
            return response.status, json.loads(response.read().decode("utf-8"))
    except error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode("utf-8"))


try:
    first_request = {
        "session_id": "exact-http",
        "message": "inspect target",
        "authority_mode": "policy",
        "idempotency_key": "host-exact-1",
    }
    status, first = call("POST", "/api/agent/run", first_request)
    assert status == 202 and first["accepted"] is True
    run_id = first["submitted_run_id"]
    assert first["receipt_busy"] is False
    assert first["active_run_id"] == ""
    assert first["busy"] is True

    status, duplicate = call("POST", "/api/agent/run", first_request)
    assert status == 200
    assert duplicate["deduplicated"] is True
    assert duplicate["submitted_run_id"] == run_id
    assert duplicate["receipt_busy"] is True
    assert duplicate["active_run_id"] == run_id
    assert duplicate["busy"] is True
    assert mock.MockState.agent_run_seq == 1

    changed = dict(first_request, message="changed semantic input")
    status, conflict = call("POST", "/api/agent/run", changed)
    assert status == 409
    assert conflict["reason"] == "idempotency_conflict"
    assert conflict["submitted_run_id"] == ""

    other = dict(first_request, idempotency_key="host-exact-2")
    status, busy = call("POST", "/api/agent/run", other)
    assert status == 409
    assert busy["queued"] is False and busy["submitted_run_id"] == ""
    assert busy["active_run_id"] == run_id

    status, wrong = call(
        "GET", "/api/agent/run/status?run_id=not-the-current-run"
    )
    assert status == 404 and wrong["reason"] == "run_id_mismatch"
    for leaked in ("job_id", "goal", "thread_id", "result"):
        assert leaked not in wrong

    status, exact = call("GET", f"/api/agent/run/status?run_id={run_id}")
    assert status == 200 and exact["found"] is True
    assert exact["run_id"] == run_id

    status, missing = call(
        "POST", "/api/agent/run/steer", {"message": "continue"}
    )
    assert status == 400
    status, wrong_steer = call(
        "POST", "/api/agent/run/steer",
        {"run_id": "not-the-current-run", "message": "continue"},
    )
    assert status == 409 and wrong_steer["reason"] == "run_id_mismatch"
    assert "job_id" not in wrong_steer and "goal" not in wrong_steer
    status, steered = call(
        "POST", "/api/agent/run/steer",
        {"run_id": run_id, "message": "continue"},
    )
    assert status == 200 and steered["accepted"] is True

    # The receipt sees the active Run while the later projection completes it.
    mock.MockState.agent_run_started_at = (
        mock.time.monotonic() - mock.AGENT_RUN_DELAY_SECONDS - 0.1
    )
    status, completed_race = call("POST", "/api/agent/run", first_request)
    assert status == 200 and completed_race["deduplicated"] is True
    assert completed_race["receipt_busy"] is True
    assert completed_race["active_run_id"] == run_id
    assert completed_race["busy"] is False

    # A later terminal dedupe has a terminal receipt as well as terminal live
    # status, so it must not claim an active Run.
    status, terminal_duplicate = call("POST", "/api/agent/run", first_request)
    assert status == 200 and terminal_duplicate["deduplicated"] is True
    assert terminal_duplicate["receipt_busy"] is False
    assert terminal_duplicate["active_run_id"] == ""
    assert terminal_duplicate["busy"] is False

    replacement_request = dict(
        first_request,
        message="replacement race source",
        idempotency_key="host-exact-replace",
    )
    status, replacement_first = call(
        "POST", "/api/agent/run", replacement_request
    )
    assert status == 202
    replacement_run_id = replacement_first["submitted_run_id"]

    # Simulate another Run replacing the retained status between submit's
    # exact receipt and the live projection. Receipt fields survive; unrelated
    # status identifiers do not leak into the response.
    mock.MockState.agent_run_replace_before_projection = True
    status, replaced_projection = call(
        "POST", "/api/agent/run", replacement_request
    )
    assert status == 200 and replaced_projection["deduplicated"] is True
    assert replaced_projection["receipt_busy"] is True
    assert replaced_projection["busy"] is True
    assert replaced_projection["submitted_run_id"] == replacement_run_id
    assert replaced_projection["active_run_id"] == replacement_run_id
    assert replaced_projection["status_retained"] is False
    assert "run_id" not in replaced_projection
    assert "job_id" not in replaced_projection

    # A deliberately bounded mock page models event JSON allocation stopping
    # after one complete item. latest_seq remains informational while the
    # acknowledgement cursor advances only through that item.
    mock._append_agent_run_event("agent", "output_received", "second event")
    mock._append_agent_run_event("agent", "verifying", "third event")
    mock.MockState.agent_run_event_page_limit = 1
    status, event_page = call(
        "GET", f"/api/agent/run/events?job_id={replacement_run_id}&after_seq=0"
    )
    assert status == 200
    assert event_page["latest_seq"] == 3
    assert event_page["next_after_seq"] == 1
    assert event_page["truncated"] is True
    assert [item["seq"] for item in event_page["events"]] == [1]

    mock.MockState.agent_run_event_page_limit = None
    status, event_tail = call(
        "GET", f"/api/agent/run/events?job_id={replacement_run_id}&after_seq=1"
    )
    assert status == 200
    assert event_tail["latest_seq"] == 3
    assert event_tail["next_after_seq"] == 3
    assert event_tail["truncated"] is False
    assert [item["seq"] for item in event_tail["events"]] == [2, 3]

    status, beyond_tail = call(
        "GET", f"/api/agent/run/events?job_id={replacement_run_id}&after_seq=99"
    )
    assert status == 200
    assert beyond_tail["latest_seq"] == 3
    assert beyond_tail["next_after_seq"] == 99
    assert beyond_tail["events"] == []
finally:
    mock.MockState.agent_run_event_page_limit = None
    mock.MockState.agent_run_replace_before_projection = False
    server.shutdown()
    server.server_close()
    thread.join(timeout=2)


print("Agent HTTP exact control contract: PASS")
