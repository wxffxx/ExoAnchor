from __future__ import annotations

import json
import threading
import unittest
import urllib.error
import urllib.request
from unittest import mock
from unittest.mock import patch

from exoanchor_toolkit.discovery import DiscoveredDevice
from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.gui import ToolkitController, create_gui_server
from exoanchor_toolkit.http_device import DeviceTarget
from exoanchor_toolkit.network import NetworkSnapshot


class FakeNetworkProvisioner:
    def __init__(self) -> None:
        self.calls: list[object] = []
        self.snapshot = NetworkSnapshot(
            runtime={
                "connected": "1",
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
                "gateway": "none",
                "dns1": "none",
                "dns2": "none",
            },
            staged=None,
            raw="",
        )

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


class FakeDeviceLabelClient:
    def __init__(self) -> None:
        self.target = DeviceTarget("192.0.2.223", 80)
        self.username = "admin"
        self.password = "test-only"
        self.token = "session-token"
        self.paths: list[str] = []
        self.posts: list[tuple[str, dict[str, object]]] = []

    def get_json(self, path: str) -> dict[str, object]:
        self.paths.append(path)
        return {
            "device_label": "机房 KVM",
            "hostname": "exoanchor-test",
        }

    def post_json(
        self,
        path: str,
        payload: dict[str, object],
    ) -> dict[str, object]:
        self.posts.append((path, payload))
        return {"ok": True, "label": payload["label"]}


class FakeController:
    def __init__(self) -> None:
        self.payloads: list[dict[str, object]] = []

    def ports(self) -> dict[str, object]:
        return {
            "ports": [
                {
                    "device": "/dev/test",
                    "description": "ExoAnchor",
                    "hwid": "test",
                    "vid": 0x1A86,
                    "pid": 0x55D3,
                    "serial_number": "1",
                    "manufacturer": None,
                    "likely_exoanchor": True,
                }
            ]
        }

    def discover(self, payload: dict[str, object]) -> dict[str, object]:
        self.payloads.append(payload)
        return {"devices": []}

    def network(self, payload: dict[str, object]) -> dict[str, object]:
        self.payloads.append(payload)
        return {"output": "ok"}

    def device_label(self, payload: dict[str, object]) -> dict[str, object]:
        self.payloads.append(payload)
        return {
            "device_id": payload["device_id"],
            "label": payload["label"],
        }

    def choose_manifest(self) -> dict[str, object]:
        return {"path": "/tmp/exoanchor-firmware.json"}

    def latest_firmware(self, payload: dict[str, object]) -> dict[str, object]:
        return {
            "download": {
                "directory": "/tmp/firmware",
                "package": {"version": "test"},
                "release": {"tag": "firmware-vtest"},
                "cached": False,
            }
        }

    def firmware_versions(self, payload: dict[str, object]) -> dict[str, object]:
        self.payloads.append(payload)
        return {
            "repository": "wxffxx/ExoAnchor",
            "releases": [
                {
                    "tag": "firmware-v0.87.3-dev",
                    "name": "P4 IndigoShore v0.87.3-dev",
                    "prerelease": True,
                }
            ],
        }

    def download_firmware(self, payload: dict[str, object]) -> dict[str, object]:
        self.payloads.append(payload)
        return {
            "download": {
                "directory": "/tmp/firmware",
                "package": {"version": "test"},
                "release": {"tag": payload["tag"]},
                "cached": True,
            }
        }

    def verify_firmware(self, payload: dict[str, object]) -> dict[str, object]:
        return {"package": {"version": "test"}}

    def flash_firmware(self, payload: dict[str, object]) -> dict[str, object]:
        return {"package": {"version": "test"}, "output": []}


class GuiServerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.controller = FakeController()
        self.server = create_gui_server(0, self.controller)
        self.thread = threading.Thread(
            target=self.server.serve_forever,
            kwargs={"poll_interval": 0.01},
            daemon=True,
        )
        self.thread.start()
        self.origin = (
            f"http://127.0.0.1:{self.server.server_address[1]}"
        )

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=1)

    def _post(
        self,
        path: str,
        payload: dict[str, object],
        *,
        token: str | None = None,
        origin: str | None = None,
    ) -> tuple[int, dict[str, object]]:
        headers = {"Content-Type": "application/json"}
        if token is not None:
            headers["X-ExoAnchor-Token"] = token
        if origin is not None:
            headers["Origin"] = origin
        request = urllib.request.Request(
            self.origin + path,
            data=json.dumps(payload).encode(),
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=2) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as exc:
            return exc.code, json.loads(exc.read())

    def test_root_requires_token_and_renders_device_style(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as denied:
            urllib.request.urlopen(self.origin + "/", timeout=2)
        self.assertEqual(denied.exception.code, 403)

        with urllib.request.urlopen(self.server.url, timeout=2) as response:
            html = response.read().decode()
            csp = response.headers["Content-Security-Policy"]
        self.assertIn("ExoAnchor", html)
        self.assertIn("Provisioning Toolkit", html)
        self.assertIn("--bg:#0b0d10", html)
        self.assertIn("设备发现", html)
        self.assertIn("GitHub 固件版本", html)
        self.assertIn("获取所选版本", html)
        self.assertIn("操作通道", html)
        self.assertIn("设备标注名", html)
        self.assertIn('placeholder="输入设备 IP"', html)
        self.assertIn('placeholder="输入设备用户名"', html)
        self.assertIn('placeholder="输入设备标注名"', html)
        self.assertIn('placeholder="输入网段"', html)
        self.assertIn('placeholder="选择固件包"', html)
        self.assertIn('id="identityRegistry"', html)
        self.assertIn('id="boardRecord"', html)
        self.assertNotIn('value="admin"', html)
        self.assertNotIn('value="255.255.255.0"', html)
        self.assertNotIn("通过 UDP 广播", html)
        self.assertNotIn("通过网络广播", html)
        self.assertNotIn("UART 执行分段烧录", html)
        self.assertNotIn("烧录前会校验", html)
        self.assertNotIn("仅监听本机", html)
        self.assertIn('authentication_required:"需认证"', html)
        self.assertIn('error:"读取失败"', html)
        self.assertNotIn("本地备注", html)
        self.assertIn("网络更新…", html)
        self.assertIn('id="scanTargetIp">扫描 IP</button>', html)
        self.assertIn(
            'id="connectDevice" class="primary">连接设备</button>',
            html,
        )
        self.assertIn('id="activityPanel" class="card activity" hidden', html)
        self.assertIn('id="saveNetworkConfig"', html)
        self.assertIn('id="networkMode"', html)
        self.assertIn("静态 IPv4", html)
        self.assertNotIn("应用暂存", html)
        self.assertNotIn("确认保存", html)
        self.assertNotIn("先 Stage", html)
        self.assertIn(
            '$("connectDevice").addEventListener("click",readNetworkState);',
            html,
        )
        self.assertNotIn(
            'if(button.dataset.page==="network")readNetworkState();',
            html,
        )
        self.assertNotIn(
            'state.transport==="network"&&!$("devicePassword").value',
            html,
        )
        self.assertNotIn(
            '$("devicePassword").addEventListener("change",()=>',
            html,
        )
        self.assertNotIn("style=", html)
        self.assertIn("frame-ancestors 'none'", csp)
        self.assertNotIn("'unsafe-inline'", csp)

    def test_uart_flash_requires_identity_fields_before_flash_backend(self) -> None:
        controller = ToolkitController()
        package = mock.Mock()
        package.to_dict.return_value = {"board": "exoanchor-prototype-v2.3"}
        context = mock.MagicMock()
        context.__enter__.return_value = package
        with (
            patch(
                "exoanchor_toolkit.gui.open_firmware_package",
                return_value=context,
            ),
            patch("exoanchor_toolkit.gui.flash_firmware") as flash,
        ):
            with self.assertRaisesRegex(ToolkitError, "identity_registry"):
                controller.flash_firmware(
                    {
                        "package": "/tmp/package",
                        "transport": "uart",
                        "port": "/dev/cu.test",
                    }
                )
        flash.assert_not_called()

    def test_api_requires_token_and_same_origin(self) -> None:
        status, _ = self._post("/api/ports", {})
        self.assertEqual(status, 403)
        status, _ = self._post(
            "/api/ports",
            {},
            token=self.server.token,
            origin="https://example.com",
        )
        self.assertEqual(status, 403)

        status, result = self._post(
            "/api/ports",
            {},
            token=self.server.token,
            origin=self.origin,
        )
        self.assertEqual(status, 200)
        self.assertTrue(result["ok"])
        self.assertEqual(result["ports"][0]["device"], "/dev/test")

    def test_discovery_payload_is_forwarded(self) -> None:
        status, result = self._post(
            "/api/discover",
            {"timeout": 1.5, "subnet": "192.0.2.0/24"},
            token=self.server.token,
            origin=self.origin,
        )
        self.assertEqual(status, 200)
        self.assertEqual(result["devices"], [])
        self.assertEqual(
            self.controller.payloads,
            [{"timeout": 1.5, "subnet": "192.0.2.0/24"}],
        )

    def test_device_label_route_is_available(self) -> None:
        payload = {
            "transport": "network",
            "target": "192.0.2.223",
            "device_id": "ea-p4-test",
            "label": "ESP32P4_PROTOTYPEv2.3b4",
        }
        status, result = self._post(
            "/api/device/label",
            payload,
            token=self.server.token,
            origin=self.origin,
        )
        self.assertEqual(status, 200)
        self.assertEqual(result["device_id"], "ea-p4-test")
        self.assertEqual(result["label"], "ESP32P4_PROTOTYPEv2.3b4")
        self.assertEqual(self.controller.payloads[-1], payload)

    def test_network_configuration_is_forwarded_as_one_operation(self) -> None:
        payload = {
            "action": "configure",
            "transport": "uart",
            "mode": "dhcp",
            "hostname": "exoanchor-test",
            "port": "/dev/test",
            "baud": 115200,
        }
        status, result = self._post(
            "/api/network",
            payload,
            token=self.server.token,
            origin=self.origin,
        )
        self.assertEqual(status, 200)
        self.assertTrue(result["ok"])
        self.assertEqual(self.controller.payloads[-1], payload)

    def test_firmware_release_selection_routes_are_available(self) -> None:
        status, versions = self._post(
            "/api/firmware/releases",
            {"repository": "wxffxx/ExoAnchor"},
            token=self.server.token,
            origin=self.origin,
        )
        self.assertEqual(status, 200)
        self.assertEqual(
            versions["releases"][0]["tag"], "firmware-v0.87.3-dev"
        )

        status, downloaded = self._post(
            "/api/firmware/download",
            {
                "repository": "wxffxx/ExoAnchor",
                "tag": "firmware-v0.87.3-dev",
            },
            token=self.server.token,
            origin=self.origin,
        )
        self.assertEqual(status, 200)
        self.assertEqual(
            downloaded["download"]["release"]["tag"],
            "firmware-v0.87.3-dev",
        )


class NetworkConfigurationTests(unittest.TestCase):
    def test_device_label_is_written_to_device_settings(self) -> None:
        controller = ToolkitController()
        client = FakeDeviceLabelClient()
        with patch(
            "exoanchor_toolkit.gui.DeviceHttpClient",
            return_value=client,
        ):
            result = controller.device_label(
                {
                    "transport": "network",
                    "target": "192.0.2.223",
                    "username": "admin",
                    "password": "test-only",
                    "device_id": "ea-p4-test",
                    "label": "ESP32P4_PROTOTYPEv2.3b4",
                }
            )
        self.assertEqual(result["device_id"], "ea-p4-test")
        self.assertEqual(result["label"], "ESP32P4_PROTOTYPEv2.3b4")
        self.assertEqual(
            client.posts,
            [
                (
                    "/api/settings/device",
                    {"label": "ESP32P4_PROTOTYPEv2.3b4"},
                )
            ],
        )

    def test_discovery_reads_device_config_label_without_replacing_id(self) -> None:
        controller = ToolkitController()
        client = FakeDeviceLabelClient()
        device = DiscoveredDevice(
            device_id="ea-p4-test",
            hostname="exoanchor-test",
            ipv4="192.0.2.223",
            source_ip="192.0.2.223",
            interface="ethernet",
            address_source="static",
            firmware="0.87.3-dev",
            board="exoanchor-prototype-v2.3",
            http_port=80,
        )
        with (
            patch(
                "exoanchor_toolkit.gui.discover_devices",
                return_value=[device],
            ),
            patch(
                "exoanchor_toolkit.gui.DeviceHttpClient",
                return_value=client,
            ),
        ):
            result = controller.discover(
                {
                    "transport": "network",
                    "username": "admin",
                    "password": "test-only",
                    "timeout": 1,
                }
            )
        discovered = result["devices"][0]
        self.assertEqual(discovered["device_id"], "ea-p4-test")
        self.assertEqual(discovered["device_label"], "机房 KVM")
        self.assertEqual(discovered["device_label_status"], "read")
        self.assertEqual(client.paths, ["/api/system/info"])

    def test_discovery_reports_when_device_label_needs_authentication(self) -> None:
        controller = ToolkitController()
        device = DiscoveredDevice(
            device_id="ea-p4-test",
            hostname="exoanchor-test",
            ipv4="192.0.2.223",
            source_ip="192.0.2.223",
            interface="ethernet",
            address_source="static",
            firmware="0.87.3-dev",
            board="exoanchor-prototype-v2.3",
            http_port=80,
        )
        with (
            patch(
                "exoanchor_toolkit.gui.discover_devices",
                return_value=[device],
            ),
            patch("exoanchor_toolkit.gui.DeviceHttpClient") as http_client,
        ):
            result = controller.discover(
                {
                    "transport": "network",
                    "username": "admin",
                    "password": "",
                    "timeout": 1,
                }
            )
        discovered = result["devices"][0]
        self.assertEqual(discovered["device_label"], "")
        self.assertEqual(
            discovered["device_label_status"],
            "authentication_required",
        )
        http_client.assert_not_called()

    def test_uart_static_configuration_is_verified_before_commit(self) -> None:
        provisioner = FakeNetworkProvisioner()
        controller = ToolkitController()
        payload = {
            "action": "configure",
            "transport": "uart",
            "mode": "static",
            "hostname": "exoanchor-test",
            "address": "192.0.2.240",
            "netmask": "255.255.255.0",
            "gateway": "192.0.2.1",
            "dns_primary": "192.0.2.1",
            "dns_secondary": "1.1.1.1",
            "port": "/dev/test",
            "baud": 115200,
        }
        with (
            patch(
                "exoanchor_toolkit.gui.resolve_port",
                return_value="/dev/test",
            ),
            patch("exoanchor_toolkit.gui.DiagnosticConsole"),
            patch(
                "exoanchor_toolkit.gui.NetworkProvisioner",
                return_value=provisioner,
            ),
        ):
            result = controller.network(payload)
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
        self.assertEqual(result["snapshot"]["runtime"]["ip"], "192.0.2.240")


if __name__ == "__main__":
    unittest.main()
