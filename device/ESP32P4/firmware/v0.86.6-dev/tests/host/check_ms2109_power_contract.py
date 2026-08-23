#!/usr/bin/env python3
"""Guard the production TypeC MS2109 boot-power contract."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def require(text: str, fragment: str, label: str) -> None:
    if fragment not in text:
        raise AssertionError(f"{label}: missing {fragment!r}")


defaults = (
    ROOT / "configs/boards/sdkconfig.defaults.exoanchor-production-typec"
).read_text(encoding="utf-8")
driver = (ROOT / "main/drivers/ms2109_power.c").read_text(encoding="utf-8")
header = (ROOT / "main/drivers/ms2109_power.h").read_text(encoding="utf-8")
app = (ROOT / "main/app/app_main.c").read_text(encoding="utf-8")
build = (ROOT / "tools/build-firmware.sh").read_text(encoding="utf-8")
http = (
    ROOT / "main/services/web/ms2109_test_http_module.inc"
).read_text(encoding="utf-8")
web_server = (ROOT / "main/services/web_server.c").read_text(encoding="utf-8")
device_http = (ROOT / "main/services/device_http.c").read_text(encoding="utf-8")

for setting in (
    "CONFIG_SI_BOARD_EXOANCHOR_PRODUCTION_TYPEC=y",
    "CONFIG_SI_MS2109_POWER_ENABLE=y",
    "CONFIG_SI_MS2109_SWITCH_GPIO=17",
    "CONFIG_SI_MS2109_CORE_ENABLE_GPIO=18",
    "CONFIG_SI_MS2109_POWER_SEQUENCE_DELAY_MS=10",
    "CONFIG_SI_MS2109_EEPROM_WP_GPIO=-1",
    "CONFIG_SI_POWER_LOCATOR_GPIO=19",
    "CONFIG_SI_POWER_LOCATOR_ACTIVE_HIGH=y",
    "# CONFIG_SI_MS2109_TEST_ENABLE is not set",
    "# CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE is not set",
):
    require(defaults, setting, "TypeC defaults")

power_sequence = driver.split("static esp_err_t set_power_locked", 1)[-1]
power_sequence = power_sequence.split("esp_err_t si_ms2109_power_init", 1)[0]
on_sequence, off_sequence = power_sequence.split("} else {", 1)
enable_3v3 = on_sequence.find("SI_CFG_MS2109_SWITCH_GPIO")
on_delay = on_sequence.find("vTaskDelay")
enable_1v2 = on_sequence.find("SI_CFG_MS2109_CORE_ENABLE_GPIO", on_delay)
if min(enable_3v3, on_delay, enable_1v2) < 0 or not (
    enable_3v3 < on_delay < enable_1v2
):
    raise AssertionError("MS2109 driver must enable 3.3 V, wait, then enable 1.2 V")

disable_1v2 = off_sequence.find("SI_CFG_MS2109_CORE_ENABLE_GPIO")
off_delay = off_sequence.find("vTaskDelay")
disable_3v3 = off_sequence.find("SI_CFG_MS2109_SWITCH_GPIO", off_delay)
if min(disable_1v2, off_delay, disable_3v3) < 0 or not (
    disable_1v2 < off_delay < disable_3v3
):
    raise AssertionError("MS2109 driver must disable 1.2 V, wait, then disable 3.3 V")

require(driver, "invalid MS2109 power GPIO map", "GPIO validation")
require(driver, "MS2109 rails enabled", "boot evidence log")
for api in (
    "si_ms2109_power_get_status(si_ms2109_power_status_t *status)",
    "si_ms2109_power_set(bool on)",
    "si_ms2109_power_cycle(uint32_t off_time_ms)",
):
    require(header, api, "production power API",)
    require(driver, api, "production power implementation")
for marker in (
    "xSemaphoreCreateMutex()",
    "xSemaphoreTake(s_lock",
    "*status = (si_ms2109_power_status_t) {",
    ".last_result = ESP_ERR_TIMEOUT,",
    "off_time_ms >= 10U && off_time_ms <= 5000U",
    "s_status.operation_count++",
):
    require(driver, marker, "thread-safe production power control")
require(app, "si_ms2109_power_init()", "composition root")
require(build, "Production TypeC build is missing MS2109 power sequencing", "build gate")
require(build, "CONFIG_SI_MS2109_EEPROM_WP_GPIO=-1", "EEPROM exclusion gate")

require(
    http,
    "#if SI_CFG_MS2109_POWER_ENABLED || SI_CFG_MS2109_TEST_ENABLED",
    "shared power HTTP gate",
)
for marker in (
    "si_ms2109_power_get_status(&status)",
    "si_ms2109_power_set(true)",
    "si_ms2109_power_set(false)",
    "si_ms2109_power_cycle(off_time_ms)",
):
    require(http, marker, "production power HTTP dispatch")

power_routes = web_server.split(
    "#if SI_CFG_MS2109_POWER_ENABLED || SI_CFG_MS2109_TEST_ENABLED", 1
)[-1].split("#endif", 1)[0]
for endpoint in ("/api/ms2109/status", "/api/ms2109/power"):
    require(power_routes, endpoint, "shared production/test power route")
if "/api/ms2109/eeprom/" in power_routes:
    raise AssertionError("EEPROM routes escaped the V2.4 test-only gate")

eeprom_routes = web_server.split("#if SI_CFG_MS2109_TEST_ENABLED", 1)[-1].split(
    "#endif", 1
)[0]
for endpoint in (
    "/api/ms2109/eeprom/probe",
    "/api/ms2109/eeprom/read",
    "/api/ms2109/eeprom/program",
):
    require(eeprom_routes, endpoint, "V2.4-only EEPROM route")

for marker in (
    'cJSON_AddObjectToObject(root, "ms2109_power")',
    '"power_control"',
    '"power_cycle"',
    '"/api/ms2109/status"',
):
    require(device_http, marker, "production power capability")

print("Production TypeC MS2109 power contract: PASS")
