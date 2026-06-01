#!/usr/bin/env python3
"""Read ESP serial logs and extract the board IP address."""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError as exc:  # pragma: no cover - depends on ESP-IDF env
    raise SystemExit("pyserial is required; source ESP-IDF export.sh first") from exc


IP_RE = re.compile(
    r"(?:Ethernet got IP:|WiFi got IP:|ETHIP:|got ip:|ipv4[^0-9:]*:|ip[^0-9:]*:)\s*"
    r"(?P<ip>(?:\d{1,3}\.){3}\d{1,3})",
    re.IGNORECASE,
)


def valid_ip(ip: str) -> bool:
    try:
        parts = [int(part) for part in ip.split(".")]
    except ValueError:
        return False
    return len(parts) == 4 and all(0 <= part <= 255 for part in parts) and ip not in {
        "0.0.0.0",
        "255.255.255.255",
    }


def extract_ip(line: str) -> str | None:
    match = IP_RE.search(line)
    if not match:
        return None
    ip = match.group("ip")
    return ip if valid_ip(ip) else None


def print_found_ip(ip: str, output: Path | None) -> None:
    if output:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(f"{ip}\n", encoding="utf-8")
    print(f"\n[serial-ip] Board IP: {ip}", flush=True)
    print(f"[serial-ip] Dashboard: http://{ip}/", flush=True)
    print(f"[serial-ip] KVM:       http://{ip}/kvm", flush=True)
    if output:
        print(f"[serial-ip] Saved to:   {output}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Serial port, e.g. /dev/cu.usbmodem...")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--timeout", type=float, default=60.0, help="Seconds to wait for the first IP")
    parser.add_argument("--output", type=Path, help="Write the detected IP to this file")
    parser.add_argument("--exit-on-ip", action="store_true", help="Exit after the first IP is detected")
    args = parser.parse_args()

    deadline = time.monotonic() + args.timeout if args.timeout > 0 else None
    found_ip: str | None = None

    print(
        f"[serial-ip] Listening on {args.port} at {args.baud} baud; "
        f"waiting up to {args.timeout:g}s for Ethernet IP...",
        flush=True,
    )

    try:
        with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
            ser.reset_input_buffer()
            buffer = bytearray()
            while True:
                chunk = ser.read(256)
                if chunk:
                    buffer.extend(chunk)
                    while b"\n" in buffer:
                        raw_line, _, buffer = buffer.partition(b"\n")
                        line = raw_line.decode("utf-8", errors="replace").rstrip("\r")
                        print(line, flush=True)
                        ip = extract_ip(line)
                        if ip and ip != found_ip:
                            found_ip = ip
                            print_found_ip(ip, args.output)
                            if args.exit_on_ip:
                                return 0

                if deadline is not None and found_ip is None and time.monotonic() > deadline:
                    print("[serial-ip] Timed out waiting for board IP.", file=sys.stderr, flush=True)
                    return 2
    except serial.SerialException as exc:
        print(f"[serial-ip] Serial error: {exc}", file=sys.stderr, flush=True)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
