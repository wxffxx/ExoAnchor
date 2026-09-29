import unittest
from unittest.mock import patch

from exoanchor_toolkit.discovery import DiscoveredDevice
from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.http_device import DeviceTarget
from exoanchor_toolkit.network import NetworkSnapshot
from exoanchor_toolkit.network_configure import (
    NetworkConfiguration,
    _snapshot_is_ready,
    configure_network,
)


def snapshot(*, ready: bool = True) -> NetworkSnapshot:
    return NetworkSnapshot(
        runtime={
            "connected": "1" if ready else "0",
            "state": "pending",
            "ip": "192.0.2.240",
            "source": "static",
        },
        identity={
            "device_id": "ea-p4-test",
            "hostname": "exoanchor-test",
        },
        active={
            "mode": "static",
            "hostname": "exoanchor-test",
            "address": "192.0.2.240",
            "netmask": "255.255.255.0",
        },
        staged=None,
        raw="network state=pending connected=1 ip=192.0.2.240",
    )


class FakeProvisioner:
    def __init__(self, *, ready: bool = True) -> None:
        self.calls: list[object] = []
        self.snapshot = snapshot(ready=ready)

    def show(self) -> NetworkSnapshot:
        self.calls.append("show")
        return self.snapshot

    def stage_dhcp(self, hostname: str | None = None) -> str:
        self.calls.append(("dhcp", hostname))
        return "dhcp"

    def stage_hostname(self, hostname: str) -> str:
        self.calls.append(("hostname", hostname))
        return "hostname"

    def stage_static(
        self,
        address: str,
        netmask: str,
        gateway: str,
        dns_primary: str,
        dns_secondary: str,
    ) -> str:
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
        return "static"

    def apply(self) -> str:
        self.calls.append("apply")
        return "apply"

    def commit(self) -> str:
        self.calls.append("commit")
        return "commit"


class StepClock:
    def __init__(self) -> None:
        self.value = 0.0

    def __call__(self) -> float:
        self.value += 0.1
        return self.value


class FakeHttpClient:
    def __init__(self, address: str, *, ready: bool = True) -> None:
        self.target = DeviceTarget(address=address, port=80)
        self.username = "admin"
        self.password = "test-only"
        self.token = "test-token"
        self.provisioner = FakeProvisioner(ready=ready)


