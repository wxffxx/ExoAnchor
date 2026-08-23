#!/usr/bin/env python3
"""Guard the PrototypeV2.4 product profile and its destructive-surface boundary."""

from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(text: str, fragment: str, label: str, failures: list[str]) -> None:
    if fragment not in text:
        failures.append(f"{label}: missing {fragment!r}")


def main() -> int:
    failures: list[str] = []
    defaults = (
        ROOT / "configs/boards/sdkconfig.defaults.exoanchor-prototype-v2.4"
    ).read_text(encoding="utf-8")
    kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
    cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
    build = (ROOT / "tools/build-firmware.sh").read_text(encoding="utf-8")
    cli = (ROOT / "main/services/diag_cli.c").read_text(encoding="utf-8")
    web_server = (ROOT / "main/services/web_server.c").read_text(encoding="utf-8")
    device_http = (ROOT / "main/services/device_http.c").read_text(encoding="utf-8")
    settings = (ROOT / "main/www/settings.html").read_text(encoding="utf-8")

    for setting in (
        "CONFIG_SI_BOARD_EXOANCHOR_PROTOTYPE_V24=y",
        "CONFIG_SI_ETH_PHY_DP83825=y",
        "CONFIG_SI_MS2109_POWER_ENABLE=y",
        "CONFIG_SI_MS2109_SWITCH_GPIO=13",
        "CONFIG_SI_MS2109_CORE_ENABLE_GPIO=18",
        "CONFIG_SI_MS2109_POWER_SEQUENCE_DELAY_MS=10",
        "CONFIG_SI_MS2109_EEPROM_WP_GPIO=16",
        "CONFIG_SI_MS2109_EEPROM_SCL_GPIO=47",
        "CONFIG_SI_MS2109_EEPROM_SDA_GPIO=48",
        "# CONFIG_SI_MS2109_TEST_ENABLE is not set",
        "# CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE is not set",
        "CONFIG_SI_POWER_BUTTON_GPIO=4",
        "CONFIG_SI_RESET_BUTTON_GPIO=5",
        "CONFIG_SI_POWER_DETECT_GPIO=0",
        "CONFIG_SI_STANDBY_DETECT_GPIO=1",
        "CONFIG_SI_POWER_LOCATOR_GPIO=17",
        "CONFIG_SI_POWER_LOCATOR_RETURN_GPIO=-1",
        "CONFIG_SI_TF_CARD_DETECT_GPIO=-1",
        "CONFIG_SI_TARGET_UART_ENABLE=y",
        "CONFIG_SI_TARGET_UART_PORT=1",
        "CONFIG_SI_TARGET_UART_RX_GPIO=50",
        "CONFIG_SI_TARGET_UART_TX_GPIO=51",
        "CONFIG_SI_HID_USB_DM_GPIO=26",
        "CONFIG_SI_HID_USB_DP_GPIO=27",
    ):
        require(defaults, setting, "V2.4 product defaults", failures)

    for fragment in (
        "config SI_BOARD_EXOANCHOR_PROTOTYPE_V24",
        'default "exoanchor-prototype-v2.4" if SI_BOARD_EXOANCHOR_PROTOTYPE_V24',
        'default "ExoAnchor PrototypeV2.4" if SI_BOARD_EXOANCHOR_PROTOTYPE_V24',
        'default "exoanchor-v24" if SI_BOARD_EXOANCHOR_PROTOTYPE_V24',
        'default "ExoAnchor PrototypeV2.4 HID" if SI_BOARD_EXOANCHOR_PROTOTYPE_V24',
        "default y if SI_BOARD_EXOANCHOR_PRODUCTION_TYPEC || SI_BOARD_EXOANCHOR_PRODUCTION_TYPEW || SI_BOARD_EXOANCHOR_PROTOTYPE_V24",
        "default 13 if SI_BOARD_EXOANCHOR_PROTOTYPE_V24 || SI_BOARD_EXOANCHOR_PROTOTYPE_V24_MS_TEST",
        "default 47 if SI_BOARD_EXOANCHOR_PROTOTYPE_V23 || SI_BOARD_EXOANCHOR_PROTOTYPE_V24 || SI_BOARD_EXOANCHOR_PROTOTYPE_V24_MS_TEST",
        "default 48 if SI_BOARD_EXOANCHOR_PROTOTYPE_V23 || SI_BOARD_EXOANCHOR_PROTOTYPE_V24 || SI_BOARD_EXOANCHOR_PROTOTYPE_V24_MS_TEST",
    ):
        require(kconfig, fragment, "V2.4 Kconfig identity/composition", failures)

    rev3_case = build.split('case "$BOARD" in', 1)[-1].split(";;", 1)[0]
    require(
        rev3_case,
        "exoanchor-prototype-v2.4|",
        "V2.4 esp32p4-rev3 allowlist",
        failures,
    )
    require(rev3_case, 'SILICON="esp32p4-rev3"', "V2.4 silicon map", failures)
    for fragment in (
        "PrototypeV2.4 product build is missing MS2109 power sequencing",
        "refusing PrototypeV2.4 product build with MS2109 EEPROM test/emulator enabled",
        "PrototypeV2.4 product profile mismatch",
        'CONFIG_SI_SILICON_TARGET="esp32p4-rev3"',
        "CONFIG_ESP32P4_REV_MIN_300=y",
        "product build contains excluded MS2109 EEPROM symbol",
        "product build contains excluded MS2109 EEPROM surface marker",
    ):
        require(build, fragment, "V2.4 build/composition gate", failures)

    base_sources = cmake.split(
        "if(CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE)", 1
    )[0]
    for source in ("drivers/ms2109_test.c", "drivers/ms2109_eeprom_emulator.c"):
        if source in base_sources:
            failures.append(f"product source list unconditionally includes {source}")
    require(
        cmake,
        'if(CONFIG_SI_MS2109_POWER_ENABLE)\n    list(APPEND SI_MAIN_SRCS "drivers/ms2109_power.c")',
        "production MS2109 source gate",
        failures,
    )
    require(
        cmake,
        'if(CONFIG_SI_MS2109_TEST_ENABLE)\n    list(APPEND SI_MAIN_SRCS "drivers/ms2109_test.c")',
        "destructive test source gate",
        failures,
    )

    require(
        cli,
        "#if SI_CFG_MS2109_EEPROM_WP_GPIO >= 0\nstatic void run_eeprom_wp_command",
        "UART EEPROM WP GPIO gate",
        failures,
    )
    for fragment in (
        'printf("  eeprom-wp status',
        'strcmp(action, "write-enable") == 0',
        'strcmp(action, "protect") == 0',
        "ret = gpio_input_enable(gpio);",
        "ret = ESP_ERR_INVALID_RESPONSE;",
    ):
        require(cli, fragment, "V2.4 UART EEPROM WP control", failures)
    for fragment in (
        "if (SI_CFG_MS2109_POWER_ENABLED || SI_CFG_MS2109_TEST_ENABLED)",
        '"ms2109_power_switch", "MS2109 3V3 POWER", "ms2109", "output"',
        '"ms2109_core_enable", "MS2109 1V2 ENABLE", "ms2109", "output"',
        '"ms2109_eeprom_wp", "MS2109 EEPROM WP", "ms2109", "output"',
        '"ms2109_eeprom_scl", "MS2109 EEPROM SCL", "ms2109", "inout"',
        '"ms2109_eeprom_sda", "MS2109 EEPROM SDA", "ms2109", "inout"',
    ):
        require(device_http, fragment, "V2.4 GPIO matrix ownership", failures)
    eeprom_routes = web_server.split("#if SI_CFG_MS2109_TEST_ENABLED", 1)[-1].split(
        "#endif", 1
    )[0]
    for endpoint in (
        "/api/ms2109/eeprom/probe",
        "/api/ms2109/eeprom/read",
        "/api/ms2109/eeprom/program",
    ):
        require(eeprom_routes, endpoint, "test-only EEPROM HTTP gate", failures)

    for fragment in (
        "capabilities.ms2109_power??capabilities.ms2109_test",
        'ms:"device",ms2109:"device"',
        'id="ms2109PowerControl" class="ui-setting-row" data-ms2109-control hidden',
        "settingsLocatorReverseRow.hidden=!reported",
        "const locator=lastSettingsLocator,reported=!!power.locator,supported=!!locator.supported,bidirectional=!!locator.bidirectional,configurable=supported&&bidirectional",
    ):
        require(settings, fragment, "V2.4 Settings capability gate", failures)
    for forbidden_surface in (
        "ms2109Eeprom",
        "/api/ms2109/eeprom/",
        "PROGRAM MS2109 EEPROM",
        "AT24C16",
    ):
        if forbidden_surface in settings:
            failures.append(
                "V2.4 product Settings exposes excluded EEPROM surface "
                f"{forbidden_surface!r}"
            )

    help_result = subprocess.run(
        ["bash", str(ROOT / "tools/build-firmware.sh"), "--help"],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    if help_result.returncode != 0 or "exoanchor-prototype-v2.4 (V2.4 product profile, ESP32-P4 rev3)" not in help_result.stdout:
        failures.append("V2.4 product board is missing from build CLI help")

    if failures:
        print("PrototypeV2.4 product contract failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print("PrototypeV2.4 product contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
