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
    width = 640
    height = 480
    default_username = "admin"
    default_password = "admin"
    username = default_username
    password = "admin"
    token = "local-mock-token"
    local_credentials = False
    device_label = "ESP32-P4"
    ota_manifest_url = "https://example.com/exoanchor/esp32p4/manifest.json"
    ota_channel = "stable"
    ota_update_available = False
    ota_checked = False
    ota_message = "not checked"
    hid_frames = 0


def _uptime() -> int:
    return int(time.monotonic() - MockState.started_at)


def _format_uptime(seconds: int) -> str:
    days, rem = divmod(seconds, 86400)
    hours, rem = divmod(rem, 3600)
    minutes, _ = divmod(rem, 60)
    return f"{days}d {hours}h {minutes}m"


def _video_svg() -> bytes:
    now = time.strftime("%H:%M:%S")
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="640" height="480" viewBox="0 0 640 480">
<defs>
  <linearGradient id="g" x1="0" x2="1" y1="0" y2="1">
    <stop stop-color="#101820"/>
    <stop offset="0.55" stop-color="#20363a"/>
    <stop offset="1" stop-color="#331b2b"/>
  </linearGradient>
</defs>
<rect width="640" height="480" fill="url(#g)"/>
<g opacity="0.25" stroke="#ffffff">
  <path d="M0 96H640M0 192H640M0 288H640M0 384H640"/>
  <path d="M128 0V480M256 0V480M384 0V480M512 0V480"/>
