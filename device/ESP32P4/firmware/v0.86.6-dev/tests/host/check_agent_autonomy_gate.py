#!/usr/bin/env python3
"""Pin the embedded Agent autonomy gate (SAE-style L0..L4).

Every real tool call is classified by firmware, gated by the operator's
autonomy level before Broker authorization, and only then dispatched.  HID
stays closed, degraded mode stays read-only, and L0/L1 keep a second,
independent observe-only line in the execution guard.
"""

from pathlib import Path


PROJECT = Path(__file__).resolve().parents[2]
DISPATCH_PATH = PROJECT / "main/services/web/agent_tool_dispatch_module.inc"
BROKER_PATH = PROJECT / "main/services/web/agent_broker_http_module.inc"
DISPATCH = DISPATCH_PATH.read_text(encoding="utf-8")
BROKER = BROKER_PATH.read_text(encoding="utf-8")
GATE = (PROJECT / "main/services/web/agent_autonomy_gate_module.inc").read_text(encoding="utf-8")
WEB_SERVER = (PROJECT / "main/services/web_server.c").read_text(encoding="utf-8")
SETTINGS_HTTP = (PROJECT / "main/services/web/agent_tools_settings_http_module.inc").read_text(encoding="utf-8")
TOOLS_SETTINGS = (PROJECT / "main/application/agent_tools_settings.c").read_text(encoding="utf-8")
AUTONOMY_H = (PROJECT / "main/core/agent_autonomy.h").read_text(encoding="utf-8")
REQUEST = (PROJECT / "main/services/web/agent_request_module.inc").read_text(encoding="utf-8")


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
degraded = function_body(DISPATCH, "static bool agent_recovery_tool_allowed(")
tools = function_body(DISPATCH, "static void agent_execute_tool_calls(")
hid = function_body(DISPATCH, "static void agent_execute_actions(")
classify = function_body(GATE, "static si_agent_action_class_t agent_tool_action_class(")
bound = function_body(DISPATCH, "static bool agent_run_task_service_is_bound(")
authorize = function_body(BROKER, "static bool agent_request_authorize_ex(")
guard_mode = function_body(BROKER, "static si_authorization_mode_t agent_request_authorization_mode(")

gate_include = WEB_SERVER.find('#include "web/agent_autonomy_gate_module.inc"')
if not (0 <= gate_include < WEB_SERVER.find('#include "web/agent_broker_http_module.inc"')):
    failures.append("autonomy gate module must be compiled before the Broker bridge")
if "agent_compat_dispatch_allowed" in DISPATCH:
    failures.append("the old blanket shadow-cutover gate came back next to the autonomy gate")

# Order: recovery gate < real-call branch < autonomy gate < risk remap <
# Broker authorization < first effect.
positions = [
    tools.find("if (!agent_recovery_tool_allowed(policy_name, call))"),
    tools.find("if (!dry_run && policy_name && !agent_tool_is_ask_user(call))"),
    tools.find("agent_tool_action_class(policy_name, call, risk)"),
    tools.find("!si_agent_autonomy_allows(autonomy, action_class)"),
    tools.find("risk = agent_action_class_risk(action_class);"),
    tools.find("agent_request_authorize("),
    tools.find("} else if (agent_tool_is_ssh_exec(call))"),
]
if not all(a >= 0 for a in positions) or positions != sorted(positions):
    failures.append("autonomy gate must classify, gate and remap risk before Broker authorization and any effect")
for marker, reason in (
    ("if (lease_required ||", "leased (HID/Console injection) calls must stay rejected"),
    ("keyboard/mouse (HID) actions are not open to the embedded Agent yet", "HID rejection reason missing"),
    ("raise the Agent autonomy level in Settings", "level rejection must tell the user where to change it"),
    ("!task_service_bound &&", "degraded journal mode must stay read-only"),
):
    require(tools, marker, reason, failures)

