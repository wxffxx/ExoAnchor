from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
import urllib.request
import urllib.parse
from pathlib import Path
from dataclasses import replace
from unittest import mock

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.firmware import FirmwarePackage, FirmwareSegment
from exoanchor_toolkit.http_device import (
    DeviceHttpClient,
    HttpNetworkProvisioner,
    NetworkOtaUpdater,
    parse_device_target,
)


class FakeResponse:
    def __init__(self, status: int, payload: object) -> None:
        self.status = status
        self.content = (
            payload
            if isinstance(payload, bytes)
            else json.dumps(payload).encode("utf-8")
        )

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        return False

    def read(self, size: int = -1) -> bytes:
        if size < 0:
            return self.content
        return self.content[:size]


class Router:
    def __init__(self) -> None:
        self.requests = []

    def __call__(self, request, timeout=0):
        self.requests.append(request)
        path = request.full_url.split("192.0.2.223", 1)[1]
        if path == "/api/auth/login":
            return FakeResponse(
                202,
                {"pending": True, "job_id": "abc", "poll_after_ms": 50},
            )
        if path == "/api/auth/login/status?job_id=abc":
            return FakeResponse(200, {"token": "session-token"})
        if path == "/api/v1/network/status":
            return FakeResponse(
                200,
                {
                    "device_id": "ea-p4-test",
                    "hostname": "exoanchor-test",
                    "ipv4": "192.0.2.223",
                    "link_up": True,
                },
            )
        if path == "/api/v1/network/config" and request.method == "GET":
            return FakeResponse(
                200,
                {
                    "active": {
                        "mode": "dhcp",
                        "hostname": "exoanchor-test",
                    },
                    "have_staged": False,
                },
            )
        if path == "/api/v1/network/config" and request.method == "POST":
            return FakeResponse(
                200,
                {
                    "ok": True,
                    "action": "dhcp",
                    "message": "network dhcp accepted",
                },
            )
        raise AssertionError(path)


class FakeOtaClient:
    def __init__(self, board: str) -> None:
        self.board = board
        self.uploaded = b""
        self.rebooted = False
        self.active = False
        self.session_id = "ota-test-session"
        self.expected_size = 0
        self.expected_sha256 = ""

    def get_json(self, path: str):
        if path == "/api/capabilities":
            return {"device": {"board": self.board}}
        if path == "/api/ota/upload/status":
            return {
                "ok": True,
                "active": self.active,
                "session_id": self.session_id if self.active else "",
                "offset": len(self.uploaded),
                "size": self.expected_size,
                "sha256": self.expected_sha256 if self.active else "",
                "chunk_size": 8,
            }
        raise AssertionError(path)

    def post_binary(self, path: str, payload: bytes):
        parsed = urllib.parse.urlsplit(path)
        self.assert_path(parsed.path, "/api/ota/upload/chunk")
        query = urllib.parse.parse_qs(parsed.query)
        if query.get("session") != [self.session_id]:
            raise AssertionError(query)
        offset = int(query["offset"][0])
        if offset != len(self.uploaded):
            raise AssertionError((offset, len(self.uploaded)))
        self.uploaded += payload
        return {"ok": True, "offset": len(self.uploaded)}

    def post_json(self, path: str, payload: dict[str, object]):
        if path == "/api/ota/upload/start":
            self.active = True
            self.expected_size = int(payload["size"])
            self.expected_sha256 = str(payload["sha256"])
            return {
                "ok": True,
                "session_id": self.session_id,
                "offset": 0,
                "chunk_size": 8,
            }
        if path == "/api/ota/upload/finish":
            self.active = False
            return {
                "ok": True,
                "sha256": hashlib.sha256(self.uploaded).hexdigest(),
            }
        if path == "/api/ota/reboot":
            self.rebooted = True
            return {"ok": True}
        raise AssertionError(path)

    @staticmethod
    def assert_path(actual: str, expected: str) -> None:
        if actual != expected:
            raise AssertionError((actual, expected))


