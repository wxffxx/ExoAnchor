#!/usr/bin/env python3
"""Guard MS2109 product power vs. V2.4 EEPROM-test composition boundaries."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(text: str, fragment: str, label: str, failures: list[str]) -> None:
    if fragment not in text:
        failures.append(f"{label}: missing {fragment!r}")


def require_between(
    text: str,
    start: str,
    end: str,
    fragment: str,
    label: str,
    failures: list[str],
) -> None:
    start_at = text.find(start)
    end_at = text.find(end, start_at + len(start)) if start_at >= 0 else -1
    fragment_at = text.find(fragment, start_at + len(start)) if start_at >= 0 else -1
    if min(start_at, end_at, fragment_at) < 0 or fragment_at >= end_at:
        failures.append(f"{label}: {fragment!r} is not inside {start!r}")


def main() -> int:
    failures: list[str] = []
    cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
    cli = (ROOT / "main/services/diag_cli.c").read_text(encoding="utf-8")
    build = (ROOT / "tools/build-firmware.sh").read_text(encoding="utf-8")
    v24 = (
        ROOT / "configs/boards/sdkconfig.defaults.exoanchor-prototype-v2.4-ms-test"
    ).read_text(encoding="utf-8")
    v24_product = (
        ROOT / "configs/boards/sdkconfig.defaults.exoanchor-prototype-v2.4"
    ).read_text(encoding="utf-8")

    base_sources = cmake.split("if(CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE)", 1)[0]
    for source in (
        "drivers/ms2109_eeprom_emulator.c",
        "drivers/ms2109_power.c",
        "drivers/ms2109_test.c",
    ):
        if source in base_sources:
            failures.append(f"base firmware sources unconditionally include {source}")

    require_between(
        cmake,
        "if(CONFIG_SI_MS2109_TEST_ENABLE)",
        "endif()",
        '"drivers/ms2109_test.c"',
        "V2.4 test source gate",
        failures,
    )
    require_between(
        cmake,
        "if(CONFIG_SI_MS2109_POWER_ENABLE)",
        "endif()",
        '"drivers/ms2109_power.c"',
        "production power source gate",
        failures,
    )
    require_between(
        cmake,
        "if(CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE)",
        "endif()",
        '"drivers/ms2109_eeprom_emulator.c"',
        "EEPROM emulator source gate",
        failures,
    )

    require(cli, "#if SI_CFG_MS2109_EEPROM_WP_GPIO >= 0\nstatic void run_eeprom_wp_command", "UART EEPROM WP GPIO gate", failures)
    require(cli, "ret = gpio_input_enable(gpio);", "UART EEPROM WP status input enable", failures)
    require(cli, "ret = ESP_ERR_INVALID_RESPONSE;", "UART EEPROM WP level verification", failures)
    require(cli, "#if SI_CFG_MS2109_TEST_ENABLED\nstatic void print_ms_eeprom_status", "UART physical EEPROM gate", failures)
    require(cli, "#if SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED\nstatic void print_eeprom_emulator_status", "UART emulator gate", failures)
    require(cli, "#if SI_CFG_MS2109_POWER_ENABLED || SI_CFG_MS2109_TEST_ENABLED\nstatic void print_ms_switch_status", "UART power gate", failures)
    for marker in (
        "si_ms2109_power_get_status",
        "si_ms2109_power_set(true)",
        "si_ms2109_power_set(false)",
        "si_ms2109_power_cycle(duration_ms)",
    ):
        require(cli, marker, "production UART power control", failures)
    require(cli, "#if SI_CFG_MS2109_TEST_ENABLED\n    } else if (strncmp(action, \"cycle-seq\"", "V2.4-only sequence experiment", failures)

    for marker in (
        "refusing product build with V2.4 MS2109 EEPROM test controls",
        "MS2109_EEPROM_SYMBOLS=(",
        "MS2109_EEPROM_TEST_STRINGS=(",
        "MS2109_EEPROM_WP_STRINGS=(",
        '"/api/ms2109/eeprom/probe"',
        '"/api/ms2109/eeprom/read"',
        '"/api/ms2109/eeprom/program"',
        '"eeprom-wp write-enable"',
        "EEPROM-WP-capable build is missing UART marker",
        '"ms-eeprom program-b64"',
        '"PROGRAM MS2109 EEPROM"',
        "product build contains excluded MS2109 EEPROM symbol",
        "product build contains excluded MS2109 EEPROM surface marker",
        "V2.4 MS test build is missing EEPROM symbol",
    ):
        require(build, marker, "post-link EEPROM composition audit", failures)

    for setting in (
        "CONFIG_SI_MS2109_TEST_ENABLE=y",
        "# CONFIG_SI_MS2109_POWER_ENABLE is not set",
    ):
        require(v24, setting, "V2.4 test defaults", failures)
    for setting in (
        "CONFIG_SI_MS2109_POWER_ENABLE=y",
        "# CONFIG_SI_MS2109_TEST_ENABLE is not set",
    ):
        require(v24_product, setting, "V2.4 product defaults", failures)
    require(
        v24_product,
        "# CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE is not set",
        "V2.4 product defaults",
        failures,
    )

    if failures:
        print("MS2109 composition boundary failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print("MS2109 composition boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