for marker, reason in (
    ("si_agent_classify_shell_command(agent_tool_command(call))",
     "ssh_exec must be classified from the exact command the executor runs"),
    ('json_string_any(args, "host", "ip", "target")',
     "SSH host/account overrides must escalate the class"),
    ("SI_AGENT_ACTION_SYSTEM);", "SSH override escalation must reach the system class"),
    ('strcasecmp(action, "force_off") == 0', "forced power-off must be classified"),
    ("return SI_AGENT_ACTION_IRREVERSIBLE;", "irreversible class missing"),
    ("si_agent_classify_path_write(path)", "file targets must be classified by path"),
):
    require(classify, marker, reason, failures)

for marker, reason in (
    ("const bool delegated = kind == SI_AGENT_REQUEST_KIND_ACTION &&",
     "questions and context requests must never be auto-approved"),
    ("si_agent_autonomy_auto_through(si_agent_autonomy_get())",
     "Broker auto-approval must follow the autonomy level"),
    ("expected_policy_revision != si_agent_tools_policy_revision()",
     "policy revision drift must still invalidate requests"),
):
    require(authorize, marker, reason, failures)
if "si_agent_access_mode_get()" in authorize:
    failures.append("embedded Agent approval must not follow the shared MCP access mode")
require(guard_mode, "default: return SI_AUTHZ_MODE_OBSERVE;",
        "L0/L1 must keep the execution guard in observe-only mode", failures)
require(guard_mode, "case SI_AGENT_AUTONOMY_L2: return SI_AUTHZ_MODE_SUPERVISED;",
        "L2 must map to supervised guard mode", failures)

for marker, reason in (
    ("#define SI_AGENT_AUTONOMY_DEFAULT SI_AGENT_AUTONOMY_L1", "default level must stay L1 read-only"),
    ("#define SI_AGENT_AUTONOMY_MAX SI_AGENT_AUTONOMY_L4", "L5 must not be offered"),
):
    require(AUTONOMY_H, marker, reason, failures)
require(SETTINGS_HTTP, "browser operator required to change Agent autonomy",
        "only a browser operator may change the autonomy level", failures)
require(TOOLS_SETTINGS, "agent_tools_policy_revision_bump();\n    }\n    return ret;\n}\n\nstatic bool strategy_valid",
        "changing the level must bump the policy revision", failures)
require(REQUEST, "cJSON_AddBoolToObject(snapshot, \"hid_actions_route_allowed\", false);",
        "the model must not be told HID actions are available", failures)

# Degraded/recovery read allowlist stays narrow.
require(degraded, "if (!name)", "read-only allowlist does not reject unnamed calls", failures)
for forbidden in (
    'strcmp(name, "memory_write") == 0',
    'strcmp(name, "uart_write") == 0',
    'strcmp(name, "uart_baud") == 0',
    'strcmp(name, "ssh_exec") == 0',
    'strcmp(operation, "execute") == 0',
):
    if forbidden in degraded:
        failures.append(f"degraded read-only allowlist contains unsafe entry: {forbidden}")
recovery_branch = bound[bound.find("#if AGENT_RUNTIME_RECOVERY_RAM_ONLY"):bound.find("#else")]
if "return false;" not in recovery_branch:
    failures.append("recovery binding probe may default into Broker/Task journal I/O")

# Browser HID has a separate action array and must never reach the owned driver.
hid_deny = hid.find("if (!dry_run && count > 0)")
hid_return = hid.find("return;", hid_deny)
hid_effect = hid.find("si_hid_json_execute_owned(")
if not (0 <= hid_deny < hid_return < hid_effect):
    failures.append("non-dry-run HID deny must return before the owned HID effect")

for marker in (
    "kind == SI_AGENT_REQUEST_KIND_ACTION &&",
    "risk != SI_AGENT_REQUEST_RISK_LOW || lease_required",
):
    require(BROKER, marker, f"Broker action.started mutation binding missing {marker}", failures)

if failures:
    print("Agent autonomy gate: FAIL")
    for failure in failures:
        print(f"- {failure}")
    raise SystemExit(1)

print("Agent autonomy gate: PASS")
