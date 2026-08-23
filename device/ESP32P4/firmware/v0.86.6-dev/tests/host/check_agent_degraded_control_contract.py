#!/usr/bin/env python3
"""P1 contract: a degraded legacy Agent remains cooperatively controllable."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HTTP = (ROOT / "main/services/web/agent_run_http_module.inc").read_text()
CONTROL = (ROOT / "main/services/web/agent_run_control_module.inc").read_text()
DISPATCH = (ROOT / "main/services/web/agent_tool_dispatch_module.inc").read_text()
AGENT_HTML = (ROOT / "main/www/agent.html").read_text()
UI_SHELL = (ROOT / "main/www/assets/ui-shell.js").read_text()


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


handler = function_body(HTTP, "static esp_err_t agent_run_control_handler(")

# Browser controls are exact-run commands. A late command for run A must not
# capture and cancel a newly published run B.
for marker in (
    'cJSON_GetObjectItemCaseSensitive(root, "run_id")',
    '"exact run_id required"',
    "action, expected_job, controlled_job",
    'httpd_resp_set_status(req, "409 Conflict")',
    'cJSON_AddStringToObject(resp, "reason", "run_id_mismatch")',
):
    if marker not in handler:
        raise AssertionError(f"exact HTTP control contract missing: {marker}")

exact = function_body(CONTROL, "static esp_err_t agent_run_control_exact(")
identity_check = exact.index(
    "strcmp(expected_job, s_agent_run_job.job_id) != 0"
)
epoch_publish = exact.index("++s_agent_run_job.control_epoch")
local_pause = exact.index("s_agent_run_job.pause_requested = true")
local_abort = exact.index("s_agent_run_job.cancel_requested = true")
unlock = exact.index("xSemaphoreGive(s_agent_run_lock);", local_abort)
unbound_return = exact.index("if (!shadow_bound)", unlock)
journal = exact.index("si_agent_task_service_pause(exact_job)", unbound_return)
if not identity_check < epoch_publish < local_pause < local_abort < unlock < unbound_return < journal:
    raise AssertionError(
        "exact identity and local fail-stop must precede any Task journal I/O"
    )

# Run lock is not held while TASKS.LOG/Event Store may be slow. Unbound legacy
# controls return locally without touching Task Service.
if "return ESP_OK;" not in exact[unbound_return:journal]:
    raise AssertionError("degraded control does not return before Task journal")
if "Never hold the Web Run lock across Task Service/Event Store I/O" not in exact:
    raise AssertionError("control lock-order boundary is not explicit")

# Resume is linearized by the exact job/epoch and never clears a later pause
# or an abort. Journal failure demotes only the same job and leaves local
# control available.
for marker in (
    "s_agent_run_job.control_epoch == exact_epoch",
    "s_agent_run_job.cancel_requested",
    "s_agent_run_job.task_service_bound = false",
    '"control storage_failure; shadow demoted:',
    "journal_ret == ESP_OK || shadow_degraded",
):
    if marker not in exact[journal:]:
        raise AssertionError(f"bound control linearization missing: {marker}")

abort_cleanup = exact.index("if (action == AGENT_RUN_CONTROL_ABORT)", unlock)
for marker in (
    "si_agent_request_broker_cancel_run(exact_job)",
    "agent_run_release_hid_control(exact_job)",
    "si_video_control_release_agent()",
):
    if marker not in exact[abort_cleanup:journal]:
        raise AssertionError(f"pre-journal abort cleanup missing: {marker}")

control_point = function_body(
    CONTROL, "static bool agent_run_control_point(const char *job_id)"
)
for marker in (
    "agent_run_ensure_live_source(job_id)",
    "s_agent_run_job.cancel_requested",
    "s_agent_run_job.pause_requested",
    "AGENT_RUN_STATE_PAUSED",
    "waiting for the Run lock",
):
    if marker not in control_point:
        raise AssertionError(f"legacy worker control point missing: {marker}")

# UART diagnostics share the same transport-neutral helper, but unlike the old
# maintenance surface they must name an exact run. A delayed serial command for
# run A may never capture a newer run B.
debug_control = function_body(
    HTTP, "static void web_debug_agent_run_control("
)
for marker in (
    "if (!run_id || !run_id[0])",
    '"exact run_id required\\n"',
    "action, run_id, controlled_job",
    '" reason=run_id_mismatch"',
):
    if marker not in debug_control:
        raise AssertionError(f"exact UART control contract missing: {marker}")

source_guard = function_body(
    HTTP, "static bool web_debug_agent_run_has_diagnostics_source_locked("
)
for marker in (
    "SI_AGENT_TASK_ORIGIN_SYSTEM",
    "SI_AGENT_TASK_AUTH_SYSTEM",
    "SI_PRINCIPAL_SYSTEM",
    ".auth_generation = 1U",
    "SI_CAPABILITY_OBSERVE | SI_CAPABILITY_AGENT_RUN",
    '"device-diagnostics"',
    "si_diagnostics_agent_source_identity_matches(",
):
    if marker not in source_guard:
        raise AssertionError(f"UART same-source identity check missing: {marker}")
if "action == AGENT_RUN_CONTROL_RESUME" not in debug_control or \
   "web_debug_agent_run_require_diagnostics_source(run_id)" not in debug_control:
    raise AssertionError("UART resume is not restricted to its diagnostics source")
debug_steer = function_body(HTTP, "static void web_debug_agent_run_steer(")
for marker in (
    "web_debug_agent_run_has_diagnostics_source_locked()",
    "ESP_ERR_NOT_ALLOWED",
    "reason=cross_source_denied",
):
    if marker not in debug_steer:
        raise AssertionError(f"UART steer same-source gate missing: {marker}")

debug_cancel = function_body(HTTP, "static void web_debug_agent_run_cancel(")
debug_resume = function_body(HTTP, "static void web_debug_agent_run_resume(")
for label, debug in (("cancel", debug_cancel), ("resume", debug_resume)):
    if "run_id," not in debug or "web_debug_agent_run_control(" not in debug:
        raise AssertionError(f"UART {label} does not forward an exact run ID")

# Freeze the intended degraded workload: only the bounded read-only allowlist
# may continue, but its worker can always be paused, resumed, or aborted.
tool_loop = function_body(DISPATCH, "static void agent_execute_tool_calls(")
for marker in (
    "!task_service_bound &&",
    "agent_compat_degraded_readonly(",
    "degraded read-only compatibility path",
):
    if marker not in tool_loop:
        raise AssertionError(f"degraded read-only worker contract missing: {marker}")

degraded = function_body(DISPATCH, "static bool agent_recovery_tool_allowed(")
for forbidden in (
    'strcmp(name, "memory_search") == 0',
    'strcmp(name, "history_search") == 0',
    'strcmp(name, "memory_write") == 0',
    'strcmp(name, "uart_write") == 0',
    'strcmp(name, "ssh_exec") == 0',
    'strcmp(name, "check_service") == 0',
    'strcmp(name, "verify_port") == 0',
    'strcmp(name, "host_display") == 0',
    'strcmp(name, "boot_key_sequence") == 0',
):
    if forbidden in degraded:
        raise AssertionError(f"degraded allowlist reaches storage/mutation: {forbidden}")

recovery_gate = tool_loop.index("if (!agent_recovery_tool_allowed(policy_name, call))")
non_dry_gate = tool_loop.index(
    "if (!dry_run && policy_name && !agent_tool_is_ask_user(call))"
)
first_effect = tool_loop.index("} else if (agent_tool_is_ssh_exec(call))")
if not recovery_gate < non_dry_gate < first_effect:
    raise AssertionError("recovery allowlist does not guard dry-run tool effects")

if '{run_id:activeAgentJob}' not in AGENT_HTML:
    raise AssertionError("Agent page controls do not send the exact active run ID")
if '{run_id: assistant.jobId}' not in UI_SHELL:
    raise AssertionError("floating assistant abort does not send its exact run ID")

for wrapper in (
    "agent_run_pause_handler",
    "agent_run_resume_handler",
    "agent_run_abort_handler",
    "agent_run_cancel_handler",
):
    if wrapper not in HTTP:
        raise AssertionError(f"Agent control endpoint wrapper missing: {wrapper}")

print("Agent degraded legacy control contract: PASS")
