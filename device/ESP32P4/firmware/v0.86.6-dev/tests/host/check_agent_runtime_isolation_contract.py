#!/usr/bin/env python3
"""P0 contract: provider execution cannot directly block or own Web/KVM/TF."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


SERVICE = read("main/application/agent_task_service.c")
HEADER = read("main/application/agent_task_service.h")
WEB = read("main/services/web_server.c")
TASK = read("main/services/web/agent_run_task_module.inc")
CONTROL = read("main/services/web/agent_run_control_module.inc")
REQUEST = read("main/services/web/agent_request_module.inc")
STATUS = read("main/services/web/agent_status_v2_projection_module.inc")
RUN_HTTP = read("main/services/web/agent_run_http_module.inc")
DISPATCH = read("main/services/web/agent_tool_dispatch_module.inc")
CHECKPOINT = read("main/services/web/agent_run_checkpoint_module.inc")
DATA_HTTP = read("main/services/web/agent_data_http_module.inc")
SESSIONS = read("main/services/web/agent_sessions_module.inc")
ACTION_EVENTS = read("main/services/web/agent_action_event_runtime_module.inc")
REPOSITORY = read("main/infrastructure/agent_repository.c")


def body(text: str, signature: str) -> str:
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


# Large Task Service transaction candidates live in external BSS, and startup
# has one release-published lifecycle instead of a set of racy booleans.
for marker in (
    "s_runtime_candidate",
    "s_execution_candidate",
    "s_idempotency_scope",
    "s_recovery_context",
    "SI_AGENT_TASK_SERVICE_EXT_RAM si_agent_task_runtime_t",
    "TASK_SERVICE_LIFECYCLE_COLD",
    "TASK_SERVICE_LIFECYCLE_RESERVED",
    "TASK_SERVICE_LIFECYCLE_RUNNING",
    "TASK_SERVICE_LIFECYCLE_READY",
    "TASK_SERVICE_LIFECYCLE_FAILED",
    "static uint32_t s_lifecycle",
    "__atomic_compare_exchange_n(",
    "__atomic_store_n(",
):
    if marker not in SERVICE:
        raise AssertionError(f"Task Service stack/lifecycle guard missing: {marker}")
for forbidden in ("s_start_attempted", "s_recovery_reserved", "s_recovery_running"):
    if forbidden in SERVICE:
        raise AssertionError(f"split Task startup state returned: {forbidden}")

submit = body(SERVICE, "esp_err_t si_agent_task_service_submit(")
for forbidden in (
    "si_agent_task_runtime_t candidate =",
    "si_agent_task_execution_t execution =",
    "char scope[768]",
):
    if forbidden in submit:
        raise AssertionError(f"large submit stack object returned: {forbidden}")
for marker in ("input->turn_id", "input->run_id"):
    if marker not in submit:
        raise AssertionError(f"shared legacy/shadow identity missing: {marker}")
for marker in ("const char *turn_id;", "const char *run_id;"):
    if marker not in HEADER:
        raise AssertionError(f"shared legacy/shadow identity missing: {marker}")

# Main Web startup performs RAM-only prepare first, starts httpd, and only then
# schedules TF recovery on a low-priority task. History startup is mutex-only.
web_start = body(WEB, "esp_err_t si_web_server_start(void)")
prepare = web_start.index("agent_task_service_prepare_shadow();")
httpd = web_start.index("httpd_start(&s_server, &config)")
async_start = web_start.index("agent_task_service_start_shadow_async();")
if not prepare < httpd < async_start:
    raise AssertionError("Task recovery is not scheduled strictly after httpd_start")
if "si_agent_task_service_start(" in web_start[:httpd]:
    raise AssertionError("Task Service recovery still runs on Web startup stack")
recovery_task = body(TASK, "static void agent_task_service_recovery_task(")
for marker in (
    "si_agent_task_service_recover_reserved()",
    "agent_run_checkpoint_restore()",
    "vTaskDeleteWithCaps(NULL)",
):
    if marker not in recovery_task:
        raise AssertionError(f"background recovery boundary missing: {marker}")
async_helper = body(TASK, "static void agent_task_service_start_shadow_async(")
for marker in (
    "__atomic_compare_exchange_n(",
    "xTaskCreateWithCaps(",
    "tskIDLE_PRIORITY + 1",
):
    if marker not in async_helper:
        raise AssertionError(f"single low-priority recovery task missing: {marker}")
history_start = body(REPOSITORY, "esp_err_t agent_history_start(void)")
for forbidden in ("fopen(", "stat(", "agent_repository_ensure"):
    if forbidden in history_start:
        raise AssertionError(f"pre-http history prepare performs storage I/O: {forbidden}")

# The cutover candidate binds every admitted Web run to durable Runtime V2.
# Recovery-only branches remain in source for a future explicit rollback,
# but are not the active product composition.
for marker in (
    "#define AGENT_RUNTIME_V2_SHADOW_BINDING_ENABLED 1",
    "#define AGENT_RUNTIME_RECOVERY_RAM_ONLY 0",
    "#define AGENT_RUNTIME_MUTATION_ENABLED 0",
):
    if marker not in REQUEST:
        raise AssertionError(f"Runtime V2 cutover boundary missing: {marker}")
run_submit = body(TASK, "static esp_err_t agent_run_submit(")
guard = run_submit.index("#if AGENT_RUNTIME_V2_SHADOW_BINDING_ENABLED")
task_submit = run_submit.index("si_agent_task_service_submit(", guard)
disabled = run_submit.index("#else", task_submit)
launch = run_submit.index("agent_run_launch_execution(", disabled)
if not guard < task_submit < disabled < launch:
    raise AssertionError("HTTP submit may synchronously enter Task Service")
for marker in (
    "Runtime V2 binding disabled in recovery candidate",
    "task_arg->task_service_bound = task_service_bound",
):
    if marker not in run_submit[disabled:launch]:
        raise AssertionError(f"unbound recovery submit diagnostic missing: {marker}")
for marker in (
    ".completion_criteria_kind = include_screenshot ?",
    "SI_AGENT_TASK_CRITERIA_OBSERVATION :",
    "SI_AGENT_TASK_CRITERIA_DELIVERABLE,",
):
    if marker not in run_submit:
        raise AssertionError(
            f"read-only completion provenance boundary missing: {marker}"
        )
for marker in (
    "if (include_screenshot)",
    "screenshot capture is unavailable in the RAM-only recovery candidate",
    "return ESP_ERR_NOT_SUPPORTED;",
):
    if marker not in run_submit:
        raise AssertionError(f"recovery screenshot lease gate missing: {marker}")

run_worker = body(TASK, "static void agent_run_task(void *arg)")
for marker in (
    "AGENT_RUN_BROKER_RETIRE_MAX_ATTEMPTS",
    "retire_attempt <= AGENT_RUN_BROKER_RETIRE_MAX_ATTEMPTS",
    "agent_run_release_hid_control(run->job_id)",
    "#if AGENT_RUNTIME_V2_SHADOW_BINDING_ENABLED",
    "agent_run_start_next_queued();",
    "s_agent_run_job.task = NULL",
):
    if marker not in run_worker:
        raise AssertionError(f"bounded worker cleanup guard missing: {marker}")
if "si_control_lease_release_agent()" in run_worker:
    raise AssertionError("stale worker can broadly release a newer control owner")
if "for (;;)" in run_worker:
    raise AssertionError("worker cleanup again retries forever")
def require_recovery_guarded_video_cleanup(source: str, label: str) -> None:
    calls = []
    cursor = 0
    while True:
        cursor = source.find("si_video_control_release_agent();", cursor)
        if cursor < 0:
            break
        calls.append(cursor)
        cursor += 1
    if not calls:
        raise AssertionError(f"expected video cleanup disappeared: {label}")
    for call in calls:
        guard = source.rfind("#if !AGENT_RUNTIME_RECOVERY_RAM_ONLY", 0, call)
        end = source.find("#endif", guard)
        if guard < 0 or end < call:
            raise AssertionError(
                f"RAM-only path can release another Agent preview demand: {label}"
            )


require_recovery_guarded_video_cleanup(run_worker, "worker cleanup")
for signature in (
    "static bool agent_run_ensure_live_source(",
    "static esp_err_t agent_run_control_exact(",
):
    require_recovery_guarded_video_cleanup(body(CONTROL, signature), signature)

# Status/events and the compatibility binding probe must never wait on Task
# mutex or scan TASKS.LOG in the RAM-only candidate.
if "#if !AGENT_RUNTIME_RECOVERY_RAM_ONLY" not in STATUS:
    raise AssertionError("status Task snapshot is not recovery-gated")
events = body(RUN_HTTP, "static esp_err_t agent_run_events_handler(")
if "#if !AGENT_RUNTIME_RECOVERY_RAM_ONLY" not in events:
    raise AssertionError("events Task snapshot is not recovery-gated")
for marker in (
    '"current_run_ram_ring_no_tf_scan"',
    '"ram_cursor_only_no_tf_scan"',
):
    if marker not in events:
        raise AssertionError(f"RAM-only event replay diagnostic missing: {marker}")
bound_probe = body(DISPATCH, "static bool agent_run_task_service_is_bound(")
recovery_branch = bound_probe[bound_probe.index("#if AGENT_RUNTIME_RECOVERY_RAM_ONLY"):
                              bound_probe.index("#else")]
if "return false;" not in recovery_branch:
    raise AssertionError("recovery binding probe can fall through to Task/Broker")

# Provider/worker code never performs TF I/O directly. Recovery mode remains
# an explicit drop; the normal product path uses the one bounded history
# writer RPC. Auto-polled GET routes report unsupported in recovery mode
# without opening HIST.LOG/MEMORY.LOG.
persist = body(ACTION_EVENTS, "static void agent_action_event_persist(")
for forbidden in ("agent_history_file", "si_agent_repository", "fopen("):
    if forbidden in persist:
        raise AssertionError(f"action event runtime still persists: {forbidden}")
chat_append = body(DISPATCH, "static esp_err_t agent_history_store_run_turn(")
memory_append = body(DISPATCH, "agent_memory_append_task_summary(")
ram_guard = chat_append.index("#if AGENT_RUNTIME_RECOVERY_RAM_ONLY")
normal_guard = chat_append.index("#else", ram_guard)
guard_end = chat_append.index("#endif", normal_guard)
chat_ram_only = chat_append[ram_guard:normal_guard]
chat_normal = chat_append[normal_guard:guard_end]
if "return ESP_ERR_NOT_SUPPORTED;" not in chat_ram_only:
    raise AssertionError("runtime chat history is not an explicit recovery drop")
for marker in (
    "si_conversation_history_is_enabled()",
    "si_agent_history_writer_store_turn(",
    "SI_AGENT_HISTORY_WRITER_DEFAULT_WAIT_MS",
):
    if marker not in chat_normal:
        raise AssertionError(f"normal chat writer boundary missing: {marker}")
for forbidden in ("agent_history_file", "agent_memory_file", "fopen("):
    if forbidden in chat_normal:
        raise AssertionError(
            f"provider chat helper bypasses dedicated writer: {forbidden}"
        )
if "return ESP_ERR_NOT_SUPPORTED;" not in memory_append:
    raise AssertionError("runtime memory summary is not an explicit drop")
for forbidden in ("agent_history_file", "agent_memory_file", "fopen("):
    if forbidden in memory_append:
        raise AssertionError(f"runtime memory summary still touches storage: {forbidden}")
for signature, source in (
    ("static esp_err_t agent_history_get_handler(", DATA_HTTP),
    ("static esp_err_t agent_memory_get_handler(", DATA_HTTP),
    ("static esp_err_t agent_sessions_get_handler(", SESSIONS),
):
    handler = body(source, signature)
    branch = handler[handler.index("#if AGENT_RUNTIME_RECOVERY_RAM_ONLY"):
                     handler.index("#else")]
    for marker in ('"supported", false', '"ram_only_recovery_candidate"'):
        if marker not in branch:
            raise AssertionError(f"auto GET RAM-only marker missing: {signature} {marker}")
    for forbidden in ("agent_history_file", "agent_memory_file", "xSemaphoreTake"):
        if forbidden in branch:
            raise AssertionError(f"auto GET recovery branch reaches TF/lock: {signature}")

degraded = body(DISPATCH, "static bool agent_recovery_tool_allowed(")
for forbidden in (
    'strcmp(name, "memory_search") == 0',
    'strcmp(name, "history_search") == 0',
    'strcmp(name, "memory_write") == 0',
    'strcmp(name, "ssh_exec") == 0',
    'strcmp(name, "check_service") == 0',
    'strcmp(name, "verify_port") == 0',
    'strcmp(name, "host_display") == 0',
    'strcmp(name, "boot_key_sequence") == 0',
):
    if forbidden in degraded:
        raise AssertionError(f"degraded mode reaches storage/mutation: {forbidden}")
capability_snapshot = body(REQUEST, "static void agent_add_runtime_capability_snapshot(")
if "recovery_available_tool" not in capability_snapshot:
    raise AssertionError("recovery tool catalog is not a positive allowlist")
catalog_gate = capability_snapshot[
    capability_snapshot.index("bool recovery_available_tool"):
    capability_snapshot.index("si_capability_t required")
]
for marker in (
    'strcmp(tool_names[i], "observe_status") == 0',
    'strcmp(tool_names[i], "uart_read") == 0',
    'strcmp(tool_names[i], "wait") == 0',
):
    if marker not in catalog_gate:
        raise AssertionError(f"recovery tool catalog lost bounded read: {marker}")
for forbidden in (
    '"observe_screenshot"',
    '"ssh_exec"',
    '"check_service"',
    '"verify_port"',
    '"host_display"',
    '"boot_key_sequence"',
    '"memory_search"',
    '"ask_user"',
):
    if forbidden in catalog_gate:
        raise AssertionError(f"recovery tool catalog advertises closed tool: {forbidden}")
for marker in (
    "AGENT_RUNTIME_READONLY_TOOL_PROMPT",
    "SSH, screenshot capture, HID actions",
    "Always return actions as an empty array",
    "agent_runtime_tool_prompt()",
    "AGENT_RUNTIME_MUTATION_ENABLED &&",
):
    if marker not in REQUEST:
        raise AssertionError(f"recovery prompt/catalog boundary missing: {marker}")
tool_loop = body(DISPATCH, "static void agent_execute_tool_calls(")
for marker in (
    "#if AGENT_RUNTIME_RECOVERY_RAM_ONLY",
    "agent_tool_is_ask_user(call)",
    "ask_user is unavailable in the RAM-only recovery candidate",
    "if (!agent_recovery_tool_allowed(policy_name, call))",
    "tool is unavailable in the RAM-only recovery candidate",
):
    if marker not in tool_loop:
        raise AssertionError(f"recovery ask_user fail-closed gate missing: {marker}")
recovery_gate = tool_loop.index("if (!agent_recovery_tool_allowed(policy_name, call))")
non_dry_gate = tool_loop.index(
    "if (!dry_run && policy_name && !agent_tool_is_ask_user(call))"
)
first_effect = tool_loop.index("} else if (agent_tool_is_ssh_exec(call))")
if not recovery_gate < non_dry_gate < first_effect:
    raise AssertionError("dry-run tool effects bypass the recovery allowlist")

# Legacy Run initialization is RAM-only; checkpoint migration belongs only to
# the recovery task above.
start = body(CHECKPOINT, "static esp_err_t agent_run_start(void)")
for forbidden in (
    "agent_run_checkpoint_restore(",
    "si_agent_task_service_",
    "si_agent_repository_",
):
    if forbidden in start:
        raise AssertionError(f"lazy Run init performs recovery/storage: {forbidden}")
if "legacy checkpoint quarantined" not in start:
    raise AssertionError("RAM-only checkpoint quarantine is not explicit")

print("Agent Runtime V2 shadow isolation contract: OK")
