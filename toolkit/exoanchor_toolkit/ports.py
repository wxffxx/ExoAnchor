from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Iterable

from .errors import ToolkitError


EXOANCHOR_USB_IDS = {
    (0x1A86, 0x55D3),  # QinHeng USB Single Serial used by Prototype V2.3.
    (0x303A, 0x1001),  # Espressif USB Serial/JTAG fallback.
}


@dataclass(frozen=True)
class PortInfo:
    device: str
    description: str
    hwid: str
    vid: int | None
    pid: int | None
    serial_number: str | None
    manufacturer: str | None
    likely_exoanchor: bool

    def to_dict(self) -> dict[str, object]:
        return asdict(self)


def _serial_list_ports():
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise ToolkitError(
            "pyserial is required; install the toolkit with "
            "`python -m pip install ./toolkit`"
        ) from exc
    return list_ports


def list_serial_ports() -> list[PortInfo]:
    ports = []
    for port in _serial_list_ports().comports():
        usb_id = (port.vid, port.pid)
        description = port.description or ""
        likely = usb_id in EXOANCHOR_USB_IDS or "ExoAnchor" in description
        ports.append(
            PortInfo(
                device=port.device,
                description=description,
                hwid=port.hwid or "",
                vid=port.vid,
                pid=port.pid,
                serial_number=port.serial_number,
                manufacturer=port.manufacturer,
                likely_exoanchor=likely,
            )
        )
    return sorted(ports, key=lambda item: (not item.likely_exoanchor, item.device))


def resolve_port(requested: str | None, ports: Iterable[PortInfo] | None = None) -> str:
    if requested and requested.lower() != "auto":
        return requested

    available = list(ports if ports is not None else list_serial_ports())
    likely = [item for item in available if item.likely_exoanchor]
    if len(likely) == 1:
        return likely[0].device
    if not likely:
        raise ToolkitError(
            "no ExoAnchor-like serial port found; run `ports` and pass `--port`"
        )
    names = ", ".join(item.device for item in likely)
    raise ToolkitError(
        f"multiple ExoAnchor-like serial ports found ({names}); pass `--port`"
    )


def capture_port_identity(
    device: str,
    ports: Iterable[PortInfo] | None = None,
) -> PortInfo:
    """Capture the USB identity of the exact selected serial port."""
    available = list(ports if ports is not None else list_serial_ports())
    matches = [item for item in available if item.device == device]
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        raise ToolkitError(f"serial port {device} has multiple identities")
    raise ToolkitError(f"serial port disappeared before operation: {device}")


def match_port_identity(
    identity: PortInfo,
    ports: Iterable[PortInfo] | None = None,
) -> PortInfo | None:
    """Find the same USB device after re-enumeration without guessing.

    A USB serial number plus VID/PID is stable across device-path changes. If
    no serial number is available, only the original path is accepted. Never
    fall back to another merely likely ExoAnchor port because production and
    recovery workflows commonly have more than one board attached.
    """
    available = list(ports if ports is not None else list_serial_ports())
    if identity.serial_number:
        matches = [
            item
            for item in available
            if item.vid == identity.vid
            and item.pid == identity.pid
            and item.serial_number == identity.serial_number
        ]
    else:
        matches = [item for item in available if item.device == identity.device]
    if len(matches) > 1:
        raise ToolkitError(
            "multiple serial ports reported the same USB device identity"
        )
    return matches[0] if matches else None
