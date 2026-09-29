from __future__ import annotations

import ipaddress
import json
import secrets
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Callable

from .errors import ToolkitError
from .firmware import FirmwarePackage
from .network import (
    NetworkSnapshot,
    validate_hostname,
    validate_ipv4,
    validate_netmask,
)


NETWORK_OTA_BOARD_ALLOWLIST = frozenset(
    {
        "waveshare-p4-nano",
        "exoanchor-esp32p4x",
        "exoanchor-prototype0",
        "exoanchor-prototype-v2.1",
        "exoanchor-prototype-v2.3",
        "exoanchor-prototype-v2.4",
    }
)


MAX_JSON_RESPONSE_BYTES = 2 * 1024 * 1024
LOGIN_TIMEOUT_SECONDS = 30.0
DIRECT_HTTP_OPENER = urllib.request.build_opener(
    urllib.request.ProxyHandler({})
).open


@dataclass(frozen=True)
class DeviceTarget:
    address: str
    port: int

    @property
    def base_url(self) -> str:
        suffix = "" if self.port == 80 else f":{self.port}"
        return f"http://{self.address}{suffix}"


def parse_device_target(value: str) -> DeviceTarget:
    candidate = value.strip()
    if not candidate:
        raise ToolkitError("network device IP is required")
    parsed = urllib.parse.urlsplit(
        candidate if "://" in candidate else "http://" + candidate
    )
    if (
        parsed.scheme != "http"
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path not in {"", "/"}
        or parsed.query
        or parsed.fragment
    ):
        raise ToolkitError("network target must be an HTTP IPv4 address")
    try:
        address = str(ipaddress.IPv4Address(parsed.hostname or ""))
        port = parsed.port or 80
    except (ipaddress.AddressValueError, ValueError) as exc:
        raise ToolkitError("network target must be a valid IPv4 address") from exc
    if not 1 <= port <= 65535:
        raise ToolkitError("network target port is invalid")
    return DeviceTarget(address=address, port=port)


