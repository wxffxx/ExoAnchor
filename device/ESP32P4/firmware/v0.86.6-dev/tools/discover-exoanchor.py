#!/usr/bin/env python3

import argparse
import json
import secrets
import socket
import time


DISCOVERY_PORT = 39393


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Discover ExoAnchor devices on the local IPv4 network."
    )
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--port", type=int, default=DISCOVERY_PORT)
    parser.add_argument(
        "--address",
        default="255.255.255.255",
        help="IPv4 broadcast or unicast destination (default: %(default)s)",
    )
    args = parser.parse_args()

    nonce = secrets.token_hex(12)
    request = json.dumps(
        {
            "service": "exoanchor.discover",
            "version": 1,
            "nonce": nonce,
        },
        separators=(",", ":"),
    ).encode()
    found: set[tuple[str, str]] = set()
    deadline = time.monotonic() + max(0.1, args.timeout)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.settimeout(0.2)
        sock.sendto(request, (args.address, args.port))
        while time.monotonic() < deadline:
            try:
                payload, source = sock.recvfrom(2048)
            except socket.timeout:
                continue
            try:
                response = json.loads(payload)
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if (
                response.get("service") != "exoanchor"
                or response.get("version") != 1
                or response.get("nonce") != nonce
            ):
                continue
            key = (str(response.get("device_id", "")), source[0])
            if key in found:
                continue
            found.add(key)
            response["source_ip"] = source[0]
            print(json.dumps(response, ensure_ascii=False, sort_keys=True))
    return 0 if found else 1


if __name__ == "__main__":
    raise SystemExit(main())
