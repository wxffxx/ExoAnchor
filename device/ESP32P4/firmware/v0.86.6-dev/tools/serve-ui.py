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
import threading
import time
import zlib
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse


ROOT = Path(__file__).resolve().parents[1] / "main" / "www"
MAGIC_WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
H264_FIXTURES: dict[tuple[int, int], dict] = {}


def _annex_b_access_units(data: bytes) -> list[bytes]:
    """Split an Annex-B fixture at AUD NAL units without rewriting it."""
    starts: list[tuple[int, int]] = []
    index = 0
    while index + 3 <= len(data):
        if data[index:index + 4] == b"\x00\x00\x00\x01":
            starts.append((index, index + 4))
            index += 4
        elif data[index:index + 3] == b"\x00\x00\x01":
            starts.append((index, index + 3))
            index += 3
        else:
            index += 1
    if not starts:
        raise ValueError("fixture is not Annex-B H.264")
    units: list[bytes] = []
    access_start: int | None = None
    for nal_index, (prefix, payload) in enumerate(starts):
        end = starts[nal_index + 1][0] if nal_index + 1 < len(starts) else len(data)
        if payload >= end:
            continue
        nal_type = data[payload] & 0x1F
        if nal_type == 9:
            if access_start is not None and prefix > access_start:
                units.append(data[access_start:prefix])
            access_start = prefix
    if access_start is not None and access_start < len(data):
        units.append(data[access_start:])
    if not units:
        raise ValueError("fixture must contain H.264 AUD NAL units")
    return units


def _fixture_nal_types(access_unit: bytes) -> set[int]:
    types: set[int] = set()
    index = 0
    while index + 3 <= len(access_unit):
        if access_unit[index:index + 4] == b"\x00\x00\x00\x01":
            payload = index + 4
            index = payload
        elif access_unit[index:index + 3] == b"\x00\x00\x01":
            payload = index + 3
            index = payload
        else:
            index += 1
            continue
        if payload < len(access_unit):
            types.add(access_unit[payload] & 0x1F)
    return types


def _parse_h264_fixture(raw: str) -> tuple[tuple[int, int], dict]:
    try:
        mode_text, path_text = raw.split("=", 1)
        dimensions, fps_text = mode_text.lower().split("@", 1)
        width_text, height_text = dimensions.split("x", 1)
        width, height, fps = int(width_text), int(height_text), float(fps_text)
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError(
            "fixture must be WIDTHxHEIGHT@FPS=/path/to/annex-b.h264"
        ) from error
    path = Path(path_text).expanduser().resolve()
    if width < 80 or height < 80 or not 1 <= fps <= 60 or not path.is_file():
        raise argparse.ArgumentTypeError("invalid H.264 fixture mode or file")
    units = _annex_b_access_units(path.read_bytes())
    first_idr = next(
        (index for index, unit in enumerate(units)
         if 5 in _fixture_nal_types(unit)),
        None,
    )
    if first_idr is None:
        raise argparse.ArgumentTypeError("H.264 fixture contains no IDR access unit")
    units = units[first_idr:]
    first_types = _fixture_nal_types(units[0])
    if not {5, 7, 8}.issubset(first_types):
        raise argparse.ArgumentTypeError(
            "first H.264 IDR access unit must contain SPS and PPS"
        )
    return (width, height), {
        "width": width,
        "height": height,
        "fps": fps,
        "path": str(path),
        "units": units,
    }
try:
    AGENT_RUN_DELAY_SECONDS = max(
        0.2, float(os.environ.get("SI_UI_AGENT_RUN_SECONDS", "3.0"))
    )
except ValueError:
    AGENT_RUN_DELAY_SECONDS = 3.0
DEFAULT_POWER_GPIO_MAP = [
    {"role": "power_button", "label": "Power button", "direction": "output", "gpio": 4, "active_high": True, "required": True},
    {"role": "reset_button", "label": "Reset button", "direction": "output", "gpio": 5, "active_high": True, "required": True},
    {"role": "power_detect", "label": "Power detect", "direction": "input", "gpio": 0, "active_high": True, "required": False},
    {"role": "standby_detect", "label": "Power Standby detect", "direction": "input", "gpio": 1, "active_high": True, "required": False},
    {"role": "locator", "label": "Locator LED", "direction": "output", "gpio": 17, "active_high": True, "required": False},
]


class MockState:
    started_at = time.monotonic()
    quality = 75
    width = 640
    height = 480
    capture_fps_x100 = 3000
    video_stream_id = 0
    h264_ws_port = 0
    h264_force_mse = False
    h264_session_lock = threading.Lock()
    h264_session_generation = 0
    h264_session_active = False
    h264_frames_sent = 0
    h264_bytes_sent = 0
    h264_started_at = 0.0

    @classmethod
    def begin_h264_session(cls, started: float) -> int:
        """Claim mock H.264 telemetry without letting an old socket clear it."""
        with cls.h264_session_lock:
            cls.h264_session_generation += 1
            generation = cls.h264_session_generation
            cls.h264_session_active = True
            cls.h264_frames_sent = 0
            cls.h264_bytes_sent = 0
            cls.h264_started_at = started
            return generation

    @classmethod
    def end_h264_session(cls, generation: int) -> bool:
        """Clear telemetry only when this is still the newest socket."""
        with cls.h264_session_lock:
            if generation != cls.h264_session_generation:
                return False
            cls.h264_session_active = False
            return True
    default_username = "admin"
    default_password = "admin"
    username = default_username
    password = "admin"
    token = "local-mock-token"
    # Opt into the real first-claim flow for browser acceptance without
    # changing the normal visual-preview behavior.
    local_credentials = os.environ.get("SI_UI_FACTORY_CLAIM", "").lower() not in (
        "1", "true", "yes",
    )
    device_label = "ExoAnchor"
    agent_display_name = "Agent"
    target_profile = {
        "name": "Ubuntu Test Host",
        "device_type": "workstation",
        "operating_system": "linux",
        "environment": "lab",
        "location": "Prototype bench",
        "configuration": "Ubuntu 24.04, HDMI output, USB HID and UART console.",
        "purpose": "ExoAnchor KVM, UART and local Agent integration testing.",
        "notes": "Mock data for browser acceptance.",
    }
    ota_manifest_url = "https://example.com/exoanchor/esp32p4/manifest.json"
    ota_channel = "stable"
    ota_update_available = False
    ota_checked = False
    ota_message = "not checked"
    hid_frames = 0
    power_actions = 0
    power_busy_until = 0.0
    power_last_action = "idle"
    locator_on = False
    power_gpio_map = [item.copy() for item in DEFAULT_POWER_GPIO_MAP]
    auto_logout_enabled = False
    auto_logout_minutes = 15
    mcp_enabled = False
    access_mode = "manual"
    automation_operations: list[dict] = []
    ms2109_power_on = True
    ms2109_probe_valid = False
    ms2109_probe_mask = 0
    ms2109_operation_count = 0
    ms2109_last_result = "ESP_OK"
    ms2109_last_programmed_bytes = 0
    ms2109_last_verified = False
    ms2109_eeprom_image = bytes([0xFF]) * 2048
    product_features = {
        "lan_discovery_enabled": True,
        "embedded_agent_enabled": True,
        "page_context_enabled": False,
        "conversation_history_enabled": True,
        "long_term_memory_enabled": False,
        "embedded_agent_compiled": True,
        "auto_update_check_supported": False,
        "anonymous_diagnostics_supported": False,
        "stored_in_nvs": True,
    }
    preview_enabled = True
    preview_fps_x100 = 100
    video_active_owner = "preview"
    video_agent_takeover = False
    control_lease_active = False
    control_lease_owner = ""
    control_lease_mode = "supervised"
    control_lease_session_id = ""
    control_lease_reason = ""
    uart_baud_rate = 115200
    video_always_online = False
    ssh_target = {
        "configured": True,
        "host": "192.0.2.67",
        "port": 22,
        "username": "operator",
        "auth_method": "key",
        "timeout_ms": 30000,
        "password_configured": False,
        "sudo_password_configured": False,
    }
    console_credentials = {
        "configured": True,
        "username": "operator",
        "password_configured": True,
    }
    ssh_private_key_configured = True
    ssh_public_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMockKey exoanchor@mock"
    ssh_passphrase_configured = False
    agent_active_profile = "deepseek"
    agent_profiles = [
        {"id": "deepseek", "label": "DeepSeek", "provider": "deepseek", "endpoint": "https://api.deepseek.com", "model": "deepseek-v4-pro", "api_key_configured": True},
        {"id": "openai", "label": "OpenAI", "provider": "openai", "endpoint": "https://api.openai.com/v1", "model": "gpt-4o", "api_key_configured": False},
        {"id": "qwen", "label": "Qwen", "provider": "qwen", "endpoint": "https://dashscope.aliyuncs.com/compatible-mode/v1", "model": "qwen3-vl-flash", "api_key_configured": False},
        {"id": "custom", "label": "Custom", "provider": "custom", "endpoint": "", "model": "gpt-4o", "api_key_configured": False},
        {"id": "kimi", "label": "Kimi K3", "provider": "kimi", "endpoint": "https://api.moonshot.cn/v1", "model": "kimi-k3", "api_key_configured": False},
    ]
    agent_prompt = "You are the ExoAnchor local mock Agent."
    agent_prompt_default = True
    agent_tool_mode = "observe"
    agent_tool_policy = {"version": 2, "default_mode": "observe", "tools": {}}
    agent_skills = [
        {"name": "system_checklist", "enabled": True, "mode": "guided", "description": "System diagnostics", "tools": ["observe_status", "ssh_exec"]},
        {"name": "ssh_system_snapshot", "enabled": True, "mode": "scripted", "description": "SSH system snapshot", "tools": ["ssh_exec"]},
        {"name": "kvm_console_login", "enabled": True, "mode": "guided", "description": "KVM login through a device-local credential reference", "tools": ["observe_screenshot", "hid_actions", "console_login"]},
    ]
    web_search = {
        "enabled": False,
        "provider": "qwen_chat_search",
        "profile": "qwen",
        "endpoint": "",
        "model": "qwen-plus",
        "strategy": "agent",
        "api_key_configured": False,
        "available": True,
        "api_key_source": "agent_profile",
    }
    storage_files = {
        "LOGS/README.txt": b"ExoAnchor local UI mock log directory.\n",
        "AGENT/README.txt": b"Agent history and memory are stored here on device.\n",
    }
    agent_sessions = {
        "default": {"id": "default", "session_id": "default", "title": "Default session", "created_ms": 1, "updated_ms": 1, "message_count": 0}
    }
    agent_history = {"default": []}
    agent_memory = [
        {"kind": "memory", "type": "host_state", "scope": "device", "source": "local_mock", "content": "Local mock target is reachable over SSH.", "stored_ms": 1},
        {"kind": "memory", "type": "user_decision", "scope": "workspace", "source": "local_mock", "content": "Agent Workspace keeps services and automations separate from one-off Thread commands.", "stored_ms": 2},
    ]
    agent_session_seq = 0
    agent_run_seq = 0
    agent_request_seq = 0
    agent_request = {}
    agent_run = {
        "state": "idle",
        "running": False,
        "paused": False,
        "busy": False,
        "pause_requested": False,
        "cancel_requested": False,
        "abort_requested": False,
        "job_id": "",
        "run_id": "",
        "turn_id": "",
        "thread_id": "default",
        "goal": "",
        "intent_revision": 0,
        "plan_version": 0,
        "waiting_request": False,
        "session_id": "default",
        "profile": "",
        "model": "",
        "dry_run": True,
        "include_screenshot": False,
        "include_web_search": False,
        "started_ms": 0,
        "updated_ms": 0,
        "finished_ms": 0,
        "stage": "idle",
        "detail": "",
        "event_seq": 0,
        "events": [],
    }
    agent_run_started_at = 0.0
    agent_run_paused_at = 0.0
    # Mock-only volatile receipt binding; never projected by
    # _agent_run_payload().
    agent_run_idempotency_key = ""
    agent_run_request_fingerprint = ""
    # Deterministic fault hooks used by the host contract tests. They model a
    # newer live-status snapshot losing the retained Run and an event response
    # stopping after its last completely serialized item.
    agent_run_replace_before_projection = False
    agent_run_event_page_limit = None
    host_display_seq = 0
    host_display = {
        "schema_version": "exoanchor.host_display.plan.v1",
        "supported": True,
        "reference_target": "ubuntu-grub-drm",
        "state": "empty",
        "plan_id": "",
        "approval_required": False,
        "approved": False,
        "reboot_required": False,
        "temporary_apply_supported": False,
        "persistent_apply_supported": True,
        "created_ms": 0,
        "updated_ms": 0,
        "target": {
            "connector": "",
            "width": 0,
            "height": 0,
            "refresh_millihz": 0,
            "refresh_hz": 0,
            "persistent": False,
        },
    }
    boot_key_seq = 0
    boot_key = {
        "schema_version": "exoanchor.boot_key.plan.v1",
        "supported": True,
        "state": "empty",
        "plan_id": "",
        "approval_required": False,
        "approved": False,
        "cancel_requested": False,
        "reset_triggered": False,
        "attempts_sent": 0,
        "created_ms": 0,
        "started_ms": 0,
        "updated_ms": 0,
        "finished_ms": 0,
        "verification_mode": "human_kvm",
        "agent_navigation_supported": False,
        "profile": {
            "profile_id": "",
            "target": "invalid",
            "trigger": "none",
            "key_code": "",
            "start_delay_ms": 0,
            "interval_ms": 0,
            "max_attempts": 0,
            "total_timeout_ms": 0,
        },
    }


def _uptime() -> int:
    return int(time.monotonic() - MockState.started_at)


def _format_uptime(seconds: int) -> str:
    days, rem = divmod(seconds, 86400)
    hours, rem = divmod(rem, 3600)
    minutes, _ = divmod(rem, 60)
    return f"{days}d {hours}h {minutes}m"


def _video_svg() -> bytes:
    now = time.strftime("%H:%M:%S")
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="{MockState.width}" height="{MockState.height}" viewBox="0 0 640 480">
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


def _control_lease_payload() -> dict:
    kvm_view_active = bool(
        MockState.video_active_owner == "kvm" and MockState.video_stream_id
    )
    agent_active = bool(MockState.control_lease_active)
    input_control_active = bool(
        agent_active and MockState.control_lease_mode != "observe"
    )
    kvm_active = bool(kvm_view_active and not input_control_active)
    return {
        "active": bool(agent_active or kvm_active),
        "owner": (
            MockState.control_lease_owner if agent_active else
            ("kvm" if kvm_active else "none")
        ),
        "kvm_active": kvm_active,
        "kvm_view_active": kvm_view_active,
        "agent_active": agent_active,
        "input_control_active": input_control_active,
        "agent_owner": MockState.control_lease_owner if agent_active else "",
        "mode": MockState.control_lease_mode if agent_active else "",
        "session_id": MockState.control_lease_session_id if agent_active else "",
        "reason": MockState.control_lease_reason if agent_active else "",
        "can_request": not agent_active,
        "expires_in_ms": 5000 if agent_active else 0,
        "kvm_expires_in_ms": 5000 if kvm_view_active else 0,
    }