class DeviceHttpClient:
    def __init__(
        self,
        target: str,
        *,
        username: str = "admin",
        password: str = "",
        token: str = "",
        timeout: float = 10.0,
        opener: Callable[..., object] | None = None,
        sleeper: Callable[[float], None] = time.sleep,
    ) -> None:
        if timeout <= 0:
            raise ToolkitError("device HTTP timeout must be positive")
        self.target = parse_device_target(target)
        self.username = username.strip() or "admin"
        self.password = password
        self.token = token
        self.timeout = timeout
        # Device targets are validated literal private/LAN IPv4 addresses.  A
        # desktop system proxy must never intercept provisioning or multi-MB
        # OTA uploads to them.
        self._opener = opener or DIRECT_HTTP_OPENER
        self._sleep = sleeper

    def _request(
        self,
        method: str,
        path: str,
        *,
        body: bytes | None = None,
        content_type: str | None = None,
        retry_login: bool = True,
    ) -> tuple[int, bytes]:
        if not path.startswith("/") or path.startswith("//"):
            raise ToolkitError("device API path is invalid")
        headers = {
            "Accept": "application/json",
            "User-Agent": "ExoAnchor-Toolkit",
        }
        if content_type:
            headers["Content-Type"] = content_type
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        request = urllib.request.Request(
            self.target.base_url + path,
            data=body,
            headers=headers,
            method=method,
        )
        try:
            with self._opener(request, timeout=self.timeout) as response:
                content = response.read(MAX_JSON_RESPONSE_BYTES + 1)
                status = int(getattr(response, "status", 200))
        except urllib.error.HTTPError as exc:
            detail = exc.read(4096).decode("utf-8", errors="replace").strip()
            if exc.code == 401 and retry_login and self.password:
                self.login()
                return self._request(
                    method,
                    path,
                    body=body,
                    content_type=content_type,
                    retry_login=False,
                )
            if exc.code == 401:
                raise ToolkitError(
                    "device authentication required; enter its username and password"
                ) from exc
            raise ToolkitError(
                f"device returned HTTP {exc.code}: {detail or exc.reason}"
            ) from exc
        except (urllib.error.URLError, OSError) as exc:
            raise ToolkitError(
                f"cannot reach ExoAnchor at {self.target.base_url}: {exc}"
            ) from exc
        if len(content) > MAX_JSON_RESPONSE_BYTES:
            raise ToolkitError("device response is unexpectedly large")
        return status, content

    @staticmethod
    def _decode_json(content: bytes) -> dict[str, object]:
        try:
            value = json.loads(content.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ToolkitError("device returned invalid JSON") from exc
        if not isinstance(value, dict):
            raise ToolkitError("device JSON response must be an object")
        return value

    def login(self) -> None:
        body = json.dumps(
            {
                "username": self.username,
                "password": self.password,
                "request_id": secrets.token_hex(12),
            },
            separators=(",", ":"),
        ).encode("utf-8")
        status, content = self._request(
            "POST",
            "/api/auth/login",
            body=body,
            content_type="application/json",
            retry_login=False,
        )
        result = self._decode_json(content)
        deadline = time.monotonic() + LOGIN_TIMEOUT_SECONDS
        while status == 202:
            job_id = result.get("job_id")
            if not isinstance(job_id, str) or not job_id:
                raise ToolkitError("device did not create a login job")
            delay_ms = result.get("poll_after_ms", 100)
            delay = max(0.05, min(1.0, float(delay_ms) / 1000.0))
            if time.monotonic() + delay > deadline:
                raise ToolkitError("device login timed out")
            self._sleep(delay)
            status, content = self._request(
                "GET",
                "/api/auth/login/status?job_id="
                + urllib.parse.quote(job_id, safe=""),
                retry_login=False,
            )
            result = self._decode_json(content)
        token = result.get("token")
        if token is not None and not isinstance(token, str):
            raise ToolkitError("device returned an invalid login token")
        self.token = token or ""

    def get_json(self, path: str) -> dict[str, object]:
        _, content = self._request("GET", path)
        return self._decode_json(content)

    def ensure_authenticated(self) -> None:
        """Authenticate before a mutating request can reach the device.

        Retrying a POST after a 401 is unsafe when the device has already
        accepted the operation but resets or drops the response.  Reads may
        continue to use lazy authentication, while writes establish the token
        before sending their body.
        """
        if self.password and not self.token:
            self.login()

    def post_json(
        self,
        path: str,
        payload: dict[str, object],
    ) -> dict[str, object]:
        self.ensure_authenticated()
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        _, content = self._request(
            "POST",
            path,
            body=body,
            content_type="application/json",
        )
        return self._decode_json(content)

    def post_binary(self, path: str, payload: bytes) -> dict[str, object]:
        self.ensure_authenticated()
        _, content = self._request(
            "POST",
            path,
            body=payload,
            content_type="application/octet-stream",
        )
        return self._decode_json(content)


class HttpNetworkProvisioner:
    def __init__(self, client: DeviceHttpClient) -> None:
        self.client = client

    @staticmethod
    def _text_values(value: object) -> dict[str, str]:
        if not isinstance(value, dict):
            return {}
        return {
            str(key): (
                "1" if item is True else
                "0" if item is False else
                str(item)
            )
            for key, item in value.items()
            if item is not None and not isinstance(item, (dict, list))
        }

    def show(self) -> NetworkSnapshot:
        status = self.client.get_json("/api/v1/network/status")
        config = self.client.get_json("/api/v1/network/config")
        active = self._text_values(config.get("active"))
        staged = (
            self._text_values(config.get("staged"))
            if config.get("have_staged") is True
            else None
        )
        runtime = self._text_values(status)
        identity = {
            "device_id": runtime.get("device_id", ""),
            "hostname": runtime.get("hostname", active.get("hostname", "")),
            "mac": runtime.get("mac", ""),
        }
        return NetworkSnapshot(
            runtime=runtime,
            identity=identity,
            active=active,
            staged=staged,
            raw=json.dumps(
                {"status": status, "config": config},
                ensure_ascii=False,
                separators=(",", ":"),
            ),
        )

    def _action(
        self,
        action: str,
        **values: object,
    ) -> str:
        result = self.client.post_json(
            "/api/v1/network/config",
            {"action": action, **values},
        )
        message = result.get("message")
        return (
            str(message)
            if isinstance(message, str) and message
            else f"network {action} accepted"
        )

    def stage_dhcp(self, hostname: str | None = None) -> str:
        payload: dict[str, object] = {}
        if hostname:
            payload["hostname"] = validate_hostname(hostname)
        return self._action("dhcp", **payload)

    def stage_static(
        self,
        address: str,
        netmask: str,
        gateway: str = "-",
        dns_primary: str = "-",
        dns_secondary: str = "-",
    ) -> str:
        return self._action(
            "static",
            address=validate_ipv4(address, "address"),
            netmask=validate_netmask(netmask),
            gateway=validate_ipv4(gateway, "gateway", optional=True),
            dns_primary=validate_ipv4(
                dns_primary, "primary DNS", optional=True
            ),
            dns_secondary=validate_ipv4(
                dns_secondary, "secondary DNS", optional=True
            ),
        )

    def stage_hostname(self, hostname: str) -> str:
        return self._action("hostname", hostname=validate_hostname(hostname))

    def apply(self) -> str:
        return self._action("apply")

    def commit(self) -> str:
        return self._action("commit")

    def rollback(self) -> str:
        return self._action("rollback")

    def reset(self) -> str:
        return self._action("reset", confirm=True)


class NetworkOtaUpdater:
    def __init__(self, client: DeviceHttpClient) -> None:
        self.client = client

    def update(
        self,
        package: FirmwarePackage,
        *,
        reboot: bool = True,
    ) -> list[str]:
        ensure_network_ota_allowed(package)
        capabilities = self.client.get_json("/api/capabilities")
        device = capabilities.get("device")
        board = device.get("board") if isinstance(device, dict) else None
        if not isinstance(board, str) or board != package.board:
            raise ToolkitError(
                f"firmware board {package.board} does not match device "
                f"{board or 'unknown'}"
            )
        app_segments = [
            segment for segment in package.segments if segment.address == 0x20000
        ]
        if len(app_segments) != 1:
            raise ToolkitError(
                "network OTA requires exactly one application segment at 0x20000"
            )
        segment = app_segments[0]
        try:
            payload = segment.path.read_bytes()
        except OSError as exc:
            raise ToolkitError(f"cannot read OTA application image: {exc}") from exc

        status = self.client.get_json("/api/ota/upload/status")
        session_id = ""
        offset = 0
        if status.get("active") is True:
            status_sha = status.get("sha256")
            status_size = status.get("size")
            status_session = status.get("session_id")
            status_offset = status.get("offset")
            if (
                status_sha != segment.sha256
                or status_size != len(payload)
                or not isinstance(status_session, str)
                or not isinstance(status_offset, int)
                or not 0 <= status_offset <= len(payload)
            ):
                raise ToolkitError(
                    "device has a different active OTA upload session; "
                    "abort it explicitly before starting another image"
                )
            session_id = status_session
            offset = status_offset
        else:
            started = self.client.post_json(
                "/api/ota/upload/start",
                {"size": len(payload), "sha256": segment.sha256},
            )
            session_id = str(started.get("session_id") or "")
            offset_value = started.get("offset")
            if started.get("ok") is not True or not session_id:
                raise ToolkitError("device did not create an OTA upload session")
            if not isinstance(offset_value, int) or offset_value != 0:
                raise ToolkitError("device returned an invalid OTA start offset")
            offset = offset_value

        chunk_size_value = status.get("chunk_size", 64 * 1024)
        if not isinstance(chunk_size_value, int):
            chunk_size_value = 64 * 1024
        chunk_size = max(4096, min(64 * 1024, chunk_size_value))
        while offset < len(payload):
            end = min(len(payload), offset + chunk_size)
            path = (
                "/api/ota/upload/chunk?session="
                + urllib.parse.quote(session_id, safe="")
                + "&offset="
                + str(offset)
            )
            try:
                chunk_result = self.client.post_binary(path, payload[offset:end])
            except ToolkitError:
                # The device may have committed part or all of a chunk before
                # the response was lost. Read its authoritative offset instead
                # of blindly replaying bytes into the OTA stream.
                recovered = self.client.get_json("/api/ota/upload/status")
                recovered_offset = recovered.get("offset")
                if (
                    recovered.get("active") is True
                    and recovered.get("session_id") == session_id
                    and isinstance(recovered_offset, int)
                    and offset < recovered_offset <= end
                ):
                    offset = recovered_offset
                    continue
                raise
            next_offset = chunk_result.get("offset")
            if (
                chunk_result.get("ok") is not True
                or not isinstance(next_offset, int)
                or not offset < next_offset <= end
            ):
                raise ToolkitError("device returned an invalid OTA chunk offset")
            offset = next_offset

        result = self.client.post_json(
            "/api/ota/upload/finish", {"session_id": session_id}
        )
        returned_sha = result.get("sha256")
        if not isinstance(returned_sha, str) or returned_sha.lower() != segment.sha256:
            raise ToolkitError(
                "device OTA SHA-256 does not match the verified package"
            )
        lines = [
            f"OTA image uploaded: {len(payload)} bytes",
            f"SHA-256 verified: {segment.sha256}",
        ]
        if reboot:
            self.client.post_json("/api/ota/reboot", {})
            lines.append("Device reboot requested")
        else:
            lines.append("Reboot required to activate the update")
        return lines


def ensure_network_ota_allowed(package: FirmwarePackage) -> None:
    if package.board not in NETWORK_OTA_BOARD_ALLOWLIST:
        raise ToolkitError(
            "network OTA is unavailable for unsupported board profile: "
            f"{package.board}"
        )
