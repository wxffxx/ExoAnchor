#!/usr/bin/env python3
"""Backup, program and verify PrototypeV2.4 MS2109 EEPROM over UART0."""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import re
import sys
import time
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports


EEPROM_SIZE = 2048
PROMPT = b"exo> "
UART_LONG_WRITE_THRESHOLD = 512
UART_LONG_WRITE_CHUNK = 16
UART_LONG_WRITE_GAP_SECONDS = 0.02
DUMP_PATTERN = re.compile(
    rb"ms-eeprom dump bytes=2048 encoding=base64 data=([A-Za-z0-9+/=]+) result=ESP_OK"
)
PROGRAM_PATTERN = re.compile(
    rb"ms-eeprom program bytes=2048 crc32=([0-9a-fA-F]{8}) verified=1 result=ESP_OK"
)


class ToolError(RuntimeError):
    pass


def choose_port(requested: str | None) -> str:
    if requested:
        return requested
    candidates = [item.device for item in list_ports.comports()]
    if len(candidates) != 1:
        listing = "\n".join(f"  {item}" for item in candidates) or "  (none)"
        raise ToolError(
            "automatic serial selection requires exactly one port; pass --port.\n"
            + listing
        )
    return candidates[0]


class Console:
    def __init__(self, port: str, baud: int) -> None:
        # Configure modem-control outputs before opening the CH343. Opening it
        # with pyserial defaults asserted can leave the PrototypeV2.4 download
        # circuit holding the P4 in reset/download state.
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = baud
        self.serial.timeout = 0.1
        self.serial.write_timeout = 5
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        # Releasing the lines may restart the P4; wait until the UART CLI has
        # been created before discarding boot logs and issuing the first command.
        time.sleep(2.0)
        self.serial.reset_input_buffer()

    def close(self) -> None:
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.close()

    def command(self, text: str, timeout: float = 10.0) -> bytes:
        # The firmware console accepts either CR or LF as an end-of-line. A
        # CRLF pair is therefore two input lines and leaves a second prompt in
        # the RX queue, which can make the following command appear complete
        # before it has run.
        payload = text.encode("ascii") + b"\n"
        # Boot logs and a prompt emitted just before this transaction are not
        # its response. Drop them before writing so prompt detection cannot
        # complete on stale data.
        self.serial.reset_input_buffer()
        if len(payload) <= UART_LONG_WRITE_THRESHOLD:
            self.serial.write(payload)
            self.serial.flush()
        else:
            # A complete 2 KiB EEPROM image expands to about 2.7 KiB of
            # Base64. Pace long commands so the device UART RX ring can drain
            # into the larger logical CLI line buffer without losing its
            # leading bytes.
            for offset in range(0, len(payload), UART_LONG_WRITE_CHUNK):
                self.serial.write(payload[offset : offset + UART_LONG_WRITE_CHUNK])
                self.serial.flush()
                time.sleep(UART_LONG_WRITE_GAP_SECONDS)
        deadline = time.monotonic() + timeout
        output = bytearray()
        while time.monotonic() < deadline:
            chunk = self.serial.read(self.serial.in_waiting or 1)
            if chunk:
                output.extend(chunk)
                if PROMPT in output:
                    return bytes(output)
            else:
                time.sleep(0.02)
        raise ToolError(f"UART command timed out after {timeout:.0f}s: {text[:40]}")

    def synchronize(self) -> None:
        # Do not use generic prompt detection here: the CLI may still be
        # booting, and its initial prompt is not proof that our sync command
        # has run. Require the explicit acknowledgement followed by a prompt.
        for _attempt in range(3):
            self.serial.reset_input_buffer()
            self.serial.write(b"\nsync\n")
            self.serial.flush()
            deadline = time.monotonic() + 3.0
            output = bytearray()
            while time.monotonic() < deadline:
                chunk = self.serial.read(self.serial.in_waiting or 1)
                if chunk:
                    output.extend(chunk)
                    ack_at = output.find(b"sync ok")
                    if ack_at >= 0 and PROMPT in output[ack_at:]:
                        time.sleep(0.05)
                        self.serial.reset_input_buffer()
                        return
                else:
                    time.sleep(0.02)
            time.sleep(0.5)
        raise ToolError("unable to synchronize with the ExoAnchor UART CLI")


def image_summary(data: bytes) -> str:
    return (
        f"bytes={len(data)} crc32={binascii.crc32(data) & 0xffffffff:08x} "
        f"sha256={hashlib.sha256(data).hexdigest()}"
    )


