#!/usr/bin/env python3
"""Local mock server for the ESP32-P4 embedded UI."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import mimetypes
import os
import posixpath
import struct
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse


ROOT = Path(__file__).resolve().parents[1] / "main" / "www"
MAGIC_WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class MockState:
    started_at = time.monotonic()
    quality = 75
    hid_frames = 0
    ws_clients = 0


def _uptime() -> int:
    return int(time.monotonic() - MockState.started_at)


def _format_uptime(seconds: int) -> str:
    days, rem = divmod(seconds, 86400)
    hours, rem = divmod(rem, 3600)
    minutes, _ = divmod(rem, 60)
    return f"{days}d {hours}h {minutes}m"


def _video_svg() -> bytes:
    now = time.strftime("%H:%M:%S")
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="800" height="640" viewBox="0 0 800 640">
<defs>
  <linearGradient id="g" x1="0" x2="1" y1="0" y2="1">
    <stop stop-color="#101820"/>
    <stop offset="0.55" stop-color="#20363a"/>
    <stop offset="1" stop-color="#331b2b"/>
  </linearGradient>
</defs>
<rect width="800" height="640" fill="url(#g)"/>
<g opacity="0.25" stroke="#ffffff">
  <path d="M0 120H800M0 240H800M0 360H800M0 480H800"/>
  <path d="M160 0V640M320 0V640M480 0V640M640 0V640"/>
</g>
<rect x="86" y="82" width="628" height="428" rx="12" fill="#0b0d10" opacity="0.72" stroke="#2dd4bf"/>
<text x="400" y="264" fill="#eef2f6" text-anchor="middle" font-family="Menlo, monospace" font-size="34">ESP32-P4 UI MOCK</text>
<text x="400" y="316" fill="#9ca3af" text-anchor="middle" font-family="Menlo, monospace" font-size="22">800x640 CSI stream placeholder</text>
<text x="400" y="364" fill="#2dd4bf" text-anchor="middle" font-family="Menlo, monospace" font-size="20">HID frames: {MockState.hid_frames}  Q{MockState.quality}  {now}</text>
</svg>"""
    return svg.encode("utf-8")


def _status_payload() -> dict:
    up = _uptime()
    frames = max(1, up * 7)
    return {
        "server_uptime": up,
        "video": {
            "enabled": True,
            "connected": True,
            "initialized": True,
            "streaming": True,
            "source": "local-ui-mock",
            "pixel_format": "SVG/mock",
            "width": 800,
            "height": 640,
            "resolution": "800x640",
            "data_lanes": 2,
            "lane_bitrate_mbps": 200,
            "quality": MockState.quality,
            "frames_captured": frames,
            "frames_encoded": frames,
            "frames_dropped": 0,
            "last_jpeg_size": len(_video_svg()),
            "last_frame_ms": 7,
            "fps": 6.8,
            "last_error": "",
        },
        "hid": {
            "mode": "native_usb_tinyusb",
            "port": "USB-OTG mock",
            "connected": True,
            "keyboard": {"available": True},
            "mouse": {"available": True},
        },
        "control_bridge": {
            "source": "local-ui-mock",
            "connected": True,
            "port": "mock websocket",
            "capabilities": {
                "keyboard": True,
                "mouse": True,
                "websocket_hid": True,
            },
        },
        "network": {
            "configured": True,
            "connected": True,
            "link_up": True,
            "interface": "localhost",
            "driver": "mock",
            "ip": "127.0.0.1",
            "netmask": "255.0.0.0",
            "gateway": "127.0.0.1",
            "mac": "02:00:00:00:00:01",
        },
        "active_connections": MockState.ws_clients,
        "authEnabled": False,
    }


def _system_info_payload() -> dict:
    up = _uptime()
    return {
        "cpu": {"usage_percent": 12, "cores": 2, "freq_mhz": 400},
        "memory": {
            "total_mb": 32,
            "used_mb": 11,
            "available_mb": 21,
            "free_bytes": 21 * 1024 * 1024,
            "minimum_free_bytes": 18 * 1024 * 1024,
            "usage_percent": 34,
        },
        "temperature": {"celsius": 38, "source": "mock"},
        "disk": {"total_gb": 0.016, "used_gb": 0.001, "free_gb": 0.015, "usage_percent": 6},
        "network": {
            "ethernet": {
                "up": True,
                "link_up": True,
                "ipv4": "127.0.0.1",
                "netmask": "255.0.0.0",
                "gateway": "127.0.0.1",
                "mac": "02:00:00:00:00:01",
            }
        },
        "uptime": {"seconds": up, "formatted": _format_uptime(up)},
        "load": {"1min": 0.12, "5min": 0.08, "15min": 0.04},
        "hostname": "si-esp32p4-ui-mock",
    }


def _logs_payload() -> dict:
    return {
        "logs": [
            {"time": "00:00:01", "level": "INFO", "message": "Local UI mock server started"},
            {"time": "00:00:02", "level": "INFO", "message": "Mock video source online"},
            {"time": "00:00:03", "level": "INFO", "message": f"HID frames received: {MockState.hid_frames}"},
        ]
    }


