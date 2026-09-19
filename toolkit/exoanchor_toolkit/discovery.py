from __future__ import annotations

import ipaddress
import json
import re
import secrets
import socket
import time
from dataclasses import asdict, dataclass
from typing import Iterable

from .errors import ToolkitError
from .network import parse_network_show
from .serial_cli import DiagnosticConsole


DISCOVERY_PORT = 39393
DISCOVERY_VERSION = 1
MAX_RESPONSE_BYTES = 4096
MAX_SCAN_HOSTS = 1024
STATUS_VALUE_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")


@dataclass(frozen=True)
class DiscoveredDevice:
    device_id: str
    hostname: str
    ipv4: str
    source_ip: str
    interface: str
    address_source: str
    firmware: str
    board: str
    http_port: int
    device_label: str = ""

    @property
    def web_url(self) -> str:
        default_port = self.http_port in {0, 80}
        port = "" if default_port else f":{self.http_port}"
        return f"http://{self.source_ip}{port}/"

    def to_dict(self) -> dict[str, object]:
        value = asdict(self)
        value["web_url"] = self.web_url
        return value


def _validated_target(value: str) -> str:
    try:
        return str(ipaddress.IPv4Address(value))
    except ipaddress.AddressValueError as exc:
        raise ToolkitError(f"invalid IPv4 discovery target: {value}") from exc


def discovery_targets(
    addresses: Iterable[str] | None = None,
    subnets: Iterable[str] | None = None,
) -> list[str]:
    targets = {
        _validated_target(address)
        for address in (addresses or ["255.255.255.255"])
    }
    host_count = 0
    for value in subnets or []:
        try:
            network = ipaddress.IPv4Network(value, strict=False)
        except (ipaddress.AddressValueError, ipaddress.NetmaskValueError) as exc:
            raise ToolkitError(f"invalid IPv4 subnet: {value}") from exc
        count = max(0, network.num_addresses - (2 if network.prefixlen <= 30 else 0))
        host_count += count
        if host_count > MAX_SCAN_HOSTS:
            raise ToolkitError(
                f"subnet scan is limited to {MAX_SCAN_HOSTS} hosts; "
                "use a /22 or smaller range"
            )
        targets.update(str(address) for address in network.hosts())
    return sorted(targets, key=lambda value: int(ipaddress.IPv4Address(value)))


def parse_discovery_response(
    payload: bytes,
    source_ip: str,
    expected_nonce: str,
) -> DiscoveredDevice | None:
    if not payload or len(payload) > MAX_RESPONSE_BYTES:
        return None
    try:
        response = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(response, dict):
        return None
    if (
        response.get("service") != "exoanchor"
        or response.get("version") != DISCOVERY_VERSION
        or response.get("nonce") != expected_nonce
    ):
        return None
    try:
        source_ip = str(ipaddress.IPv4Address(source_ip))
        reported_ip = str(ipaddress.IPv4Address(str(response.get("ipv4", ""))))
        http_port = int(response.get("http_port", 80))
    except (ipaddress.AddressValueError, TypeError, ValueError):
        return None
    if not 1 <= http_port <= 65535:
        return None

    def text(key: str, limit: int) -> str:
        value = response.get(key)
        if not isinstance(value, str):
            return ""
        value = value.strip()
        return value if 0 < len(value) <= limit else ""

    device_id = text("device_id", 64)
    hostname = text("hostname", 63)
    device_label = text("device_label", 48) or text("label", 48)
    board = text("board", 96)
    firmware = text("firmware", 64)
    interface = text("interface", 24)
    address_source = text("address_source", 24)
    if not all((device_id, hostname, board, firmware, interface, address_source)):
        return None
    return DiscoveredDevice(
        device_id=device_id,
        hostname=hostname,
        ipv4=reported_ip,
        source_ip=source_ip,
        interface=interface,
        address_source=address_source,
        firmware=firmware,
        board=board,
        http_port=http_port,
        device_label=device_label,
    )


def discover_devices(
    *,
    timeout: float = 2.0,
    port: int = DISCOVERY_PORT,
    addresses: Iterable[str] | None = None,
    subnets: Iterable[str] | None = None,
    socket_factory=socket.socket,
) -> list[DiscoveredDevice]:
    if not 0.1 <= timeout <= 30:
        raise ToolkitError("discovery timeout must be between 0.1 and 30 seconds")
    if not 1 <= port <= 65535:
        raise ToolkitError("discovery UDP port must be between 1 and 65535")
    targets = discovery_targets(addresses, subnets)
    nonce = secrets.token_hex(12)
    request = json.dumps(
        {
            "service": "exoanchor.discover",
            "version": DISCOVERY_VERSION,
            "nonce": nonce,
        },
        separators=(",", ":"),
    ).encode("utf-8")

    found: dict[tuple[str, str], DiscoveredDevice] = {}
    deadline = time.monotonic() + timeout
    try:
        with socket_factory(socket.AF_INET, socket.SOCK_DGRAM) as device_socket:
            device_socket.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            device_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            for target in targets:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                device_socket.settimeout(min(0.2, remaining))
                try:
                    device_socket.sendto(request, (target, port))
                except OSError:
                    # One unavailable interface or directed broadcast must not hide
                    # responses from the remaining targets.
                    continue
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                device_socket.settimeout(min(0.2, remaining))
                try:
                    payload, source = device_socket.recvfrom(MAX_RESPONSE_BYTES + 1)
                except socket.timeout:
                    continue
                except OSError as exc:
                    raise ToolkitError(f"LAN discovery receive failed: {exc}") from exc
                parsed = parse_discovery_response(payload, source[0], nonce)
                if parsed is None:
                    continue
                found[(parsed.device_id, parsed.source_ip)] = parsed
    except ToolkitError:
        raise
    except OSError as exc:
        raise ToolkitError(f"cannot start LAN discovery: {exc}") from exc

    return sorted(
        found.values(),
        key=lambda item: (
            int(ipaddress.IPv4Address(item.source_ip)),
            item.device_id,
        ),
    )


def identify_uart_device(
    console: DiagnosticConsole,
) -> dict[str, object]:
    status_output = console.run("status").output
    status_lines = status_output.splitlines()
    status = (
        dict(STATUS_VALUE_RE.findall(status_lines[0]))
        if status_lines
        else {}
    )
    snapshot = parse_network_show(console.run("network show").output)
    device_id = snapshot.identity.get("device_id", "")
    hostname = snapshot.identity.get("hostname", "")
    if not device_id or not hostname:
        raise ToolkitError("UART0 did not return a complete device identity")
    ipv4 = snapshot.runtime.get("ip", "")
    if ipv4 == "none":
        ipv4 = ""
    board = status.get("board", "unknown")
    firmware = status.get("version", "unknown")
    web_url = f"http://{ipv4}/" if ipv4 else ""
    return {
        "device_id": device_id,
        "hostname": hostname,
        "ipv4": ipv4,
        "source_ip": ipv4,
        "interface": "uart0",
        "address_source": snapshot.runtime.get("source", "uart"),
        "firmware": firmware,
        "board": board,
        "http_port": 80,
        "web_url": web_url,
        "transport": "uart",
        "serial_port": console.port,
    }
