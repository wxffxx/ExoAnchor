from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.identity import (
    ChipIdentity,
    load_board_identity_credential,
    parse_chip_identity,
    require_explicit_uart_flash_port,
    verify_chip_identity,
    verify_package_profile,
)


def _registry_text(
    *,
    model_status: str = "operator-confirmed",
    flash_authorization: str = "allowed only after exact identity recheck",
) -> str:
    return f'''schema_version = 1

[[boards]]
record_id = "prototype-test"
physical_model = "ExoAnchor Prototype Test"
physical_model_status = "{model_status}"
usb_bridge = "CH343"
usb_bridge_serial = "BOARD123"
chip = "ESP32-P4"
silicon_revision = "3.2"
efuse_base_mac = "02:00:00:12:34:56"
allowed_profile = "exoanchor-prototype-v2.3"
flash_authorization = "{flash_authorization}"
'''


class IdentityGateTests(unittest.TestCase):
    def test_registry_and_chip_output_supply_all_authoritative_factors(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "BOARD_IDENTITY_REGISTRY.toml"
            path.write_text(_registry_text(), encoding="utf-8")
            credential = load_board_identity_credential(path, "prototype-test")

        self.assertEqual(credential.physical_model, "ExoAnchor Prototype Test")
        self.assertEqual(credential.ch343_serial, "BOARD123")
        self.assertEqual(credential.allowed_profile, "exoanchor-prototype-v2.3")
        observed = parse_chip_identity(
            "Chip is ESP32-P4 (revision v3.2)\n"
            "MAC: 02:00:00:12:34:56\n"
        )
        self.assertEqual(observed.efuse_base_mac, credential.efuse_base_mac)
        self.assertEqual(observed.silicon_revision, credential.silicon_revision)

    def test_unconfirmed_physical_model_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "BOARD_IDENTITY_REGISTRY.toml"
            path.write_text(
                _registry_text(model_status="unknown"),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ToolkitError, "not operator-confirmed"):
                load_board_identity_credential(path, "prototype-test")

    def test_missing_or_blocked_flash_authorization_is_rejected(self):
        for authorization in ("pending review", "blocked", "not allowed"):
            with self.subTest(authorization=authorization):
                with tempfile.TemporaryDirectory() as temp:
                    path = Path(temp) / "BOARD_IDENTITY_REGISTRY.toml"
                    path.write_text(
                        _registry_text(flash_authorization=authorization),
                        encoding="utf-8",
                    )
                    with self.assertRaisesRegex(
                        ToolkitError, "does not explicitly authorize"
                    ):
                        load_board_identity_credential(path, "prototype-test")

    def test_profile_mac_and_revision_mismatches_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "BOARD_IDENTITY_REGISTRY.toml"
            path.write_text(_registry_text(), encoding="utf-8")
            credential = load_board_identity_credential(path, "prototype-test")

        with self.assertRaisesRegex(ToolkitError, "firmware profile"):
            verify_package_profile("exoanchor-production-typec-local", credential)
        with self.assertRaisesRegex(ToolkitError, "eFuse base MAC"):
            verify_chip_identity(
                ChipIdentity("02:00:00:00:00:00", "3.2"),
                credential,
            )
        with self.assertRaisesRegex(ToolkitError, "silicon revision"):
            verify_chip_identity(
                ChipIdentity("02:00:00:12:34:56", "1.3"),
                credential,
            )

    def test_auto_and_wildcard_ports_are_rejected(self):
        for value in ("auto", " AUTO ", "", "/dev/cu.usbmodem*"):
            with self.subTest(value=value):
                with self.assertRaises(ToolkitError):
                    require_explicit_uart_flash_port(value)

    def test_incomplete_chip_output_is_rejected(self):
        with self.assertRaisesRegex(ToolkitError, "did not report"):
            parse_chip_identity("Chip is ESP32-P4\nMAC: 02:00:00:12:34:56\n")


if __name__ == "__main__":
    unittest.main()
