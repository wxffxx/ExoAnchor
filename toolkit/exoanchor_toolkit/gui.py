from __future__ import annotations

import json
import os
import secrets
import shutil
import subprocess
import sys
import threading
import webbrowser
from contextlib import contextmanager
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib import resources
from pathlib import Path
from typing import Iterator
from urllib.parse import parse_qs, urlsplit

from . import __version__
from .discovery import discover_devices, identify_uart_device
from .errors import ToolkitError
from .firmware import flash_firmware, load_firmware_package
from .http_device import (
    DeviceHttpClient,
    HttpNetworkProvisioner,
    NetworkOtaUpdater,
)
from .network import NetworkProvisioner
from .network_configure import NetworkConfiguration, configure_network
from .ports import list_serial_ports, resolve_port
from .releases import (
    DEFAULT_GITHUB_REPOSITORY,
    download_firmware_release,
    download_latest_firmware,
    firmware_releases,
    open_firmware_package,
)
from .serial_cli import DiagnosticConsole


MAX_REQUEST_BYTES = 32 * 1024
LOOPBACK_HOST = "127.0.0.1"
TRANSPORTS = {"network", "uart"}


def _string(
    payload: dict[str, object],
    key: str,
    *,
    default: str = "",
    required: bool = False,
    limit: int = 1024,
) -> str:
    value = payload.get(key, default)
    if not isinstance(value, str):
        raise ToolkitError(f"{key} must be text")
    value = value.strip()
    if required and not value:
        raise ToolkitError(f"{key} is required")
    if len(value) > limit:
        raise ToolkitError(f"{key} is too long")
    return value


def _integer(
    payload: dict[str, object],
    key: str,
    *,
    default: int,
    minimum: int = 1,
    maximum: int = 2_000_000,
) -> int:
    value = payload.get(key, default)
    if isinstance(value, bool):
        raise ToolkitError(f"{key} must be an integer")
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise ToolkitError(f"{key} must be an integer") from exc
    if not minimum <= parsed <= maximum:
        raise ToolkitError(f"{key} must be between {minimum} and {maximum}")
    return parsed


def _number(
    payload: dict[str, object],
    key: str,
    *,
    default: float,
    minimum: float,
    maximum: float,
) -> float:
    value = payload.get(key, default)
    if isinstance(value, bool):
        raise ToolkitError(f"{key} must be a number")
    try:
        parsed = float(value)
    except (TypeError, ValueError) as exc:
        raise ToolkitError(f"{key} must be a number") from exc
    if not minimum <= parsed <= maximum:
        raise ToolkitError(f"{key} must be between {minimum} and {maximum}")
    return parsed


def _transport(payload: dict[str, object]) -> str:
    value = _string(payload, "transport", default="network", limit=16)
    if value not in TRANSPORTS:
        raise ToolkitError("transport must be network or uart")
    return value


def _choose_manifest() -> str:
    """Open a platform-native file chooser without making Tk a dependency."""
    if sys.platform == "darwin":
        command = [
            "/usr/bin/osascript",
            "-e",
            'POSIX path of (choose file with prompt "Choose ExoAnchor firmware manifest")',
        ]
    elif os.name == "nt":
        command = [
            "powershell",
            "-NoProfile",
            "-STA",
            "-Command",
            (
                "Add-Type -AssemblyName System.Windows.Forms;"
                "$d=New-Object System.Windows.Forms.OpenFileDialog;"
                "$d.Title='Choose ExoAnchor firmware manifest';"
                "$d.Filter='ExoAnchor firmware|exoanchor-firmware.json|JSON|*.json';"
                "if($d.ShowDialog() -eq 'OK'){$d.FileName}"
            ),
        ]
    elif shutil.which("zenity"):
        command = [
            "zenity",
            "--file-selection",
            "--title=Choose ExoAnchor firmware manifest",
            "--file-filter=exoanchor-firmware.json",
            "--file-filter=JSON files | *.json",
        ]
    else:
        raise ToolkitError(
            "no native file chooser is available; paste the manifest path instead"
        )
    try:
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=300,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ToolkitError(f"cannot open the file chooser: {exc}") from exc
    selected = result.stdout.strip()
    if result.returncode != 0 or not selected:
        raise ToolkitError("file selection cancelled")
    return str(Path(selected).expanduser().resolve())


