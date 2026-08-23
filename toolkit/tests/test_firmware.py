import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.firmware import (
    MANIFEST_NAME,
    THIRD_PARTY_NOTICES_NAME,
    build_esptool_command,
    create_firmware_archive,
    create_firmware_package,
    flash_firmware,
    load_firmware_package,
)
from exoanchor_toolkit.identity import ChipIdentity
from exoanchor_toolkit.ports import PortInfo


OFFSETS = {
    "0x2000": "bootloader/bootloader.bin",
    "0x8000": "partition_table/partition-table.bin",
    "0xf000": "ota_data_initial.bin",
    "0x20000": "si_esphost_esp32p4.bin",
}


class FirmwarePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.build = self.root / "build"
        self.build.mkdir()
        for index, relative in enumerate(OFFSETS.values(), start=1):
            path = self.build / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(bytes([index]) * (1024 + index))
        (self.build / "flasher_args.json").write_text(
            json.dumps(
                {
                    "flash_files": OFFSETS,
                    "flash_settings": {
                        "flash_mode": "dio",
                        "flash_size": "16MB",
                        "flash_freq": "80m",
                    },
                    "extra_esptool_args": {
                        "after": "hard_reset",
                        "before": "default_reset",
                        "stub": True,
                        "chip": "esp32p4",
                    },
                }
            ),
            encoding="utf-8",
        )
        (self.build / "project_description.json").write_text(
            json.dumps(
                {
                    "project_name": "si_esphost_esp32p4",
                    "project_version": "0.87.3-dev",
                    "target": "esp32p4",
                }
            ),
            encoding="utf-8",
        )
        (self.build / "sdkconfig").write_text(
            'CONFIG_SI_BOARD_ID="exoanchor-prototype-v2.3"\n',
            encoding="utf-8",
        )
        (self.build / THIRD_PARTY_NOTICES_NAME).write_text(
            "Third-party test notices\n",
            encoding="utf-8",
        )

    def tearDown(self):
        self.temp.cleanup()

    def _identity_registry(self) -> Path:
        registry = self.root / "BOARD_IDENTITY_REGISTRY.toml"
        registry.write_text(
            '''schema_version = 1

[[boards]]
record_id = "prototype-test"
physical_model = "ExoAnchor Prototype V2.3"
physical_model_status = "operator-confirmed"
usb_bridge = "CH343"
usb_bridge_serial = "board-a"
chip = "ESP32-P4"
silicon_revision = "3.2"
efuse_base_mac = "02:00:00:12:34:56"
allowed_profile = "exoanchor-prototype-v2.3"
flash_authorization = "allowed only after exact identity recheck"
''',
            encoding="utf-8",
        )
        return registry

    def test_create_and_verify_package(self):
        output = self.root / "package"
        manifest = create_firmware_package(self.build, output)
        self.assertEqual(manifest, (output / MANIFEST_NAME).resolve())
        package = load_firmware_package(output)
        self.assertEqual(package.version, "0.87.3-dev")
        self.assertEqual(package.board, "exoanchor-prototype-v2.3")
        self.assertEqual(len(package.segments), 4)
        self.assertEqual(
            {segment.address for segment in package.segments},
            {0x2000, 0x8000, 0xF000, 0x20000},
        )

    def test_package_refuses_overwrite(self):
        output = self.root / "package"
        output.mkdir()
        with self.assertRaisesRegex(ToolkitError, "refusing to overwrite"):
            create_firmware_package(self.build, output)

    def test_package_requires_third_party_notices(self):
        (self.build / THIRD_PARTY_NOTICES_NAME).unlink()
        with self.assertRaisesRegex(ToolkitError, "missing required third-party"):
            create_firmware_package(self.build, self.root / "package")

    def test_corruption_is_detected(self):
        output = self.root / "package"
        create_firmware_package(self.build, output)
        package = load_firmware_package(output)
        package.segments[-1].path.write_bytes(b"corrupt")
        with self.assertRaisesRegex(ToolkitError, "size mismatch|SHA-256 mismatch"):
            load_firmware_package(output)

    def test_path_escape_is_rejected(self):
        output = self.root / "package"
        manifest = create_firmware_package(self.build, output)
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["segments"][0]["file"] = "../outside.bin"
        outside = self.root / "outside.bin"
        outside.write_bytes(b"x")
        data["segments"][0]["size"] = 1
        data["segments"][0]["sha256"] = hashlib.sha256(b"x").hexdigest()
        manifest.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaisesRegex(ToolkitError, "unsafe firmware file path"):
            load_firmware_package(output)

    def test_flash_command_has_safety_flags_and_no_full_erase(self):
        output = self.root / "package"
        package = load_firmware_package(
            create_firmware_package(self.build, output)
        )
        command = build_esptool_command(
            package, "/dev/test", 460800, esptool_major=5
        )
        self.assertIn("esp32p4", command)
        self.assertIn("write-flash", command)
        self.assertIn("--flash-size", command)
        self.assertNotIn("--verify", command)
        self.assertNotIn("--erase-all", command)
        self.assertNotIn("--force", command)
        self.assertEqual(command.count("0x2000"), 1)
        self.assertEqual(command.count("0x20000"), 1)

        legacy = build_esptool_command(
            package, "/dev/test", 460800, esptool_major=4
        )
        self.assertIn("write_flash", legacy)
        self.assertIn("--verify", legacy)
        self.assertIn("--flash_size", legacy)

        with self.assertRaisesRegex(ToolkitError, "forbids 'auto'"):
            build_esptool_command(package, "auto", 460800)

    def test_release_archive_is_deterministic_and_complete(self):
        package = self.root / "package"
        create_firmware_package(self.build, package)
        first = create_firmware_archive(package, self.root / "first.zip")
        second = create_firmware_archive(package, self.root / "second.zip")
        self.assertEqual(
            hashlib.sha256(first.read_bytes()).hexdigest(),
            hashlib.sha256(second.read_bytes()).hexdigest(),
        )
        import zipfile

        verified = load_firmware_package(package)
        with zipfile.ZipFile(first) as archive:
            self.assertEqual(
                set(archive.namelist()),
                {
                    MANIFEST_NAME,
                    THIRD_PARTY_NOTICES_NAME,
                    *(segment.relative_path for segment in verified.segments),
                },
            )

    def test_uart_flash_reacquires_exact_device_and_verifies_runtime(self):
        package_path = self.root / "package"
        package = load_firmware_package(
            create_firmware_package(self.build, package_path)
        )
        identity = PortInfo(
            device="/dev/cu.old",
            description="ExoAnchor",
            hwid="test",
            vid=0x1A86,
            pid=0x55D3,
            serial_number="board-a",
            manufacturer="QinHeng",
            likely_exoanchor=True,
        )
        verification = {
            "verified": True,
            "port": "/dev/cu.new",
            "device_id": "ea-p4-test",
            "firmware": "0.87.3-dev",
            "board": "exoanchor-prototype-v2.3",
        }
        process = mock.Mock()
        process.stdout = iter(["Connecting...\n", "Writing...\n"])
        process.wait.return_value = 0
        lines: list[str] = []

        with (
            mock.patch(
                "exoanchor_toolkit.firmware.importlib.util.find_spec",
                return_value=object(),
            ),
            mock.patch(
                "exoanchor_toolkit.firmware.subprocess.Popen",
                return_value=process,
            ),
            mock.patch(
                "exoanchor_toolkit.ports.capture_port_identity",
                return_value=identity,
            ),
            mock.patch(
                "exoanchor_toolkit.firmware.probe_chip_identity",
                return_value=ChipIdentity(
                    efuse_base_mac="02:00:00:12:34:56",
                    silicon_revision="3.2",
                ),
            ) as probe,
            mock.patch(
                "exoanchor_toolkit.uart_recovery.wait_for_uart_runtime",
                return_value=verification,
            ) as wait_runtime,
        ):
            result = flash_firmware(
                package,
                identity.device,
                output=lines.append,
                identity_registry=self._identity_registry(),
                board_record="prototype-test",
            )

        self.assertEqual(result, verification)
        probe.assert_called_once_with(identity.device)
        wait_runtime.assert_called_once_with(
            identity,
            expected_version="0.87.3-dev",
            expected_board="exoanchor-prototype-v2.3",
            baud=115200,
            timeout=30.0,
        )
        self.assertIn(
            "Flash write completed; waiting for the same device runtime",
            lines,
        )
        self.assertIn(
            "UART0 runtime verified: exoanchor-prototype-v2.3 "
            "0.87.3-dev on /dev/cu.new",
            lines,
        )

    def test_uart_flash_without_credentials_never_starts_esptool(self):
        package = load_firmware_package(
            create_firmware_package(self.build, self.root / "package")
        )
        with (
            mock.patch(
                "exoanchor_toolkit.firmware.probe_chip_identity"
            ) as probe,
            mock.patch(
                "exoanchor_toolkit.firmware.subprocess.Popen"
            ) as start_write,
        ):
            with self.assertRaisesRegex(ToolkitError, "identity-registry"):
                flash_firmware(package, "/dev/cu.test")
        probe.assert_not_called()
        start_write.assert_not_called()

    def test_uart_flash_usb_mismatch_stops_before_any_esptool(self):
        package = load_firmware_package(
            create_firmware_package(self.build, self.root / "package")
        )
        wrong_port = PortInfo(
            device="/dev/cu.test",
            description="ExoAnchor",
            hwid="test",
            vid=0x1A86,
            pid=0x55D3,
            serial_number="another-board",
            manufacturer="QinHeng",
            likely_exoanchor=True,
        )
        with (
            mock.patch(
                "exoanchor_toolkit.ports.capture_port_identity",
                return_value=wrong_port,
            ),
            mock.patch(
                "exoanchor_toolkit.firmware.probe_chip_identity"
            ) as probe,
            mock.patch(
                "exoanchor_toolkit.firmware.subprocess.Popen"
            ) as start_write,
        ):
            with self.assertRaisesRegex(ToolkitError, "CH343 serial"):
                flash_firmware(
                    package,
                    wrong_port.device,
                    identity_registry=self._identity_registry(),
                    board_record="prototype-test",
                )
        probe.assert_not_called()
        start_write.assert_not_called()

    def test_uart_flash_chip_mismatch_never_starts_write_process(self):
        package = load_firmware_package(
            create_firmware_package(self.build, self.root / "package")
        )
        port = PortInfo(
            device="/dev/cu.test",
            description="ExoAnchor",
            hwid="test",
            vid=0x1A86,
            pid=0x55D3,
            serial_number="board-a",
            manufacturer="QinHeng",
            likely_exoanchor=True,
        )
        with (
            mock.patch(
                "exoanchor_toolkit.ports.capture_port_identity",
                return_value=port,
            ),
            mock.patch(
                "exoanchor_toolkit.firmware.importlib.util.find_spec",
                return_value=object(),
            ),
            mock.patch(
                "exoanchor_toolkit.firmware.probe_chip_identity",
                return_value=ChipIdentity(
                    efuse_base_mac="02:00:00:00:00:00",
                    silicon_revision="3.2",
                ),
            ),
            mock.patch(
                "exoanchor_toolkit.firmware.subprocess.Popen"
            ) as start_write,
        ):
            with self.assertRaisesRegex(ToolkitError, "eFuse base MAC"):
                flash_firmware(
                    package,
                    port.device,
                    identity_registry=self._identity_registry(),
                    board_record="prototype-test",
                )
        start_write.assert_not_called()


if __name__ == "__main__":
    unittest.main()
