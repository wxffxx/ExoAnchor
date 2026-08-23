#!/usr/bin/env python3
"""Contract for bounded, off-worker conversation history persistence."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
HEADER_PATH = ROOT / "main/infrastructure/agent_history_writer.h"
SOURCE_PATH = ROOT / "main/infrastructure/agent_history_writer.c"


def read(path: Path) -> str:
    if not path.is_file():
        raise AssertionError(f"missing conversation history writer: {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8")


def body(source: str, signature: str) -> str:
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
                return source[start:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


HEADER = read(HEADER_PATH)
SOURCE = read(SOURCE_PATH)
DISPATCH = read(ROOT / "main/services/web/agent_tool_dispatch_module.inc")
ENGINE = read(ROOT / "main/services/web/agent_run_engine_module.inc")
TASK = read(ROOT / "main/services/web/agent_run_task_module.inc")
REQUEST = read(ROOT / "main/services/web/agent_request_module.inc")
WEB = read(ROOT / "main/services/web_server.c")
CMAKE = read(ROOT / "main/CMakeLists.txt")


# This is a deliberately small queue. It bounds both memory consumption and
# the amount of work a shutdown/recovery path can inherit.
capacity_match = re.search(
    r"#define\s+SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY\s+(\d+)U?", HEADER
)
if not capacity_match:
    raise AssertionError("history writer queue capacity is not a public constant")
capacity = int(capacity_match.group(1))
if not 2 <= capacity <= 8:
    raise AssertionError(f"history writer queue is not tightly bounded: {capacity}")
wait_match = re.search(
    r"#define\s+SI_AGENT_HISTORY_WRITER_DEFAULT_WAIT_MS\s+(\d+)U?", HEADER
)
if not wait_match or not 1 <= int(wait_match.group(1)) <= 5000:
    raise AssertionError("history writer result wait is absent or not tightly bounded")

for marker in (
    "turn_id",
    "run_id",
    "user_content",
    "assistant_content",
    "bool saved;",
    "esp_err_t user_status;",
    "esp_err_t assistant_status;",
    "si_agent_history_writer_start(",
    "si_agent_history_writer_store_turn(",
):
    if marker not in HEADER:
        raise AssertionError(f"history writer public contract missing: {marker}")

if '"infrastructure/agent_history_writer.c"' not in CMAKE:
    raise AssertionError("history writer is absent from the embedded-Agent composition")

# Startup order is repository mutex -> writer -> run manager. A run can never
# publish history work into an uninitialised queue.
web_start = body(WEB, "esp_err_t si_web_server_start(void)")
repository_start = web_start.find("agent_history_start()")
writer_start = web_start.find("si_agent_history_writer_start()")
run_start = web_start.find("agent_run_start()")
if not 0 <= repository_start < writer_start < run_start:
    raise AssertionError("history writer startup is not between repository and run startup")

start = body(SOURCE, "esp_err_t si_agent_history_writer_start(")
for marker in (
    "xQueueCreate(",
    "SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY",
    "sizeof(uint8_t)",
    "xTaskCreateWithCaps(",
    "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT",
    "if (s_writer_task && s_writer_queue && s_writer_turns && s_writer_lock)",
    "si_agent_repository_lock()",
):
    if marker not in start:
        raise AssertionError(f"bounded external-stack writer startup missing: {marker}")
if "portMAX_DELAY" in start:
    raise AssertionError("history writer startup contains an unbounded wait")
if not re.search(
    r"static\s+history_writer_slot_t\s+s_writer_slots\s*\[\s*"
    r"SI_AGENT_HISTORY_WRITER_QUEUE_CAPACITY\s*\]", SOURCE
):
    raise AssertionError("history writer control slots are not a fixed bounded array")
if not re.search(
    r"s_writer_turns\s*=\s*heap_caps_calloc\([\s\S]*?"
    r"MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT", start
):
    raise AssertionError("large bounded history turn payloads are not allocated in PSRAM")

store = body(SOURCE, "esp_err_t si_agent_history_writer_store_turn(")
for marker in (
    "turn->conversation_history_enabled",
    "ESP_ERR_NOT_SUPPORTED",
    "ESP_ERR_TIMEOUT",
    "xQueueSend(",
):
    if marker not in store:
        raise AssertionError(f"history writer admission/status contract missing: {marker}")
if "portMAX_DELAY" in store:
    raise AssertionError("provider-facing history RPC can wait forever")
if any(call in store for call in (
    "agent_history_file_", "fopen(", "fwrite(", "fflush(", "stat(",
    "malloc(", "calloc(", "realloc("
)):
    raise AssertionError("provider-facing history RPC performs repository/TF I/O")
for marker in (
    "xQueueSend(s_writer_queue, &slot_index, 0)",
    "pdMS_TO_TICKS(wait_ms)",
    "HISTORY_WRITER_RESULT_LOCK_WAIT_MS",
    '"history writer queue full"',
    '"history writer completion timeout"',
    "result->pending = true;",
):
    if marker not in store:
        raise AssertionError(f"bounded queue/wait failure semantics missing: {marker}")

# The storage task is the sole owner of repository calls. It de-duplicates
# stable role records, then writes user before assistant so a partial write is
# recoverable without reversing a conversation.
writer = body(SOURCE, "static void history_writer_task(")
for marker in (
    "agent_history_file_ensure_ready(",
    "agent_history_file_for_each_locked(",
    "agent_history_file_append_locked(",
    'history_record_id(user_record_id,',
    'history_record_id(assistant_record_id,',
    'turn, "user")',
    'turn,\n                      "assistant")',
):
    if marker not in SOURCE:
        raise AssertionError(f"durable/idempotent history writer missing: {marker}")

write_turn = body(SOURCE, "static esp_err_t history_write_turn_locked(")
user_gate = write_turn.find("if (!scan.user_found)")
user_append = write_turn.find("history_append_record_locked(", user_gate)
assistant_gate = write_turn.find("if (!scan.assistant_found)", user_append)
assistant_append = write_turn.find("history_append_record_locked(", assistant_gate)
user_durable = write_turn.find("result->user_status = ESP_OK;", user_append)
assistant_durable = write_turn.find(
    "result->assistant_status = ESP_OK;", assistant_append
)
saved = write_turn.find("result->saved = true;", assistant_append)
if not (0 <= user_gate < user_append < user_durable < assistant_gate <
        assistant_append < assistant_durable < saved):
    raise AssertionError("history writer does not durably append user before assistant")
for marker in (
    "history_scan_ids_locked(&scan)",
    "scan.assistant_found && !scan.user_found",
    "history_record_present_after_error_locked(",
    "result->user_status = ESP_OK;",
    "result->assistant_status = ESP_OK;",
):
    if marker not in write_turn:
        raise AssertionError(f"history retry/durability proof missing: {marker}")

for forbidden in (
    "nvs_",
    "settings_store",
    "device_settings.h",
    "si_conversation_history_is_enabled",
    "si_product_feature_settings_set",
    "si_auth_",
    "si_video_",
    "si_hid_",
    "httpd_",
    "esp_http_",
    "esp_flash",
    "spi_flash",
    "cache_hal",
    "esp_cache_msync",
    "Cache_Disable",
):
    if forbidden in SOURCE:
        raise AssertionError(f"history writer task crosses forbidden boundary: {forbidden}")

for marker in (
    "static void history_writer_reap_abandoned(",
    "HISTORY_WRITER_REAP_INTERVAL_MS",
    "!__atomic_load_n(&slot->caller_waiting",
    "slot->state = HISTORY_SLOT_FREE;",
    "uxTaskGetStackHighWaterMark(NULL)",
):
    if marker not in SOURCE:
        raise AssertionError(f"writer timeout/recovery lifecycle missing: {marker}")

record_id = body(SOURCE, "static void history_record_id(")
for marker in ("turn->run_id", "turn->turn_id", "role"):
    if marker not in record_id:
        raise AssertionError(f"stable record id omits identity field: {marker}")

# RAM-only recovery remains an explicit drop. Normal product execution can
# only use the writer RPC; it cannot silently fall back to repository/FATFS.
store_run_turn = body(DISPATCH, "static esp_err_t agent_history_store_run_turn(")
ram_guard = store_run_turn.find("#if AGENT_RUNTIME_RECOVERY_RAM_ONLY")
normal_branch = store_run_turn.find("#else", ram_guard)
guard_end = store_run_turn.find("#endif", normal_branch)
if not 0 <= ram_guard < normal_branch < guard_end:
    raise AssertionError("chat persistence lacks explicit RAM-only/normal branches")
ram_only = store_run_turn[ram_guard:normal_branch]
normal = store_run_turn[normal_branch:guard_end]
if "return ESP_ERR_NOT_SUPPORTED;" not in ram_only:
    raise AssertionError("RAM-only recovery no longer explicitly drops chat persistence")
if "si_agent_history_writer_store_turn(" not in normal:
    raise AssertionError("normal chat persistence does not use the dedicated writer")
if "si_conversation_history_is_enabled()" not in normal:
    raise AssertionError("normal chat persistence does not enforce the feature switch")
for forbidden in ("agent_history_file_", "fopen(", "fwrite(", "fflush("):
    if forbidden in normal:
        raise AssertionError(f"provider helper directly touches TF/repository: {forbidden}")

# A truthful UI status is part of the backend contract. The success bit cannot
# be inferred from enqueue alone or from one durable role.
status = body(DISPATCH, "static void agent_history_add_run_status_json(")
for marker in ("write_result->saved", "write_result->user_status",
               "write_result->assistant_status", "!write_result->pending",
               '"history_saved"', '"history_pending"'):
    if marker not in status:
        raise AssertionError(f"history_saved durable-pair proof missing: {marker}")

run_worker = body(TASK, "static void agent_run_task(void *arg)")
history_call = run_worker.find("agent_history_store_run_turn(run, result, aborted)")
serialize = run_worker.find("result_json = cJSON_PrintUnformatted(result)")
publish = run_worker.find("s_agent_run_job.result_json = result_json")
if not 0 <= history_call < serialize < publish:
    raise AssertionError("run result is published before durable history status is known")

# No provider/run module may bypass the writer. Manual History HTTP APIs are a
# separate explicit user request and remain allowed to use the repository.
for name, source in (
    ("agent_run_engine_module.inc", ENGINE),
    ("agent_run_task_module.inc", TASK),
):
    for forbidden in ("agent_history_file_", "fopen(", "fwrite(", "fflush("):
        if forbidden in source:
            raise AssertionError(f"{name} directly touches TF/repository: {forbidden}")

if "#define AGENT_RUNTIME_RECOVERY_RAM_ONLY 0" not in REQUEST:
    raise AssertionError("normal product composition is not exercising the writer path")

if "agent_history_append_chat_record(" in ENGINE or \
        "agent_history_append_chat_record(" in DISPATCH:
    raise AssertionError("retired synchronous per-record history helper returned")

writer_calls = []
for path in (ROOT / "main").rglob("*"):
    if path.suffix not in (".c", ".inc") or path == SOURCE_PATH:
        continue
    source = path.read_text(encoding="utf-8")
    if "si_agent_history_writer_store_turn(" in source:
        writer_calls.append(path.relative_to(ROOT).as_posix())
if writer_calls != ["main/services/web/agent_tool_dispatch_module.inc"]:
    raise AssertionError(
        f"automatic history has multiple or unexpected writer callers: {writer_calls}"
    )

print("Agent history writer contract: PASS")
