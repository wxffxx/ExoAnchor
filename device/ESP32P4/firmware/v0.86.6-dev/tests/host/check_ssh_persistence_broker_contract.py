#!/usr/bin/env python3
"""Guard the cache-disable-safe SSH TOFU persistence boundary."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
CLIENT = (ROOT / "main/services/ssh_client.c").read_text(encoding="utf-8")
BROKER = (ROOT / "main/services/ssh_hostkey_store.c").read_text(encoding="utf-8")
CMAKE = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")

failures: list[str] = []

if '"services/ssh_hostkey_store.c"' not in CMAKE:
    failures.append("SSH host-key broker is not compiled into firmware")

for forbidden in ("si_settings_store_", "nvs_"):
    if forbidden in CLIENT:
        failures.append(f"PSRAM SSH worker still accesses persistence via {forbidden}")

for marker in (
    '#include "ssh_hostkey_store.h"',
    "si_ssh_hostkey_verify_or_trust(",
    'ESP_LOGW(TAG, "Trusted first SSH host key for %s:%u"',
):
    if marker not in CLIENT:
        failures.append(f"SSH client broker integration missing {marker}")

if re.search(r"ESP_LOG.\([^\n]*fingerprint", CLIENT, re.IGNORECASE):
    failures.append("SSH client logs a host-key fingerprint")

for marker in (
    "SI_SSH_HOSTKEY_BROKER_STACK_SIZE 4096",
    "SI_SSH_HOSTKEY_BROKER_WAIT_MS 5000U",
    "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT",
    "heap_caps_calloc(",
    "xSemaphoreCreateBinaryWithCaps(",
    "xTaskCreatePinnedToCore(",
    "vSemaphoreDeleteWithCaps(",
    "vTaskDelete(NULL)",
    "__atomic_sub_fetch(",
    "__atomic_compare_exchange_n(&s_ssh_hostkey_broker_active",
    "__atomic_store_n(&s_ssh_hostkey_broker_active, false",
    "esp_ptr_in_dram((const void *)&stack_probe)",
    "esp_ptr_in_dram((const void *)request)",
    "si_settings_store_open_read(",
    "si_settings_store_open_write(",
    "si_settings_store_commit(",
    "ssh_hostkey_secure_clear(request, sizeof(*request))",
):
    if marker not in BROKER:
        failures.append(f"SSH host-key broker safety marker missing {marker}")

if "portMAX_DELAY" in BROKER:
    failures.append("SSH host-key broker has an unbounded wait")
if "xTaskCreatePinnedToCoreWithCaps(" in BROKER or "vTaskDeleteWithCaps(NULL)" in BROKER:
    failures.append("SSH host-key broker retains the abort-prone WithCaps self-delete path")

for marker in (
    "TaskHandle_t task = NULL;",
    "vTaskDeleteWithCaps(task);",
    "&shell->task, tskNO_AFFINITY",
    "vTaskDeleteWithCaps(shell->task);",
    "xSemaphoreGive(ctx->done);\n    for (;;) {\n        vTaskSuspend(NULL);",
    "xSemaphoreGive(shell->done);\n    for (;;) {\n        vTaskSuspend(NULL);",
):
    if marker not in CLIENT:
        failures.append(f"SSH PSRAM worker owner-delete marker missing {marker}")
if "vTaskDeleteWithCaps(NULL)" in CLIENT:
    failures.append("SSH PSRAM worker retains the abort-prone WithCaps self-delete path")

for marker in (
    "si_ssh_exec_cancel_cb_t cancel_cb,",
    "if (cancel_cb && cancel_cb(cancel_user_ctx))",
    "ssh_shell_cancel_requested, shell",
    "deadline, ssh_shell_cancel_requested, shell",
):
    if marker not in CLIENT:
        failures.append(f"SSH connect/auth cancellation marker missing {marker}")

read_pos = BROKER.find("si_settings_store_open_read(")
write_pos = BROKER.find("si_settings_store_open_write(")
if read_pos < 0 or write_pos < 0 or read_pos >= write_pos:
    failures.append("SSH TOFU broker must read before first-write")

request_match = re.search(
    r"typedef struct \{(?P<body>.*?)\} ssh_hostkey_request_t;", BROKER, re.DOTALL
)
if not request_match:
    failures.append("SSH host-key broker request is missing")
elif "const char *" in request_match.group("body") or "char *" in request_match.group("body"):
    failures.append("SSH host-key broker request retains caller memory pointers")

if failures:
    raise AssertionError("\n".join(failures))

print("ssh persistence broker contract: PASS")