def _ws_send_text(sock, text: str) -> None:
    payload = text.encode("utf-8")
    size = len(payload)
    if size < 126:
        header = struct.pack("!BB", 0x81, size)
    elif size < 65536:
        header = struct.pack("!BBH", 0x81, 126, size)
    else:
        header = struct.pack("!BBQ", 0x81, 127, size)
    sock.sendall(header + payload)


def _read_ws_frame(rfile) -> bytes | None:
    header = rfile.read(2)
    if len(header) < 2:
        return None
    b1, b2 = header
    opcode = b1 & 0x0F
    if opcode == 0x08:
        return None
    masked = b2 & 0x80
    size = b2 & 0x7F
    if size == 126:
        size = struct.unpack("!H", rfile.read(2))[0]
    elif size == 127:
        size = struct.unpack("!Q", rfile.read(8))[0]
    mask = rfile.read(4) if masked else b""
    payload = rfile.read(size) if size else b""
    if masked and mask:
        payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    return payload


class Handler(BaseHTTPRequestHandler):
    server_version = "si-ui-mock/1.0"

    def log_message(self, fmt: str, *args) -> None:
        message = fmt % args
        if "GET /api/snapshot" in message:
            return
        print(f"[ui-mock] {self.address_string()} - {message}", flush=True)

    def _send_json(self, payload: dict, status: int = 200) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _send_bytes(self, body: bytes, content_type: str, status: int = 200) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _send_file(self, path: str) -> None:
        path = posixpath.normpath(unquote(path.split("?", 1)[0]))
        if path in ("", "/"):
            rel = "index.html"
        elif path == "/kvm":
            rel = "kvm.html"
        else:
            rel = path.lstrip("/")

        target = (ROOT / rel).resolve()
        if ROOT not in target.parents and target != ROOT:
            self.send_error(HTTPStatus.FORBIDDEN)
            return
        if not target.is_file():
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        content_type = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
        self._send_bytes(target.read_bytes(), content_type)

    def _handle_websocket(self) -> None:
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(hashlib.sha1((key + MAGIC_WS_GUID).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()

        MockState.ws_clients += 1
        try:
            while True:
                payload = _read_ws_frame(self.rfile)
                if payload is None:
                    break
                MockState.hid_frames += 1
                _ws_send_text(self.connection, json.dumps({"type": "ack", "ok": True}))
        finally:
            MockState.ws_clients = max(0, MockState.ws_clients - 1)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/api/ws/hid" and self.headers.get("Upgrade", "").lower() == "websocket":
            self._handle_websocket()
            return
        if path in ("/api/status", "/api/video/status", "/api/hid/status"):
            payload = _status_payload()
            if path == "/api/video/status":
                payload = payload["video"]
            elif path == "/api/hid/status":
                payload = payload["hid"]
            self._send_json(payload)
            return
        if path == "/api/system/info":
            self._send_json(_system_info_payload())
            return
        if path == "/api/system/logs":
            self._send_json(_logs_payload())
            return
        if path in ("/api/snapshot", "/api/stream"):
            self._send_bytes(_video_svg(), "image/svg+xml")
            return
        if path == "/api/control-bridge/status":
            self._send_json(_status_payload()["control_bridge"])
            return
        if path == "/api/control-bridge/config":
            self._send_json({
                "source": "local-ui-mock",
                "transport": "websocket_to_mock",
                "commands": {
                    "keydown": "mock keyboard press",
                    "keyup": "mock keyboard release",
                    "mousemove": "mock relative mouse move",
                    "wheel": "mock mouse wheel",
                },
            })
            return

        self._send_file(path)

    def do_HEAD(self) -> None:
        self.do_GET()

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        size = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(size) if size else b""

        if path == "/api/auth/login":
            self._send_json({"token": "local-mock", "type": "bearer"})
            return
        if path == "/api/video/quality":
            query = parse_qs(parsed.query)
            quality = query.get("quality", [None])[0]
            if quality is None and raw:
                try:
                    quality = json.loads(raw.decode("utf-8")).get("quality")
                except json.JSONDecodeError:
                    quality = None
            try:
                MockState.quality = max(1, min(100, int(quality)))
            except (TypeError, ValueError):
                self._send_json({"error": "quality must be 1..100"}, 400)
                return
            self._send_json(_status_payload()["video"])
            return

        self.send_error(HTTPStatus.NOT_FOUND)


def main() -> None:
    parser = argparse.ArgumentParser(description="Serve ESP32-P4 UI locally with mocked device APIs.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=int(os.environ.get("SI_UI_PORT", "5080")))
    args = parser.parse_args()

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"[ui-mock] serving {ROOT} at http://{args.host}:{args.port}/", flush=True)
    print(f"[ui-mock] KVM: http://{args.host}:{args.port}/kvm", flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
