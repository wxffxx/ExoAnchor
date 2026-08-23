from __future__ import annotations

import math
import time
from dataclasses import dataclass
from typing import Callable, Protocol

from .discovery import (
    DiscoveredDevice,
    discover_devices,
    discovery_targets,
)
from .errors import ToolkitError
from .http_device import DeviceHttpClient, HttpNetworkProvisioner
from .network import (
    NetworkSnapshot,
    validate_hostname,
    validate_ipv4,
    validate_netmask,
)


VERIFY_TIMEOUT_SECONDS = 20.0
MAX_VERIFY_TIMEOUT_SECONDS = 110.0
POLL_INTERVAL_SECONDS = 0.25
DISCOVERY_TIMEOUT_SECONDS = 0.35
HTTP_PROBE_TIMEOUT_SECONDS = 1.5


class NetworkProvisioningChannel(Protocol):
    def show(self) -> NetworkSnapshot: ...

    def stage_dhcp(self, hostname: str | None = None) -> str: ...

    def stage_static(
        self,
        address: str,
        netmask: str,
        gateway: str = "-",
        dns_primary: str = "-",
        dns_secondary: str = "-",
    ) -> str: ...

    def stage_hostname(self, hostname: str) -> str: ...

    def apply(self) -> str: ...

    def commit(self) -> str: ...


@dataclass(frozen=True)
class NetworkConfiguration:
    mode: str
    hostname: str = ""
    address: str = ""
    netmask: str = ""
    gateway: str = "-"
    dns_primary: str = "-"
    dns_secondary: str = "-"


@dataclass(frozen=True)
class ConfiguredNetwork:
    transport: str
    target: str
    snapshot: NetworkSnapshot
    output: tuple[str, ...]
    client: DeviceHttpClient | None = None

    def to_dict(self) -> dict[str, object]:
        return {
            "transport": self.transport,
            "target": self.target,
            "snapshot": self.snapshot.to_dict(),
            "output": list(self.output),
        }


Discoverer = Callable[..., list[DiscoveredDevice]]


def _stage_configuration(
    provisioner: NetworkProvisioningChannel,
    configuration: NetworkConfiguration,
) -> tuple[list[str], str | None]:
    mode = configuration.mode.strip().lower()
    hostname = (
        validate_hostname(configuration.hostname)
        if configuration.hostname.strip()
        else ""
    )
    if mode == "dhcp":
        return [provisioner.stage_dhcp(hostname or None)], None
    if mode != "static":
        raise ToolkitError("network mode must be dhcp or static")
    if not configuration.address.strip():
        raise ToolkitError("address is required for static network mode")
    if not configuration.netmask.strip():
        raise ToolkitError("netmask is required for static network mode")

    address = validate_ipv4(configuration.address, "address")
    netmask = validate_netmask(configuration.netmask)
    gateway = validate_ipv4(
        configuration.gateway or "-", "gateway", optional=True
    )
    dns_primary = validate_ipv4(
        configuration.dns_primary or "-", "primary DNS", optional=True
    )
    dns_secondary = validate_ipv4(
        configuration.dns_secondary or "-", "secondary DNS", optional=True
    )
    output: list[str] = []
    if hostname:
        output.append(provisioner.stage_hostname(hostname))
    output.append(
        provisioner.stage_static(
            address,
            netmask,
            gateway,
            dns_primary,
            dns_secondary,
        )
    )
    return output, address


def _snapshot_is_ready(
    snapshot: NetworkSnapshot,
    *,
    expected_address: str | None,
    expected_device_id: str,
) -> bool:
    if (
        expected_device_id
        and snapshot.identity.get("device_id") != expected_device_id
    ):
        return False
    connected = snapshot.runtime.get("connected", "").lower()
    if connected not in {"1", "true", "yes"}:
        return False
    pending_confirmation = snapshot.runtime.get(
        "pending_confirmation", ""
    ).lower()
    config_state = (
        snapshot.runtime.get("config_state")
        or snapshot.runtime.get("state")
        or ""
    ).lower()
    if pending_confirmation:
        if pending_confirmation not in {"1", "true", "yes"}:
            return False
        # A static address is exposed as pending_confirmation while its
        # asynchronous address-conflict check is still running.  Committing
        # during that window returns ESP_ERR_INVALID_STATE, so only the
        # manager's terminal pending state is safe to confirm.
        if config_state and config_state != "pending":
            return False
    elif config_state != "pending":
        return False
    address = snapshot.runtime.get("ipv4") or snapshot.runtime.get("ip") or ""
    if not address or address == "none":
        return False
    return expected_address is None or address == expected_address


