from __future__ import annotations

import time
from collections.abc import Callable, Iterable

from .discovery import identify_uart_device
from .errors import ToolkitError
from .ports import PortInfo, list_serial_ports, match_port_identity
from .serial_cli import DiagnosticConsole


def wait_for_uart_runtime(
    identity: PortInfo,
    *,
    expected_version: str,
    expected_board: str,
    baud: int = 115200,
    timeout: float = 30.0,
    probe_timeout: float = 3.0,
    ports_provider: Callable[[], Iterable[PortInfo]] | None = None,
    identify_device: (
        Callable[[DiagnosticConsole], dict[str, object]] | None
    ) = None,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, object]:
    """Wait for the flashed board to return and prove the new runtime booted."""
    if timeout <= 0 or probe_timeout <= 0:
        raise ToolkitError("UART runtime verification timeout must be positive")
    provide_ports = ports_provider or list_serial_ports
    identify = identify_device or identify_uart_device
    deadline = monotonic() + timeout
    last_error = "serial device has not re-enumerated"

    while monotonic() < deadline:
        matched = match_port_identity(identity, provide_ports())
        if matched is not None:
            remaining = max(0.1, deadline - monotonic())
            console = DiagnosticConsole(
                port=matched.device,
                baud=baud,
                timeout=min(probe_timeout, remaining),
            )
            try:
                device = identify(console)
            except ToolkitError as exc:
                last_error = str(exc)
            else:
                firmware = str(device.get("firmware", ""))
                board = str(device.get("board", ""))
                if firmware != expected_version:
                    raise ToolkitError(
                        "flash completed but UART0 reported firmware "
                        f"{firmware or 'unknown'} instead of {expected_version}"
                    )
                if board != expected_board:
                    raise ToolkitError(
                        "flash completed but UART0 reported board "
                        f"{board or 'unknown'} instead of {expected_board}"
                    )
                return {
                    "verified": True,
                    "port": matched.device,
                    "device_id": str(device.get("device_id", "")),
                    "firmware": firmware,
                    "board": board,
                }
        remaining = deadline - monotonic()
        if remaining > 0:
            sleep(min(0.25, remaining))

    raise ToolkitError(
        "flash completed but the same device did not return a verified UART0 "
        f"runtime within {timeout:g}s: {last_error}"
    )