</g>
<rect x="60" y="58" width="520" height="326" rx="10" fill="#0b0d10" opacity="0.72" stroke="#2dd4bf"/>
<text x="320" y="204" fill="#eef2f6" text-anchor="middle" font-family="Menlo, monospace" font-size="30">ExoAnchor(KV) MOCK</text>
<text x="320" y="252" fill="#9ca3af" text-anchor="middle" font-family="Menlo, monospace" font-size="20">{MockState.width}x{MockState.height} USB UVC MJPEG</text>
<text x="320" y="300" fill="#2dd4bf" text-anchor="middle" font-family="Menlo, monospace" font-size="18">Q{MockState.quality}  {now}</text>
</svg>"""
    return svg.encode("utf-8")


def _status_payload() -> dict:
    up = _uptime()
    frames = max(1, up * 7)
    modes = [
        {"pixel_format": "MJPEG", "width": 1920, "height": 1080, "resolution": "1920x1080", "fps": 50.0, "fps_x100": 5000, "selected": MockState.width == 1920 and MockState.height == 1080},
        {"pixel_format": "MJPEG", "width": 1920, "height": 1080, "resolution": "1920x1080", "fps": 30.0, "fps_x100": 3000, "selected": False},
        {"pixel_format": "MJPEG", "width": 1280, "height": 720, "resolution": "1280x720", "fps": 60.0, "fps_x100": 6000, "selected": MockState.width == 1280 and MockState.height == 720},
        {"pixel_format": "MJPEG", "width": 1280, "height": 720, "resolution": "1280x720", "fps": 30.0, "fps_x100": 3000, "selected": False},
        {"pixel_format": "MJPEG", "width": 640, "height": 480, "resolution": "640x480", "fps": 30.0, "fps_x100": 3000, "selected": MockState.width == 640 and MockState.height == 480},
    ]
    return {
        "server_uptime": up,
        "video": {
            "enabled": True,
            "connected": True,
            "initialized": True,
            "streaming": True,
            "source": "usb-uvc",
            "pixel_format": "MJPEG",
            "width": MockState.width,
            "height": MockState.height,
            "resolution": f"{MockState.width}x{MockState.height}",
            "target_width": MockState.width,
            "target_height": MockState.height,
            "target_resolution": f"{MockState.width}x{MockState.height}",
            "target_fps": 30.0,
            "modes_count": len(modes),
            "modes": modes,
            "control": {
                "preview_enabled": True,
                "active_owner": "preview",
                "kvm_mode": {"width": MockState.width, "height": MockState.height, "fps_x100": 3000},
            },
            "capture_enabled": True,
            "capture_owner": "preview",
            "data_lanes": 0,
            "lane_bitrate_mbps": 0,
            "quality": MockState.quality,
            "frames_captured": frames,
            "frames_encoded": frames,
            "frames_dropped": 0,
            "last_jpeg_size": len(_video_svg()),
            "last_frame_ms": 7,
            "fps": 6.8,
            "last_error": "",
        },
        "hid": _hid_payload(),
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
            "phy": "IP101GRI",
            "speed_mbps": 100,
            "full_duplex": True,
        },
        "active_connections": 0,
        "authEnabled": True,
    }


def _hid_payload() -> dict:
    return {
        "enabled": True,
        "initialized": True,
        "connected": True,
        "mounted": True,
        "ready": True,
        "hid_ready": True,
        "mode": "usb-fs-gpio26-27",
        "port": "USB FS GPIO26/27",
        "transport": "tinyusb",
        "pins": {"dm_gpio": 26, "dp_gpio": 27},
        "keyboard": {"available": True},
        "mouse": {"available": True},
        "stats": {"tx_messages": MockState.hid_frames, "failed_messages": 0},
        "last_error": "",
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
        "hostname": "exoanchor-p4-ui-mock",
        "device_label": MockState.device_label,
        "auth": {"login_count": 1},
    }


def _ota_payload() -> dict:
    return {
        "busy": False,
        "current_version": "v0.84",
        "settings": {
            "manifest_url": MockState.ota_manifest_url,
            "default_manifest_url": "https://example.com/exoanchor/esp32p4/manifest.json",
            "channel": MockState.ota_channel,
            "auto_check": False,
        },
        "last_check": {
            "checked": MockState.ota_checked,
            "update_available": MockState.ota_update_available,
            "version": "v0.85" if MockState.ota_update_available else "v0.84",
            "message": MockState.ota_message,
            "size": 1048576 if MockState.ota_update_available else 0,
        },
        "partition": {"running": "factory", "boot": "factory"},
    }


def _logs_payload() -> dict:
    return {
        "logs": [
            {"time": "00:00:01", "level": "INFO", "message": "Local UI mock server started"},
            {"time": "00:00:02", "level": "INFO", "message": "Mock USB UVC video source online"},
            {"time": "00:00:03", "level": "INFO", "message": "Mock GPIO26/27 HID device online"},
            {"time": "00:00:04", "level": "INFO", "message": f"Mock video mode: {MockState.width}x{MockState.height}"},
        ]
    }


def _make_token(username: str, password: str) -> str:
    raw = f"{username}:{password}".encode("utf-8")
    return "local-mock-token-" + hashlib.sha1(raw).hexdigest()[:12]


def _auth_header_valid(headers) -> bool:
    auth = headers.get("Authorization", "")
    return auth == f"Bearer {MockState.token}"


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
        elif path == "/settings":
            rel = "settings.html"
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
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self.send_error(HTTPStatus.BAD_REQUEST)
            return
        accept = base64.b64encode(hashlib.sha1((key + MAGIC_WS_GUID).encode("ascii")).digest()).decode("ascii")
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        while True:
            header = self.rfile.read(2)
            if len(header) != 2:
                return
            first, second = header
            opcode = first & 0x0F
            masked = bool(second & 0x80)
            length = second & 0x7F
            if length == 126:
                raw = self.rfile.read(2)
                if len(raw) != 2:
                    return
                length = struct.unpack("!H", raw)[0]
            elif length == 127:
                raw = self.rfile.read(8)
                if len(raw) != 8:
                    return
                length = struct.unpack("!Q", raw)[0]
            mask = self.rfile.read(4) if masked else b""
            payload = self.rfile.read(length)
            if len(payload) != length:
                return
            if masked:
                payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
            if opcode == 8:
                return
            if opcode == 1:
                MockState.hid_frames += 1

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path

        if path in ("/api/status", "/api/video/status"):
            payload = _status_payload()
            if path == "/api/video/status":
                payload = payload["video"]
            self._send_json(payload)
            return
        if path == "/api/hid/status":
            self._send_json(_hid_payload())
            return
        if path == "/api/ws/hid":
            self._handle_websocket()
            return
        if path == "/api/auth/status":
            self._send_json({
                "enabled": True,
                "username": MockState.username,
                "default_username": MockState.default_username,
                "local_password": MockState.local_credentials,
                "local_credentials": MockState.local_credentials,
                "using_default": not MockState.local_credentials,
                "must_change_credentials": not MockState.local_credentials,
                "token_valid": _auth_header_valid(self.headers),
                "username_max_length": 32,
                "min_length": 4,
                "max_length": 64,
            })
            return
        if path == "/api/system/info":
            self._send_json(_system_info_payload())
            return
        if path == "/api/system/logs":
            self._send_json(_logs_payload())
            return
        if path == "/api/ota/status":
            self._send_json(_ota_payload())
            return
        if path in ("/api/snapshot", "/api/stream"):
            self._send_bytes(_video_svg(), "image/svg+xml")
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
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                payload = {}
            username = payload.get("username")
            password = payload.get("password")
            if username != MockState.username or password != MockState.password:
                self._send_json({"error": "invalid credentials"}, 401)
                return
            self._send_json({
                "token": MockState.token,
                "type": "bearer",
                "enabled": True,
                "username": MockState.username,
                "local_password": MockState.local_credentials,
                "local_credentials": MockState.local_credentials,
                "using_default": not MockState.local_credentials,
                "must_change_credentials": not MockState.local_credentials,
            })
            return
        if path in ("/api/settings/account", "/api/settings/password"):
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            current_username = payload.get("current_username") or payload.get("currentUsername")
            current = payload.get("current_password") or payload.get("currentPassword")
            username = payload.get("username") or payload.get("new_username") or payload.get("newUsername") or MockState.username
            new_password = payload.get("new_password") or payload.get("newPassword") or payload.get("password")
            current_ok = current_username == MockState.username and current == MockState.password
            if not (_auth_header_valid(self.headers) or current_ok):
                self._send_json({"error": "unauthorized"}, 401)
                return
            if not isinstance(username, str) or len(username) < 1 or len(username) > 32 or any(ord(ch) < 33 or ord(ch) > 126 for ch in username):
                self._send_json({"error": "username must be 1..32 printable ASCII characters"}, 400)
                return
            if not isinstance(new_password, str) or len(new_password) < 4 or len(new_password) > 64 or any(ord(ch) < 33 or ord(ch) > 126 for ch in new_password):
                self._send_json({"error": "password must be 4..64 printable ASCII characters"}, 400)
                return
            MockState.username = username
            MockState.password = new_password
            MockState.token = _make_token(username, new_password)
            MockState.local_credentials = True
            self._send_json({
                "ok": True,
                "token": MockState.token,
                "type": "bearer",
                "username": MockState.username,
                "local_password": True,
                "local_credentials": True,
                "using_default": False,
                "must_change_credentials": False,
            })
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
        if path == "/api/video/resolution":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                width = int(payload.get("width"))
                height = int(payload.get("height"))
            except (TypeError, ValueError, json.JSONDecodeError):
                self._send_json({"error": "width and height required"}, 400)
                return
            if (width, height) not in {(1920, 1080), (1280, 720), (640, 480)}:
                self._send_json({"error": "unsupported resolution"}, 400)
                return
            MockState.width = width
            MockState.height = height
            self._send_json(_status_payload()["video"])
            return
        if path == "/api/settings/device":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            label = str(payload.get("label") or "ESP32-P4").strip()[:48] or "ESP32-P4"
            MockState.device_label = label
            self._send_json({"ok": True, "label": MockState.device_label})
            return
        if path == "/api/ota/status":
            self._send_json(_ota_payload())
            return
        if path == "/api/ota/settings":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            MockState.ota_manifest_url = str(payload.get("manifest_url") or MockState.ota_manifest_url)
            MockState.ota_channel = str(payload.get("channel") or "stable")
            self._send_json({"ok": True, "settings": _ota_payload()["settings"]})
            return
        if path == "/api/ota/check":
            MockState.ota_checked = True
            MockState.ota_update_available = False
            MockState.ota_message = "already latest"
            self._send_json(_ota_payload())
            return
        if path == "/api/ota/install":
            self._send_json({"ok": True, "size": 0})
            return
        if path == "/api/ota/upload":
            self._send_json({"ok": True, "size": size})
            return
        if path == "/api/ota/reboot":
            self._send_json({"ok": True})
            return

        self.send_error(HTTPStatus.NOT_FOUND)


def main() -> None:
    parser = argparse.ArgumentParser(description="Serve ESP32-P4 UI locally with mocked device APIs.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=int(os.environ.get("SI_UI_PORT", "5080")))
    args = parser.parse_args()

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"[ui-mock] serving {ROOT} at http://{args.host}:{args.port}/", flush=True)
    print(f"[ui-mock] Video: http://{args.host}:{args.port}/kvm", flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