class ToolkitController:
    """JSON-facing adapter over the existing CLI implementation modules."""

    def __init__(self) -> None:
        self._operation_lock = threading.Lock()
        self._device_tokens: dict[tuple[str, str], str] = {}

    @contextmanager
    def _exclusive(self) -> Iterator[None]:
        if not self._operation_lock.acquire(blocking=False):
            raise ToolkitError("another serial or firmware operation is active")
        try:
            yield
        finally:
            self._operation_lock.release()

    def ports(self) -> dict[str, object]:
        return {"ports": [item.to_dict() for item in list_serial_ports()]}

    def _http_client(
        self,
        payload: dict[str, object],
        *,
        timeout: float = 15,
    ) -> tuple[DeviceHttpClient, tuple[str, str]]:
        target = _string(payload, "target", required=True, limit=64)
        username = _string(payload, "username", default="admin", limit=32)
        password = _string(payload, "password", limit=64)
        key = (target, username)
        return (
            DeviceHttpClient(
                target,
                username=username,
                password=password,
                token=self._device_tokens.get(key, ""),
                timeout=timeout,
            ),
            key,
        )

    def _remember_device_token(
        self,
        key: tuple[str, str],
        client: DeviceHttpClient,
    ) -> None:
        if client.token:
            self._device_tokens[key] = client.token
        else:
            self._device_tokens.pop(key, None)

    def _device_config_label(
        self,
        device: dict[str, object],
        payload: dict[str, object],
    ) -> dict[str, str]:
        existing = device.get("device_label")
        if isinstance(existing, str) and existing.strip():
            return {
                "device_label": existing.strip()[:48],
                "device_label_status": "discovery",
            }

        address = device.get("source_ip")
        port = device.get("http_port", 80)
        if not isinstance(address, str) or not address:
            return {
                "device_label": "",
                "device_label_status": "unavailable",
            }
        try:
            http_port = int(port)
        except (TypeError, ValueError):
            return {
                "device_label": "",
                "device_label_status": "error",
                "device_label_error": "device reported an invalid HTTP port",
            }
        suffix = "" if http_port == 80 else f":{http_port}"
        target = address + suffix
        username = _string(payload, "username", default="admin", limit=32)
        password = _string(payload, "password", limit=64)
        key = (target, username)
        token = self._device_tokens.get(key, "")
        if not password and not token:
            return {
                "device_label": "",
                "device_label_status": "authentication_required",
            }

        client = DeviceHttpClient(
            target,
            username=username,
            password=password,
            token=token,
            timeout=2,
        )
        try:
            info = client.get_json("/api/system/info")
        except ToolkitError as exc:
            return {
                "device_label": "",
                "device_label_status": "error",
                "device_label_error": str(exc),
            }
        self._remember_device_token(key, client)
        label = info.get("device_label") or info.get("label")
        if not isinstance(label, str):
            return {
                "device_label": "",
                "device_label_status": "empty",
            }
        label = label.strip()
        if not label:
            return {
                "device_label": "",
                "device_label_status": "empty",
            }
        if len(label) > 48:
            return {
                "device_label": "",
                "device_label_status": "error",
                "device_label_error": "device returned an invalid label",
            }
        return {
            "device_label": label,
            "device_label_status": "read",
        }

    def discover(self, payload: dict[str, object]) -> dict[str, object]:
        transport = _transport(payload)
        if transport == "uart":
            port = resolve_port(
                _string(payload, "port", default="auto", limit=512)
            )
            baud = _integer(payload, "baud", default=115200)
            with self._exclusive():
                device = identify_uart_device(
                    DiagnosticConsole(port=port, baud=baud, timeout=5)
                )
            device.update(self._device_config_label(device, payload))
            return {
                "transport": transport,
                "devices": [device],
            }
        timeout = _number(
            payload, "timeout", default=2.0, minimum=0.1, maximum=30.0
        )
        subnet = _string(payload, "subnet", limit=32)
        devices = discover_devices(
            timeout=timeout,
            subnets=[subnet] if subnet else None,
        )
        values = []
        for device in devices:
            value = device.to_dict()
            value["transport"] = "network"
            value.update(self._device_config_label(value, payload))
            values.append(value)
        return {"transport": transport, "devices": values}

    def network(self, payload: dict[str, object]) -> dict[str, object]:
        action = _string(payload, "action", required=True, limit=24)
        transport = _transport(payload)
        client: DeviceHttpClient | None = None
        token_key: tuple[str, str] | None = None
        original_token_key: tuple[str, str] | None = None
        if transport == "uart":
            port = resolve_port(
                _string(payload, "port", default="auto", limit=512)
            )
            baud = _integer(payload, "baud", default=115200)
            provisioner = NetworkProvisioner(
                DiagnosticConsole(port=port, baud=baud, timeout=5)
            )
            target: str = port
        else:
            client, token_key = self._http_client(payload)
            original_token_key = token_key
            provisioner = HttpNetworkProvisioner(client)
            target = client.target.base_url
        with self._exclusive():
            if action == "configure":
                configured = configure_network(
                    provisioner,
                    NetworkConfiguration(
                        mode=_string(
                            payload, "mode", required=True, limit=16
                        ),
                        hostname=_string(payload, "hostname", limit=63),
                        address=_string(payload, "address", limit=15),
                        netmask=_string(payload, "netmask", limit=15),
                        gateway=(
                            _string(
                                payload, "gateway", default="-", limit=15
                            )
                            or "-"
                        ),
                        dns_primary=(
                            _string(
                                payload,
                                "dns_primary",
                                default="-",
                                limit=15,
                            )
                            or "-"
                        ),
                        dns_secondary=(
                            _string(
                                payload,
                                "dns_secondary",
                                default="-",
                                limit=15,
                            )
                            or "-"
                        ),
                    ),
                    transport=transport,
                    target=target,
                    client=client,
                )
                result = configured.to_dict()
                configured_client = configured.client
                if configured_client is not None:
                    client = configured_client
                    normalized_target = configured_client.target.address + (
                        ""
                        if configured_client.target.port == 80
                        else f":{configured_client.target.port}"
                    )
                    token_key = (normalized_target, configured_client.username)
            elif action == "show":
                result = {
                    "transport": transport,
                    "target": target,
                    "snapshot": provisioner.show().to_dict(),
                }
            elif action == "dhcp":
                hostname = _string(payload, "hostname", limit=63) or None
                output = provisioner.stage_dhcp(hostname)
            elif action == "static":
                output = provisioner.stage_static(
                    _string(payload, "address", required=True, limit=15),
                    _string(payload, "netmask", required=True, limit=15),
                    _string(payload, "gateway", default="-", limit=15) or "-",
                    _string(payload, "dns_primary", default="-", limit=15) or "-",
                    _string(payload, "dns_secondary", default="-", limit=15) or "-",
                )
            elif action == "apply":
                output = provisioner.apply()
            elif action == "commit":
                output = provisioner.commit()
            elif action == "rollback":
                output = provisioner.rollback()
            elif action == "reset":
                output = provisioner.reset()
            else:
                raise ToolkitError(f"unsupported network action: {action}")
            if action not in {"show", "configure"}:
                result = {
                    "transport": transport,
                    "target": target,
                    "output": output,
                }
        if client is not None and token_key is not None:
            self._remember_device_token(token_key, client)
            if (
                original_token_key is not None
                and original_token_key != token_key
            ):
                self._device_tokens.pop(original_token_key, None)
        return result

    def device_label(self, payload: dict[str, object]) -> dict[str, object]:
        if _transport(payload) != "network":
            raise ToolkitError(
                "device label settings require the network transport"
            )
        label = _string(payload, "label", required=True, limit=48)
        if any(
            ord(character) < 32 or ord(character) == 127
            for character in label
        ):
            raise ToolkitError("device label contains control characters")
        device_id = _string(payload, "device_id", limit=64)
        client, token_key = self._http_client(payload)
        with self._exclusive():
            response = client.post_json(
                "/api/settings/device",
                {"label": label},
            )
        saved = response.get("label") or response.get("device_label")
        if not isinstance(saved, str) or not saved.strip():
            raise ToolkitError(
                "device did not confirm its updated label"
            )
        saved = saved.strip()
        if len(saved) > 48:
            raise ToolkitError("device returned an invalid label")
        self._remember_device_token(token_key, client)
        return {
            "device_id": device_id,
            "label": saved,
            "target": client.target.base_url,
        }

    def choose_manifest(self) -> dict[str, object]:
        return {"path": _choose_manifest()}

    def latest_firmware(self, payload: dict[str, object]) -> dict[str, object]:
        repository = _string(
            payload,
            "repository",
            default=DEFAULT_GITHUB_REPOSITORY,
            limit=256,
        )
        with self._exclusive():
            downloaded = download_latest_firmware(repository)
        return {"download": downloaded.to_dict()}

    def firmware_versions(self, payload: dict[str, object]) -> dict[str, object]:
        repository = _string(
            payload,
            "repository",
            default=DEFAULT_GITHUB_REPOSITORY,
            limit=256,
        )
        releases = firmware_releases(repository)
        return {
            "repository": repository,
            "releases": [release.to_dict() for release in releases],
        }

    def download_firmware(self, payload: dict[str, object]) -> dict[str, object]:
        repository = _string(
            payload,
            "repository",
            default=DEFAULT_GITHUB_REPOSITORY,
            limit=256,
        )
        tag = _string(payload, "tag", required=True, limit=256)
        with self._exclusive():
            downloaded = download_firmware_release(repository, tag=tag)
        return {"download": downloaded.to_dict()}

    def verify_firmware(self, payload: dict[str, object]) -> dict[str, object]:
        package_path = _string(payload, "package", required=True, limit=4096)
        with self._exclusive():
            with open_firmware_package(package_path) as package:
                result = package.to_dict()
        return {"package": result}

    def flash_firmware(self, payload: dict[str, object]) -> dict[str, object]:
        package_path = _string(payload, "package", required=True, limit=4096)
        transport = _transport(payload)
        lines: list[str] = []
        verification: dict[str, object] | None = None
        with self._exclusive():
            with open_firmware_package(package_path) as package:
                package_result = package.to_dict()
                if transport == "uart":
                    target = _string(
                        payload, "port", required=True, limit=512
                    )
                    identity_registry = _string(
                        payload,
                        "identity_registry",
                        required=True,
                        limit=4096,
                    )
                    board_record = _string(
                        payload,
                        "board_record",
                        required=True,
                        limit=256,
                    )
                    baud = _integer(payload, "baud", default=460800)
                    verification = flash_firmware(
                        package,
                        target,
                        baud,
                        output=lines.append,
                        identity_registry=identity_registry,
                        board_record=board_record,
                    )
                else:
                    client, token_key = self._http_client(
                        payload, timeout=180
                    )
                    target = client.target.base_url
                    lines.extend(
                        NetworkOtaUpdater(client).update(
                            package,
                            reboot=payload.get("reboot") is not False,
                        )
                    )
                    self._remember_device_token(token_key, client)
        result = {
            "transport": transport,
            "target": target,
            "package": package_result,
            "output": lines,
        }
        if verification is not None:
            result["verification"] = verification
        return result


class ToolkitHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        address: tuple[str, int],
        controller: ToolkitController | None = None,
    ) -> None:
        super().__init__(address, ToolkitRequestHandler)
        self.controller = controller or ToolkitController()
        self.token = secrets.token_urlsafe(24)
        self.csp_nonce = secrets.token_urlsafe(18)

    @property
    def url(self) -> str:
        port = int(self.server_address[1])
        return f"http://{LOOPBACK_HOST}:{port}/?token={self.token}"


class ToolkitRequestHandler(BaseHTTPRequestHandler):
    server: ToolkitHTTPServer
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: object) -> None:
        return

    def _host_is_loopback(self) -> bool:
        value = self.headers.get("Host", "")
        if value.startswith("["):
            hostname = value.split("]", 1)[0][1:]
        else:
            hostname = value.rsplit(":", 1)[0]
        return hostname.lower() in {"127.0.0.1", "localhost", "::1"}

    def _authorized(self, query: dict[str, list[str]] | None = None) -> bool:
        if not self._host_is_loopback():
            return False
        supplied = self.headers.get("X-ExoAnchor-Token", "")
        if not supplied and query:
            supplied = (query.get("token") or [""])[0]
        return secrets.compare_digest(supplied, self.server.token)

    def _send(
        self,
        status: HTTPStatus,
        content: bytes,
        content_type: str,
    ) -> None:
        self.send_response(int(status))
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header(
            "Content-Security-Policy",
            (
                "default-src 'none'; "
                f"style-src 'nonce-{self.server.csp_nonce}'; "
                f"script-src 'nonce-{self.server.csp_nonce}'; "
                "img-src 'self' data:; connect-src 'self'; "
                "base-uri 'none'; form-action 'none'; frame-ancestors 'none'"
            ),
        )
        self.end_headers()
        self.wfile.write(content)

    def _json(
        self,
        status: HTTPStatus,
        payload: dict[str, object],
    ) -> None:
        content = json.dumps(
            payload, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        self._send(status, content, "application/json; charset=utf-8")

    def do_GET(self) -> None:
        request = urlsplit(self.path)
        query = parse_qs(request.query)
        if request.path != "/" or not self._authorized(query):
            self._json(HTTPStatus.FORBIDDEN, {"ok": False, "error": "forbidden"})
            return
        template = (
            resources.files("exoanchor_toolkit")
            .joinpath("toolkit_ui.html")
            .read_text(encoding="utf-8")
        )
        html = (
            template.replace("{{CSP_NONCE}}", self.server.csp_nonce)
            .replace("{{TOOLKIT_VERSION}}", __version__)
            .encode("utf-8")
        )
        self._send(HTTPStatus.OK, html, "text/html; charset=utf-8")

    def do_OPTIONS(self) -> None:
        self._json(HTTPStatus.FORBIDDEN, {"ok": False, "error": "forbidden"})

    def do_POST(self) -> None:
        if not self._authorized():
            self._json(HTTPStatus.FORBIDDEN, {"ok": False, "error": "forbidden"})
            return
        origin = self.headers.get("Origin")
        expected_origin = f"http://{LOOPBACK_HOST}:{self.server.server_address[1]}"
        if origin and origin != expected_origin:
            self._json(HTTPStatus.FORBIDDEN, {"ok": False, "error": "bad origin"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if not 0 <= length <= MAX_REQUEST_BYTES:
            self._json(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                {"ok": False, "error": "request is too large"},
            )
            return
        try:
            payload = json.loads(self.rfile.read(length) or b"{}")
        except (UnicodeDecodeError, json.JSONDecodeError):
            self._json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "request body must be JSON"},
            )
            return
        if not isinstance(payload, dict):
            self._json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "request body must be an object"},
            )
            return

        routes = {
            "/api/ports": lambda: self.server.controller.ports(),
            "/api/discover": lambda: self.server.controller.discover(payload),
            "/api/device/label": lambda: self.server.controller.device_label(
                payload
            ),
            "/api/network": lambda: self.server.controller.network(payload),
            "/api/firmware/choose": self.server.controller.choose_manifest,
            "/api/firmware/latest": lambda: self.server.controller.latest_firmware(
                payload
            ),
            "/api/firmware/releases": lambda: (
                self.server.controller.firmware_versions(payload)
            ),
            "/api/firmware/download": lambda: (
                self.server.controller.download_firmware(payload)
            ),
            "/api/firmware/verify": lambda: self.server.controller.verify_firmware(
                payload
            ),
            "/api/firmware/flash": lambda: self.server.controller.flash_firmware(
                payload
            ),
        }
        operation = routes.get(urlsplit(self.path).path)
        if operation is None:
            self._json(
                HTTPStatus.NOT_FOUND,
                {"ok": False, "error": "unknown endpoint"},
            )
            return
        try:
            result = operation()
        except ToolkitError as exc:
            self._json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": str(exc)},
            )
            return
        except Exception as exc:
            self._json(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                {"ok": False, "error": f"operation failed: {exc}"},
            )
            return
        self._json(HTTPStatus.OK, {"ok": True, **result})


def create_gui_server(
    port: int = 0,
    controller: ToolkitController | None = None,
) -> ToolkitHTTPServer:
    if not 0 <= port <= 65535:
        raise ToolkitError("GUI port must be between 0 and 65535")
    try:
        return ToolkitHTTPServer((LOOPBACK_HOST, port), controller)
    except OSError as exc:
        raise ToolkitError(f"cannot start local GUI server: {exc}") from exc


def launch_gui(*, port: int = 0, open_browser: bool = True) -> None:
    server = create_gui_server(port)
    print(f"ExoAnchor Toolkit {__version__}: {server.url}")
    print("The interface is local-only. Press Ctrl+C to stop.")
    if open_browser:
        threading.Timer(0.15, lambda: webbrowser.open(server.url)).start()
    try:
        server.serve_forever(poll_interval=0.25)
    finally:
        server.server_close()
