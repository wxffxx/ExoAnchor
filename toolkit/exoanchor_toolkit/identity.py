from __future__ import annotations

import importlib.metadata
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - exercised on Python 3.10
    import tomli as tomllib

from .errors import ToolkitError
from .ports import PortInfo


REGISTRY_SCHEMA_VERSION = 1
CH343_USB_ID = (0x1A86, 0x55D3)
MAC_RE = re.compile(r"^[0-9a-f]{2}(?::[0-9a-f]{2}){5}$", re.IGNORECASE)
REVISION_RE = re.compile(r"^[0-9]+(?:\.[0-9]+)?$")
OUTPUT_MAC_RE = re.compile(
    r"(?im)^\s*(?:base\s+)?mac\s*:\s*"
    r"([0-9a-f]{2}(?::[0-9a-f]{2}){5})\s*$"
)
OUTPUT_REVISION_RE = re.compile(
    r"(?i)\brevision(?:\s*[:=])?\s*v?([0-9]+(?:\.[0-9]+)?)\b"
)


@dataclass(frozen=True)
class BoardIdentityCredential:
    record_id: str
    physical_model: str
    ch343_serial: str
    efuse_base_mac: str
    silicon_revision: str
    allowed_profile: str


@dataclass(frozen=True)
class ChipIdentity:
    efuse_base_mac: str
    silicon_revision: str


def require_explicit_uart_flash_port(port: str) -> str:
    """Require one literal device path for a destructive UART operation."""
    if not isinstance(port, str):
        raise ToolkitError("UART flashing requires an explicit serial port")
    normalized = port.strip()
    if not normalized or normalized.casefold() == "auto":
        raise ToolkitError(
            "UART flashing forbids 'auto'; pass one exact serial port"
        )
    if any(character in normalized for character in "*?[]"):
        raise ToolkitError(
            "UART flashing requires one exact serial port without wildcards"
        )
    return normalized


def _required_text(record: dict[str, object], key: str, record_id: str) -> str:
    value = record.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ToolkitError(
            f"board identity record {record_id} is missing {key}"
        )
    return value.strip()


def load_board_identity_credential(
    registry_path: str | Path | None,
    record_id: str | None,
) -> BoardIdentityCredential:
    if registry_path is None or not str(registry_path).strip():
        raise ToolkitError(
            "UART flashing requires --identity-registry with the authoritative "
            "board registry"
        )
    if not isinstance(record_id, str) or not record_id.strip():
        raise ToolkitError(
            "UART flashing requires --board-record for the operator-confirmed "
            "physical model"
        )
    record_id = record_id.strip()

    path = Path(registry_path).expanduser().resolve()
    try:
        with path.open("rb") as handle:
            document = tomllib.load(handle)
    except FileNotFoundError as exc:
        raise ToolkitError(f"board identity registry does not exist: {path}") from exc
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise ToolkitError(f"cannot read board identity registry {path}: {exc}") from exc

    if document.get("schema_version") != REGISTRY_SCHEMA_VERSION:
        raise ToolkitError(
            "unsupported board identity registry schema: "
            f"{document.get('schema_version')}"
        )
    records = document.get("boards")
    if not isinstance(records, list):
        raise ToolkitError("board identity registry has no boards list")
    matches = [
        item
        for item in records
        if isinstance(item, dict) and item.get("record_id") == record_id
    ]
    if len(matches) != 1:
        if matches:
            raise ToolkitError(
                f"board identity record {record_id} is not unique"
            )
        raise ToolkitError(f"board identity record not found: {record_id}")
    record = matches[0]

    physical_model = _required_text(record, "physical_model", record_id)
    model_status = _required_text(record, "physical_model_status", record_id)
    if not model_status.casefold().startswith("operator-confirmed"):
        raise ToolkitError(
            f"physical model is not operator-confirmed for {record_id}"
        )
    if _required_text(record, "usb_bridge", record_id).casefold() != "ch343":
        raise ToolkitError(
            f"board identity record {record_id} is not a CH343 identity"
        )
    if _required_text(record, "chip", record_id).casefold() != "esp32-p4":
        raise ToolkitError(
            f"board identity record {record_id} is not an ESP32-P4 identity"
        )
    ch343_serial = _required_text(record, "usb_bridge_serial", record_id)
    efuse_base_mac = _required_text(record, "efuse_base_mac", record_id).lower()
    silicon_revision = _required_text(record, "silicon_revision", record_id)
    allowed_profile = _required_text(record, "allowed_profile", record_id)
    flash_authorization = _required_text(
        record, "flash_authorization", record_id
    ).casefold()
    if re.search(
        r"\b(?:blocked|denied|forbidden|revoked|not\s+authorized|not\s+allowed)\b",
        flash_authorization,
    ) or not re.search(r"\b(?:allowed|authorized)\b", flash_authorization):
        raise ToolkitError(
            f"board identity record {record_id} does not explicitly authorize flashing"
        )
    if not MAC_RE.fullmatch(efuse_base_mac):
        raise ToolkitError(
            f"board identity record {record_id} has an invalid eFuse base MAC"
        )
    if not REVISION_RE.fullmatch(silicon_revision):
        raise ToolkitError(
            f"board identity record {record_id} has an invalid silicon revision"
        )
    return BoardIdentityCredential(
        record_id=record_id,
        physical_model=physical_model,
        ch343_serial=ch343_serial,
        efuse_base_mac=efuse_base_mac,
        silicon_revision=silicon_revision,
        allowed_profile=allowed_profile,
    )


