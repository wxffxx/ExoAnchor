#!/usr/bin/env python3
"""Fail if the retry candidate opens a Runtime V2 compatibility effect path."""

from pathlib import Path


PROJECT = Path(__file__).resolve().parents[2]
DISPATCH_PATH = PROJECT / "main/services/web/agent_tool_dispatch_module.inc"
BROKER_PATH = PROJECT / "main/services/web/agent_broker_http_module.inc"
DISPATCH = DISPATCH_PATH.read_text(encoding="utf-8")
BROKER = BROKER_PATH.read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    while start >= 0:
        brace = source.find("{", start)
        semicolon = source.find(";", start)
        if brace >= 0 and (semicolon < 0 or brace < semicolon):
            break
        start = source.find(signature, start + len(signature))
    if start < 0 or brace < 0:
        return ""
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    return ""


def require(haystack: str, marker: str, reason: str, failures: list[str]) -> None:
    if marker not in haystack:
        failures.append(reason)


failures: list[str] = []
allow = function_body(DISPATCH, "static bool agent_compat_dispatch_allowed(")
degraded = function_body(DISPATCH, "static bool agent_recovery_tool_allowed(")
tools = function_body(DISPATCH, "static void agent_execute_tool_calls(")
hid = function_body(DISPATCH, "static void agent_execute_actions(")
risk = function_body(DISPATCH, "static si_agent_request_risk_t agent_tool_request_risk(")
bound = function_body(DISPATCH, "static bool agent_run_task_service_is_bound(")

# Risk metadata alone is not authority.  The compatibility path must use an
# explicit read-only name/operation allowlist, with manager plan creation as
# the only state-creating exception while the durable Task Service is bound.
require(
    allow,
    "agent_compat_degraded_readonly(name, call, risk, lease_required)",
    "compatibility gate no longer delegates to the explicit read-only allowlist",
    failures,
)
for marker, reason in (
    ("risk != SI_AGENT_REQUEST_RISK_LOW || lease_required",
     "compatibility gate does not reject non-low-risk or leased calls"),
    ('strcmp(name, "host_display") != 0',
     "host-display manager plan is not explicitly named"),
    ('strcmp(name, "boot_key_sequence") != 0',
     "boot-key manager plan is not explicitly named"),
    ('strcmp(operation, "plan") == 0',
     "manager exception is not restricted to pure plan creation"),
):
    require(allow, marker, reason, failures)

require(
    degraded,
    "if (!name)",
    "read-only allowlist does not reject unnamed calls",
    failures,
)
for marker, reason in (
    ('strcmp(name, "observe_status") == 0', "observe_status read path missing"),
    ('strcmp(name, "uart_read") == 0', "uart_read path missing"),
    ('strcmp(stage, "status") == 0', "console status read path missing"),
):
    require(degraded, marker, reason, failures)
for forbidden in (
    'strcmp(name, "memory_search") == 0',
    'strcmp(name, "history_search") == 0',
    'strcmp(name, "memory_write") == 0',
    'strcmp(name, "uart_write") == 0',
    'strcmp(name, "uart_baud") == 0',
    'strcmp(name, "ssh_exec") == 0',
    'strcmp(name, "check_service") == 0',
    'strcmp(name, "verify_port") == 0',
    'strcmp(name, "host_display") == 0',
    'strcmp(name, "boot_key_sequence") == 0',
    'strcmp(operation, "plan") == 0',
    'strcmp(operation, "execute") == 0',
):
    if forbidden in degraded:
        failures.append(f"read-only allowlist contains unsafe entry: {forbidden}")

require(
    tools,
    "if (!dry_run && policy_name && !agent_tool_is_ask_user(call))",
    "real calls no longer keep ask_user as the sole approval-only exception",
    failures,
)
for marker, reason in (
    ("#if AGENT_RUNTIME_RECOVERY_RAM_ONLY",
     "ask_user recovery gate is not compile-time explicit"),
    ("agent_tool_is_ask_user(call)",
     "ask_user recovery gate is missing"),
    ("ask_user is unavailable in the RAM-only recovery candidate",
     "ask_user rejection is not stable/diagnosable"),
):
    require(tools, marker, reason, failures)
