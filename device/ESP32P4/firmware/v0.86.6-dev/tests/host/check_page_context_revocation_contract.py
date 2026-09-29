#!/usr/bin/env python3
"""Runtime V2 contract for device-wide page-context revocation."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "main/services/web"

FEATURES = (WEB / "product_features_http_module.inc").read_text(encoding="utf-8")
CHECKPOINT = (WEB / "agent_run_checkpoint_module.inc").read_text(encoding="utf-8")
TASK = (WEB / "agent_run_task_module.inc").read_text(encoding="utf-8")
ENGINE = (WEB / "agent_run_engine_module.inc").read_text(encoding="utf-8")
CONTROL = (WEB / "agent_run_control_module.inc").read_text(encoding="utf-8")
REQUEST = (WEB / "agent_request_module.inc").read_text(encoding="utf-8")
REQUEST += "\n" + (WEB / "agent_cloud_transport_module.inc").read_text(
    encoding="utf-8"
)
WEB_SERVER = (ROOT / "main/services/web_server.c").read_text(encoding="utf-8")
TASK_SERVICE = (ROOT / "main/application/agent_task_service.c").read_text(
    encoding="utf-8"
)
REPOSITORY = (ROOT / "main/infrastructure/agent_repository.c").read_text(
    encoding="utf-8"
)


def require(text: str, fragment: str, label: str) -> None:
    if fragment not in text:
        raise AssertionError(f"{label}: missing {fragment!r}")


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


# Saving the device-wide setting must revoke all transient runtime copies,
# record the revocation, scrub any legacy checkpoint field, and only then
# commit the disabled setting.
require(
    FEATURES,
    "if (page_context_was_enabled && !settings.page_context_enabled) {\n"
    "        ret = agent_run_revoke_page_context(&settings);",
    "Settings true-to-false revocation hook",
)
revoke_runtime_body = function_body(
    TASK, "static esp_err_t agent_run_revoke_page_context("
)
for fragment, label in (
    (
        "memset(s_agent_run_job.page_context_json, 0,",
        "legacy projection RAM context clear",
    ),
    (
        "__atomic_add_fetch(&s_agent_page_context_generation",
        "active request generation revocation",
    ),
    (
        "si_agent_task_service_revoke_page_context();",
        "Runtime V2 RAM revocation and journal event",
    ),
    (
        "si_agent_repository_run_checkpoint_revoke_page_context();",
        "legacy checkpoint field scrub",
    ),
    (
        "ret = si_product_feature_settings_set(settings);",
        "atomic NVS disable commit",
    ),
):
    require(revoke_runtime_body, fragment, label)

ordered_revoke_steps = (
    "memset(s_agent_run_job.page_context_json, 0,",
    "__atomic_add_fetch(&s_agent_page_context_generation",
    "xSemaphoreGive(s_agent_run_lock);",
    "si_agent_task_service_revoke_page_context();",
    "si_agent_repository_run_checkpoint_revoke_page_context();",
    "ret = si_product_feature_settings_set(settings);",
)
positions = [revoke_runtime_body.index(step) for step in ordered_revoke_steps]
if positions != sorted(positions):
    raise AssertionError("page-context revoke transaction is ordered incorrectly")
require(
    revoke_runtime_body,
    "#if AGENT_RUNTIME_RECOVERY_RAM_ONLY",
    "recovery candidate fast rejection gate",
)
if revoke_runtime_body.index("xSemaphoreGive(s_agent_run_lock);") > \
        revoke_runtime_body.index("si_agent_task_service_revoke_page_context();"):
    raise AssertionError("page-context revoke holds Run lock across Task journal")
if "s_agent_run_job.task_arg" in TASK:
    raise AssertionError("revocation must not mutate a live task-owned buffer")


# TASKS.LOG records only bounded metadata. The raw goal and page context remain
# in volatile execution records and are never copied into turn.submitted.
submit_body = function_body(
    TASK_SERVICE, "esp_err_t si_agent_task_service_submit("
)
for fragment, label in (
    ('cJSON_AddStringToObject(payload, "goal_hash", goal_hash)', "goal hash"),
    ('cJSON_AddNumberToObject(payload, "goal_bytes", strlen(input->goal))', "goal length"),
    (
        'cJSON_AddBoolToObject(payload, "raw_goal_persisted", false)',
        "raw-goal non-persistence marker",
    ),
    (
        'cJSON_AddStringToObject(payload, "completion_criteria_hash",',
        "completion-criteria hash",
    ),
    (
        'cJSON_AddBoolToObject(payload, "page_context_attached",',
        "page-context presence-only marker",
    ),
    (
        'append_event(\n        "turn.submitted"',
        "canonical submitted event",
    ),
):
    require(submit_body, fragment, label)

payload_start = submit_body.index("cJSON *payload = cJSON_CreateObject();")
payload_end = submit_body.index("cJSON_Delete(payload);", payload_start)
submitted_payload = submit_body[payload_start:payload_end]
for forbidden_field in (
    'cJSON_AddStringToObject(payload, "goal",',
    'cJSON_AddStringToObject(payload, "message",',
    'cJSON_AddStringToObject(payload, "page_context",',
    'cJSON_AddStringToObject(payload, "page_context_json",',
    'cJSON_AddStringToObject(payload, "completion_criteria",',
):
    if forbidden_field in submitted_payload:
        raise AssertionError(
            f"turn.submitted persists a forbidden raw field: {forbidden_field}"
        )


# Runtime V2 revocation writes context.revoked before clearing every in-memory
# active/queued Task context. A journal failure therefore leaves RAM intact and
# prevents the outer setting transaction from committing.
service_revoke = function_body(
    TASK_SERVICE, "esp_err_t si_agent_task_service_revoke_page_context(void)"
)
for fragment, label in (
    ("SI_AGENT_TASK_RECORD_CAPACITY", "bounded all-record scan"),
    ('"context.revoked"', "canonical revocation event"),
    (
        'cJSON_AddNumberToObject(payload, "cleared_task_count", cleared)',
        "bounded cleared-count metadata",
    ),
    (
        'cJSON_AddBoolToObject(payload, "raw_context_persisted", false)',
        "raw-context non-persistence marker",
    ),
    ("if (result == ESP_OK)", "append-before-clear gate"),
    (
        "memset(s_records[index].execution.page_context_json, 0,",
        "all Task RAM context clear",
    ),
):
    require(service_revoke, fragment, label)
if service_revoke.index('"context.revoked"') > service_revoke.index(
    "memset(s_records[index].execution.page_context_json, 0,"
):
    raise AssertionError("Runtime V2 context was cleared before revocation was durable")


# New active Runs capture the current generation exactly once. There is no
# recovered executable Run in V2: legacy checkpoints become terminal unknown.
require(
    TASK,
    "__atomic_store_n(\n        &s_agent_run_page_context_generation,",
    "new active Run generation capture",
)
require(
    TASK,
    "page_context_enabled && agent_run_page_context_allowed() &&",
    "new Task context admission",
)
allowed_body = function_body(
    WEB_SERVER, "static bool agent_run_page_context_allowed(void)"
)
if "si_page_context_is_enabled" in allowed_body:
    raise AssertionError("Agent task must not race on the mutable feature struct")

migration_body = function_body(
    CHECKPOINT, "static esp_err_t agent_run_checkpoint_append_unknown("
)
for marker in (
    '"turn.outcome_unknown"',
    '"legacy_checkpoint_migrated"',
    '"blind_retry_forbidden", true',
    '"raw_goal_replayed", false',
    '"page_context_replayed", false',
):
    require(migration_body, marker, "legacy checkpoint terminal migration")
if "agent_run_resume_recovered" in TASK or "recovered_after_reboot = true" in CHECKPOINT:
    raise AssertionError("legacy checkpoint still restores executable work")


# A running task drops context at every model-request safe point. Requests
# already handed to the provider are intentionally outside the revocation
# boundary; keep this concurrency gate independent of checkpoint migration.
require(ENGINE, "agent_run_build_initial_request(", "initial request gate")
if ENGINE.count("agent_run_page_context_allowed()") < 5:
    raise AssertionError("all model request/report safe points must honor revocation")
require(
    CONTROL,
    "agent_run_page_context_request_lock(const char *job_id)",
    "provider request lock linearization",
)
if ENGINE.count("agent_run_page_context_request_lock(") < 4:
    raise AssertionError("every context-bearing provider request needs a lock")
require(
    CONTROL,
    "lock deliberately remains held until agent_cloud_post_json() performs",
    "provider handoff lock ownership",
)
require(
    REQUEST,
    "The first perform hands the request to the HTTP state machine.",
    "provider handoff occurs at first HTTP perform",
)
require(
    CONTROL,
    "agent_run_page_context_allowed()) {\n"
    "        return true;\n"
    "    }\n"
    "    xSemaphoreGive(s_agent_run_lock);",
    "successful provider request keeps the Run lock",
)
require(
    REQUEST,
    "ret = esp_http_client_perform(client);\n"
    "        /* The first perform hands the request to the HTTP state machine.",
    "HTTP perform precedes request-lock release",
)
if REQUEST.count("agent_cloud_release_context_request_lock(") < 5:
    raise AssertionError("provider request lock must be released on every exit path")
require(
    ENGINE,
    'cJSON_DeleteItemFromObjectCaseSensitive(ctx->resp, "page_context")',
    "continuation safe-point isolation",
)
require(
    ENGINE,
    'cJSON_DeleteItemFromObjectCaseSensitive(run_resp, "page_context")',
    "final report isolation",
)
task_body = function_body(TASK, "static void agent_run_task(void *arg)")
require(
    task_body,
    'cJSON_DeleteItemFromObjectCaseSensitive(result, "page_context")',
    "final result never persists page context",
)
if task_body.index(
    'cJSON_DeleteItemFromObjectCaseSensitive(result, "page_context")'
) > task_body.index("cJSON_PrintUnformatted(result)"):
    raise AssertionError("final result serialized before page-context revocation")
for marker in (
    "char *revoked_result_json = s_agent_run_job.result_json;",
    "s_agent_run_job.result_json = NULL;",
    "memset(revoked_result_json, 0, strlen(revoked_result_json));",
):
    require(revoke_runtime_body, marker, "completed result secure drop")


# The obsolete RUN.CHECKPOINT is never resumed, but disabling page context must
# still scrub that field atomically in case a legacy file is awaiting boot
# migration. Its identity-safe terminal deletion is covered separately.
revoke_body = function_body(
    REPOSITORY,
    "esp_err_t si_agent_repository_run_checkpoint_revoke_page_context(void)",
)
for fragment, label in (
    ("si_agent_page_context_strip_checkpoint(", "field-only checkpoint sanitizer"),
    ("agent_repository_ensure_tf_dir_locked();", "mounted TF precondition"),
    (
        "agent_repository_run_checkpoint_read_file_locked(",
        "locked checkpoint file read",
    ),
    (
        "ret = agent_repository_run_checkpoint_read_file_locked(&checkpoint);\n"
        "        if (ret == ESP_ERR_NOT_FOUND) {",
        "missing file handling after mount",
    ),
    (
        "agent_repository_run_checkpoint_write_locked(",
        "locked sanitized checkpoint rewrite",
    ),
):
    require(revoke_body, fragment, label)
if "si_agent_repository_run_checkpoint_clear" in revoke_body:
    raise AssertionError("page-context field revocation clears the whole checkpoint")
if "si_agent_repository_run_checkpoint_read(" in revoke_body or \
        "si_agent_repository_run_checkpoint_write(" in revoke_body:
    raise AssertionError("revocation must not unlock between checkpoint read and write")
if revoke_body.count("xSemaphoreTake(s_agent_history_lock") != 1:
    raise AssertionError("checkpoint revocation must use exactly one repository lock cycle")


print("Runtime V2 page-context revocation contract: PASS")