def dump_eeprom(console: Console) -> bytes:
    output = console.command("ms-eeprom dump-b64", timeout=15.0)
    match = DUMP_PATTERN.search(output)
    if not match:
        raise ToolError("device did not return a complete verified EEPROM dump")
    try:
        data = base64.b64decode(match.group(1), validate=True)
    except (ValueError, binascii.Error) as exc:
        raise ToolError(f"invalid base64 EEPROM dump: {exc}") from exc
    if len(data) != EEPROM_SIZE:
        raise ToolError(f"EEPROM dump size is {len(data)}, expected {EEPROM_SIZE}")
    return data


def command_status(console: Console, _args: argparse.Namespace) -> None:
    output = console.command("ms-eeprom status", timeout=10.0)
    sys.stdout.buffer.write(output)
    if b"address_mask=0xff" not in output or b"probe=ESP_OK" not in output:
        raise ToolError("AT24C16 did not acknowledge all addresses 0x50-0x57")


def command_power(console: Console, args: argparse.Namespace) -> None:
    command = f"ms-switch {args.state}"
    if args.state == "cycle":
        command += f" {args.off_ms}"
    output = console.command(command, timeout=10.0)
    sys.stdout.buffer.write(output)
    success_token = b"last=ESP_OK" if args.state == "status" else b"result=ESP_OK"
    if success_token not in output:
        raise ToolError("MS2109 load-switch command failed")


def command_dump(console: Console, args: argparse.Namespace) -> None:
    data = dump_eeprom(console)
    output = args.output.expanduser().resolve()
    if output.exists() and not args.force:
        raise ToolError(f"refusing to overwrite {output}; use --force")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    print(f"saved {output}\n{image_summary(data)}")


def command_program(console: Console, args: argparse.Namespace) -> None:
    if not args.yes:
        raise ToolError("programming is destructive; pass --yes after checking the image")
    image_path = args.image.expanduser().resolve()
    image = image_path.read_bytes()
    if len(image) != EEPROM_SIZE:
        raise ToolError(
            f"{image_path} is {len(image)} bytes; AT24C16 image must be exactly {EEPROM_SIZE}"
        )

    existing = dump_eeprom(console)
    backup_path = (
        args.backup.expanduser().resolve()
        if args.backup
        else image_path.with_name(
            f"{image_path.stem}.backup-{datetime.now().strftime('%Y%m%d-%H%M%S')}.bin"
        )
    )
    if backup_path.exists():
        raise ToolError(f"refusing to overwrite backup {backup_path}")
    backup_path.write_bytes(existing)
    print(f"backup saved: {backup_path}\nbackup {image_summary(existing)}")
    print(f"input  {image_summary(image)}")

    payload = base64.b64encode(image).decode("ascii")
    output = console.command(
        f"ms-eeprom program-b64 {payload} CONFIRM", timeout=args.timeout
    )
    match = PROGRAM_PATTERN.search(output)
    if not match:
        sys.stdout.buffer.write(output)
        raise ToolError(
            "device did not report a successful byte-for-byte verification; "
            f"restore {backup_path} before using this board"
        )
    device_crc = int(match.group(1), 16)
    local_crc = binascii.crc32(image) & 0xFFFFFFFF
    if device_crc != local_crc:
        raise ToolError(
            f"CRC mismatch after programming: device={device_crc:08x} local={local_crc:08x}"
        )

    readback = dump_eeprom(console)
    if readback != image:
        raise ToolError(
            "second independent readback differs from input; "
            f"restore {backup_path}"
        )
    print("program and independent readback verification passed")
    print(image_summary(readback))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="PrototypeV2.4 MS2109 switch and AT24C16 test tool"
    )
    parser.add_argument("--port", help="diagnostic UART0 serial device")
    parser.add_argument("--baud", type=int, default=115200)
    commands = parser.add_subparsers(dest="command", required=True)

    status = commands.add_parser("status", help="probe the physical AT24C16")
    status.set_defaults(handler=command_status)

    power = commands.add_parser("power", help="control the MS2109 load switch")
    power.add_argument("state", choices=("status", "on", "off", "cycle"))
    power.add_argument("--off-ms", type=int, default=250)
    power.set_defaults(handler=command_power)

    dump = commands.add_parser("dump", help="save a full 2 KiB EEPROM backup")
    dump.add_argument("output", type=Path)
    dump.add_argument("--force", action="store_true")
    dump.set_defaults(handler=command_dump)

    program = commands.add_parser("program", help="backup, program and verify EEPROM")
    program.add_argument("image", type=Path)
    program.add_argument("--backup", type=Path)
    program.add_argument("--timeout", type=float, default=90.0)
    program.add_argument("--yes", action="store_true")
    program.set_defaults(handler=command_program)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        port = choose_port(args.port)
        console = Console(port, args.baud)
        try:
            console.synchronize()
            args.handler(console, args)
        finally:
            console.close()
        return 0
    except (OSError, serial.SerialException, ToolError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
