import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from exoanchor_toolkit import cli
from exoanchor_toolkit.discovery import DiscoveredDevice
from exoanchor_toolkit.network import NetworkSnapshot
from exoanchor_toolkit.ports import PortInfo


class FakeProvisioner:
    def __init__(self):
        self.console = mock.Mock(port="/dev/test")
        self.calls = []

    def show(self):
        return NetworkSnapshot(
            runtime={
                "connected": "1",
                "state": "pending",
                "ip": "192.0.2.223",
            },
            identity={"device_id": "ea-p4-test"},
            active={"mode": "dhcp"},
            staged=None,
            raw="network ip=192.0.2.223",
        )

    def stage_dhcp(self, hostname):
        self.calls.append(("dhcp", hostname))
        return "stage ok"

    def stage_hostname(self, hostname):
        self.calls.append(("hostname", hostname))
        return "hostname ok"

    def stage_static(
        self,
        address,
        netmask,
        gateway,
        dns_primary,
        dns_secondary,
    ):
        self.calls.append(
            (
                "static",
                address,
                netmask,
                gateway,
                dns_primary,
                dns_secondary,
            )
        )
        return "static ok"

    def apply(self):
        self.calls.append(("apply",))
        return "apply ok"

    def commit(self):
        self.calls.append(("commit",))
        return "commit ok"


class FakeDownload:
    def to_dict(self):
        return {
            "repository": "wxffxx/ExoAnchor",
            "cached": False,
            "package": {"version": "0.87.3-dev"},
        }


class FakeRelease:
    tag = "firmware-v0.87.3-dev"
    name = "P4 IndigoShore v0.87.3-dev"
    published_at = "2026-07-31T00:00:00Z"
    prerelease = True

    def to_dict(self):
        return {
            "tag": self.tag,
            "name": self.name,
            "published_at": self.published_at,
            "prerelease": self.prerelease,
        }


