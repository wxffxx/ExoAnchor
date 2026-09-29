#!/usr/bin/env python3
"""Guard the PrototypeV2.4 MS power and physical EEPROM safety contract."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(text: str, fragment: str, label: str, failures: list[str]) -> None:
    if fragment not in text:
        failures.append(f"{label}: missing {fragment!r}")


def main() -> int:
    failures: list[str] = []
    defaults = (
        ROOT / "configs/boards/sdkconfig.defaults.exoanchor-prototype-v2.4-ms-test"
    ).read_text(encoding="utf-8")
    driver = (ROOT / "main/drivers/ms2109_test.c").read_text(encoding="utf-8")
    cli = (ROOT / "main/services/diag_cli.c").read_text(encoding="utf-8")
    tool = (ROOT / "tools/ms2109-eeprom-tool.py").read_text(encoding="utf-8")
    http = (
        ROOT / "main/services/web/ms2109_test_http_module.inc"
    ).read_text(encoding="utf-8")
    web_server = (ROOT / "main/services/web_server.c").read_text(encoding="utf-8")
    settings = (ROOT / "main/www/settings.html").read_text(encoding="utf-8")

    for setting in (
        "CONFIG_SI_BOARD_EXOANCHOR_PROTOTYPE_V24_MS_TEST=y",
        "CONFIG_SI_MS2109_TEST_ENABLE=y",
        "# CONFIG_SI_MS2109_POWER_ENABLE is not set",
        "CONFIG_SI_MS2109_SWITCH_GPIO=13",
        "CONFIG_SI_MS2109_CORE_ENABLE_GPIO=18",
        "CONFIG_SI_MS2109_EEPROM_WP_GPIO=16",
        "CONFIG_SI_MS2109_EEPROM_SCL_GPIO=47",
        "CONFIG_SI_MS2109_EEPROM_SDA_GPIO=48",
        "# CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE is not set",
    ):
        require(defaults, setting, "V2.4 board defaults", failures)

    for marker in (
        "set_power_on_sequence_locked",
        "SI_MS2109_POWER_SEQUENCE_3V3_FIRST",
        "SI_MS2109_POWER_SEQUENCE_CORE_PRE_ENABLE",
        "SI_MS2109_MAX_SEQUENCE_DELAY_MS",
        "si_ms2109_test_cycle_power_sequence",
    ):
        require(driver, marker, "MS power sequence experiment", failures)
    require(cli, "ms-switch cycle-seq", "UART power sequence command", failures)

    transaction = driver.split("static esp_err_t transaction_open", 1)[-1]
    require(
        transaction,
        "transaction->restore_power_on = s_status.power_on;",
        "EEPROM power restore",
        failures,
    )
    require(
        transaction,
        "if (!transaction->restore_power_on)",
        "EEPROM powered-bus arbitration",
        failures,
    )
    require(
        transaction,
        "set_power_locked(true)",
        "EEPROM pull-up rail enable",
        failures,
    )
    require(
        driver,
        ".flags.enable_internal_pullup = true",
        "EEPROM I2C pull-up fallback",
        failures,
    )
    require(driver, "prepare_eeprom_lines_locked()", "I2C bus recovery", failures)
    require(
        driver,
        "SI_MS2109_I2C_RECOVERY_PULSES",
        "I2C recovery clock count",
        failures,
    )
    require(driver, "probe_locked(&transaction", "pre-program probe gate", failures)
    require(driver, "set_write_protect_locked(true)", "EEPROM WP restore", failures)
    require(driver, "memcmp(image, readback, size)", "device readback", failures)

    line_match = re.search(r"#define DIAG_CLI_LINE_MAX\s+(\d+)", cli)
    if not line_match or int(line_match.group(1)) < 2800:
        failures.append("UART0 line buffer cannot carry a full 2 KiB Base64 image")
    for command in ("ms-switch", "ms-eeprom", "program-b64", "CONFIRM"):
        require(cli, command, "UART0 MS command", failures)

    backup_at = tool.find("existing = dump_eeprom(console)")
    program_at = tool.find("ms-eeprom program-b64")
    verify_at = tool.find("readback = dump_eeprom(console)")
    if min(backup_at, program_at, verify_at) < 0 or not (
        backup_at < program_at < verify_at
    ):
        failures.append("host tool must back up, program, then independently read back")
    require(tool, "len(image) != EEPROM_SIZE", "host image-size gate", failures)
    require(tool, "readback != image", "host byte comparison", failures)
    require(tool, "UART_LONG_WRITE_CHUNK", "paced UART EEPROM upload", failures)
    require(tool, "self.serial.dtr = False", "released CH343 DTR", failures)
    require(tool, "self.serial.rts = False", "released CH343 RTS", failures)

    for endpoint in (
        "/api/ms2109/status",
        "/api/ms2109/power",
        "/api/ms2109/eeprom/probe",
        "/api/ms2109/eeprom/read",
        "/api/ms2109/eeprom/program",
    ):
        require(web_server, endpoint, "MS2109 HTTP route", failures)
    for marker in (
        "SI_MS2109_AT24C16_SIZE",
        "PROGRAM MS2109 EEPROM",
        "si_ms2109_test_program_eeprom",
        "readback_verification",
    ):
        require(http, marker, "MS2109 HTTP safety", failures)
    for marker in (
        'id="ms2109PowerControl" class="ui-setting-row" data-ms2109-control hidden',
        'id="ms2109PowerToggle"',
        "capabilities.ms2109_power??capabilities.ms2109_test",
    ):
        require(settings, marker, "MS2109 power-only settings UI", failures)
    for marker in (
        "ms2109Eeprom",
        "/api/ms2109/eeprom/",
        "PROGRAM MS2109 EEPROM",
        "AT24C16",
    ):
        if marker in settings:
            failures.append(
                f"0.87.5 Settings exposes removed EEPROM control: {marker!r}"
            )

    if failures:
        print("PrototypeV2.4 MS test contract failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print("PrototypeV2.4 MS test contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