class DeviceHttpTests(unittest.TestCase):
    def test_default_device_transport_bypasses_system_proxies(self) -> None:
        client = DeviceHttpClient("192.0.2.244")
        director = getattr(client._opener, "__self__", None)
        handlers = getattr(director, "handlers", [])
        proxy_handlers = [
            handler
            for handler in handlers
            if isinstance(handler, urllib.request.ProxyHandler)
        ]
        self.assertIsNot(client._opener, urllib.request.urlopen)
        self.assertEqual(proxy_handlers, [])

    def test_target_is_limited_to_http_ipv4(self) -> None:
        self.assertEqual(
            parse_device_target("192.0.2.223:8080").base_url,
            "http://192.0.2.223:8080",
        )
        with self.assertRaises(ToolkitError):
            parse_device_target("https://192.0.2.223")
        with self.assertRaises(ToolkitError):
            parse_device_target("example.com")

    def test_async_login_and_network_operations(self) -> None:
        router = Router()
        client = DeviceHttpClient(
            "192.0.2.223",
            username="admin",
            password="123456",
            opener=router,
            sleeper=lambda _: None,
        )
        client.login()
        self.assertEqual(client.token, "session-token")
        provisioner = HttpNetworkProvisioner(client)
        snapshot = provisioner.show()
        self.assertEqual(snapshot.identity["device_id"], "ea-p4-test")
        self.assertEqual(
            provisioner.stage_dhcp("exoanchor-test"),
            "network dhcp accepted",
        )
        authenticated = [
            request
            for request in router.requests
            if request.full_url.endswith("/api/v1/network/status")
        ][0]
        self.assertEqual(
            authenticated.headers["Authorization"],
            "Bearer session-token",
        )

    def test_mutating_request_authenticates_before_posting(self) -> None:
        router = Router()
        client = DeviceHttpClient(
            "192.0.2.223",
            username="admin",
            password="123456",
            opener=router,
            sleeper=lambda _: None,
        )

        result = client.post_json(
            "/api/v1/network/config",
            {"action": "dhcp"},
        )

        self.assertTrue(result["ok"])
        self.assertEqual(
            [request.method for request in router.requests],
            ["POST", "GET", "POST"],
        )
        self.assertTrue(
            router.requests[0].full_url.endswith("/api/auth/login")
        )
        mutation = router.requests[-1]
        self.assertTrue(mutation.full_url.endswith("/api/v1/network/config"))
        self.assertEqual(
            mutation.headers["Authorization"],
            "Bearer session-token",
        )

    def test_ota_upload_checks_board_and_digest(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            image = Path(temp) / "app.bin"
            image.write_bytes(b"verified app image")
            digest = hashlib.sha256(image.read_bytes()).hexdigest()
            segment = FirmwareSegment(
                address=0x20000,
                path=image,
                relative_path="app.bin",
                size=image.stat().st_size,
                sha256=digest,
            )
            package = FirmwarePackage(
                root=Path(temp),
                manifest_path=Path(temp) / "exoanchor-firmware.json",
                version="0.87.3-dev",
                board="exoanchor-prototype-v2.3",
                chip="esp32p4",
                flash_size="16MB",
                flash_mode="dio",
                flash_frequency="80m",
                before="default_reset",
                after="hard_reset",
                use_stub=True,
                segments=(segment,),
            )
            client = FakeOtaClient(package.board)
            lines = NetworkOtaUpdater(client).update(package)
            self.assertEqual(client.uploaded, image.read_bytes())
            self.assertTrue(client.rebooted)
            self.assertTrue(any("SHA-256 verified" in line for line in lines))

            resumed = FakeOtaClient(package.board)
            resumed.active = True
            resumed.expected_size = len(image.read_bytes())
            resumed.expected_sha256 = digest
            resumed.uploaded = image.read_bytes()[:5]
            NetworkOtaUpdater(resumed).update(package, reboot=False)
            self.assertEqual(resumed.uploaded, image.read_bytes())
            self.assertFalse(resumed.rebooted)

            with self.assertRaisesRegex(ToolkitError, "does not match"):
                NetworkOtaUpdater(FakeOtaClient("other-board")).update(package)

            for blocked_board in (
                "retired-board-profile",
                "exoanchor-prototype-v2.4-ms-test",
            ):
                with self.subTest(blocked_board=blocked_board):
                    blocked = replace(package, board=blocked_board)
                    blocked_client = mock.Mock()
                    with self.assertRaisesRegex(
                        ToolkitError, "unsupported board profile"
                    ):
                        NetworkOtaUpdater(blocked_client).update(blocked)
                    blocked_client.get_json.assert_not_called()

if __name__ == "__main__":
    unittest.main()