def _status_payload() -> dict:
    up = _uptime()
    frames = max(1, up * 7)
    h264_elapsed = (
        max(0.001, time.monotonic() - MockState.h264_started_at)
        if MockState.h264_session_active and MockState.h264_started_at
        else 0.0
    )
    h264_fps = MockState.h264_frames_sent / h264_elapsed if h264_elapsed else 0.0
    h264_mbps = (
        MockState.h264_bytes_sent * 8 / h264_elapsed / 1_000_000
        if h264_elapsed else 0.0
    )
    capture_active = (
        MockState.video_agent_takeover
        or MockState.preview_enabled
        or MockState.video_always_online
    )
    capture_owner = (
        MockState.video_active_owner
        if MockState.video_agent_takeover
        else (
            "preview"
            if MockState.preview_enabled
            else ("always-on" if MockState.video_always_online else "off")
        )
    )
    modes = [
        {"pixel_format": "MJPEG", "width": 1920, "height": 1080, "resolution": "1920x1080", "fps": 50.0, "fps_x100": 5000, "selected": MockState.width == 1920 and MockState.height == 1080 and MockState.capture_fps_x100 == 5000},
        {"pixel_format": "MJPEG", "width": 1920, "height": 1080, "resolution": "1920x1080", "fps": 30.0, "fps_x100": 3000, "selected": MockState.width == 1920 and MockState.height == 1080 and MockState.capture_fps_x100 == 3000},
        {"pixel_format": "MJPEG", "width": 1280, "height": 720, "resolution": "1280x720", "fps": 50.0, "fps_x100": 5000, "selected": MockState.width == 1280 and MockState.height == 720 and MockState.capture_fps_x100 == 5000},
        {"pixel_format": "MJPEG", "width": 1280, "height": 720, "resolution": "1280x720", "fps": 30.0, "fps_x100": 3000, "selected": MockState.width == 1280 and MockState.height == 720 and MockState.capture_fps_x100 == 3000},
        {"pixel_format": "MJPEG", "width": 640, "height": 480, "resolution": "640x480", "fps": 30.0, "fps_x100": 3000, "selected": MockState.width == 640 and MockState.height == 480 and MockState.capture_fps_x100 == 3000},
    ]
    return {
        "server_uptime": up,
        "performance": {
            "cpu": {"valid": True, "usage_percent": 12.0, "cores": 2},
            "memory": {
                "heap": {"free_bytes": 21 * 1024 * 1024},
                "internal": {"total_bytes": 4 * 1024 * 1024, "used_bytes": 1536 * 1024, "free_bytes": 2560 * 1024, "minimum_free_bytes": 2304 * 1024, "usage_percent": 37.5},
                "psram": {"total_bytes": 32 * 1024 * 1024, "used_bytes": 13 * 1024 * 1024, "free_bytes": 19 * 1024 * 1024, "minimum_free_bytes": 18 * 1024 * 1024, "usage_percent": 40.6},
            },
            "storage": {
                "schema_version": "exoanchor.storage.v1",
                "flash": {"total_bytes": 16 * 1024 * 1024, "used_bytes": 9 * 1024 * 1024, "free_bytes": 7 * 1024 * 1024, "usage_percent": 56.2},
                "tf_card": {"mounted": True, "total_bytes": 64 * 1024 * 1024 * 1024, "used_bytes": 18 * 1024 * 1024 * 1024, "free_bytes": 46 * 1024 * 1024 * 1024, "usage_percent": 28.1, "card_type": "SDXC", "card_name": "MOCK64"},
                "devices": [
                    {"id": "internal_flash", "label": "Flash", "kind": "nor_flash", "bus": "internal_spi", "role": "firmware", "supported": True, "detected": True, "mounted": False, "total_bytes": 16 * 1024 * 1024, "used_bytes": 9 * 1024 * 1024, "free_bytes": 7 * 1024 * 1024, "usage_percent": 56.2, "last_error": ""},
                    {"id": "tf_card", "label": "TF Card", "kind": "removable_card", "bus": "sdmmc", "role": "data", "supported": True, "detected": True, "mounted": True, "total_bytes": 64 * 1024 * 1024 * 1024, "used_bytes": 18 * 1024 * 1024 * 1024, "free_bytes": 46 * 1024 * 1024 * 1024, "usage_percent": 28.1, "last_error": ""},
                ],
                "expansion_slots": [
                    {"id": "spi_data", "label": "SPI Data", "bus": "spi", "role": "data", "supported": False, "implemented": False},
                ],
            },
            "video": {"fps": 29.8 if capture_active else 0.0, "target_fps": 30.0, "frame_interval_ms": 33.6 if capture_active else 0.0, "expected_frame_interval_ms": 33.3, "estimated_mbps": 8.4 if capture_active else 0.0, "last_jpeg_size": 36700 if capture_active else 0, "drop_percent": 0.02 if capture_active else 0.0},
            "task_count": 34,
            "active_connections": 2,
        },
        "video": {
            "enabled": True,
            "connected": True,
            "device_connected": True,
            "frame_ready": capture_active,
            "state": "active" if capture_active else "standby",
            "initialized": True,
            "streaming": capture_active,
            "source": "usb-uvc",
            "pixel_format": "MJPEG",
            "width": MockState.width,
            "height": MockState.height,
            "resolution": f"{MockState.width}x{MockState.height}",
            "target_width": MockState.width,
            "target_height": MockState.height,
            "target_resolution": f"{MockState.width}x{MockState.height}",
            "target_fps": MockState.capture_fps_x100 / 100,
            "target_fps_x100": MockState.capture_fps_x100,
            "modes_count": len(modes),
            "modes": modes,
            "control": {
                "preview_enabled": MockState.preview_enabled,
                "active_owner": capture_owner,
                "always_on": MockState.video_always_online,
                "agent_active": MockState.video_agent_takeover,
                "agent_takeover": MockState.video_agent_takeover,
                "preview_mode": {"width": 1280, "height": 720, "fps": MockState.preview_fps_x100 / 100, "fps_x100": MockState.preview_fps_x100, "stride_ms": 100000 // MockState.preview_fps_x100, "stored_in_nvs": True},
                "kvm_mode": {"width": MockState.width, "height": MockState.height, "fps_x100": MockState.capture_fps_x100},
            },
            "always_online": MockState.video_always_online,
            "capture_enabled": capture_active,
            "capture_owner": capture_owner,
            "data_lanes": 0,
            "lane_bitrate_mbps": 0,
            "quality": MockState.quality,
            "frames_captured": frames,
            "frames_encoded": frames,
            "frames_dropped": 0,
            "last_jpeg_size": len(_video_svg()),
            "frame_interval_ms": 147 if capture_active else 0,
            "last_frame_ms": 7 if capture_active else 0,
            "fps": 6.8 if capture_active else 0.0,
            "last_error": "",
            "stream_metrics": {
                "valid": MockState.h264_session_active and h264_fps > 0,
                "stream_id": MockState.video_stream_id,
                "fps": h264_fps,
                "bitrate_mbps": h264_mbps,
                "sample_window_ms": int(h264_elapsed * 1000),
            },
            "h264_runtime": {
                "compiled": bool(H264_FIXTURES),
                "service_ready": bool(H264_FIXTURES),
                "resource_ready": bool(H264_FIXTURES),
                "available": bool(H264_FIXTURES),
                "session_active": MockState.h264_session_active,
                "pipeline_ready": MockState.h264_session_active,
                "restore_pending": False,
                "teardown_poisoned": False,
                "width": MockState.width,
                "height": MockState.height,
                "target_fps": min(MockState.capture_fps_x100 // 100, 25),
                "resources": {
                    "yuv_surfaces": 2,
                    "au_slots": 2,
                    "overlap_enabled": True,
                    "serial_fallback": False,
                },
                "chain_metrics": {
                    "valid": MockState.h264_session_active and h264_fps > 0,
                    "age_ms": 0,
                    "window_ms": int(h264_elapsed * 1000),
                    "source_fps": h264_fps,
                    "output_fps": h264_fps,
                    "source_drops": 0,
                    "bitrate_bps": int(h264_mbps * 1_000_000),
                    "backpressure_drops_total": 0,
                    "encode_failures_total": 0,
                },
            },
        },
        "control_lease": _control_lease_payload(),
        "hid": _hid_payload(),
        "power": _power_payload(),
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
            "speed_mbps": 100,
            "full_duplex": True,
        },
        "active_connections": 0,
        "authEnabled": True,
    }

def _network_payload() -> dict:
    return {
        "configured": True,
        "initialized": True,
        "link_up": True,
        "connected": True,
        "speed_mbps": 100,
        "full_duplex": True,
        "interface": "ethernet",
        "device_id": "ea-p4-000001",
        "hostname": "exoanchor-000001",
        "mode": "dhcp",
        "address_source": "dhcp",
        "config_state": "active",
        "config_generation": 0,
        "pending_confirmation": False,
        "confirm_remaining_seconds": 0,
        "recovered_pending": False,
        "ipv4": "127.0.0.1",
        "netmask": "255.0.0.0",
        "gateway": "127.0.0.1",
        "dns_primary": "127.0.0.1",
        "dns_secondary": "",
        "mac": "02:00:00:00:00:01",
        "last_error": "",
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


def _power_role(role: str) -> dict:
    for item in MockState.power_gpio_map:
        if item["role"] == role:
            return item
    return {"role": role, "label": role, "direction": "input", "gpio": -1, "active_high": True, "required": False}


def _power_map_entry(item: dict, busy: bool) -> dict:
    gpio = int(item.get("gpio", -1))
    enabled = gpio >= 0
    state = "not-wired"
    active = False
    if enabled:
        if item.get("role") == "locator":
            active = MockState.locator_on
            state = "on" if active else "off"
        elif item.get("direction") == "output":
            state = "busy" if busy else "idle"
        else:
            state = "off"
    return {
        "role": item["role"],
        "label": item["label"],
        "direction": item["direction"],
        "gpio": gpio,
        "enabled": enabled,
        "implemented": enabled,
        "active_high": bool(item.get("active_high", True)),
        "active_level": "high" if item.get("active_high", True) else "low",
        "active": active,
        "configurable": True,
        "required": bool(item.get("required", False)),
        "state": state,
    }


def _power_payload() -> dict:
    busy = time.monotonic() < MockState.power_busy_until
    pwr = _power_role("power_button")
    rst = _power_role("reset_button")
    pwr_det = _power_role("power_detect")
    stby_det = _power_role("standby_detect")
    locator = _power_role("locator")
    pwr_det_supported = int(pwr_det.get("gpio", -1)) >= 0
    stby_det_supported = int(stby_det.get("gpio", -1)) >= 0
    locator_supported = int(locator.get("gpio", -1)) >= 0
    return {
        "enabled": True,
        "initialized": True,
        "available": True,
        "busy": busy,
        "active_high": bool(pwr.get("active_high", True)),
        "active_level": "high" if pwr.get("active_high", True) else "low",
        "default_press_ms": 500,
        "force_off_ms": 5000,
        "action_count": MockState.power_actions,
        "last_action": "busy" if busy else MockState.power_last_action,
        "last_error": "",
        "buttons": {
            "power": {"gpio": pwr.get("gpio", -1), "label": "Power button", "available": int(pwr.get("gpio", -1)) >= 0, "press_ms": 500},
            "reset": {"gpio": rst.get("gpio", -1), "label": "Reset button", "available": int(rst.get("gpio", -1)) >= 0, "press_ms": 500},
        },
        "detect": {
            "power": {"gpio": pwr_det.get("gpio", -1), "supported": pwr_det_supported, "active": False, "active_high": bool(pwr_det.get("active_high", True)), "active_level": "high" if pwr_det.get("active_high", True) else "low", "state": "off" if pwr_det_supported else "not-wired"},
            "standby": {"gpio": stby_det.get("gpio", -1), "supported": stby_det_supported, "active": False, "active_high": bool(stby_det.get("active_high", True)), "active_level": "high" if stby_det.get("active_high", True) else "low", "state": "off" if stby_det_supported else "not-wired"},
        },
        "locator": {"gpio": locator.get("gpio", -1), "supported": locator_supported, "active": MockState.locator_on and locator_supported, "active_high": bool(locator.get("active_high", True)), "state": "on" if MockState.locator_on and locator_supported else ("off" if locator_supported else "not-wired")},
        "gpio_map": [_power_map_entry(item, busy) for item in MockState.power_gpio_map],
    }


def _system_info_payload() -> dict:
    up = _uptime()
    flash_total = 16 * 1024 * 1024
    flash_app_partitions = 3 * 4 * 1024 * 1024
    flash_data_partitions = 0x6000 + 0x2000 + 0x1000
    flash_used = flash_app_partitions + flash_data_partitions
    flash_layout_end = 0xC20000
    tf_total = 64 * 1024 * 1024 * 1024
    tf_used = 18 * 1024 * 1024 * 1024
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
        "disk": {
            "total_gb": flash_total / 1024 / 1024 / 1024,
            "used_gb": flash_used / 1024 / 1024 / 1024,
            "free_gb": (flash_total - flash_used) / 1024 / 1024 / 1024,
            "usage_percent": flash_used * 100 / flash_total,
            "flash": {
                "detected": True,
                "usage_basis": "partition_map_allocated",
                "total_bytes": flash_total,
                "used_bytes": flash_used,
                "free_bytes": flash_total - flash_used,
                "allocated_bytes": flash_used,
                "unallocated_bytes": flash_total - flash_used,
                "partition_bytes": flash_used,
                "layout_end_bytes": flash_layout_end,
                "layout_gap_bytes": flash_layout_end - flash_used,
                "usage_percent": flash_used * 100 / flash_total,
                "app_partition_bytes": flash_app_partitions,
                "data_partition_bytes": flash_data_partitions,
                "nvs": {
                    "stats_available": True,
                    "partition_bytes": 24 * 1024,
                    "total_entries": 684,
                    "used_entries": 146,
                    "free_entries": 538,
                    "namespace_count": 8,
                    "usage_percent": 21.3,
                },
            },
            "tf_card": {
                "supported": True,
                "mounted": True,
                "total_bytes": tf_total,
                "used_bytes": tf_used,
                "free_bytes": tf_total - tf_used,
                "usage_percent": tf_used * 100 / tf_total,
                "root_path": "/sdcard/EA",
            },
        },
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
        "current_version": "v0.87.5-dev",
        "settings": {
            "manifest_url": MockState.ota_manifest_url,
            "default_manifest_url": "https://example.com/exoanchor/esp32p4/manifest.json",
            "channel": MockState.ota_channel,
            "auto_check": False,
        },
        "last_check": {
            "checked": MockState.ota_checked,
            "update_available": MockState.ota_update_available,
            "version": "v0.87.5-dev+mock-update" if MockState.ota_update_available else "v0.87.5-dev",
            "message": MockState.ota_message,
            "size": 1048576 if MockState.ota_update_available else 0,
        },
        "partition": {"running": "factory", "boot": "factory"},
    }


def _uart_payload() -> dict:
    return {
        "supported": True,
        "initialized": True,
        "port": 1,
        "rx_gpio": 50,
        "tx_gpio": 51,
        "baud_rate": MockState.uart_baud_rate,
        "default_baud_rate": 115200,
        "fallback_baud_rate": 9600,
        "baud_mode": "fast" if MockState.uart_baud_rate == 115200 else "fallback",
        "format": "8N1",
        "flow_control": False,
        "rx_bytes": 128,
        "tx_bytes": 32,
        "buffered_bytes": 0,
        "dropped_bytes": 0,
        "last_error": "",
        "websocket": "/api/ws/uart",
        "baud_endpoint": "/api/uart/baud",
        "authenticate_endpoint": "/api/uart/authenticate",
        "websocket_connected": False,
        "automation": {
            "available": True, "channel": "uart", "active": False,
            "manual_input_enabled": True, "cancel_requested": False,
            "can_stop": False, "generation": 0, "started_ms": 0,
            "actor": "none", "operation_id": "", "run_id": "", "label": "",
        },
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


def _model_capabilities(provider: str, model: str) -> dict:
    capabilities = {
        "text_input": True,
        "image_input": False,
        "video_input": False,
        "base64_image_input": False,
        "public_image_url": False,
        "native_tools": False,
        "structured_json": False,
        "streaming": False,
        "reasoning": False,
        "preserve_assistant_message": False,
        "fixed_sampling_parameters": False,
    }
    if provider == "deepseek":
        capabilities.update(
            native_tools=True, structured_json=True, streaming=True,
            reasoning=True,
        )
    elif provider == "openai":
        capabilities.update(
            image_input=True, base64_image_input=True,
            public_image_url=True, native_tools=True,
            structured_json=True, streaming=True, reasoning=True,
        )
    elif provider == "qwen":
        vision = "-vl" in model.lower() or "omni" in model.lower()
        capabilities.update(
            image_input=vision, video_input=vision,
            base64_image_input=vision, public_image_url=vision,
            native_tools=True, structured_json=True, streaming=True,
            reasoning=True,
        )
    elif provider == "kimi" and model.lower().startswith("kimi-k3"):
        capabilities.update(
            image_input=True, video_input=True,
            base64_image_input=True, native_tools=True,
            structured_json=True, streaming=True, reasoning=True,
            preserve_assistant_message=True,
            fixed_sampling_parameters=True,
            default_reasoning_effort="low",
        )
    elif provider == "kimi":
        capabilities.update(
            native_tools=True, structured_json=True, streaming=True,
        )
    return capabilities


def _agent_api_payload() -> dict:
    profiles = json.loads(json.dumps(MockState.agent_profiles))
    for profile in profiles:
        profile["capabilities"] = _model_capabilities(
            profile["provider"], profile["model"]
        )
    active = next((item for item in profiles if item["id"] == MockState.agent_active_profile), profiles[0])
    return {
        "active_profile": active["id"],
        "label": active["label"],
        "provider": active["provider"],
        "api_provider": active["provider"],
        "endpoint": active["endpoint"],
        "model": active["model"],
        "api_key_configured": active["api_key_configured"],
        "capabilities": active["capabilities"],
        "profiles": profiles,
        "stored_in_nvs": True,
        "profile_count": len(profiles),
    }


def _agent_tools_payload() -> dict:
    return {
        "supported": True,
        "settings_endpoint": "/api/settings/agent-tools",
        "default_mode": MockState.agent_tool_mode,
        "modes": ["observe", "supervised", "autonomous"],
        "policy_schema_version": 2,
        "policy_max_length": 4096,
        "skills_max_length": 4096,
        "policy_json": json.dumps(MockState.agent_tool_policy, separators=(",", ":")),
        "policy": json.loads(json.dumps(MockState.agent_tool_policy)),
        "skills_json": json.dumps(MockState.agent_skills, separators=(",", ":")),
        "skills": json.loads(json.dumps(MockState.agent_skills)),
        "web_search": json.loads(json.dumps(MockState.web_search)),
    }


def _ssh_target_payload() -> dict:
    payload = json.loads(json.dumps(MockState.ssh_target))
    payload.update({
        "supported": True,
        "private_key_configured": MockState.ssh_private_key_configured,
        "password_storage": "nvs_local",
        "password_policy": "local_only_not_sent_to_agent_api",
        "sudo_password_policy": "local_only_used_for_sudo_stdin_not_sent_to_agent_api",
    })
    return payload


def _console_credentials_payload() -> dict:
    payload = json.loads(json.dumps(MockState.console_credentials))
    payload.update({
        "supported": True,
        "scope": "target_console",
        "credential_ref": "console://default",
        "agent_kvm_login": bool(
            payload.get("configured") and payload.get("password_configured")
        ),
        "keyboard_layout": "us_ascii",
        "password_policy": "local_secret_not_returned_or_sent_to_agent_api",
    })
    return payload


def _ssh_key_payload() -> dict:
    return {
        "supported": True,
        "private_key_configured": MockState.ssh_private_key_configured,
        "public_key_configured": bool(MockState.ssh_public_key),
        "passphrase_configured": MockState.ssh_passphrase_configured,
        "private_key_max_length": 8191,
        "public_key_max_length": 1023,
        "passphrase_max_length": 255,
        "auth_method": "key" if MockState.ssh_private_key_configured else "password",
        "public_key": MockState.ssh_public_key,
    }


def _capabilities_payload() -> dict:
    tools = [
        {"name": "observe_status", "category": "observe", "method": "local", "endpoint": "runtime.observe_status", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Read device status"},
        {"name": "observe_video_status", "category": "observe", "method": "local", "endpoint": "runtime.observe_video_status", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Read video capture state"},
        {"name": "observe_hid_status", "category": "observe", "method": "local", "endpoint": "runtime.observe_hid_status", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Read USB HID readiness"},
        {"name": "observe_screenshot", "category": "observe", "method": "GET", "endpoint": "/api/snapshot", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "context_attachment", "runtime_callable": False, "description": "Capture the target display as attached context"},
        {"name": "video_lease", "category": "lease", "method": "POST", "endpoint": "/api/video/lease", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "runtime_manager", "runtime_callable": False, "description": "Manage video ownership"},
        {"name": "control_lease", "category": "lease", "method": "GET/POST", "endpoint": "/api/control/lease", "implemented": True, "available": True, "permission": "supervised", "risk": "medium", "runtime_surface": "runtime_manager", "runtime_callable": False, "description": "Manage HID control ownership"},
        {"name": "ssh_exec", "category": "control", "method": "local", "endpoint": "runtime.ssh_exec", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Execute an SSH command"},
        {"name": "host_display", "category": "control", "method": "local", "endpoint": "runtime.host_display", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Plan, approve, apply, verify, or roll back the Ubuntu Host display mode"},
        {"name": "boot_key_sequence", "category": "control", "method": "local", "endpoint": "runtime.boot_key_sequence", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Run a bounded, preemptible BIOS or Boot Menu key sequence"},
        {"name": "hid_actions", "category": "control", "method": "local", "endpoint": "runtime.hid_actions", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "action", "runtime_callable": True, "description": "Send keyboard and mouse actions"},
        {"name": "console_login", "category": "control", "method": "local", "endpoint": "runtime.console_login", "implemented": True, "available": bool(MockState.console_credentials.get("configured") and MockState.console_credentials.get("password_configured")), "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Inject console://default locally through KVM HID without returning the secret"},
        {"name": "conversation_history", "category": "memory", "method": "GET/POST", "endpoint": "/api/agent/history", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "manual", "runtime_callable": False, "description": "Manage conversation history"},
        {"name": "working_memory", "category": "memory", "method": "GET/POST", "endpoint": "/api/agent/memory", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "manual", "runtime_callable": False, "description": "Manage working memory"},
        {"name": "memory_search", "category": "memory", "method": "local", "endpoint": "runtime.memory_search", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Search working memory"},
        {"name": "history_search", "category": "memory", "method": "local", "endpoint": "runtime.history_search", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Search conversation history"},
        {"name": "memory_write", "category": "memory", "method": "local", "endpoint": "runtime.memory_write", "implemented": True, "available": True, "permission": "supervised", "risk": "medium", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Write durable memory"},
        {"name": "power_action", "category": "control", "method": "local", "endpoint": "runtime.power_action", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Control target power"},
        {"name": "wait", "category": "runtime", "method": "local", "endpoint": "runtime.wait", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "action", "runtime_callable": True, "description": "Pause between observations"},
        {"name": "ask_user", "category": "runtime", "method": "local", "endpoint": "runtime.ask_user", "implemented": True, "available": True, "permission": "supervised", "risk": "medium", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Ask the human for confirmation"},
        {"name": "ensure_package", "category": "control", "method": "local", "endpoint": "runtime.ensure_package", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Reconcile packages over SSH"},
        {"name": "ensure_user", "category": "control", "method": "local", "endpoint": "runtime.ensure_user", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Reconcile a host user"},
        {"name": "ensure_directory", "category": "control", "method": "local", "endpoint": "runtime.ensure_directory", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Reconcile a host directory"},
        {"name": "download_file", "category": "control", "method": "local", "endpoint": "runtime.download_file", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Download a verified file"},
        {"name": "write_file", "category": "control", "method": "local", "endpoint": "runtime.write_file", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Write a bounded host file"},
        {"name": "ensure_systemd_service", "category": "control", "method": "local", "endpoint": "runtime.ensure_systemd_service", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Reconcile a systemd service"},
        {"name": "check_service", "category": "observe", "method": "local", "endpoint": "runtime.check_service", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Read service state"},
        {"name": "verify_port", "category": "observe", "method": "local", "endpoint": "runtime.verify_port", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Verify a listening port"},
        {"name": "web_search", "category": "research", "method": "local", "endpoint": "runtime.web_search", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Research current information"},
        {"name": "uart_status", "category": "observe", "method": "local", "endpoint": "runtime.uart_status", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Read target UART status"},
        {"name": "uart_read", "category": "observe", "method": "local", "endpoint": "runtime.uart_read", "implemented": True, "available": True, "permission": "observe", "risk": "low", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Read the UART journal without consuming Terminal output"},
        {"name": "uart_write", "category": "control", "method": "local", "endpoint": "runtime.uart_write", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Write bounded UART data after exact approval"},
        {"name": "uart_baud", "category": "control", "method": "local", "endpoint": "runtime.uart_baud", "implemented": True, "available": True, "permission": "supervised", "risk": "high", "runtime_surface": "tool_call", "runtime_callable": True, "description": "Switch between primary and fallback UART modes"},
        {"name": "ssh_terminal", "category": "manual", "method": "WS", "endpoint": "/api/ws/ssh", "implemented": True, "available": True, "permission": "supervised", "risk": "medium", "runtime_surface": "manual", "runtime_callable": False, "description": "Human-operated SSH terminal"},
        {"name": "uart_terminal", "category": "manual", "method": "WS", "endpoint": "/api/ws/uart", "implemented": True, "available": True, "permission": "supervised", "risk": "medium", "runtime_surface": "manual", "runtime_callable": False, "description": "Human-operated serial terminal"},
    ]
    policy_tools = MockState.agent_tool_policy.get("tools", {})
    mcp_tools = {
        "observe_status",
        "observe_screenshot",
        "video_lease",
        "control_lease",
        "hid_actions",
        "console_login",
        "power_action",
        "ssh_exec",
    }
    for tool in tools:
        policy = policy_tools.get(tool["name"], {})
        agent_callable = bool(
            tool.get("runtime_callable")
            or tool["name"] == "observe_screenshot"
        )
        mcp_callable = tool["name"] in mcp_tools
        legacy_enabled = policy.get("enabled", True)
        tool["agent_callable"] = agent_callable
        tool["mcp_callable"] = mcp_callable
        tool["agent_enabled"] = bool(
            agent_callable and policy.get("agent_enabled", legacy_enabled)
        )
        tool["mcp_enabled"] = bool(
            mcp_callable and policy.get("mcp_enabled", True)
        )
    return {
        "ms2109_test": {
            "supported": True,
            "initialized": True,
            "power_control": True,
            "physical_eeprom": True,
            "eeprom_read": True,
            "eeprom_program_and_verify": True,
            "status_endpoint": "/api/ms2109/status",
        },
        "gpio_matrix": {
            "gpio_min": 0,
            "gpio_max": 54,
            "scope": "Firmware-owned numbered GPIOs; dedicated USB, flash, and PSRAM pads are excluded.",
            "runtime_overlay": "power.gpio_map",
            "assignments": [
                {"gpio": 16, "signal": "ms2109_eeprom_wp", "label": "MS2109 EEPROM WP", "subsystem": "video", "direction": "output", "configurable": False, "source": "board_profile"},
                {"gpio": 20, "signal": "eth_mdc", "label": "RMII MDC", "subsystem": "ethernet", "direction": "output", "configurable": False, "source": "board_profile"},
                {"gpio": 21, "signal": "eth_mdio", "label": "RMII MDIO", "subsystem": "ethernet", "direction": "inout", "configurable": False, "source": "board_profile"},
                {"gpio": 22, "signal": "eth_phy_reset", "label": "PHY RESET", "subsystem": "ethernet", "direction": "output", "configurable": False, "source": "board_profile"},
                {"gpio": 24, "signal": "usb_host_dm", "label": "USB HOST D-", "subsystem": "usb_host", "direction": "inout", "configurable": False, "source": "board_profile"},
                {"gpio": 25, "signal": "usb_host_dp", "label": "USB HOST D+", "subsystem": "usb_host", "direction": "inout", "configurable": False, "source": "board_profile"},
                {"gpio": 26, "signal": "hid_usb_dm", "label": "USB HID D-", "subsystem": "usb_hid", "direction": "inout", "configurable": False, "source": "board_profile"},
                {"gpio": 27, "signal": "hid_usb_dp", "label": "USB HID D+", "subsystem": "usb_hid", "direction": "inout", "configurable": False, "source": "board_profile"},
                {"gpio": 28, "signal": "eth_rmii_crs_dv", "label": "RMII CRS_DV", "subsystem": "ethernet", "direction": "input", "configurable": False, "source": "board_profile"},
                {"gpio": 29, "signal": "eth_rmii_rxd0", "label": "RMII RXD0", "subsystem": "ethernet", "direction": "input", "configurable": False, "source": "board_profile"},
                {"gpio": 30, "signal": "eth_rmii_rxd1", "label": "RMII RXD1", "subsystem": "ethernet", "direction": "input", "configurable": False, "source": "board_profile"},
                {"gpio": 32, "signal": "eth_rmii_clk", "label": "RMII REF_CLK", "subsystem": "ethernet", "direction": "input", "configurable": False, "source": "board_profile"},
                {"gpio": 33, "signal": "eth_rmii_tx_en", "label": "RMII TX_EN", "subsystem": "ethernet", "direction": "output", "configurable": False, "source": "board_profile"},
                {"gpio": 34, "signal": "eth_rmii_txd0", "label": "RMII TXD0", "subsystem": "ethernet", "direction": "output", "configurable": False, "source": "board_profile"},
                {"gpio": 35, "signal": "eth_rmii_txd1", "label": "RMII TXD1", "subsystem": "ethernet", "direction": "output", "configurable": False, "source": "board_profile"},
                {"gpio": 37, "signal": "debug_uart_tx", "label": "DEBUG UART TX", "subsystem": "debug", "direction": "output", "configurable": False, "source": "board_profile"},
                {"gpio": 38, "signal": "debug_uart_rx", "label": "DEBUG UART RX", "subsystem": "debug", "direction": "input", "configurable": False, "source": "board_profile"},
                {"gpio": 39, "signal": "tf_sdmmc_d0", "label": "TF SDMMC D0", "subsystem": "storage", "direction": "inout", "configurable": False, "source": "board_profile"},
                {"gpio": 40, "signal": "tf_sdmmc_d1", "label": "TF SDMMC D1", "subsystem": "storage", "direction": "inout", "configurable": False, "source": "board_profile"},
                {"gpio": 41, "signal": "tf_sdmmc_d2", "label": "TF SDMMC D2", "subsystem": "storage", "direction": "inout", "configurable": False, "source": "board_profile"},
                {"gpio": 42, "signal": "tf_sdmmc_d3", "label": "TF SDMMC D3", "subsystem": "storage", "direction": "inout", "configurable": False, "source": "board_profile"},
                {"gpio": 43, "signal": "tf_sdmmc_clk", "label": "TF SDMMC CLK", "subsystem": "storage", "direction": "output", "configurable": False, "source": "board_profile"},
                {"gpio": 44, "signal": "tf_sdmmc_cmd", "label": "TF SDMMC CMD", "subsystem": "storage", "direction": "inout", "configurable": False, "source": "board_profile"},
                {"gpio": 50, "signal": "target_uart_rx", "label": "UART1 RX", "subsystem": "target_uart", "direction": "input", "configurable": False, "source": "board_profile"},
                {"gpio": 51, "signal": "target_uart_tx", "label": "UART1 TX", "subsystem": "target_uart", "direction": "output", "configurable": False, "source": "board_profile"},
            ],
        },
        "agent_tools": {
            "supported": True,
            "schema_version": "exoanchor.agent_tools.v2",
            "policy_schema_version": 2,
            "tools": tools,
        },
        "kvm_observation_shared_during_input_control": True,
        "manual_stop_control": True,
    }


def _ms2109_payload() -> dict:
    crc32 = zlib.crc32(MockState.ms2109_eeprom_image) & 0xFFFFFFFF
    return {
        "schema": "exoanchor.ms2109-test.v1",
        "supported": True,
        "enabled": True,
        "initialized": True,
        "power_on": MockState.ms2109_power_on,
        "write_protected": True,
        "operation_count": MockState.ms2109_operation_count,
        "last_result": MockState.ms2109_last_result,
        "power": {
            "on": MockState.ms2109_power_on,
            "state_kind": "commanded",
            "rail_feedback_available": False,
            "switch_3v3_gpio": 13,
            "switch_3v3_output_level": 1 if MockState.ms2109_power_on else 0,
            "enable_1v2_gpio": 18,
            "enable_1v2_output_level": 1 if MockState.ms2109_power_on else 0,
            "sequence": "3v3-before-1v2 / 1v2-before-3v3-off",
        },
        "eeprom": {
            "type": "AT24C16",
            "size_bytes": 2048,
            "page_size_bytes": 16,
            "wp_gpio": 16,
            "scl_gpio": 47,
            "sda_gpio": 48,
            "scl_level": 1,
            "sda_level": 1,
            "write_protected": True,
            "probe_valid": MockState.ms2109_probe_valid,
            "address_mask": MockState.ms2109_probe_mask,
            "all_blocks_detected": (
                MockState.ms2109_probe_valid
                and MockState.ms2109_probe_mask == 0xFF
            ),
            "read_supported": True,
            "program_supported": True,
            "readback_verification": True,
            "last_programmed_bytes": MockState.ms2109_last_programmed_bytes,
            "last_crc32": f"{crc32:08x}",
            "last_verified": MockState.ms2109_last_verified,
        },
    }


def _storage_directories() -> set[str]:
    directories = {"", "AGENT", "ASSETS", "LOGS", "SNAPSHOTS", "EXPORTS", "MCP", "OTA"}
    for path in MockState.storage_files:
        parts = path.split("/")[:-1]
        for index in range(1, len(parts) + 1):
            directories.add("/".join(parts[:index]))
    return directories


def _storage_list_payload(path: str):
    requested = posixpath.normpath("/" + path.strip("/")).lstrip("/")
    if requested == ".":
        requested = ""
    directories = _storage_directories()
    if requested not in directories:
        return None
    prefix = requested + "/" if requested else ""
    entries: dict[str, dict] = {}
    for directory in directories:
        if not directory.startswith(prefix) or directory == requested:
            continue
        remainder = directory[len(prefix):]
        if remainder and "/" not in remainder:
            entries[remainder] = {"name": remainder, "path": prefix + remainder, "type": "directory", "size": 0, "mtime": 0}
    for file_path, content in MockState.storage_files.items():
        if not file_path.startswith(prefix):
            continue
        remainder = file_path[len(prefix):]
        if remainder and "/" not in remainder:
            entries[remainder] = {"name": remainder, "path": file_path, "type": "file", "size": len(content), "mtime": 0}
    parent = requested.rsplit("/", 1)[0] if "/" in requested else ""
    items = sorted(entries.values(), key=lambda item: (item["type"] != "directory", item["name"].lower()))
    return {"ok": True, "root": "/sdcard/EA", "path": requested, "display_path": "/EA" + ("/" + requested if requested else ""), "parent": parent, "entries": items, "count": len(items), "max_items": 96, "truncated": False}


def _agent_request_queue_payload() -> dict:
    request = json.loads(json.dumps(MockState.agent_request))
    if request and request.get("status") == "waiting_user":
        now_ms = int(time.monotonic() * 1000)
        request["remaining_ms"] = max(
            0, int(request.get("expires_at_ms", now_ms)) - now_ms
        )
        if request["remaining_ms"] == 0:
            request["status"] = "expired"
            request["decision_source"] = "ttl"
            request["error"] = "request expired"
            MockState.agent_request = json.loads(json.dumps(request))
            run = MockState.agent_run
            if (run.get("job_id") == request.get("run_id") and
                    run.get("state") == "waiting_request"):
                run.update({
                    "state": "failed",
                    "running": False,
                    "waiting_request": False,
                    "busy": False,
                    "updated_ms": now_ms,
                    "finished_ms": now_ms,
                    "stage": "request expired",
                    "detail": "The exact request expired before a decision",
                    "error": "request expired",
                })
                _append_agent_run_event(
                    "agent", "failed",
                    f"request expired · {request.get('request_id', '')}",
                    request.get("request_id", ""),
                )
    items = [request] if request else []
    return {
        "schema_version": "exoanchor.agent_request_queue.v1",
        "items": items,
        "pending_count": sum(
            1 for item in items if item.get("status") == "waiting_user"
        ),
        "owner": "device",
    }


def _mock_agent_request(run: dict) -> dict:
    MockState.agent_request_seq += 1
    now_ms = int(time.monotonic() * 1000)
    user_message = str(run.get("user_message") or "")
    asks_answer = user_message.lower().startswith("ask:")
    question = user_message[4:].strip() or "请提供继续测试所需的信息。"
    parameters = (
        {"tool": "ask_user", "args": {"question": question}}
        if asks_answer else
        {
            "tool": "ssh_exec",
            "args": {"command": "uname -a", "timeout_ms": 30000},
        }
    )
    compact = json.dumps(parameters, ensure_ascii=False, separators=(",", ":"))
    return {
        "schema_version": "exoanchor.agent_request.v1",
        "request_id": f"req-mock-{MockState.agent_request_seq:04d}",
        "run_id": run["job_id"],
        "requester": "agent",
        "kind": "context" if asks_answer else "action",
        "resource": "ask_user" if asks_answer else "ssh_exec",
        "reason": question if asks_answer else
        "Agent needs this exact bounded command to continue the mock task.",
        "risk": "medium" if asks_answer else "high",
        "sensitivity": "requested_source" if asks_answer else
        "action_parameters",
        "grant_scope": "once",
        "status": "waiting_user",
        "lease_required": False,
        "created_ms": now_ms,
        "expires_at_ms": now_ms + 120000,
        "remaining_ms": 120000,
        "argument_hash": hashlib.sha256(compact.encode("utf-8")).hexdigest(),
        "expected_effect": (
            "Provide one bounded human answer to the current task step"
            if asks_answer else "Execute one exact bounded SSH command"
        ),
        "verification": (
            "Return the answer only to the same Run"
            if asks_answer else "Capture exit status and command output"
        ),
        "parameters": parameters,
    }


def _agent_run_payload() -> dict:
    run = MockState.agent_run
    if (run["running"] and
            time.monotonic() - MockState.agent_run_started_at >=
            AGENT_RUN_DELAY_SECONDS):
        elapsed = int((time.monotonic() - MockState.agent_run_started_at) * 1000)
        dry_run = bool(run["dry_run"])
        phase = "planned" if dry_run else "input_sent"
        _append_agent_run_event(
            "ssh", phase,
            "tool[1] ssh_exec dry-run $ uname -a" if dry_run else
            "tool[1] ssh_exec $ uname -a",
            "ssh-1",
        )
        _append_agent_run_event(
            "hid", phase, "hid[1] absclick · x=19660 y=13106 button=0",
            "hid-2",
        )
        if not dry_run:
            _append_agent_run_event(
                "ssh", "output_received",
                "tool[1] output · Linux exoanchor-mock", "ssh-1",
            )
            _append_agent_run_event(
                "ssh", "succeeded", "tool[1] ssh_exec ok · exit=0",
                "ssh-1",
            )
            _append_agent_run_event(
                "hid", "succeeded",
                "hid[1] absclick · x=19660 y=13106 button=0 · ok", "hid-2",
            )
        run.update({
            "state": "done",
            "running": False,
            "paused": False,
            "busy": False,
            "pause_requested": False,
            "waiting_request": False,
            "updated_ms": run["started_ms"] + elapsed,
            "finished_ms": run["started_ms"] + elapsed,
            "stage": "completed",
            "detail": "Local mock Agent run completed",
            "event_schema_version": 1,
            "event_dropped": 0,
            "result": {
                "ok": True,
                "session_id": run["session_id"],
                "profile": run["profile"],
                "model": run["model"],
                "dry_run": run["dry_run"],
                "message": "Local mock Agent received the request and completed the UI contract test.",
                "trace": ["Validated the Agent run lifecycle"],
                "tool_calls": [],
                "actions": [],
                "tool_results": [],
                "results": [],
                "history_saved": True,
            },
        })
        if MockState.agent_request.get("run_id") == run.get("job_id"):
            if MockState.agent_request.get("status") == "granted":
                MockState.agent_request["status"] = "completed"
                MockState.agent_request["completed_ms"] = (
                    run["started_ms"] + elapsed
                )
        history = MockState.agent_history.setdefault(run["session_id"], [])
        if not any(item.get("mock_job_id") == run["job_id"] for item in history):
            history.extend([
                {"role": "user", "content": run.get("user_message", ""), "session_id": run["session_id"], "mock_job_id": run["job_id"]},
                {"role": "assistant", "content": run["result"]["message"], "session_id": run["session_id"], "mock_job_id": run["job_id"]},
            ])
            MockState.agent_sessions[run["session_id"]]["message_count"] = len(history)
    payload = json.loads(json.dumps(run))
    if run["started_ms"]:
        end_ms = run["finished_ms"] or int(time.monotonic() * 1000)
        payload["elapsed_ms"] = max(0, end_ms - run["started_ms"])
    else:
        payload["elapsed_ms"] = 0
    payload["turn"] = {
        "thread_id": payload.get("thread_id", payload.get("session_id", "")),
        "turn_id": payload.get("turn_id", ""),
        "run_id": payload.get("run_id", payload.get("job_id", "")),
        "goal": payload.get("goal", ""),
        "status": payload.get("state", "idle"),
        "intent_revision": payload.get("intent_revision", 0),
    }
    payload["plan"] = {
        "version": payload.get("plan_version", 0),
        "steps": [{
            "step_id": "step-runtime",
            "title": payload.get("stage", "idle"),
            "status": (
                "completed" if payload.get("state") == "done" else
                "failed" if payload.get("state") in ("failed", "aborted") else
                "blocked" if payload.get("state") == "waiting_request" else
                "in_progress" if payload.get("busy") else "pending"
            ),
            "completion_condition": "tool results and final verification",
        }],
    }
    if MockState.agent_request.get("run_id") == payload.get("job_id"):
        payload["request"] = json.loads(json.dumps(MockState.agent_request))
    return payload


def _agent_run_request_fingerprint(payload: dict) -> str:
    """Canonicalize every request-controlled executor input for mock dedupe."""
    authority_policy = str(payload.get("authority_mode") or "") == "policy"
    canonical = {
        "thread_id": str(
            payload.get("thread_id") or payload.get("conversation_id") or
            payload.get("session_id") or "default"
        ),
        "message": str(
            payload.get("message") or payload.get("prompt") or
            payload.get("input") or ""
        ),
        "completion_criteria": str(
            payload.get("completion_criteria") or payload.get("done_when") or
            "Produce one bounded final response artifact."
        ),
        "profile": str(payload.get("profile") or payload.get("profile_id") or ""),
        "model": str(payload.get("model") or payload.get("model_name") or ""),
        "dry_run": False if authority_policy else bool(payload.get("dry_run", True)),
        "include_screenshot": bool(payload.get("include_screenshot", False)),
        "include_web_search": bool(
            payload.get("include_web_search", payload.get("allow_web_search", False))
        ),
        "page_context": payload.get("page_context")
        if isinstance(payload.get("page_context"), dict) else None,
    }
    return json.dumps(canonical, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"))


def _agent_event(seq: int, timestamp_ms: int, channel: str, phase: str,
                 text: str, action_id: str = "", redacted: bool = False,
                 run_id: str = "") -> dict:
    legacy_kind = {
        "input_sent": "cmd",
        "output_received": "progress",
        "verifying": "progress",
        "succeeded": "ok",
        "failed": "bad",
        "cancelled": "warn",
    }.get(phase, "info")
    return {
        "schema_version": 1,
        "seq": seq,
        "ms": timestamp_ms,
        "run_id": run_id or MockState.agent_run.get("job_id", ""),
        "action_id": action_id or f"a{seq}",
        "actor": "agent",
        "channel": channel,
        "phase": phase,
        "redacted": redacted,
        "background": True,
        "kind": legacy_kind,
        "text": text,
    }


def _append_agent_run_event(channel: str, phase: str, text: str,
                            action_id: str = "") -> None:
    run = MockState.agent_run
    seq = int(run.get("event_seq", 0)) + 1
    timestamp_ms = int(time.monotonic() * 1000)
    run.setdefault("events", []).append(
        _agent_event(seq, timestamp_ms, channel, phase, text, action_id)
    )
    run["event_seq"] = seq


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
        elif path in ("/agent", "/terminal"):
            rel = path.lstrip("/") + ".html"
        elif path == "/favicon.svg":
            rel = "assets/favicon.svg"
        elif path == "/skills":
            self.send_response(HTTPStatus.FOUND)
            self.send_header("Location", "/settings#agent")
            self.end_headers()
            return
        elif path == "/assets/xterm/xterm.css":
            rel = "vendor/xterm/xterm.css"
        elif path == "/assets/xterm/xterm.js":
            rel = "vendor/xterm/xterm.js"
        elif path == "/assets/xterm/addon-fit.js":
            rel = "vendor/xterm/addon-fit.js"
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
        body = target.read_bytes()
        if path == "/assets/ui-core.js" and H264_FIXTURES:
            profile = {
                "profile": "dev-h264-browser-fixture",
                "version": "0.87.5-dev",
                "embeddedAgent": True,
                "uartTerminal": True,
                "h264Video": True,
                "externalMcp": True,
                "h264WsPort": MockState.h264_ws_port,
                "h264ForceMse": MockState.h264_force_mse,
            }
            body = (
                "window.ExoAnchorBuild="
                + json.dumps(profile, separators=(",", ":"))
                + ";\n"
            ).encode("utf-8") + body
        self._send_bytes(body, content_type)

    def _handle_websocket(self, echo: bool = False) -> None:
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
                if echo:
                    message = b"\r\n[local mock] SSH WebSocket contract connected.\r\n$ "
                    if len(message) < 126:
                        frame = bytes((0x81, len(message))) + message
                    else:
                        frame = bytes((0x81, 126)) + struct.pack("!H", len(message)) + message
                    self.wfile.write(frame)
                    self.wfile.flush()

    def _send_websocket_binary(self, payload: bytes) -> None:
        length = len(payload)
        if length < 126:
            header = bytes((0x82, length))
        elif length <= 0xFFFF:
            header = bytes((0x82, 126)) + struct.pack("!H", length)
        else:
            header = bytes((0x82, 127)) + struct.pack("!Q", length)
        self.wfile.write(header)
        self.wfile.write(payload)
        self.wfile.flush()

    def _handle_h264_websocket(self, stream_id: int) -> None:
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self.send_error(HTTPStatus.BAD_REQUEST)
            return
        fixture = H264_FIXTURES.get((MockState.width, MockState.height))
        if not fixture or not stream_id or stream_id != MockState.video_stream_id:
            self.send_error(HTTPStatus.CONFLICT, "no matching H.264 fixture or lease")
            return
        accept = base64.b64encode(
            hashlib.sha1((key + MAGIC_WS_GUID).encode("ascii")).digest()
        ).decode("ascii")
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()

        fps = float(fixture["fps"])
        period = 1.0 / fps
        units: list[bytes] = fixture["units"]
        sequence = 0
        started = time.monotonic()
        next_frame = started
        session_generation = MockState.begin_h264_session(started)
        try:
            while stream_id == MockState.video_stream_id:
                unit = units[sequence % len(units)]
                flags = 1 if 5 in _fixture_nal_types(unit) else 0
                pts_ms = int(sequence * 1000.0 / fps) & 0xFFFFFFFF
                wire = struct.pack(
                    "<4sBBHHHIII",
                    b"EAH1", 1, flags, 24,
                    int(fixture["width"]), int(fixture["height"]),
                    sequence & 0xFFFFFFFF, pts_ms, len(unit),
                ) + unit
                self._send_websocket_binary(wire)
                MockState.h264_frames_sent += 1
                MockState.h264_bytes_sent += len(unit)
                sequence += 1
                next_frame += period
                delay = next_frame - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                elif delay < -period * 2:
                    next_frame = time.monotonic()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            MockState.end_h264_session(session_generation)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path

        if path in ("/api/status", "/api/video/status"):
            payload = _status_payload()
            if path == "/api/video/status":
                payload = payload["video"]
            self._send_json(payload)
            return
        if path == "/api/automation/status":
            operations = [dict(item) for item in MockState.automation_operations]
            self._send_json({
                "schema": "exoanchor.automation.v1",
                "operations": operations,
                "active_count": len(operations),
                "access_mode": MockState.access_mode,
                "manual_takeover_available": any(
                    item.get("cancellable", True) for item in operations
                ),
            })
            return
        if path == "/api/v1/network/status":
            self._send_json(_network_payload())
            return
        if path == "/api/hid/status":
            self._send_json(_hid_payload())
            return
        if path == "/api/control/lease":
            self._send_json(_control_lease_payload())
            return
        if path in ("/api/power/status", "/api/settings/gpio-map"):
            self._send_json(_power_payload())
            return
        if path == "/api/ms2109/status":
            self._send_json(_ms2109_payload())
            return
        if path == "/api/ms2109/eeprom/read":
            image = MockState.ms2109_eeprom_image
            self._send_json({
                "type": "AT24C16",
                "size_bytes": len(image),
                "crc32": f"{zlib.crc32(image) & 0xFFFFFFFF:08x}",
                "encoding": "base64",
                "image_b64": base64.b64encode(image).decode("ascii"),
            })
            return
        if path == "/api/ws/hid":
            self._handle_websocket()
            return
        if path == "/api/ws/ssh":
            self._handle_websocket(echo=True)
            return
        if path == "/api/ws/uart":
            self._handle_websocket(echo=True)
            return
        if path == "/api/ws/video/h264":
            query = parse_qs(parsed.query)
            try:
                stream_id = int(query.get("stream_id", ["0"])[0]) & 0xFFFFFFFF
            except ValueError:
                stream_id = 0
            self._handle_h264_websocket(stream_id)
            return
        if path == "/api/uart/status":
            self._send_json(_uart_payload())
            return
        if path == "/api/uart/read":
            self._send_json({"bytes": 0, "data": "", "base64": "", "empty": True})
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
                "min_length": 6,
                "max_length": 64,
            })
            return
        if path == "/api/system/info":
            self._send_json(_system_info_payload())
            return
        if path == "/api/system/logs":
            self._send_json(_logs_payload())
            return
        if path == "/api/system/logs/download":
            lines = [f'{item["time"]} {item["level"]} {item["message"]}' for item in _logs_payload()["logs"]]
            self._send_bytes(("\n".join(lines) + "\n").encode("utf-8"), "text/plain; charset=utf-8")
            return
        if path == "/api/settings/session":
            self._send_json({"auto_logout_enabled": MockState.auto_logout_enabled, "auto_logout_minutes": MockState.auto_logout_minutes, "min_minutes": 1, "max_minutes": 1440, "stored_in_nvs": True})
            return
        if path == "/api/settings/device":
            self._send_json({"label": MockState.device_label, "device_label": MockState.device_label, "default_label": "ExoAnchor", "max_length": 48, "agent_name": MockState.agent_display_name, "default_agent_name": "Agent", "agent_name_max_length": 32, "stored_in_nvs": True})
            return
        if path == "/api/settings/target-profile":
            response = MockState.target_profile.copy()
            response.update({
                "device_types": ["unknown", "desktop", "workstation", "server", "laptop", "embedded", "virtual_machine", "network_appliance", "other"],
                "operating_systems": ["unknown", "linux", "windows", "macos", "bsd", "appliance", "other"],
                "environments": ["unknown", "development", "testing", "staging", "production", "lab", "home", "other"],
                "agent_context_enabled": True,
                "agent_context_scope": "untrusted metadata only; never grants permission or authority",
            })
            self._send_json(response)
            return
        if path == "/api/settings/ssh-target":
            self._send_json(_ssh_target_payload())
            return
        if path == "/api/settings/console-credentials":
            self._send_json(_console_credentials_payload())
            return
        if path == "/api/settings/ssh-key":
            self._send_json(_ssh_key_payload())
            return
        if path == "/api/settings/mcp":
            self._send_json({"enabled": MockState.mcp_enabled, "default_enabled": False, "control_owner": "mcp", "client_header": "X-ExoAnchor-Client: exoanchor-mcp", "scope": "external_mcp_controller"})
            return
        if path == "/api/settings/access-mode":
            self._send_json({
                "schema": "exoanchor.access_mode.v1",
                "mode": MockState.access_mode,
                "stored_in_nvs": True,
                "secret_export_allowed": False,
                "modes": [
                    {"value": "manual", "label": "手动授权", "description": "每个会产生副作用的动作都等待浏览器确认"},
                    {"value": "assisted", "label": "替我审批", "description": "自动批准低、中风险动作，高风险动作仍需确认"},
                    {"value": "full", "label": "完全访问", "description": "在既定策略和控制租约内自动批准全部风险等级"},
                ],
            })
            return
        if path == "/api/settings/product-features":
            self._send_json(MockState.product_features.copy())
            return
        if path == "/api/settings/agent-api":
            self._send_json(_agent_api_payload())
            return
        if path == "/api/settings/agent-api/models":
            query = parse_qs(parsed.query)
            profile_id = query.get("profile", [MockState.agent_active_profile])[0]
            profile = next((item for item in MockState.agent_profiles if item["id"] == profile_id), MockState.agent_profiles[0])
            models = [profile["model"]]
            if profile["provider"] == "openai":
                models += ["gpt-4o-mini", "gpt-4.1"]
            elif profile["provider"] == "deepseek":
                models += ["deepseek-chat", "deepseek-reasoner"]
            elif profile["provider"] == "qwen":
                models += ["qwen-plus", "qwen-max"]
            self._send_json({"profile": profile["id"], "provider": profile["provider"], "models": models, "selected_model": profile["model"], "count": len(models)})
            return
        if path == "/api/settings/agent-prompt":
            self._send_json({"system_prompt": MockState.agent_prompt, "default_system_prompt": "You are the ExoAnchor local mock Agent.", "using_default": MockState.agent_prompt_default, "stored_in_nvs": not MockState.agent_prompt_default, "max_length": 2048})
            return
        if path == "/api/settings/agent-tools":
            self._send_json(_agent_tools_payload())
            return
        if path == "/api/capabilities":
            self._send_json(_capabilities_payload())
            return
        if path == "/api/host/display":
            self._send_json(json.loads(json.dumps(MockState.host_display)))
            return
        if path == "/api/host/boot-key":
            self._send_json(json.loads(json.dumps(MockState.boot_key)))
            return
        if path == "/api/storage/list":
            query = parse_qs(parsed.query)
            payload = _storage_list_payload(query.get("path", [""])[0])
            if payload is None:
                self._send_json({"error": "storage directory not found"}, 404)
            else:
                self._send_json(payload)
            return
        if path in ("/api/storage/download", "/api/storage/view"):
            query = parse_qs(parsed.query)
            storage_path = posixpath.normpath("/" + query.get("path", [""])[0].strip("/")).lstrip("/")
            content = MockState.storage_files.get(storage_path)
            if content is None:
                self._send_json({"error": "storage file not found"}, 404)
            elif path == "/api/storage/download":
                self._send_bytes(content, "application/octet-stream")
            else:
                text = content.decode("utf-8", errors="replace")
                self._send_json({"ok": True, "name": storage_path.rsplit("/", 1)[-1], "path": storage_path, "mode": "text", "preview_bytes": len(content), "size": len(content), "truncated": False, "content": text})
            return
        if path == "/api/agent/sessions":
            sessions = sorted(MockState.agent_sessions.values(), key=lambda item: item["updated_ms"], reverse=True)
            self._send_json({"supported": True, "sessions": sessions, "session_count": len(sessions), "empty_allowed": True, "default_session_id": "default"})
            return
        if path == "/api/agent/history":
            query = parse_qs(parsed.query)
            session_id = query.get("session_id", query.get("session", ["default"]))[0]
            records = MockState.agent_history.get(session_id, [])
            self._send_json({"supported": True, "storage": "tf_card", "tf_mounted": True, "session_id": session_id, "records": records, "returned": len(records), "record_count": len(records)})
            return
        if path == "/api/agent/memory":
            records = json.loads(json.dumps(MockState.agent_memory))
            self._send_json({"supported": True, "storage": "tf_card", "tf_mounted": True, "records": records, "returned": len(records), "record_count": len(records), "used_bytes": len(json.dumps(records))})
            return
        if path == "/api/agent/requests":
            self._send_json(_agent_request_queue_payload())
            return
        if path == "/api/agent/context-catalog":
            self._send_json({
                "schema_version": "exoanchor.context_catalog.v1",
                "context_catalog": [
                    {"source": "device.status", "sensitivity": "low", "requestable": True, "browser_provider_required": False, "max_bytes": 8192},
                    {"source": "kvm.snapshot", "sensitivity": "screen", "requestable": True, "browser_provider_required": False, "max_bytes": 262144},
                    {"source": "terminal.selection", "sensitivity": "medium", "requestable": True, "browser_provider_required": True, "max_bytes": 8192},
                    {"source": "settings.unsaved_diff", "sensitivity": "medium", "requestable": True, "browser_provider_required": True, "max_bytes": 8192},
                    {"source": "page.summary", "sensitivity": "low", "requestable": True, "browser_provider_required": True, "max_bytes": 4096},
                ],
            })
            return
        if path == "/api/agent/run/status":
            query = parse_qs(parsed.query, keep_blank_values=True)
            if "run_id" in query:
                requested_run = str(query.get("run_id", [""])[0])
                if not requested_run or len(requested_run) > 48:
                    self._send_json({"error": "valid run_id required"}, 400)
                    return
                if requested_run != MockState.agent_run.get("job_id"):
                    self._send_json({
                        "found": False,
                        "run_id": requested_run,
                        "reason": "run_id_mismatch",
                    }, 404)
                    return
                response = _agent_run_payload()
                response["found"] = True
                self._send_json(response)
                return
            self._send_json(_agent_run_payload())
            return
        if path == "/api/agent/run/events":
            run = _agent_run_payload()
            query = parse_qs(parsed.query)
            requested_job = query.get("job_id", [""])[0]
            if requested_job and requested_job != run.get("job_id"):
                self._send_json({"error": "requested run is no longer retained"}, 409)
                return
            try:
                after_seq = int(query.get("after_seq", query.get("since_seq", ["0"]))[0])
            except (TypeError, ValueError):
                self._send_json({"error": "invalid after_seq"}, 400)
                return
            available_events = [
                item for item in run.get("events", [])
                if int(item.get("seq", 0)) > after_seq
            ]
            events = available_events
            truncated = False
            page_limit = MockState.agent_run_event_page_limit
            if isinstance(page_limit, int) and page_limit >= 0:
                events = available_events[:page_limit]
                truncated = len(events) < len(available_events)
            latest_seq = int(run.get("event_seq", 0))
            next_after_seq = after_seq
            if events:
                next_after_seq = int(events[-1].get("seq", after_seq))
            self._send_json({
                "schema_version": 1,
                "job_id": run.get("job_id", ""),
                "state": run.get("state", "idle"),
                "after_seq": after_seq,
                "latest_seq": latest_seq,
                "next_after_seq": next_after_seq,
                "truncated": truncated,
                "history_lost": False,
                "persisted": False,
                "replay_scope": "current_run_ram_ring_no_tf_scan",
                "events": events,
            })
            return
        if path == "/api/ssh/status":
            self._send_json({"supported": True, "pty": True, "private_key_configured": MockState.ssh_private_key_configured, "automation": {"available": True, "channel": "ssh", "active": False, "manual_input_enabled": True, "cancel_requested": False, "can_stop": False, "generation": 0, "started_ms": 0, "actor": "none", "operation_id": "", "run_id": "", "label": ""}})
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

        if path == "/api/ms2109/power":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            action = str(payload.get("action") or "").lower()
            if action not in ("on", "off", "cycle"):
                self._send_json({"error": "action must be on, off, or cycle"}, 400)
                return
            MockState.ms2109_power_on = action != "off"
            MockState.ms2109_operation_count += 1
            MockState.ms2109_last_result = "ESP_OK"
            self._send_json(_ms2109_payload())
            return
        if path == "/api/ms2109/eeprom/probe":
            MockState.ms2109_probe_valid = True
            MockState.ms2109_probe_mask = 0xFF
            MockState.ms2109_operation_count += 1
            MockState.ms2109_last_result = "ESP_OK"
            self._send_json(_ms2109_payload())
            return
        if path == "/api/ms2109/eeprom/program":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                encoded = str(payload.get("image_b64") or "")
                image = base64.b64decode(encoded, validate=True)
            except (json.JSONDecodeError, ValueError):
                self._send_json({"error": "invalid EEPROM image"}, 400)
                return
            if payload.get("confirm") != "PROGRAM MS2109 EEPROM":
                self._send_json({"error": "explicit EEPROM programming confirmation required"}, 400)
                return
            if len(image) != 2048:
                self._send_json({"error": "EEPROM image must decode to exactly 2048 bytes"}, 400)
                return
            MockState.ms2109_eeprom_image = image
            MockState.ms2109_last_programmed_bytes = len(image)
            MockState.ms2109_last_verified = True
            MockState.ms2109_operation_count += 1
            MockState.ms2109_last_result = "ESP_OK"
            crc32 = zlib.crc32(image) & 0xFFFFFFFF
            self._send_json({
                "programmed": True,
                "verified": True,
                "size_bytes": len(image),
                "crc32": f"{crc32:08x}",
                "status": _ms2109_payload(),
            })
            return

        if path == "/api/uart/baud":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                baud_rate = int(payload.get("baud_rate"))
            except (json.JSONDecodeError, TypeError, ValueError):
                self._send_json({"error": "baud_rate number required"}, 400)
                return
            if baud_rate not in (115200, 9600):
                self._send_json({"error": "unsupported UART baud mode"}, 400)
                return
            MockState.uart_baud_rate = baud_rate
            response = _uart_payload()
            response["ok"] = True
            self._send_json(response)
            return

        if path == "/api/uart/authenticate":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            expected_prompt = str(payload.get("expected_prompt") or "")
            credential_ref = str(payload.get("credential_ref") or "auto://sudo")
            if (not expected_prompt or "password" not in expected_prompt.lower()
                    or expected_prompt[-1:].isspace()):
                self._send_json({"error": "exact password expected_prompt required"}, 400)
                return
            if credential_ref not in (
                "auto://sudo", "ssh-sudo://default", "console://default",
            ):
                self._send_json({"error": "unsupported credential_ref"}, 400)
                return
            source = (
                "ssh_sudo"
                if credential_ref == "auto://sudo"
                else ("ssh_sudo" if credential_ref == "ssh-sudo://default"
                      else "console")
            )
            self._send_json({
                "ok": True,
                "submitted": True,
                "credential_ref": credential_ref,
                "credential_source": source,
                "credential_redacted": True,
                "cursor_start": 0,
                "cursor_end": 0,
                "bytes": 0,
                "data": "",
                "base64": "",
                "empty": True,
            })
            return

        if path == "/api/terminal/control/stop":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                channel = str(payload.get("channel") or "")
                generation = int(payload.get("generation") or 0)
            except (json.JSONDecodeError, TypeError, ValueError):
                self._send_json({"error": "channel and generation required"}, 400)
                return
            if channel not in ("ssh", "uart") or generation <= 0:
                self._send_json({"error": "channel and generation required"}, 400)
                return
            self._send_json({"accepted": True, "automation": {"available": True, "channel": channel, "active": False, "manual_input_enabled": True, "cancel_requested": True, "can_stop": False, "generation": generation, "actor": "none", "operation_id": "", "run_id": "", "label": ""}})
            return

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
        if path == "/api/host/display":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            action = str(payload.get("action") or "")
            now_ms = int(time.monotonic() * 1000)
            if action == "plan":
                connector = str(payload.get("connector") or "")
                try:
                    width = int(payload.get("width"))
                    height = int(payload.get("height"))
                    refresh_hz = float(payload.get("refresh_hz"))
                except (TypeError, ValueError):
                    self._send_json({"error": "invalid display plan"}, 400)
                    return
                safe_connector = (
                    connector and len(connector) <= 31 and
                    connector[0].isalnum() and
                    all(ch.isalnum() or ch in "-_." for ch in connector)
                )
                if (not safe_connector or width < 320 or width > 7680 or
                        height < 200 or height > 4320 or
                        refresh_hz != int(refresh_hz) or
                        refresh_hz < 10 or refresh_hz > 240):
                    self._send_json({"error": "invalid display plan"}, 400)
                    return
                MockState.host_display_seq += 1
                plan_id = f"hd-mock-{MockState.host_display_seq:04d}"
                MockState.host_display = {
                    "schema_version": "exoanchor.host_display.plan.v1",
                    "supported": True,
                    "reference_target": "ubuntu-grub-drm",
                    "state": "planned",
                    "plan_id": plan_id,
                    "approval_required": True,
                    "approved": False,
                    "reboot_required": bool(payload.get("persistent", False)),
                    "temporary_apply_supported": False,
                    "persistent_apply_supported": True,
                    "created_ms": now_ms,
                    "updated_ms": now_ms,
                    "target": {
                        "connector": connector,
                        "width": width,
                        "height": height,
                        "refresh_millihz": int(refresh_hz * 1000),
                        "refresh_hz": int(refresh_hz),
                        "persistent": bool(payload.get("persistent", False)),
                    },
                }
            elif action == "approve":
                if (MockState.host_display["state"] != "planned" or
                        payload.get("plan_id") !=
                        MockState.host_display["plan_id"] or
                        payload.get("approved") is not True):
                    self._send_json({"error": "display plan state conflict"}, 409)
                    return
                MockState.host_display.update({
                    "state": "approved",
                    "approved": True,
                    "approved_by_session": "local-mock-session",
                    "updated_ms": now_ms,
                })
            elif action == "cancel":
                if (MockState.host_display["state"] not in
                        ("planned", "approved") or
                        payload.get("plan_id") !=
                        MockState.host_display["plan_id"]):
                    self._send_json({"error": "display plan state conflict"}, 409)
                    return
                MockState.host_display.update({
                    "state": "cancelled",
                    "approved": False,
                    "updated_ms": now_ms,
                })
            else:
                self._send_json(
                    {"error": "action must be plan, approve, or cancel"}, 400
                )
                return
            self._send_json(json.loads(json.dumps(MockState.host_display)))
            return
        if path == "/api/host/boot-key":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            action = str(payload.get("action") or "")
            now_ms = int(time.monotonic() * 1000)
            if action == "plan":
                profile_id = str(payload.get("profile_id") or "")
                target = str(payload.get("target") or "")
                trigger = str(payload.get("trigger") or "")
                key_code = str(payload.get("key_code") or "")
                try:
                    start_delay = int(payload.get("start_delay_ms"))
                    interval = int(payload.get("interval_ms"))
                    attempts = int(payload.get("max_attempts"))
                    timeout = int(payload.get("total_timeout_ms"))
                except (TypeError, ValueError):
                    self._send_json({"error": "invalid boot-key plan"}, 400)
                    return
                allowed_keys = {
                    "Delete", "Escape", "F1", "F2", "F3", "F4", "F5",
                    "F6", "F7", "F8", "F9", "F10", "F11", "F12",
                }
                safe_profile = (
                    profile_id and len(profile_id) <= 31 and
                    profile_id[0].isalnum() and
                    all(ch.isalnum() or ch in "-_." for ch in profile_id)
                )
                last_due = start_delay + max(0, attempts - 1) * interval
                if (not safe_profile or target not in ("bios_setup", "boot_menu")
                        or trigger not in ("none", "reset")
                        or key_code not in allowed_keys
                        or start_delay < 0 or start_delay > 10000
                        or interval < 100 or interval > 5000
                        or attempts < 1 or attempts > 20
                        or timeout < 1000 or timeout > 60000
                        or timeout < last_due + 100):
                    self._send_json({"error": "invalid boot-key plan"}, 400)
                    return
                MockState.boot_key_seq += 1
                plan_id = f"bk-mock-{MockState.boot_key_seq:04d}"
                MockState.boot_key = {
                    "schema_version": "exoanchor.boot_key.plan.v1",
                    "supported": True,
                    "state": "planned",
                    "plan_id": plan_id,
                    "approval_required": True,
                    "approved": False,
                    "cancel_requested": False,
                    "reset_triggered": False,
                    "attempts_sent": 0,
                    "created_ms": now_ms,
                    "started_ms": 0,
                    "updated_ms": now_ms,
                    "finished_ms": 0,
                    "verification_mode": "human_kvm",
                    "agent_navigation_supported": False,
                    "profile": {
                        "profile_id": profile_id,
                        "target": target,
                        "trigger": trigger,
                        "key_code": key_code,
                        "start_delay_ms": start_delay,
                        "interval_ms": interval,
                        "max_attempts": attempts,
                        "total_timeout_ms": timeout,
                    },
                }
            elif action == "approve":
                if (MockState.boot_key["state"] != "planned" or
                        payload.get("plan_id") != MockState.boot_key["plan_id"]
                        or payload.get("approved") is not True):
                    self._send_json({"error": "boot-key plan state conflict"}, 409)
                    return
                MockState.boot_key.update({
                    "state": "approved",
                    "approved": True,
                    "approved_by_session": "local-mock-session",
                    "updated_ms": now_ms,
                })
            elif action == "start":
                if (MockState.boot_key["state"] != "approved" or
                        payload.get("plan_id") != MockState.boot_key["plan_id"]
                        or payload.get("start") is not True):
                    self._send_json({"error": "boot-key plan state conflict"}, 409)
                    return
                MockState.boot_key.update({
                    "state": "awaiting_verification",
                    "started_ms": now_ms,
                    "updated_ms": now_ms,
                    "attempts_sent":
                        MockState.boot_key["profile"]["max_attempts"],
                    "reset_triggered":
                        MockState.boot_key["profile"]["trigger"] == "reset",
                })
            elif action == "cancel":
                if payload.get("plan_id") != MockState.boot_key["plan_id"]:
                    self._send_json({"error": "boot-key plan state conflict"}, 409)
                    return
                MockState.boot_key.update({
                    "state": "cancelled",
                    "approved": False,
                    "updated_ms": now_ms,
                    "finished_ms": now_ms,
                })
            elif action == "confirm":
                if (MockState.boot_key["state"] != "awaiting_verification"
                        or payload.get("plan_id") != MockState.boot_key["plan_id"]
                        or payload.get("confirmed") is not True
                        or payload.get("evidence_source") != "human_kvm"):
                    self._send_json({"error": "boot-key plan state conflict"}, 409)
                    return
                MockState.boot_key.update({
                    "state": "verified",
                    "verified_by_session": "local-mock-session",
                    "evidence": str(payload.get("evidence") or
                                    "human KVM confirmed"),
                    "updated_ms": now_ms,
                    "finished_ms": now_ms,
                })
            else:
                self._send_json({"error": "invalid boot-key action"}, 400)
                return
            self._send_json(json.loads(json.dumps(MockState.boot_key)))
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
            if not isinstance(new_password, str) or len(new_password) < 6 or len(new_password) > 64 or any(ord(ch) < 33 or ord(ch) > 126 for ch in new_password):
                self._send_json({"error": "password must be 6..64 printable ASCII characters"}, 400)
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
        if path == "/api/settings/session":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                minutes = int(payload.get("auto_logout_minutes", 15))
            except (json.JSONDecodeError, TypeError, ValueError):
                self._send_json({"error": "invalid session settings"}, 400)
                return
            if minutes < 1 or minutes > 1440:
                self._send_json({"error": "auto logout minutes out of range"}, 400)
                return
            MockState.auto_logout_enabled = bool(payload.get("auto_logout_enabled", False))
            MockState.auto_logout_minutes = minutes
            self._send_json({"ok": True, "auto_logout_enabled": MockState.auto_logout_enabled, "auto_logout_minutes": minutes, "min_minutes": 1, "max_minutes": 1440, "stored_in_nvs": True})
            return
        if path == "/api/settings/ssh-target":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            if payload.get("clear") is True:
                MockState.ssh_target = {"configured": False, "host": "", "port": 22, "username": "", "auth_method": "auto", "timeout_ms": 30000, "password_configured": False, "sudo_password_configured": False}
            else:
                host = str(payload.get("host") or "").strip()
                username = str(payload.get("username") or "").strip()
                if not host or not username:
                    self._send_json({"error": "host and username required"}, 400)
                    return
                target = MockState.ssh_target.copy()
                target.update({"configured": True, "host": host, "port": int(payload.get("port") or 22), "username": username, "auth_method": str(payload.get("auth_method") or "auto"), "timeout_ms": int(payload.get("timeout_ms") or 30000)})
                if payload.get("clear_password") is True:
                    target["password_configured"] = False
                elif payload.get("password"):
                    target["password_configured"] = True
                if payload.get("clear_sudo_password") is True:
                    target["sudo_password_configured"] = False
                elif payload.get("sudo_password"):
                    target["sudo_password_configured"] = True
                MockState.ssh_target = target
            self._send_json(_ssh_target_payload())
            return
        if path == "/api/settings/console-credentials":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            if payload.get("clear") is True:
                MockState.console_credentials = {
                    "configured": False,
                    "username": "",
                    "password_configured": False,
                }
            else:
                username = str(payload.get("username") or "").strip()
                if not username or len(username) >= 64:
                    self._send_json({"error": "username required"}, 400)
                    return
                credentials = MockState.console_credentials.copy()
                credentials.update({
                    "configured": True,
                    "username": username,
                })
                if payload.get("clear_password") is True:
                    credentials["password_configured"] = False
                elif payload.get("password"):
                    credentials["password_configured"] = True
                MockState.console_credentials = credentials
            response = _console_credentials_payload()
            response["ok"] = True
            self._send_json(response)
            return
        if path == "/api/settings/ssh-key":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            if payload.get("clear") is True:
                MockState.ssh_private_key_configured = False
                MockState.ssh_public_key = ""
                MockState.ssh_passphrase_configured = False
            else:
                private_key = str(payload.get("private_key") or "")
                if "PRIVATE KEY" not in private_key:
                    self._send_json({"error": "private key format invalid"}, 400)
                    return
                MockState.ssh_private_key_configured = True
                MockState.ssh_public_key = str(payload.get("public_key") or "")
                MockState.ssh_passphrase_configured = bool(payload.get("passphrase"))
            self._send_json(_ssh_key_payload())
            return
        if path == "/api/settings/mcp":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            MockState.mcp_enabled = bool(payload.get("enabled", False))
            self._send_json({"ok": True, "enabled": MockState.mcp_enabled, "default_enabled": False, "control_owner": "mcp", "client_header": "X-ExoAnchor-Client: exoanchor-mcp", "scope": "external_mcp_controller"})
            return
        if path == "/api/settings/access-mode":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            mode = str(payload.get("mode") or "")
            if mode not in ("manual", "assisted", "full"):
                self._send_json({"error": "mode must be manual, assisted, or full"}, 400)
                return
            MockState.access_mode = mode
            MockState.automation_operations = []
            self._send_json({
                "ok": True,
                "schema": "exoanchor.access_mode.v1",
                "mode": MockState.access_mode,
                "stored_in_nvs": True,
            })
            return
        if path == "/api/settings/product-features":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            for name in (
                "lan_discovery_enabled", "embedded_agent_enabled",
                "page_context_enabled", "conversation_history_enabled",
                "long_term_memory_enabled",
            ):
                if name in payload:
                    if not isinstance(payload[name], bool):
                        self._send_json({"error": f"{name} must be boolean"}, 400)
                        return
                    MockState.product_features[name] = payload[name]
            self._send_json(MockState.product_features.copy())
            return
        if path == "/api/settings/agent-api":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            profile_id = str(payload.get("profile") or payload.get("profile_id") or MockState.agent_active_profile)
            profile = next((item for item in MockState.agent_profiles if item["id"] == profile_id), None)
            if profile is None:
                self._send_json({"error": "unknown profile"}, 400)
                return
            for key in ("label", "provider", "endpoint", "model"):
                if key in payload:
                    profile[key] = str(payload[key])
            if payload.get("clear_api_key") is True:
                profile["api_key_configured"] = False
            elif payload.get("api_key"):
                profile["api_key_configured"] = True
            MockState.agent_active_profile = profile_id
            self._send_json(_agent_api_payload())
            return
        if path == "/api/settings/agent-prompt":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            if payload.get("reset_default") is True:
                MockState.agent_prompt = "You are the ExoAnchor local mock Agent."
                MockState.agent_prompt_default = True
            else:
                prompt = str(payload.get("system_prompt") or "").strip()
                if not prompt or len(prompt.encode("utf-8")) > 2048:
                    self._send_json({"error": "system prompt must be 1..2048 bytes"}, 400)
                    return
                MockState.agent_prompt = prompt
                MockState.agent_prompt_default = False
            self._send_json({"ok": True, "system_prompt": MockState.agent_prompt, "default_system_prompt": "You are the ExoAnchor local mock Agent.", "using_default": MockState.agent_prompt_default, "stored_in_nvs": not MockState.agent_prompt_default, "max_length": 2048})
            return
        if path == "/api/settings/agent-tools":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            if payload.get("reset_default") is True:
                MockState.agent_tool_mode = "observe"
                MockState.agent_tool_policy = {"version": 2, "default_mode": "observe", "tools": {}}
                MockState.web_search["enabled"] = False
            else:
                if "default_mode" in payload:
                    MockState.agent_tool_mode = str(payload["default_mode"])
                if "policy_json" in payload:
                    try:
                        MockState.agent_tool_policy = json.loads(payload["policy_json"])
                    except (json.JSONDecodeError, TypeError):
                        self._send_json({"error": "policy_json invalid"}, 400)
                        return
                if "skills_json" in payload:
                    try:
                        MockState.agent_skills = json.loads(payload["skills_json"])
                    except (json.JSONDecodeError, TypeError):
                        self._send_json({"error": "skills_json invalid"}, 400)
                        return
                if isinstance(payload.get("web_search"), dict):
                    MockState.web_search.update({key: value for key, value in payload["web_search"].items() if key != "api_key"})
                    if payload["web_search"].get("clear_api_key") is True:
                        MockState.web_search["api_key_configured"] = False
                    elif payload["web_search"].get("api_key"):
                        MockState.web_search["api_key_configured"] = True
            self._send_json(_agent_tools_payload())
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
                fps_x100 = int(payload.get("fps_x100", 3000))
            except (TypeError, ValueError, json.JSONDecodeError):
                self._send_json({"error": "width and height required"}, 400)
                return
            supported_modes = {
                (1920, 1080, 5000), (1920, 1080, 3000),
                (1280, 720, 5000), (1280, 720, 3000),
                (640, 480, 3000),
            }
            if (width, height, fps_x100) not in supported_modes:
                self._send_json({"error": "unsupported resolution"}, 400)
                return
            MockState.width = width
            MockState.height = height
            MockState.capture_fps_x100 = fps_x100
            self._send_json(_status_payload()["video"])
            return
        if path == "/api/video/settings":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            try:
                preview_fps_x100 = int(payload.get("preview_fps_x100", MockState.preview_fps_x100))
            except (TypeError, ValueError):
                self._send_json({"error": "preview_fps_x100 must be numeric"}, 400)
                return
            if preview_fps_x100 < 100 or preview_fps_x100 > 1000 or preview_fps_x100 % 100:
                self._send_json({"error": "preview_fps must be an integer from 1 to 10"}, 400)
                return
            MockState.video_always_online = bool(payload.get("always_online", payload.get("always_on", False)))
            MockState.preview_fps_x100 = preview_fps_x100
            self._send_json(_status_payload()["video"])
            return
        if path == "/api/control/lease":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            active = bool(payload.get("active", True))
            requested_owner = str(payload.get("owner") or "").lower()
            if requested_owner not in ("agent", "mcp"):
                requested_owner = (
                    "mcp" if self.headers.get("X-ExoAnchor-Client", "") ==
                    "exoanchor-mcp" else "agent"
                )
            if active:
                if (MockState.control_lease_active and
                        MockState.control_lease_owner != requested_owner and
                        not payload.get("force")):
                    self._send_json({"error": "control lease held by other"}, 409)
                    return
                mode = str(payload.get("mode") or "supervised").lower()
                if mode not in ("observe", "supervised", "autonomous"):
                    self._send_json({"error": "invalid control mode"}, 400)
                    return
                MockState.control_lease_active = True
                MockState.control_lease_owner = requested_owner
                MockState.control_lease_mode = mode
                MockState.control_lease_session_id = str(
                    payload.get("session_id") or "mock-session"
                )
                MockState.control_lease_reason = str(payload.get("reason") or "")
            elif (payload.get("force") or not MockState.control_lease_active or
                  MockState.control_lease_owner == requested_owner):
                MockState.control_lease_active = False
                MockState.control_lease_owner = ""
                MockState.control_lease_mode = "supervised"
                MockState.control_lease_session_id = ""
                MockState.control_lease_reason = ""
            else:
                self._send_json({"error": "control lease held by other"}, 409)
                return
            self._send_json(_control_lease_payload())
            return
        if path == "/api/video/lease":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            owner = str(payload.get("owner") or "preview")
            active = bool(payload.get("active", payload.get("enabled", True)))
            if owner == "preview":
                MockState.preview_enabled = active
            elif owner == "agent":
                MockState.video_agent_takeover = bool(
                    active and payload.get("takeover")
                )
                if active:
                    MockState.video_active_owner = "agent"
                elif MockState.video_active_owner == "agent":
                    MockState.video_active_owner = "preview"
            elif owner == "kvm":
                try:
                    stream_id = int(payload.get("stream_id", 0)) & 0xFFFFFFFF
                except (TypeError, ValueError):
                    self._send_json({"error": "stream_id must be numeric"}, 400)
                    return
                force = bool(payload.get("force") or payload.get("reclaim"))
                if force:
                    MockState.video_agent_takeover = False
                if active:
                    MockState.video_active_owner = "kvm"
                    MockState.video_stream_id = stream_id
                elif MockState.video_active_owner == "kvm":
                    MockState.video_active_owner = "preview"
                    if stream_id == MockState.video_stream_id:
                        MockState.video_stream_id = 0
            self._send_json(_status_payload()["video"])
            return
        if path == "/api/storage/upload":
            query = parse_qs(parsed.query)
            storage_path = posixpath.normpath("/" + query.get("path", [""])[0].strip("/")).lstrip("/")
            if not storage_path or storage_path.endswith("/") or ".." in storage_path.split("/"):
                self._send_json({"error": "storage upload path is not allowed"}, 400)
                return
            MockState.storage_files[storage_path] = raw
            self._send_json({"ok": True, "path": storage_path, "display_path": "/EA/" + storage_path, "size": size})
            return
        if path == "/api/settings/gpio-map":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            if payload.get("reset") is True:
                MockState.power_gpio_map = [item.copy() for item in DEFAULT_POWER_GPIO_MAP]
                MockState.locator_on = False
                self._send_json(_power_payload())
                return
            items = payload.get("gpio_map") or payload.get("map")
            if not isinstance(items, list):
                self._send_json({"error": "gpio_map array required"}, 400)
                return
            by_role = {item["role"]: item.copy() for item in MockState.power_gpio_map}
            for item in items:
                role = item.get("role") if isinstance(item, dict) else None
                if role not in by_role:
                    self._send_json({"error": "invalid gpio map"}, 400)
                    return
                try:
                    gpio = int(item.get("gpio", -1))
                except (TypeError, ValueError):
                    self._send_json({"error": "invalid gpio map"}, 400)
                    return
                if gpio < -1 or gpio > 54:
                    self._send_json({"error": "invalid gpio map"}, 400)
                    return
                by_role[role]["gpio"] = gpio
                if "active_high" in item:
                    by_role[role]["active_high"] = bool(item["active_high"])
            if by_role["power_button"]["gpio"] < 0 or by_role["reset_button"]["gpio"] < 0:
                self._send_json({"error": "invalid gpio map"}, 400)
                return
            used = [item["gpio"] for item in by_role.values() if item["gpio"] >= 0]
            if len(used) != len(set(used)):
                self._send_json({"error": "invalid gpio map"}, 400)
                return
            MockState.power_gpio_map = [by_role[item["role"]] for item in DEFAULT_POWER_GPIO_MAP]
            if _power_role("locator").get("gpio", -1) < 0:
                MockState.locator_on = False
            self._send_json(_power_payload())
            return
        if path == "/api/automation/stop":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                resource = str(payload.get("resource") or "")
                generation = int(payload.get("generation") or 0)
            except (json.JSONDecodeError, TypeError, ValueError):
                self._send_json({"error": "invalid request"}, 400)
                return
            matched = False
            retained = []
            for operation in MockState.automation_operations:
                if (str(operation.get("resource") or "") == resource and
                        int(operation.get("generation") or 0) == generation and
                        operation.get("cancellable", True)):
                    matched = True
                    continue
                retained.append(operation)
            if not matched:
                self._send_json({"error": "operation changed, ended, or cannot be cancelled"}, 409)
                return
            MockState.automation_operations = retained
            self._send_json({
                "accepted": True,
                "schema": "exoanchor.automation.v1",
                "operations": [dict(item) for item in retained],
                "active_count": len(retained),
                "manual_takeover_available": any(
                    item.get("cancellable", True) for item in retained
                ),
            })
            return
        if path == "/api/power/action":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            action = str(payload.get("action") or "")
            if action in {"locator_on", "identify_on"}:
                if _power_role("locator").get("gpio", -1) < 0:
                    self._send_json({"error": "unsupported power action or gpio not configured"}, 400)
                    return
                MockState.locator_on = True
                MockState.power_actions += 1
                MockState.power_last_action = "locator_on"
                self._send_json(_power_payload())
                return
            if action in {"locator_off", "identify_off"}:
                if _power_role("locator").get("gpio", -1) < 0:
                    self._send_json({"error": "unsupported power action or gpio not configured"}, 400)
                    return
                MockState.locator_on = False
                MockState.power_actions += 1
                MockState.power_last_action = "locator_off"
                self._send_json(_power_payload())
                return
            if action in {"locator_toggle", "identify"}:
                if _power_role("locator").get("gpio", -1) < 0:
                    self._send_json({"error": "unsupported power action or gpio not configured"}, 400)
                    return
                MockState.locator_on = not MockState.locator_on
                MockState.power_actions += 1
                MockState.power_last_action = "locator_on" if MockState.locator_on else "locator_off"
                self._send_json(_power_payload())
                return
            if action not in {"power", "press_power", "reset", "press_reset", "force_off", "forceoff"}:
                self._send_json({"error": "unsupported power action"}, 400)
                return
            MockState.power_actions += 1
            MockState.power_last_action = {"press_power": "power", "press_reset": "reset", "forceoff": "force_off"}.get(action, action)
            duration = 5.0 if MockState.power_last_action == "force_off" else 0.5
            MockState.power_busy_until = time.monotonic() + min(duration, 0.2)
            self._send_json(_power_payload())
            return
        if path == "/api/agent/sessions":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            MockState.agent_session_seq += 1
            session_id = f"s{MockState.agent_session_seq}"
            now_ms = int(time.monotonic() * 1000)
            session = {"id": session_id, "session_id": session_id, "title": str(payload.get("title") or "New session")[:48], "created_ms": now_ms, "updated_ms": now_ms, "message_count": 0}
            MockState.agent_sessions[session_id] = session
            MockState.agent_history[session_id] = []
            self._send_json({"ok": True, **session})
            return
        if path == "/api/agent/sessions/delete":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            session_id = str(payload.get("session_id") or "")
            if session_id:
                MockState.agent_sessions.pop(session_id, None)
                MockState.agent_history.pop(session_id, None)
            self._send_json({"ok": True, "session_id": session_id})
            return
        if path == "/api/agent/data/clear":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            if payload.get("confirm") != "DELETE_ALL_AGENT_DATA":
                self._send_json(
                    {"error": "confirm must be DELETE_ALL_AGENT_DATA"}, 400)
                return
            if MockState.agent_run.get("busy"):
                self._send_json(
                    {"error": "stop the active Agent task before deleting conversations and memory"},
                    409,
                )
                return
            MockState.agent_sessions.clear()
            MockState.agent_history.clear()
            MockState.agent_memory.clear()
            MockState.agent_request = {}
            MockState.agent_run.update({
                "state": "idle",
                "running": False,
                "paused": False,
                "busy": False,
                "job_id": "",
                "run_id": "",
                "turn_id": "",
                "thread_id": "",
                "session_id": "",
                "goal": "",
                "events": [],
                "event_seq": 0,
                "result": None,
            })
            self._send_json({
                "ok": True,
                "history_cleared": True,
                "memory_cleared": True,
                "session_count": 0,
                "conversation_record_count": 0,
                "memory_record_count": 0,
                "mcp_exposed": False,
            })
            return
        if path == "/api/agent/history":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            session_id = str(payload.get("session_id") or "default")
            record = payload.get("record") if isinstance(payload.get("record"), dict) else payload
            MockState.agent_history.setdefault(session_id, []).append(record)
            self._send_json({"ok": True, "supported": True, "session_id": session_id, "record_count": len(MockState.agent_history[session_id])})
            return
        if path == "/api/agent/run":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            message = str(
                payload.get("message") or payload.get("prompt") or
                payload.get("input") or ""
            )
            if not message:
                self._send_json({"error": "message required"}, 400)
                return
            idempotency_key = str(
                payload.get("idempotency_key") or payload.get("request_id") or
                f"mock-{time.monotonic_ns():x}"
            )
            if len(idempotency_key) > 64:
                self._send_json({
                    "error": "idempotency_key must be 1..64 bytes"
                }, 400)
                return
            fingerprint = _agent_run_request_fingerprint(payload)
            retained_run_id = str(MockState.agent_run.get("job_id") or "")
            if (retained_run_id and idempotency_key ==
                    MockState.agent_run_idempotency_key):
                if fingerprint != MockState.agent_run_request_fingerprint:
                    active = bool(MockState.agent_run.get("busy"))
                    self._send_json({
                        "accepted": False,
                        "queued": False,
                        "busy": active,
                        "deduplicated": False,
                        "submitted_run_id": "",
                        "active_run_id": retained_run_id if active else "",
                        "conflicting_run_id": retained_run_id,
                        "reason": "idempotency_conflict",
                    }, 409)
                    return
                receipt_busy = bool(MockState.agent_run.get("busy"))
                if MockState.agent_run_replace_before_projection:
                    MockState.agent_run_replace_before_projection = False
                    # The submit receipt is still authoritative, but the
                    # independently sampled status projection no longer owns
                    # this retained Run. Do not leak a replacement Run.
                    response = {
                        "busy": receipt_busy,
                        "status_retained": False,
                    }
                else:
                    response = _agent_run_payload()
                response.update({
                    "accepted": True,
                    "queued": False,
                    "deduplicated": True,
                    "receipt_busy": receipt_busy,
                    "submitted_run_id": retained_run_id,
                    "active_run_id": retained_run_id
                    if receipt_busy else "",
                })
                self._send_json(response)
                return
            if MockState.agent_run["busy"]:
                self._send_json({
                    "accepted": False,
                    "queued": False,
                    "busy": True,
                    "deduplicated": False,
                    "submitted_run_id": "",
                    "active_run_id": retained_run_id,
                    "reason": "agent_run_busy",
                }, 409)
                return
            session_id = str(payload.get("session_id") or "default")
            if session_id not in MockState.agent_sessions:
                now_ms = int(time.monotonic() * 1000)
                MockState.agent_sessions[session_id] = {"id": session_id, "session_id": session_id, "title": session_id, "created_ms": now_ms, "updated_ms": now_ms, "message_count": 0}
                MockState.agent_history[session_id] = []
            MockState.agent_run_seq += 1
            started_ms = int(time.monotonic() * 1000)
            job_id = f"r{MockState.agent_run_seq}"
            authority_policy = str(payload.get("authority_mode") or "") == "policy"
            navigation_race = message.lower().startswith("navigation-race:")
            needs_request = navigation_race or message.lower().startswith(
                ("request:", "ask:")
            )
            MockState.agent_run = {
                "state": "waiting_request" if needs_request else "running",
                "running": not needs_request, "paused": False,
                "waiting_request": needs_request,
                "busy": True, "pause_requested": False,
                "cancel_requested": False, "abort_requested": False,
                "job_id": job_id, "run_id": job_id,
                "turn_id": f"t{MockState.agent_run_seq:08d}",
                "session_id": session_id, "thread_id": session_id,
                "goal": message, "intent_revision": 1, "plan_version": 1,
                "profile": str(payload.get("profile") or MockState.agent_active_profile),
                "model": str(payload.get("model") or _agent_api_payload()["model"]),
                "dry_run": False if authority_policy else bool(payload.get("dry_run", True)),
                "include_screenshot": bool(payload.get("include_screenshot", False)),
                "include_web_search": bool(payload.get("include_web_search", False)),
                "page_context_attached": isinstance(payload.get("page_context"), dict),
                "page_context": payload.get("page_context") if isinstance(payload.get("page_context"), dict) else None,
                "started_ms": started_ms, "updated_ms": started_ms, "finished_ms": 0,
                "stage": "waiting for decision" if needs_request else "running",
                "detail": "Exact request is device-owned and survives page changes" if needs_request else "Local mock Agent is validating the UI contract",
                "event_seq": 1, "event_schema_version": 1, "event_dropped": 0,
                "events": [_agent_event(1, started_ms, "agent", "started",
                                        "mock agent run started",
                                        run_id=job_id)],
                "user_message": message,
            }
            MockState.agent_run_idempotency_key = idempotency_key
            MockState.agent_run_request_fingerprint = fingerprint
            MockState.agent_request = (
                _mock_agent_request(MockState.agent_run) if needs_request else {}
            )
            MockState.agent_run_started_at = time.monotonic()
            MockState.agent_run_paused_at = 0.0
            response = _agent_run_payload()
            response.update({
                "accepted": True,
                "queued": False,
                "deduplicated": False,
                "receipt_busy": False,
                "submitted_run_id": job_id,
                "active_run_id": "",
            })
            if navigation_race:
                # Let browser tests navigate after the device accepted the run
                # but before the originating document receives the response.
                time.sleep(0.75)
            self._send_json(response, 202)
            return
        if path == "/api/agent/run/steer":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            message = str(payload.get("message") or payload.get("input") or "").strip()
            requested_run = str(payload.get("run_id") or payload.get("job_id") or "")
            if not requested_run or len(requested_run) > 48:
                self._send_json({"error": "exact run_id required"}, 400)
                return
            if requested_run != MockState.agent_run.get("job_id"):
                self._send_json({
                    "accepted": False,
                    "control": "steer",
                    "run_id": requested_run,
                    "reason": "run_id_mismatch",
                }, 409)
                return
            accepted = bool(
                message and MockState.agent_run.get("busy")
            )
            if accepted:
                MockState.agent_run["intent_revision"] = int(
                    MockState.agent_run.get("intent_revision", 1)
                ) + 1
                MockState.agent_run["plan_version"] = int(
                    MockState.agent_run.get("plan_version", 1)
                ) + 1
                MockState.agent_run["updated_ms"] = int(time.monotonic() * 1000)
                MockState.agent_run["detail"] = (
                    "Steer accepted and merged at the next safe checkpoint"
                )
                _append_agent_run_event(
                    "agent", "output_received",
                    f"steer accepted · intent revision={MockState.agent_run['intent_revision']}",
                )
            response = _agent_run_payload()
            response.update({
                "accepted": accepted,
                "control": "steer",
                "intent_revision": MockState.agent_run.get("intent_revision", 0),
            })
            self._send_json(response)
            return
        if path in ("/api/agent/requests/decision",
                    "/api/agent/requests/cancel"):
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            request_id = str(payload.get("request_id") or payload.get("id") or "")
            request = MockState.agent_request
            if not request or request_id != request.get("request_id"):
                self._send_json({"error": "Agent request not found"}, 404)
                return
            if request.get("status") != "waiting_user":
                self._send_json(
                    {"error": "Agent request is no longer waiting"}, 409
                )
                return
            approved = (
                path.endswith("/decision") and
                (payload.get("approved") is True or
                 str(payload.get("decision") or "") in
                 ("approve", "allow_once"))
            )
            response = str(payload.get("response") or payload.get("answer") or "").strip()
            if (approved and request.get("resource") == "ask_user" and not response):
                self._send_json({"error": "response is required"}, 400)
                return
            now_ms = int(time.monotonic() * 1000)
            request["status"] = "granted" if approved else "cancelled"
            request["decision_source"] = "browser"
            request["decided_by_session"] = "mock-browser"
            request["decided_ms"] = now_ms
            if approved and response:
                request["response"] = response[:512]
            if not approved:
                request["error"] = "request cancelled"
            if MockState.agent_run.get("job_id") == request.get("run_id"):
                MockState.agent_run.update({
                    "state": "running",
                    "running": True,
                    "waiting_request": False,
                    "busy": True,
                    "updated_ms": now_ms,
                    "stage": "continuing",
                    "detail": (
                        "Allow-once decision received; resuming original step"
                        if approved else
                        "Request denied; continuing to produce a bounded result"
                    ),
                })
                _append_agent_run_event(
                    "agent", "started" if approved else "cancelled",
                    f"request {'approved once' if approved else 'cancelled'} · {request_id}",
                    request_id,
                )
            self._send_json(json.loads(json.dumps(request)))
            return
        if path in ("/api/agent/run/cancel", "/api/agent/run/abort"):
            accepted = bool(MockState.agent_run["busy"])
            if accepted:
                now_ms = int(time.monotonic() * 1000)
                MockState.agent_run.update({
                    "state": "aborted", "running": False, "paused": False,
                    "waiting_request": False,
                    "busy": False, "pause_requested": False,
                    "cancel_requested": True, "abort_requested": True,
                    "updated_ms": now_ms, "finished_ms": now_ms,
                    "stage": "aborted", "detail": "Aborted by local UI",
                    "error": "run aborted",
                })
                _append_agent_run_event(
                    "agent", "cancelled",
                    "run aborted; HID state and control lease released",
                )
                MockState.agent_run_paused_at = 0.0
                if MockState.agent_request.get("run_id") == MockState.agent_run.get("job_id") and MockState.agent_request.get("status") in ("waiting_user", "granted"):
                    MockState.agent_request["status"] = "cancelled"
                    MockState.agent_request["decision_source"] = "runtime"
                    MockState.agent_request["error"] = "run cancelled"
            response = _agent_run_payload()
            response.update({"accepted": accepted, "control": "abort"})
            self._send_json(response)
            return
        if path == "/api/agent/run/pause":
            accepted = bool(
                MockState.agent_run["busy"] and
                MockState.agent_run["state"] in
                ("running", "waiting_request")
            )
            if accepted:
                now_ms = int(time.monotonic() * 1000)
                MockState.agent_run_paused_at = time.monotonic()
                paused_from = MockState.agent_run["state"]
                MockState.agent_run.update({
                    "state": "paused", "running": False, "paused": True,
                    "waiting_request": False,
                    "paused_from": paused_from,
                    "pause_requested": True, "updated_ms": now_ms,
                    "stage": "paused",
                    "detail": "Mock run paused at cooperative checkpoint",
                })
                _append_agent_run_event(
                    "agent", "waiting_approval",
                    "run paused at cooperative checkpoint",
                )
            response = _agent_run_payload()
            response.update({"accepted": accepted, "control": "pause"})
            self._send_json(response)
            return
        if path == "/api/agent/run/resume":
            accepted = bool(
                MockState.agent_run["busy"] and
                MockState.agent_run["state"] == "paused"
            )
            if accepted:
                now_ms = int(time.monotonic() * 1000)
                resumed_at = time.monotonic()
                if MockState.agent_run_paused_at > 0.0:
                    MockState.agent_run_started_at += (
                        resumed_at - MockState.agent_run_paused_at
                    )
                MockState.agent_run_paused_at = 0.0
                MockState.agent_run.update({
                    "state": MockState.agent_run.get("paused_from", "running"),
                    "running": MockState.agent_run.get("paused_from", "running") == "running",
                    "waiting_request": MockState.agent_run.get("paused_from") == "waiting_request",
                    "paused": False,
                    "pause_requested": False, "updated_ms": now_ms,
                    "stage": "running",
                    "detail": "Mock run resumed from cooperative checkpoint",
                })
                _append_agent_run_event(
                    "agent", "started",
                    "run resumed from cooperative checkpoint",
                )
            response = _agent_run_payload()
            response.update({"accepted": accepted, "control": "resume"})
            self._send_json(response)
            return
        if path == "/api/settings/device":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            if "label" in payload:
                MockState.device_label = str(payload.get("label") or "").strip()[:48] or "ExoAnchor"
            if "agent_name" in payload:
                MockState.agent_display_name = str(payload.get("agent_name") or "").strip()[:32] or "Agent"
            self._send_json({"ok": True, "label": MockState.device_label, "device_label": MockState.device_label, "default_label": "ExoAnchor", "max_length": 48, "agent_name": MockState.agent_display_name, "default_agent_name": "Agent", "agent_name_max_length": 32, "stored_in_nvs": True})
            return
        if path == "/api/settings/target-profile":
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._send_json({"error": "invalid json"}, 400)
                return
            limits = {
                "name": 64, "device_type": 24, "operating_system": 24,
                "environment": 24, "location": 96, "configuration": 256,
                "purpose": 256, "notes": 256,
            }
            for key, limit in limits.items():
                if key in payload:
                    MockState.target_profile[key] = str(payload.get(key) or "").strip()[:limit]
            response = {"ok": True, **MockState.target_profile}
            response.update({
                "agent_context_enabled": True,
                "agent_context_scope": "untrusted metadata only; never grants permission or authority",
            })
            self._send_json(response)
            return
        if path == "/api/settings/system":
            self._send_json({"ok": True, "reset_scheduled": True})
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
    parser.add_argument(
        "--h264-fixture",
        action="append",
        type=_parse_h264_fixture,
        default=[],
        metavar="WIDTHxHEIGHT@FPS=FILE",
        help=(
            "repeatable Annex-B fixture generated with AUD and repeated headers; "
            "enables the browser H.264 output path on the mock server"
        ),
    )
    parser.add_argument(
        "--h264-force-mse",
        action="store_true",
        help="force the local browser fixture through MediaSource instead of WebCodecs",
    )
    args = parser.parse_args()

    if args.h264_force_mse and not args.h264_fixture:
        parser.error("--h264-force-mse requires at least one --h264-fixture")

    if args.h264_fixture:
        H264_FIXTURES.update(dict(args.h264_fixture))
        first = next(iter(H264_FIXTURES.values()))
        MockState.width = int(first["width"])
        MockState.height = int(first["height"])
        MockState.capture_fps_x100 = 3000
        MockState.h264_ws_port = args.port
        MockState.h264_force_mse = args.h264_force_mse

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"[ui-mock] serving {ROOT} at http://{args.host}:{args.port}/", flush=True)
    print(f"[ui-mock] Video: http://{args.host}:{args.port}/kvm", flush=True)
    for fixture in H264_FIXTURES.values():
        print(
            "[ui-mock] H.264 fixture: "
            f"{fixture['width']}x{fixture['height']}@{fixture['fps']:g} "
            f"{len(fixture['units'])} AU from {fixture['path']}",
            flush=True,
        )
    httpd.serve_forever()


if __name__ == "__main__":
    main()