class CliTests(unittest.TestCase):
    def test_discover_json(self):
        device = DiscoveredDevice(
            device_id="ea-p4-test",
            hostname="exoanchor-test",
            ipv4="192.0.2.50",
            source_ip="192.0.2.50",
            interface="ethernet",
            address_source="dhcp",
            firmware="0.87.3-dev",
            board="exoanchor-prototype-v2.3",
            http_port=80,
        )
        output = io.StringIO()
        with mock.patch.object(cli, "discover_devices", return_value=[device]):
            with contextlib.redirect_stdout(output):
                code = cli.main(["discover", "--json"])
        self.assertEqual(code, 0)
        self.assertEqual(
            json.loads(output.getvalue())[0]["source_ip"], "192.0.2.50"
        )

    def test_ports_json(self):
        fake = PortInfo(
            device="/dev/test",
            description="ExoAnchor",
            hwid="test",
            vid=0x1A86,
            pid=0x55D3,
            serial_number="1",
            manufacturer=None,
            likely_exoanchor=True,
        )
        output = io.StringIO()
        with mock.patch.object(cli, "list_serial_ports", return_value=[fake]):
            with contextlib.redirect_stdout(output):
                code = cli.main(["ports", "--json"])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output.getvalue())[0]["device"], "/dev/test")

    def test_network_json(self):
        provisioner = FakeProvisioner()
        output = io.StringIO()
        with mock.patch.object(cli, "_network_client", return_value=provisioner):
            with contextlib.redirect_stdout(output):
                code = cli.main(["network", "--json", "show"])
        self.assertEqual(code, 0)
        self.assertEqual(
            json.loads(output.getvalue())["runtime"]["ip"], "192.0.2.223"
        )

    def test_network_configure_uses_safe_shared_workflow(self):
        provisioner = FakeProvisioner()
        output = io.StringIO()
        with mock.patch.object(cli, "_network_client", return_value=provisioner):
            with contextlib.redirect_stdout(output):
                code = cli.main(
                    [
                        "network",
                        "configure",
                        "--mode",
                        "static",
                        "--hostname",
                        "exoanchor-test",
                        "--address",
                        "192.0.2.223",
                        "--netmask",
                        "255.255.255.0",
                        "--gateway",
                        "192.0.2.1",
                        "--dns-primary",
                        "192.0.2.1",
                        "--dns-secondary",
                        "1.1.1.1",
                        "--discovery-subnet",
                        "192.0.2.0/24",
                        "--json",
                    ]
                )
        self.assertEqual(code, 0)
        result = json.loads(output.getvalue())
        self.assertEqual(result["target"], "/dev/test")
        self.assertEqual(result["snapshot"]["runtime"]["ip"], "192.0.2.223")
        self.assertEqual(
            provisioner.calls,
            [
                ("hostname", "exoanchor-test"),
                (
                    "static",
                    "192.0.2.223",
                    "255.255.255.0",
                    "192.0.2.1",
                    "192.0.2.1",
                    "1.1.1.1",
                ),
                ("apply",),
                ("commit",),
            ],
        )

    def test_network_transport_options_work_after_subcommand(self):
        args = cli.build_parser().parse_args(
            [
                "network",
                "show",
                "--transport",
                "network",
                "--device",
                "192.0.2.223",
                "--username",
                "admin",
                "--password",
                "123456",
                "--timeout",
                "9",
                "--json",
            ]
        )
        self.assertEqual(args.transport, "network")
        self.assertEqual(args.device, "192.0.2.223")
        self.assertEqual(args.username, "admin")
        self.assertEqual(args.password, "123456")
        self.assertEqual(args.timeout, 9)
        self.assertTrue(args.json)

    def test_network_transport_options_remain_compatible_before_subcommand(self):
        args = cli.build_parser().parse_args(
            [
                "network",
                "--transport",
                "network",
                "--device",
                "192.0.2.223",
                "--json",
                "show",
            ]
        )
        self.assertEqual(args.transport, "network")
        self.assertEqual(args.device, "192.0.2.223")
        self.assertTrue(args.json)

    def test_commit_requires_apply(self):
        provisioner = FakeProvisioner()
        error = io.StringIO()
        with mock.patch.object(cli, "_network_client", return_value=provisioner):
            with contextlib.redirect_stderr(error):
                code = cli.main(["network", "dhcp", "--commit"])
        self.assertEqual(code, 2)
        self.assertIn("--commit requires --apply", error.getvalue())
        self.assertNotIn(("commit",), provisioner.calls)

    def test_noninteractive_flash_requires_yes(self):
        with tempfile.TemporaryDirectory() as temp:
            missing = Path(temp) / "missing"
            error = io.StringIO()
            with contextlib.redirect_stderr(error):
                code = cli.main(["firmware", "flash", str(missing)])
            self.assertEqual(code, 2)
            self.assertIn("missing firmware manifest", error.getvalue())

    def test_uart_flash_auto_is_rejected_without_port_or_hardware_access(self):
        package = mock.Mock(board="exoanchor-prototype-v2.3")
        context = mock.MagicMock()
        context.__enter__.return_value = package
        error = io.StringIO()
        with (
            mock.patch.object(cli, "open_firmware_package", return_value=context),
            mock.patch.object(cli, "resolve_port") as resolve,
            contextlib.redirect_stderr(error),
        ):
            code = cli.main(
                ["firmware", "flash", "/tmp/package", "--dry-run"]
            )
        self.assertEqual(code, 2)
        self.assertIn("forbids 'auto'", error.getvalue())
        resolve.assert_not_called()

    def test_gui_forwards_runtime_options(self):
        with mock.patch(
            "exoanchor_toolkit.gui.launch_gui"
        ) as launch_gui:
            code = cli.main(["gui", "--port", "8765", "--no-open"])
        self.assertEqual(code, 0)
        launch_gui.assert_called_once_with(port=8765, open_browser=False)

    def test_latest_firmware_uses_release_downloader(self):
        output = io.StringIO()
        with mock.patch.object(
            cli, "download_latest_firmware", return_value=FakeDownload()
        ) as download:
            with contextlib.redirect_stdout(output):
                code = cli.main(
                    [
                        "firmware",
                        "latest",
                        "--repository",
                        "owner/repo",
                        "--output",
                        "/tmp/firmware",
                        "--json",
                    ]
                )
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output.getvalue())["cached"], False)
        download.assert_called_once_with(
            "owner/repo",
            output_dir=Path("/tmp/firmware"),
            include_prerelease=True,
        )

    def test_firmware_versions_json(self):
        output = io.StringIO()
        with mock.patch.object(
            cli, "firmware_releases", return_value=[FakeRelease()]
        ) as releases:
            with contextlib.redirect_stdout(output):
                code = cli.main(
                    [
                        "firmware",
                        "versions",
                        "--repository",
                        "owner/repo",
                        "--json",
                    ]
                )
        self.assertEqual(code, 0)
        self.assertEqual(
            json.loads(output.getvalue())[0]["tag"],
            "firmware-v0.87.3-dev",
        )
        releases.assert_called_once_with(
            "owner/repo",
            include_prerelease=True,
        )

    def test_firmware_download_uses_selected_tag(self):
        output = io.StringIO()
        with mock.patch.object(
            cli, "download_firmware_release", return_value=FakeDownload()
        ) as download:
            with contextlib.redirect_stdout(output):
                code = cli.main(
                    [
                        "firmware",
                        "download",
                        "firmware-v0.87.3-dev",
                        "--repository",
                        "owner/repo",
                        "--output",
                        "/tmp/firmware",
                        "--json",
                    ]
                )
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output.getvalue())["cached"], False)
        download.assert_called_once_with(
            "owner/repo",
            tag="firmware-v0.87.3-dev",
            output_dir=Path("/tmp/firmware"),
            include_prerelease=True,
        )


if __name__ == "__main__":
    unittest.main()
