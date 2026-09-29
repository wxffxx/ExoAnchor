#!/usr/bin/env python3
"""Runtime V2 contract for identity-safe legacy checkpoint migration."""

from pathlib import Path


PROJECT_DIR = Path(__file__).resolve().parents[2]
REPOSITORY = (
    PROJECT_DIR / "main" / "infrastructure" / "agent_repository.c"
).read_text(encoding="utf-8")
HEADER = (
    PROJECT_DIR / "main" / "infrastructure" / "agent_repository.h"
).read_text(encoding="utf-8")
CHECKPOINT = (
    PROJECT_DIR / "main" / "services" / "web" /
    "agent_run_checkpoint_module.inc"
).read_text(encoding="utf-8")
TASK = (
    PROJECT_DIR / "main" / "services" / "web" /
    "agent_run_task_module.inc"
).read_text(encoding="utf-8")
HTTP = (
    PROJECT_DIR / "main" / "services" / "web" /
    "agent_run_http_module.inc"
).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function: {signature}")
    brace = source.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing function body: {signature}")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise AssertionError(f"unterminated function body: {signature}")


def block_from(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"missing block: {marker}")
    brace = source.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing block body: {marker}")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise AssertionError(f"unterminated block: {marker}")


# The legacy file may be removed only when its durable job identity still
# matches the migration that just wrote the canonical terminal event.
API = "si_agent_repository_run_checkpoint_clear_if_job_id"
if API not in HEADER:
    raise AssertionError("conditional checkpoint-clear API is not declared")

conditional = function_body(
    REPOSITORY,
    "esp_err_t si_agent_repository_run_checkpoint_clear_if_job_id(",
)
for marker in (
    "if (!expected_job_id || !expected_job_id[0])",
    "xSemaphoreTake(s_agent_history_lock",
    "ret = agent_repository_ensure_tf_dir_locked();",
    "ret = agent_repository_run_checkpoint_read_file_locked(&checkpoint);",
    "if (ret == ESP_ERR_NOT_FOUND)",
    "root = cJSON_Parse(checkpoint);",
    'cJSON_GetObjectItemCaseSensitive(root, "job_id")',
    "!cJSON_IsObject(root)",
    "strcmp(job_id->valuestring, expected_job_id) == 0",
    "remove(AGENT_RUN_CHECKPOINT_TF_FILE)",
    "xSemaphoreGive(s_agent_history_lock);",
):
    if marker not in conditional:
        raise AssertionError(f"conditional checkpoint clear missing: {marker}")

ensure_block = block_from(
    conditional[conditional.find("ret = agent_repository_ensure_tf_dir_locked();"):],
    "if (ret == ESP_OK)",
)
if "agent_repository_run_checkpoint_read_file_locked(&checkpoint)" not in ensure_block:
    raise AssertionError(
        "missing-file handling must be nested after a successful TF mount check"
    )
if "if (ret == ESP_ERR_NOT_FOUND)" not in ensure_block:
    raise AssertionError("only a missing checkpoint file may be treated as success")

match_block = block_from(
    conditional,
    "else if (strcmp(job_id->valuestring, expected_job_id) == 0)",
)
if "remove(AGENT_RUN_CHECKPOINT_TF_FILE)" not in match_block:
    raise AssertionError("checkpoint unlink must be inside the job-id match block")
if conditional.count("remove(AGENT_RUN_CHECKPOINT_TF_FILE)") != 1:
    raise AssertionError("conditional clear has an unlink outside its identity gate")
if conditional.find("xSemaphoreTake(s_agent_history_lock") > conditional.find(
    "agent_repository_run_checkpoint_read_file_locked(&checkpoint)"
):
    raise AssertionError("checkpoint identity must be read while repository lock is held")
if conditional.find("xSemaphoreGive(s_agent_history_lock)") < conditional.find(
    "remove(AGENT_RUN_CHECKPOINT_TF_FILE)"
):
    raise AssertionError("repository lock must cover identity comparison and unlink")


# A valid schema-v1 checkpoint is not executable state. Migration appends an
# outcome_unknown terminal fact, then clears exactly that legacy job. Raw goal
# and page context are neither copied into TASKS.LOG nor replayed into RAM.
restore = function_body(
    CHECKPOINT, "static esp_err_t agent_run_checkpoint_restore(void)"
)
for marker in (
    'schema->valueint == 1',
    'cJSON_GetObjectItemCaseSensitive(root, "job_id")',
    'cJSON_GetObjectItemCaseSensitive(root, "turn_id")',
    'cJSON_GetObjectItemCaseSensitive(\n        root, "session_id")',
    'cJSON_GetObjectItemCaseSensitive(root, "message")',
    "agent_run_checkpoint_append_unknown(thread_copy, turn_copy,",
    f"{API}(run_copy,",
    "if (ret != ESP_OK || !cleared)",
    "AGENT_RUN_STATE_OUTCOME_UNKNOWN",
    '"legacy run outcome unknown after reboot"',
):
    if marker not in restore:
        raise AssertionError(f"legacy terminal migration missing: {marker}")