def _http_targets(
    client: DeviceHttpClient,
    *,
    expected_address: str | None,
    expected_device_id: str,
    discovery_subnets: tuple[str, ...],
    discoverer: Discoverer,
) -> tuple[list[str], str]:
    suffix = "" if client.target.port == 80 else f":{client.target.port}"
    if expected_address:
        return [expected_address + suffix], ""

    targets = [client.target.address + suffix]
    last_error = ""
    try:
        devices = discoverer(
            timeout=DISCOVERY_TIMEOUT_SECONDS,
            subnets=discovery_subnets or None,
        )
    except ToolkitError as exc:
        last_error = str(exc)
        devices = []
    for device in devices:
        if expected_device_id and device.device_id != expected_device_id:
            continue
        device_suffix = "" if device.http_port == 80 else f":{device.http_port}"
        candidate = device.source_ip + device_suffix
        if candidate not in targets:
            targets.append(candidate)
    return targets, last_error


def configure_network(
    provisioner: NetworkProvisioningChannel,
    configuration: NetworkConfiguration,
    *,
    transport: str,
    target: str,
    client: DeviceHttpClient | None = None,
    discovery_subnets: tuple[str, ...] = (),
    verify_timeout: float = VERIFY_TIMEOUT_SECONDS,
    discoverer: Discoverer = discover_devices,
    monotonic: Callable[[], float] = time.monotonic,
    sleeper: Callable[[float], None] = time.sleep,
) -> ConfiguredNetwork:
    """Apply, verify, and commit one protected network configuration change.

    A verification failure intentionally leaves the device in its pending
    state so the firmware rollback timer can restore the last-good address.
    """

    if transport not in {"network", "uart"}:
        raise ToolkitError("transport must be network or uart")
    if transport == "network" and client is None:
        raise ToolkitError("network transport requires a device HTTP client")
    if (
        not math.isfinite(verify_timeout)
        or not 0.1 <= verify_timeout <= MAX_VERIFY_TIMEOUT_SECONDS
    ):
        raise ToolkitError(
            "network verification timeout must be between 0.1 and "
            f"{MAX_VERIFY_TIMEOUT_SECONDS:g} seconds"
        )
    if discovery_subnets:
        discovery_targets(subnets=discovery_subnets)

    before = provisioner.show()
    expected_device_id = before.identity.get("device_id", "")
    if not expected_device_id:
        raise ToolkitError(
            "device did not report a device_id; network configuration "
            "was not applied"
        )
    output, expected_address = _stage_configuration(
        provisioner, configuration
    )
    output.append(provisioner.apply())

    deadline = monotonic() + verify_timeout
    verified_snapshot: NetworkSnapshot | None = None
    verified_provisioner = provisioner
    verified_client = client
    last_error = ""
    while monotonic() < deadline:
        if transport == "uart":
            candidates: list[DeviceHttpClient | None] = [None]
        else:
            assert client is not None
            targets, discovery_error = _http_targets(
                client,
                expected_address=expected_address,
                expected_device_id=expected_device_id,
                discovery_subnets=discovery_subnets,
                discoverer=discoverer,
            )
            if discovery_error:
                last_error = discovery_error
            candidates = [
                DeviceHttpClient(
                    candidate,
                    username=client.username,
                    password=client.password,
                    token=client.token,
                    timeout=HTTP_PROBE_TIMEOUT_SECONDS,
                )
                for candidate in targets
            ]

        for candidate_client in candidates:
            try:
                candidate_provisioner: NetworkProvisioningChannel = (
                    provisioner
                    if candidate_client is None
                    else HttpNetworkProvisioner(candidate_client)
                )
                snapshot = candidate_provisioner.show()
            except ToolkitError as exc:
                last_error = str(exc)
                continue
            if not _snapshot_is_ready(
                snapshot,
                expected_address=expected_address,
                expected_device_id=expected_device_id,
            ):
                continue
            verified_snapshot = snapshot
            verified_provisioner = candidate_provisioner
            verified_client = candidate_client or client
            break
        if verified_snapshot is not None:
            break
        sleeper(POLL_INTERVAL_SECONDS)

    if verified_snapshot is None:
        detail = f": {last_error}" if last_error else ""
        raise ToolkitError(
            "the new network configuration could not be verified; "
            "it was not saved and the device will automatically roll "
            f"back to its previous address{detail}"
        )

    output.append(verified_provisioner.commit())
    final_snapshot = verified_provisioner.show()
    final_target = (
        verified_client.target.base_url
        if verified_client is not None
        else target
    )
    return ConfiguredNetwork(
        transport=transport,
        target=final_target,
        snapshot=final_snapshot,
        output=tuple(output),
        client=verified_client,
    )
