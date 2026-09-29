#!/usr/bin/env python3
"""Static cross-module contract for Agent source identity and auth liveness."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HTTP = (ROOT / "main/services/web/agent_run_http_module.inc").read_text(
    encoding="utf-8"
)
TASK_RUN = (ROOT / "main/services/web/agent_run_task_module.inc").read_text(
    encoding="utf-8"
)
CONTROL = (ROOT / "main/services/web/agent_run_control_module.inc").read_text(
    encoding="utf-8"
)
REQUEST = (ROOT / "main/services/web/agent_request_module.inc").read_text(
    encoding="utf-8"
)
REQUEST += "\n" + (
    ROOT / "main/services/web/agent_cloud_transport_module.inc"
).read_text(encoding="utf-8")
DISPATCH = (ROOT / "main/services/web/agent_tool_dispatch_module.inc").read_text(
    encoding="utf-8"
)
TYPES = (ROOT / "main/services/web/agent_runtime_types_module.inc").read_text(
    encoding="utf-8"
)
STATUS = (
    ROOT / "main/services/web/agent_status_v2_projection_module.inc"
).read_text(encoding="utf-8")
BROKER = (ROOT / "main/services/web/agent_broker_http_module.inc").read_text(
    encoding="utf-8"
)
TASK_HEADER = (ROOT / "main/application/agent_task_service.h").read_text(
    encoding="utf-8"
)
TASK_SERVICE = (ROOT / "main/application/agent_task_service.c").read_text(
    encoding="utf-8"
)
GUARD = (ROOT / "main/core/agent_execution_guard.c").read_text(encoding="utf-8")
AUTH_HEADER = (ROOT / "main/application/auth_service.h").read_text(
    encoding="utf-8"
)

failures: list[str] = []


def require(text: str, marker: str, description: str) -> None:
    if marker not in text:
        failures.append(f"{description}: missing {marker!r}")


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        failures.append(f"missing function {signature!r}")
        return ""
    brace = source.find("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    failures.append(f"unterminated function {signature!r}")
    return ""


for marker in (
    "si_agent_task_auth_kind_t auth_kind;",
    "si_principal_kind_t principal;",
    "const char *auth_id;",
    "uint32_t auth_generation;",
    "char source_auth_id[SI_AGENT_TASK_SOURCE_ID_MAX_LEN + 1U];",
    "uint32_t source_auth_generation;",
    "SI_AGENT_TASK_QQ_AUTHORITY_CEILING",
):
    require(TASK_HEADER, marker, "Task source authentication binding")

for marker in (
    "source_auth_valid(&input->source)",
    "input->source.auth_id",
    "input->source.auth_generation",
    '"source_auth_id_hash"',
    "copy_text(s_execution_candidate.source_auth_id",
    "~SI_AGENT_TASK_QQ_AUTHORITY_CEILING",
):
    require(TASK_SERVICE, marker, "Task Service trusted source capture")
if 'cJSON_AddStringToObject(payload, "source_auth_id",' in TASK_SERVICE:
    failures.append("Task journal must not persist the raw source auth ID")

for marker in (
    'json_string_any(root, "thread_id", "conversation_id", "session_id")',
    ".source_account_id = session.session_id",
    ".auth_id = session.session_id",
    ".auth_generation = session.generation",
    "agent_run_submit(requested_thread,",
    "&trusted_source, message",
):
    require(HTTP, marker, "HTTP conversation/auth identity separation")
if "agent_run_submit(requested_thread,\n                                            session.capabilities" in HTTP:
    failures.append("HTTP submit still passes only a capability snapshot")
require(TASK_RUN, "const si_agent_task_source_t *source", "trusted submit API")
require(TASK_RUN, "si_agent_task_source_t trusted_source = *source;", "trusted submit API")

# The legacy executor carries the same exact source binding into both its RAM
# job and worker argument. Recovery admission is deliberately narrower than
# generic Task Service auth: exact live full-cap Browser/Web only.
for marker in (
    "si_agent_task_origin_t source_origin;",
    "si_agent_task_auth_kind_t source_auth_kind;",
    "si_principal_kind_t source_principal;",
    "uint32_t source_auth_generation;",
    "si_capability_set_t source_authority_ceiling;",
    "char source_auth_id[SI_AGENT_TASK_SOURCE_ID_MAX_LEN + 1U];",
):
    require(TYPES, marker, "legacy job/worker auth binding")
if TYPES.count("source_auth_generation") < 2 or TYPES.count("source_auth_id[") < 2:
    failures.append("exact source binding is not present in both job and task arg")

legacy_submit = function_body(TASK_RUN, "static esp_err_t agent_run_submit(")
for marker in (
    "agent_run_source_binding_live(",
    "source->origin != SI_AGENT_TASK_ORIGIN_WEB",
    "source->auth_kind != SI_AGENT_TASK_AUTH_DEVICE_SESSION",
    "source->principal != SI_PRINCIPAL_BROWSER",
    "source->authority_ceiling != SI_CAPABILITIES_WEB_DEFAULT",
    "recovery candidate admits full-cap browser sessions only",
    "s_agent_run_job.source_auth_generation = source->auth_generation",
    "task_arg->source_auth_generation = source->auth_generation",
    "s_agent_run_job.source_authority_ceiling = source->authority_ceiling",
    "task_arg->source_authority_ceiling = source->authority_ceiling",
):
    require(legacy_submit, marker, "full-cap Browser recovery admission")
require(STATUS, '"admission_scope"', "status admission diagnostic")
require(STATUS, '"full_cap_browser_only"', "status admission diagnostic")

require(
    AUTH_HEADER,
    "si_auth_session_get_live_context(",
    "auth service live session API",
)
for marker in (
    "agent_request_refresh_live_source(",
    "si_auth_session_get_live_context(",
    "binding->source_auth_id, binding->source_auth_generation",
    "live.principal != binding->source_principal",
    "SI_AGENT_TASK_AUTH_DEVICE_SESSION",
    ".authenticated = consume_binding.live_authenticated",
    ".principal = consume_binding.live_principal",
    ".principal_capabilities =\n                    consume_binding.live_capabilities",
    ".expected_auth_generation =",
    ".current_auth_generation =",
):
    require(BROKER, marker, "Broker/Guard live authorization boundary")
if BROKER.count("agent_request_refresh_live_source(") < 3:
    failures.append(
        "live source must be resolved at request creation, grant consumption, "
        "and the final Guard boundary"
    )
if ".authenticated = true" in BROKER:
    failures.append("Broker Guard bridge still hard-codes authenticated=true")

final_refresh = BROKER.rfind("agent_request_refresh_live_source(")
guard_call = BROKER.find("si_agent_execution_guard_evaluate(&execution_intent)")
action_started = BROKER.find("si_agent_task_service_record_action_started(", guard_call)
if not (0 <= final_refresh < guard_call < action_started):
    failures.append(
        "final live session resolution and Guard must precede durable action.started"
    )

for marker in (
    "intent->expected_auth_generation == 0U",
    "intent->current_auth_generation == 0U",
    "intent->expected_auth_generation !=",
    "intent->current_auth_generation",
    "SI_AGENT_EXECUTION_DENY_AUTH_CHANGED",
):
    require(GUARD, marker, "Execution Guard auth generation check")

# Every provider slice and each read effect refresh exact generation/current
# capabilities. Revocation publishes local cancel and exact cleanup before its
# optional Task journal attempt.
source_live = function_body(CONTROL, "static bool agent_run_source_binding_live(")
for marker in (
    "si_auth_session_get_live_context(",
    "live.generation == auth_generation",
    "strcmp(live.session_id, auth_id) == 0",
    "(live.capabilities & authority_ceiling) ==",
    "authority_ceiling",
):
    require(source_live, marker, "legacy current auth resolution")
ensure_live = function_body(CONTROL, "static bool agent_run_ensure_live_source(")
for marker in (
    "s_agent_run_job.cancel_requested = true",
    "si_agent_request_broker_cancel_run(job_id)",
    "agent_run_release_hid_control(job_id)",
    "si_video_control_release_agent()",
):
    require(ensure_live, marker, "legacy auth revoke fail-stop")
cleanup = ensure_live.find("si_agent_request_broker_cancel_run(job_id)")
journal = ensure_live.find("si_agent_task_service_cancel(job_id)")
if not 0 <= cleanup < journal:
    failures.append("local exact auth-revoke cleanup must precede Task journal")

cloud = function_body(REQUEST, "static esp_err_t agent_cloud_post_json(")
loop = cloud.find("do {")
perform = cloud.find("esp_http_client_perform(client)", loop)
live_check = cloud.find("agent_run_ensure_live_source(job_id)", loop)
if not 0 <= loop < live_check < perform:
    failures.append("provider slice does not refresh auth before each perform")

capability = function_body(CONTROL, "static bool agent_run_captured_capability(")
if capability.find("agent_run_ensure_live_source(job_id)") > capability.find(
    "si_capability_set_has("
):
    failures.append("tool capability check is not immediately preceded by live auth")
tool_loop = function_body(DISPATCH, "static void agent_execute_tool_calls(")
for marker in (
    "agent_tool_required_runtime_capability(policy_name)",
    "agent_run_captured_capability(",
    "source capability ceiling denies this tool",
):
    require(tool_loop, marker, "per-tool runtime capability boundary")

if failures:
    print("Agent auth liveness contract: FAIL")
    for failure in failures:
        print(f"- {failure}")
    raise SystemExit(1)

print("Agent auth liveness contract: PASS")
