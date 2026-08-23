import unittest

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.network import (
    NetworkProvisioner,
    parse_network_show,
    validate_hostname,
    validate_ipv4,
    validate_netmask,
)
from exoanchor_toolkit.serial_cli import CommandResult


NETWORK_SHOW = """\
network initialized=1 link=1 connected=1 state=active source=dhcp ip=192.0.2.223 remaining=0s error=none
identity device_id=ea-p4-123456 hostname=exoanchor-123456 mac=02:00:00:12:34:56
active generation=1 mode=dhcp hostname=exoanchor-123456 autoip=1
staged generation=2 mode=static hostname=exoanchor-123456 autoip=0 address=192.0.2.50 netmask=255.255.255.0 gateway=192.0.2.1 dns1=1.1.1.1 dns2=none
"""


class FakeConsole:
    def __init__(self, outputs=None):
        self.port = "/dev/test"
        self.commands = []
        self.outputs = outputs or {}

    def run(self, command):
        self.commands.append(command)
        output = self.outputs.get(command, "network stage result=ESP_OK")
        return CommandResult(command, output, 0.01)


class NetworkValidationTests(unittest.TestCase):
    def test_parse_show(self):
        result = parse_network_show(NETWORK_SHOW)
        self.assertEqual(result.runtime["ip"], "192.0.2.223")
        self.assertEqual(result.identity["device_id"], "ea-p4-123456")
        self.assertEqual(result.active["mode"], "dhcp")
        self.assertEqual(result.staged["address"], "192.0.2.50")

    def test_parse_requires_active_sections(self):
        with self.assertRaisesRegex(ToolkitError, "missing"):
            parse_network_show("network initialized=1")

    def test_parse_preserves_a_multiword_runtime_error(self):
        result = parse_network_show(
            NETWORK_SHOW.replace(
                "error=none",
                "error=static IPv4 address conflict: ESP_ERR_INVALID_STATE",
            )
        )
        self.assertEqual(
            result.runtime["error"],
            "static IPv4 address conflict: ESP_ERR_INVALID_STATE",
        )

    def test_hostname_validation(self):
        self.assertEqual(validate_hostname("exoanchor-01"), "exoanchor-01")
        for value in ("", "-bad", "bad-", "bad_name", "a" * 64):
            with self.subTest(value=value):
                with self.assertRaises(ToolkitError):
                    validate_hostname(value)

    def test_ipv4_and_netmask_validation(self):
        self.assertEqual(validate_ipv4("192.0.2.1", "address"), "192.0.2.1")
        self.assertEqual(validate_ipv4("", "gateway", optional=True), "-")
        self.assertEqual(validate_ipv4("none", "gateway", optional=True), "-")
        self.assertEqual(validate_ipv4("NONE", "gateway", optional=True), "-")
        self.assertEqual(validate_netmask("255.255.255.0"), "255.255.255.0")
        with self.assertRaises(ToolkitError):
            validate_ipv4("999.1.1.1", "address")
        with self.assertRaisesRegex(ToolkitError, "contiguous"):
            validate_netmask("255.0.255.0")

    def test_provisioning_commands_are_exact(self):
        console = FakeConsole()
        network = NetworkProvisioner(console)
        network.stage_dhcp("exoanchor-test")
        network.stage_static(
            "192.0.2.50",
            "255.255.255.0",
            "192.0.2.1",
            "1.1.1.1",
            "-",
        )
        network.stage_hostname("exoanchor-renamed")
        network.apply()
        network.commit()
        network.rollback()
        network.reset()
        self.assertEqual(
            console.commands,
            [
                "network dhcp exoanchor-test",
                "network static 192.0.2.50 255.255.255.0 "
                "192.0.2.1 1.1.1.1 -",
                "network hostname exoanchor-renamed",
                "network apply",
                "network commit",
                "network rollback",
                "network reset CONFIRM",
            ],
        )

    def test_device_error_is_not_silently_accepted(self):
        console = FakeConsole(
            {"network apply": "network apply result=ESP_ERR_INVALID_STATE"}
        )
        with self.assertRaisesRegex(ToolkitError, "ESP_ERR_INVALID_STATE"):
            NetworkProvisioner(console).apply()


if __name__ == "__main__":
    unittest.main()
