from __future__ import annotations

import re
import time
from dataclasses import dataclass

from .errors import ToolkitError


PROMPT = b"exo> "
ANSI_ESCAPE_RE = re.compile(rb"\x1b(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")
WRITE_PACE_SECONDS = 0.002


def _serial_module():
    try:
        import serial
    except ImportError as exc:
        raise ToolkitError(
            "pyserial is required; install the toolkit with "
            "`python -m pip install ./toolkit`"
        ) from exc
    return serial


@dataclass(frozen=True)
class CommandResult:
    command: str
    output: str
    elapsed_seconds: float


class DiagnosticConsole:
    def __init__(
        self,
        port: str,
        baud: int = 115200,
        timeout: float = 5.0,
    ) -> None:
        if baud <= 0:
            raise ToolkitError("serial baud must be positive")
        if timeout <= 0:
            raise ToolkitError("serial timeout must be positive")
        self.port = port
        self.baud = baud
        self.timeout = timeout

    def _read_until_prompt(self, serial_port, deadline: float) -> bytes:
        data = bytearray()
        while time.monotonic() < deadline:
            chunk = serial_port.read(512)
            if chunk:
                data.extend(chunk)
                if data.rstrip().endswith(PROMPT.rstrip()):
                    return bytes(data)
        raise ToolkitError(
            f"timed out waiting for ExoAnchor UART0 prompt on {self.port}"
        )

    def _synchronize(self, serial_port) -> None:
        """Finish stale device-side input before starting a transaction.

        Resetting pyserial's input buffer only discards bytes already returned by
        the device.  It does not clear a partial command buffered by the firmware.
        The diagnostic CLI's ``sync`` command is deliberately recoverable even
        when it is appended to stale input, so wait for its acknowledgement and
        the following prompt before writing the caller's command.
        """
        serial_port.reset_input_buffer()
        self._write_line(serial_port, b"sync")

        # The response timeout starts after the deliberately paced write.  Slow
        # host schedulers must not consume the device's acknowledgement window.
        deadline = time.monotonic() + self.timeout
        data = bytearray()
        acknowledged = False
        while time.monotonic() < deadline:
            chunk = serial_port.read(512)
            if not chunk:
                continue
            data.extend(chunk)
            if b"sync ok" in data:
                acknowledged = True
            if acknowledged and data.rstrip().endswith(PROMPT.rstrip()):
                return
        raise ToolkitError(
            f"cannot synchronize ExoAnchor UART0 console on {self.port}"
        )

    @staticmethod
    def _clean_output(command: str, raw: bytes) -> str:
        raw = ANSI_ESCAPE_RE.sub(b"", raw).replace(b"\x00", b"")
        text = raw.decode("utf-8", errors="replace").replace("\r\n", "\n")
        text = text.replace("\r", "\n")
        lines = [line.rstrip() for line in text.splitlines()]
        while lines and not lines[0].strip():
            lines.pop(0)
        while lines and lines[-1].strip() in {"exo>", "exo> exo>"}:
            lines.pop()
        if lines and lines[0].strip() == command:
            lines.pop(0)
        return "\n".join(line for line in lines if line.strip() != "exo>").strip()

    def run(self, command: str) -> CommandResult:
        if not command or any(ch in command for ch in "\r\n\x00"):
            raise ToolkitError("diagnostic command must be one non-empty line")

        serial = _serial_module()
        started = time.monotonic()
        try:
            with serial.Serial(
                self.port,
                self.baud,
                timeout=min(0.2, self.timeout),
                write_timeout=self.timeout,
            ) as device:
                self._synchronize(device)

                self._write_line(device, command.encode("utf-8"))
                raw = self._read_until_prompt(
                    device, time.monotonic() + self.timeout
                )
        except ToolkitError:
            raise
        except (serial.SerialException, OSError) as exc:
            raise ToolkitError(f"cannot use serial port {self.port}: {exc}") from exc

        output = self._clean_output(command, raw)
        if not output:
            raise ToolkitError(f"device returned no output for `{command}`")
        return CommandResult(
            command=command,
            output=output,
            elapsed_seconds=time.monotonic() - started,
        )
    @staticmethod
    def _write_line(serial_port, payload: bytes) -> None:
        """Pace UART0 writes so the small firmware console RX ring cannot overrun."""
        for byte in payload + b"\r\n":
            written = serial_port.write(bytes((byte,)))
            if written != 1:
                raise ToolkitError("short write to ExoAnchor UART0 console")
            serial_port.flush()
            time.sleep(WRITE_PACE_SECONDS)
