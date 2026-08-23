import json
import socket
import unittest

from exoanchor_toolkit.discovery import (
    DiscoveredDevice,
    discover_devices,
    discovery_targets,
    parse_discovery_response,
)
from exoanchor_toolkit.errors import ToolkitError


def response(nonce: str, **overrides) -> bytes:
    value = {
        "service": "exoanchor",
        "version": 1,
        "nonce": nonce,
        "device_id": "ea-p4-123456",
        "hostname": "exoanchor-123456",
        "device_label": "机房 KVM",
        "ipv4": "192.0.2.223",
        "interface": "ethernet",
        "address_source": "dhcp",
        "firmware": "0.87.3-dev",
        "board": "exoanchor-prototype-v2.3",
        "http_port": 80,
    }
    value.update(overrides)
    return json.dumps(value).encode()


class FakeDiscoverySocket:
    def __init__(self, *args):
        self.responses = []

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def setsockopt(self, *args):
        pass

    def settimeout(self, value):
        pass

    def sendto(self, request, target):
        nonce = json.loads(request)["nonce"]
        self.responses.append((response(nonce), ("192.0.2.223", target[1])))

    def recvfrom(self, size):
        if self.responses:
            return self.responses.pop(0)
        raise socket.timeout()


class DiscoveryTests(unittest.TestCase):
    def test_valid_response(self):
        device = parse_discovery_response(
            response("abc"), "192.0.2.223", "abc"
        )
        self.assertIsNotNone(device)
        self.assertEqual(device.device_id, "ea-p4-123456")
        self.assertEqual(device.device_label, "机房 KVM")
        self.assertEqual(device.source_ip, "192.0.2.223")
        self.assertEqual(device.web_url, "http://192.0.2.223/")

    def test_nonce_and_required_fields_are_enforced(self):
        self.assertIsNone(
            parse_discovery_response(response("wrong"), "192.0.2.223", "expected")
        )
        self.assertIsNone(
            parse_discovery_response(
                response("abc", board=""), "192.0.2.223", "abc"
            )
        )

    def test_source_ip_is_authoritative_for_web_url(self):
        device = parse_discovery_response(
            response("abc", ipv4="192.0.2.99", http_port=8080),
            "192.0.2.223",
            "abc",
        )
        self.assertEqual(device.ipv4, "192.0.2.99")
        self.assertEqual(device.web_url, "http://192.0.2.223:8080/")

    def test_subnet_expansion_and_limit(self):
        targets = discovery_targets([], ["192.0.2.0/30"])
        self.assertIn("192.0.2.1", targets)
        self.assertIn("192.0.2.2", targets)
        with self.assertRaisesRegex(ToolkitError, "limited"):
            discovery_targets([], ["198.51.100.0/16"])

    def test_active_discovery_and_deduplication(self):
        devices = discover_devices(
            timeout=0.1,
            addresses=["255.255.255.255", "192.0.2.223"],
            socket_factory=FakeDiscoverySocket,
        )
        self.assertEqual(len(devices), 1)
        self.assertEqual(devices[0].firmware, "0.87.3-dev")


if __name__ == "__main__":
    unittest.main()
