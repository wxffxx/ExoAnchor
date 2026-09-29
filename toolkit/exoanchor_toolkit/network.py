from __future__ import annotations

import ipaddress
import re
from dataclasses import dataclass

from .errors import ToolkitError
from .serial_cli import DiagnosticConsole


HOSTNAME_RE = re.compile(
    r"^(?=.{1,63}$)[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?$"
)
KEY_VALUE_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")


@dataclass(frozen=True)
class NetworkSnapshot:
    runtime: dict[str, str]
    identity: dict[str, str]
    active: dict[str, str]
    staged: dict[str, str] | None
    raw: str

    def to_dict(self) -> dict[str, object]:
        return {
            "runtime": self.runtime,
            "identity": self.identity,
            "active": self.active,
            "staged": self.staged,
            "raw": self.raw,
        }


def validate_hostname(hostname: str) -> str:
    value = hostname.strip()
    if not HOSTNAME_RE.fullmatch(value):
        raise ToolkitError(
            "hostname must be 1-63 letters, digits, or hyphens and cannot "
            "start or end with a hyphen"
        )
    return value


def validate_ipv4(value: str, field: str, *, optional: bool = False) -> str:
    value = value.strip()
    if optional and value.lower() in {"", "-", "none"}:
        return "-"
    try:
        return str(ipaddress.IPv4Address(value))
    except ipaddress.AddressValueError as exc:
        raise ToolkitError(f"{field} is not a valid IPv4 address: {value}") from exc


def validate_netmask(value: str) -> str:
    value = validate_ipv4(value, "netmask")
    try:
        ipaddress.IPv4Network(f"0.0.0.0/{value}")
    except ValueError as exc:
        raise ToolkitError(f"netmask is not contiguous: {value}") from exc
    return value


def parse_network_show(output: str) -> NetworkSnapshot:
    sections: dict[str, dict[str, str]] = {}
    for line in output.splitlines():
        line = line.strip()
        if not line:
            continue
        label = line.split(maxsplit=1)[0]
        if label in {"network", "identity", "active", "staged"}:
            sections[label] = dict(KEY_VALUE_RE.findall(line))
            if label == "network":
                error_match = re.search(r"(?:^|\s)error=(.*)$", line)
                if error_match:
                    sections[label]["error"] = error_match.group(1).strip()
    required = {"network", "identity", "active"}
    missing = required - sections.keys()
    if missing:
        raise ToolkitError(
            "unexpected `network show` response; missing " + ", ".join(sorted(missing))
        )
    return NetworkSnapshot(
        runtime=sections["network"],
        identity=sections["identity"],
        active=sections["active"],
        staged=sections.get("staged"),
        raw=output,
    )


class NetworkProvisioner:
    def __init__(self, console: DiagnosticConsole) -> None:
        self.console = console

    @staticmethod
    def _ensure_ok(output: str, operation: str) -> str:
        if "result=ESP_OK" not in output:
            raise ToolkitError(f"{operation} failed: {output}")
        return output

    def show(self) -> NetworkSnapshot:
        return parse_network_show(self.console.run("network show").output)

    def stage_dhcp(self, hostname: str | None = None) -> str:
        command = "network dhcp"
        if hostname:
            command += " " + validate_hostname(hostname)
        return self._ensure_ok(
            self.console.run(command).output, "stage DHCP configuration"
        )

    def stage_static(
        self,
        address: str,
        netmask: str,
        gateway: str = "-",
        dns_primary: str = "-",
        dns_secondary: str = "-",
    ) -> str:
        address = validate_ipv4(address, "address")
        netmask = validate_netmask(netmask)
        gateway = validate_ipv4(gateway, "gateway", optional=True)
        dns_primary = validate_ipv4(dns_primary, "primary DNS", optional=True)
        dns_secondary = validate_ipv4(dns_secondary, "secondary DNS", optional=True)
        command = (
            f"network static {address} {netmask} {gateway} "
            f"{dns_primary} {dns_secondary}"
        )
        return self._ensure_ok(
            self.console.run(command).output, "stage static configuration"
        )

    def stage_hostname(self, hostname: str) -> str:
        command = "network hostname " + validate_hostname(hostname)
        return self._ensure_ok(
            self.console.run(command).output, "stage hostname"
        )

    def apply(self) -> str:
        return self._ensure_ok(
            self.console.run("network apply").output, "apply network configuration"
        )

    def commit(self) -> str:
        return self._ensure_ok(
            self.console.run("network commit").output, "commit network configuration"
        )

    def rollback(self) -> str:
        return self._ensure_ok(
            self.console.run("network rollback").output, "rollback network configuration"
        )

    def reset(self) -> str:
        return self._ensure_ok(
            self.console.run("network reset CONFIRM").output,
            "reset network configuration",
        )
