#!/usr/bin/env python3
"""Guard cold-boot network recovery and multi-device flash selection."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    manager = (ROOT / "main/drivers/net_manager.c").read_text(encoding="utf-8")
    settings = (ROOT / "main/application/network_settings.c").read_text(
        encoding="utf-8"
    )
    settings_store = (ROOT / "main/infrastructure/settings_store.c").read_text(
        encoding="utf-8"
    )
    defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
    build = (ROOT / "tools/build-firmware.sh").read_text(encoding="utf-8")
    flash = (ROOT / "tools/flash-monitor.sh").read_text(encoding="utf-8")
    failures: list[str] = []

    for marker in (
        "status != ESP_NETIF_DHCP_STOPPED",
        "apply_config(&s_factory_config)",
        'update_config_status_locked(&s_factory_config, "recovery")',
        "s_status.recovery_active = true",
        "esp_eth_start(s_eth_handle)",
    ):
        if marker not in manager:
            failures.append(f"network cold-boot recovery marker missing: {marker}")

    for marker in (
        "#define NETWORK_SETTINGS_WORKER_STACK 3072U",
        "rollback_settings_on_internal_stack",
        "current_task_stack_is_internal",
        "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT",
        'network_settings_rollback_task, "si_net_nvs"',
        "caller != s_manager_task",
        "ulTaskNotifyTake(pdTRUE, portMAX_DELAY)",
        "xTaskNotifyGive(caller)",
        "uxTaskGetStackHighWaterMark(NULL)",
    ):
        if marker not in manager:
            failures.append(f"internal network persistence worker missing: {marker}")
    if manager.count("rollback_settings_on_internal_stack();") < 2:
        failures.append("automatic timeout/ACD rollback does not use internal worker")
    manager_creation = manager.split("if (!s_manager_task) {", 1)[1].split(
        'ESP_LOGI(TAG,\n             "Network ready', 1
    )[0]
    if "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" not in manager_creation:
        failures.append("H.264 network manager no longer preserves Agent internal heap")
    worker_body = manager.split(
        "static void network_settings_rollback_task(void *opaque)", 1
    )[1].split("static esp_err_t rollback_settings_on_internal_stack", 1)[0]
    if "si_network_settings_rollback()" not in worker_body:
        failures.append("internal network worker does not own the NVS rollback")
    if "vTaskDeleteWithCaps" in worker_body or "vTaskSuspend(NULL)" not in worker_body:
        failures.append("network worker does not defer WithCaps deletion to manager")
    worker_call = manager.split(
        "static esp_err_t rollback_settings_on_internal_stack(void)", 1
    )[1].split("static void set_last_error", 1)[0]
    if "vTaskDeleteWithCaps(worker);" not in worker_call:
        failures.append("network worker is not reclaimed by non-self deletion")
    if worker_call.index("vTaskDeleteWithCaps(worker);") < worker_call.index(
        "ulTaskNotifyTake(pdTRUE, portMAX_DELAY)"
    ):
        failures.append("network worker is deleted before its result is published")
    acd_body = manager.split("static void finish_static_acd(", 1)[1].split(
        "static void eth_event_handler", 1
    )[0]
    if "si_network_settings_rollback()" in acd_body:
        failures.append("PSRAM ACD path still enters NVS directly")
    manager_body = manager.split("static void manager_task(void *arg)", 1)[1].split(
        "esp_err_t si_net_init(void)", 1
    )[0]
    if "si_network_settings_rollback()" in manager_body:
        failures.append("PSRAM timeout path still enters NVS directly")
    timeout_body = manager_body.split("if (timeout) {", 1)[1].split(
        "poll_static_acd();", 1
    )[0]
    for marker in (
        "xSemaphoreTake(s_operation_lock, portMAX_DELAY)",
        "still_expired",
        "s_pending_deadline_us <= esp_timer_get_time()",
        "rollback_pending_locked()",
    ):
        if marker not in timeout_body:
            failures.append(f"exact one-shot automatic rollback missing: {marker}")
    if timeout_body.index("s_pending_deadline_us = 0;") > timeout_body.index(
        "rollback_pending_locked()"
    ):
        failures.append("automatic rollback is not one-shot before persistence")

    for marker in (
        "s_cached_status",
        "publish_cached_status(&status)",
        "current_task_can_access_flash",
        "esp_ptr_in_dram((const void *)&stack_probe)",
    ):
        if marker not in settings:
            failures.append(f"PSRAM-safe network settings marker missing: {marker}")
    get_body = settings.split(
        "esp_err_t si_network_settings_get(si_network_settings_status_t *status_out)",
        1,
    )[1].split("esp_err_t si_network_settings_stage", 1)[0]
    if "copy_cached_status(status_out)" not in get_body:
        failures.append("network settings getter does not use the runtime snapshot")
    if "si_settings_store_" in get_body:
        failures.append("network settings getter still accesses NVS")
    if settings.count("REQUIRE_FLASH_SAFE_STACK();") < 6:
        failures.append("network settings persistence is not guarded on every path")

    for marker in (
        "current_task_can_access_flash",
        "esp_ptr_in_dram((const void *)&stack_probe)",
        "esp_ptr_in_rtc_dram_fast((const void *)&stack_probe)",
    ):
        if marker not in settings_store:
            failures.append(f"shared settings-store stack guard missing: {marker}")
    if settings_store.count("!current_task_can_access_flash()") < 2:
        failures.append("settings store does not guard both open and active handles")

    if 'PORT="${ESPPORT:-/dev/' in flash:
        failures.append("flash helper still has a hard-coded serial device")
    for marker in (
        "Multiple USB serial devices found; refusing automatic selection",
        "Serial device is already in use",
        'printf \'  %s\\n\' "${serial_candidates[@]}"',
        "Refusing Ethernet firmware without CONFIG_LWIP_AUTOIP",
    ):
        if marker not in flash:
            failures.append(f"multi-device flash safety marker missing: {marker}")

    if "CONFIG_LWIP_AUTOIP=y" not in defaults:
        failures.append("firmware defaults do not enable DHCP/AutoIP cooperation")
    if "Ethernet build is missing CONFIG_LWIP_AUTOIP" not in build:
        failures.append("build helper does not reject Ethernet without AutoIP")

    if failures:
        print("network boot contract failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print("network boot and flash target contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