recovery_branch = bound[bound.find("#if AGENT_RUNTIME_RECOVERY_RAM_ONLY"):
                        bound.find("#else")]
if "return false;" not in recovery_branch:
    failures.append(
        "recovery binding probe may default into Broker/Task journal I/O"
    )
recovery_gate_pos = tools.find(
    "if (!agent_recovery_tool_allowed(policy_name, call))"
)
non_dry_gate_pos = tools.find(
    "if (!dry_run && policy_name && !agent_tool_is_ask_user(call))"
)
gate_pos = tools.find("if (!agent_compat_dispatch_allowed(")
broker_pos = tools.find("agent_request_authorize(", gate_pos + 1)
effect_pos = tools.find("} else if (agent_tool_is_ssh_exec(call))", broker_pos + 1)
if not (0 <= recovery_gate_pos < non_dry_gate_pos < gate_pos < broker_pos < effect_pos):
    failures.append("shadow cutover gate must precede Broker authorization and tool effect dispatch")
require(
    tools,
    "mutation execution is disabled until the target-bound manager gateway and typed readback are available",
    "stable shadow-cutover rejection is missing",
    failures,
)

# Freeze the write classifications which make the narrow predicate meaningful.
for marker, reason in (
    ('strcmp(name, "uart_write") == 0', "uart_write risk classification missing"),
    ('strcmp(name, "uart_baud") == 0', "uart_baud risk classification missing"),
    ('strcmp(name, "memory_write") == 0', "memory_write risk classification missing"),
    ('strcmp(name, "ssh_exec") == 0', "ssh execution risk classification missing"),
    ('*lease_required = true;', "lease-requiring Console/HID classification missing"),
    ('strcmp(operation, "plan") == 0', "pure manager plan exception missing"),
    ('return SI_AGENT_REQUEST_RISK_HIGH;', "high-risk denial classification missing"),
    ('return SI_AGENT_REQUEST_RISK_MEDIUM;', "medium-risk default denial missing"),
):
    require(risk, marker, reason, failures)

manager_low = risk.find('strcmp(operation, "plan") == 0')
manager_low_return = risk.find("return SI_AGENT_REQUEST_RISK_LOW;", manager_low)
manager_execute_high = risk.find("return SI_AGENT_REQUEST_RISK_HIGH;", manager_low_return)
if not (0 <= manager_low < manager_low_return < manager_execute_high):
    failures.append("manager plan may be low-risk, but manager execution must remain high-risk")

# Browser HID has a separate action array and must never reach the owned driver
# in a real retry candidate, even if policy/Broker code is later rearranged.
hid_deny = hid.find("if (!dry_run && count > 0)")
hid_return = hid.find("return;", hid_deny)
hid_effect = hid.find("si_hid_json_execute_owned(")
if not (0 <= hid_deny < hid_return < hid_effect):
    failures.append("non-dry-run HID deny must return before the owned HID effect")
require(
    hid,
    "HID execution is disabled until target-bound readback is available",
    "stable non-dry-run HID rejection is missing",
    failures,
)

# The durable action ledger must still label any future medium/high/leased
# action as a mutation if this shadow gate is intentionally opened later.
for marker in (
    "kind == SI_AGENT_REQUEST_KIND_ACTION &&",
    "risk != SI_AGENT_REQUEST_RISK_LOW || lease_required",
):
    require(BROKER, marker, f"Broker action.started mutation binding missing {marker}", failures)

if failures:
    print("Runtime V2 shadow cutover gate: FAIL")
    for failure in failures:
        print(f"- {failure}")
    raise SystemExit(1)

print("Runtime V2 shadow cutover gate: PASS")