append_pos = restore.index("agent_run_checkpoint_append_unknown(")
append_check_pos = restore.index("if (ret != ESP_OK)", append_pos)
clear_pos = restore.index(API, append_check_pos)
if not append_pos < append_check_pos < clear_pos:
    raise AssertionError("legacy checkpoint is cleared before outcome_unknown is durable")

for forbidden in (
    'cJSON_GetObjectItemCaseSensitive(root, "page_context")',
    'cJSON_GetObjectItemCaseSensitive(root, "page_context_json")',
    "strdup(message",
    "strlcpy(s_agent_run_job.goal",
    "strlcpy(s_agent_run_job.page_context_json",
):
    if forbidden in restore:
        raise AssertionError(f"legacy migration replays private input: {forbidden}")

append_unknown = function_body(
    CHECKPOINT, "static esp_err_t agent_run_checkpoint_append_unknown("
)
for marker in (
    "si_agent_task_service_replay(",
    "if (lookup.found)",
    '"turn.outcome_unknown"',
    '"legacy_checkpoint_migrated"',
    '"execution_may_have_started", true',
    '"blind_retry_forbidden", true',
    '"raw_goal_replayed", false',
    '"page_context_replayed", false',
    "si_agent_event_store_append(&event, NULL)",
):
    if marker not in append_unknown:
        raise AssertionError(f"idempotent unknown-outcome append missing: {marker}")


# Malformed legacy data has no trustworthy job identity and may only be removed
# through the explicitly named rejection path. It must never enable execution.
clear_malformed = function_body(
    CHECKPOINT, "static esp_err_t agent_run_checkpoint_clear_malformed("
)
if "si_agent_repository_run_checkpoint_clear(&cleared)" not in clear_malformed:
    raise AssertionError("malformed legacy checkpoint rejection does not clear the file")
if CHECKPOINT.count("si_agent_repository_run_checkpoint_clear(&cleared)") != 1:
    raise AssertionError("unconditional legacy checkpoint clear escaped malformed path")

start = function_body(CHECKPOINT, "static esp_err_t agent_run_start(void)")
for marker in ("legacy checkpoint quarantined", "return ESP_OK;"):
    if marker not in start:
        raise AssertionError(f"checkpoint quarantine/degraded start missing: {marker}")
for forbidden in (
    "agent_run_checkpoint_restore(",
    "si_agent_task_service_",
    "si_agent_repository_",
    "vSemaphoreDelete(s_agent_run_lock);",
    "s_agent_run_lock = NULL;",
    "return ret;",
):
    if forbidden in start:
        raise AssertionError(
            f"Runtime V2 checkpoint failure still disables legacy Agent: {forbidden}"
        )

# Recovery/migration is allowed only on the low-priority task after Runtime V2
# has completed its own recovery; Web startup and lazy Run APIs remain RAM-only.
recovery = function_body(
    TASK, "static void agent_task_service_recovery_task(void *arg)"
)
task_recover = recovery.index("si_agent_task_service_recover_reserved()")
checkpoint_recover = recovery.index("agent_run_checkpoint_restore()")
if not task_recover < checkpoint_recover:
    raise AssertionError("legacy checkpoint migration precedes Task recovery")
for marker in (
    "Legacy checkpoint remains quarantined after shadow ",
    '"recovery: %s"',
    "vTaskDeleteWithCaps(NULL)",
):
    if marker not in recovery:
        raise AssertionError(f"asynchronous checkpoint quarantine missing: {marker}")


# New Runtime V2 work never writes or resumes RUN.CHECKPOINT. Ordinary task
# completion and browser/debug controls therefore have no legacy-file cleanup
# authority; boot migration is the sole valid-checkpoint deletion path.
task = function_body(TASK, "static void agent_run_task(void *arg)")
browser_control = function_body(
    HTTP, "static esp_err_t agent_run_control_handler("
)
debug_cancel = function_body(
    HTTP, "static void web_debug_agent_run_cancel(const char *run_id,"
)
for label, body in (
    ("task finalization", task),
    ("browser control", browser_control),
    ("debug control", debug_cancel),
):
    if "run_checkpoint_clear" in body or "run_checkpoint_write" in body:
        raise AssertionError(f"{label} still mutates legacy RUN.CHECKPOINT")
if "agent_run_checkpoint_write_locked" in TASK:
    raise AssertionError("Runtime V2 task still writes a legacy checkpoint")
if "agent_run_resume_recovered" in TASK or "agent_run_resume_recovered" in HTTP:
    raise AssertionError("Runtime V2 still exposes legacy automatic resume")
if "recovered_after_reboot = true" in CHECKPOINT:
    raise AssertionError("legacy checkpoint is still projected as resumable")


print("Runtime V2 Agent checkpoint identity contract: OK")