class NetworkConfigureTests(unittest.TestCase):
    def test_static_conflict_check_is_not_ready_to_commit(self) -> None:
        checking = NetworkSnapshot(
            runtime={
                "connected": "1",
                "pending_confirmation": "1",
                "config_state": "checking",
                "ipv4": "192.0.2.240",
            },
            identity={"device_id": "ea-p4-test"},
            active={},
            staged=None,
            raw="",
        )
        self.assertFalse(
            _snapshot_is_ready(
                checking,
                expected_address="192.0.2.240",
                expected_device_id="ea-p4-test",
            )
        )
        checking.runtime["config_state"] = "pending"
        self.assertTrue(
            _snapshot_is_ready(
                checking,
                expected_address="192.0.2.240",
                expected_device_id="ea-p4-test",
            )
        )

    def test_static_configuration_is_verified_before_commit(self) -> None:
        provisioner = FakeProvisioner()
        result = configure_network(
            provisioner,
            NetworkConfiguration(
                mode="static",
                hostname="exoanchor-test",
                address="192.0.2.240",
                netmask="255.255.255.0",
                gateway="192.0.2.1",
                dns_primary="192.0.2.1",
                dns_secondary="1.1.1.1",
            ),
            transport="uart",
            target="/dev/test",
        )
        self.assertEqual(
            provisioner.calls,
            [
                "show",
                ("hostname", "exoanchor-test"),
                (
                    "static",
                    "192.0.2.240",
                    "255.255.255.0",
                    "192.0.2.1",
                    "192.0.2.1",
                    "1.1.1.1",
                ),
                "apply",
                "show",
                "commit",
                "show",
            ],
        )
        self.assertEqual(result.target, "/dev/test")
        self.assertEqual(result.snapshot.runtime["ip"], "192.0.2.240")

    def test_verification_failure_never_commits(self) -> None:
        provisioner = FakeProvisioner(ready=False)
        with self.assertRaisesRegex(
            ToolkitError,
            "was not saved.*automatically roll back",
        ):
            configure_network(
                provisioner,
                NetworkConfiguration(mode="dhcp"),
                transport="uart",
                target="/dev/test",
                verify_timeout=0.15,
                monotonic=StepClock(),
                sleeper=lambda _: None,
            )
        self.assertIn("apply", provisioner.calls)
        self.assertNotIn("commit", provisioner.calls)

    def test_static_mode_requires_address_and_netmask(self) -> None:
        provisioner = FakeProvisioner()
        with self.assertRaisesRegex(ToolkitError, "address is required"):
            configure_network(
                provisioner,
                NetworkConfiguration(mode="static"),
                transport="uart",
                target="/dev/test",
            )
        self.assertNotIn("apply", provisioner.calls)

    def test_invalid_safety_options_are_rejected_before_device_access(self) -> None:
        provisioner = FakeProvisioner()
        with self.assertRaisesRegex(ToolkitError, "between 0.1 and 110"):
            configure_network(
                provisioner,
                NetworkConfiguration(mode="dhcp"),
                transport="uart",
                target="/dev/test",
                verify_timeout=float("inf"),
            )
        self.assertEqual(provisioner.calls, [])

        with self.assertRaisesRegex(ToolkitError, "invalid IPv4 subnet"):
            configure_network(
                provisioner,
                NetworkConfiguration(mode="dhcp"),
                transport="network",
                target="http://192.0.2.10",
                client=FakeHttpClient("192.0.2.10"),
                discovery_subnets=("not-a-cidr",),
            )
        self.assertEqual(provisioner.calls, [])

    def test_missing_device_identity_is_never_applied(self) -> None:
        provisioner = FakeProvisioner()
        provisioner.snapshot = NetworkSnapshot(
            runtime=provisioner.snapshot.runtime,
            identity={},
            active=provisioner.snapshot.active,
            staged=None,
            raw="",
        )
        with self.assertRaisesRegex(ToolkitError, "did not report a device_id"):
            configure_network(
                provisioner,
                NetworkConfiguration(mode="dhcp"),
                transport="uart",
                target="/dev/test",
            )
        self.assertEqual(provisioner.calls, ["show"])

    def test_dhcp_rediscovery_uses_device_id_and_fallback_subnet(self) -> None:
        initial_client = FakeHttpClient("192.0.2.10")
        initial_provisioner = FakeProvisioner()
        discovered_subnets: list[tuple[str, ...]] = []

        def discoverer(**kwargs):
            discovered_subnets.append(tuple(kwargs.get("subnets") or ()))
            return [
                DiscoveredDevice(
                    device_id="another-device",
                    hostname="other",
                    ipv4="192.0.2.20",
                    source_ip="192.0.2.20",
                    interface="ethernet",
                    address_source="dhcp",
                    firmware="test",
                    board="test",
                    http_port=80,
                ),
                DiscoveredDevice(
                    device_id="ea-p4-test",
                    hostname="exoanchor-test",
                    ipv4="192.0.2.30",
                    source_ip="192.0.2.30",
                    interface="ethernet",
                    address_source="dhcp",
                    firmware="test",
                    board="test",
                    http_port=80,
                ),
            ]

        candidate_clients: dict[str, FakeHttpClient] = {}

        def make_client(target, **_):
            address = str(target).split(":", 1)[0]
            client = FakeHttpClient(
                address,
                ready=address == "192.0.2.30",
            )
            candidate_clients[address] = client
            return client

        with (
            patch(
                "exoanchor_toolkit.network_configure.DeviceHttpClient",
                side_effect=make_client,
            ),
            patch(
                "exoanchor_toolkit.network_configure.HttpNetworkProvisioner",
                side_effect=lambda client: client.provisioner,
            ),
        ):
            result = configure_network(
                initial_provisioner,
                NetworkConfiguration(
                    mode="dhcp",
                    hostname="exoanchor-test",
                ),
                transport="network",
                target=initial_client.target.base_url,
                client=initial_client,
                discovery_subnets=("192.0.2.0/24",),
                discoverer=discoverer,
            )

        self.assertEqual(result.target, "http://192.0.2.30")
        self.assertEqual(discovered_subnets, [("192.0.2.0/24",)])
        self.assertIn("commit", candidate_clients["192.0.2.30"].provisioner.calls)
        self.assertNotIn(
            "commit",
            candidate_clients["192.0.2.10"].provisioner.calls,
        )


if __name__ == "__main__":
    unittest.main()