def verify_package_profile(
    package_board: str,
    credential: BoardIdentityCredential,
) -> None:
    if package_board != credential.allowed_profile:
        raise ToolkitError(
            f"firmware profile {package_board} does not match board identity "
            f"profile {credential.allowed_profile}"
        )


def verify_ch343_identity(
    port: PortInfo,
    credential: BoardIdentityCredential,
) -> None:
    if (port.vid, port.pid) != CH343_USB_ID:
        raise ToolkitError(
            f"selected port {port.device} is not the required CH343 device"
        )
    if not port.serial_number:
        raise ToolkitError(
            f"selected CH343 port {port.device} has no verifiable USB serial"
        )
    if port.serial_number != credential.ch343_serial:
        raise ToolkitError(
            f"CH343 serial {port.serial_number} does not match board identity "
            f"{credential.ch343_serial}"
        )


def parse_chip_identity(output: str) -> ChipIdentity:
    mac = OUTPUT_MAC_RE.search(output)
    revision = OUTPUT_REVISION_RE.search(output)
    if mac is None or revision is None:
        raise ToolkitError(
            "esptool chip-id did not report a verifiable eFuse MAC and "
            "silicon revision"
        )
    return ChipIdentity(
        efuse_base_mac=mac.group(1).lower(),
        silicon_revision=revision.group(1),
    )


def build_chip_identity_command(
    port: str,
    baud: int = 115200,
    *,
    esptool_major: int | None = None,
) -> list[str]:
    port = require_explicit_uart_flash_port(port)
    if baud <= 0:
        raise ToolkitError("identity probe baud must be positive")
    if esptool_major is None:
        try:
            esptool_major = int(
                importlib.metadata.version("esptool").split(".", 1)[0]
            )
        except (importlib.metadata.PackageNotFoundError, ValueError):
            esptool_major = 4
    return [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32p4",
        "--port",
        port,
        "--baud",
        str(baud),
        "chip-id" if esptool_major >= 5 else "chip_id",
    ]


def probe_chip_identity(
    port: str,
    baud: int = 115200,
    *,
    timeout: float = 20.0,
) -> ChipIdentity:
    """Run the read-only identity command after registry and USB checks pass."""
    command = build_chip_identity_command(port, baud)
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise ToolkitError("esptool chip-id timed out") from exc
    except OSError as exc:
        raise ToolkitError(f"cannot start esptool chip-id: {exc}") from exc
    output = completed.stdout or ""
    if completed.returncode != 0:
        raise ToolkitError(
            f"esptool chip-id failed with exit code {completed.returncode}"
        )
    return parse_chip_identity(output)


def verify_chip_identity(
    observed: ChipIdentity,
    credential: BoardIdentityCredential,
) -> None:
    if observed.efuse_base_mac.lower() != credential.efuse_base_mac:
        raise ToolkitError(
            f"eFuse base MAC {observed.efuse_base_mac} does not match board "
            f"identity {credential.efuse_base_mac}"
        )
    if observed.silicon_revision != credential.silicon_revision:
        raise ToolkitError(
            f"silicon revision {observed.silicon_revision} does not match "
            f"board identity {credential.silicon_revision}"
        )
