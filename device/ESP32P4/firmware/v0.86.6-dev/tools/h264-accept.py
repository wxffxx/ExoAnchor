#!/usr/bin/env python3
"""Run repeatable raw-transport H.264 acceptance against one ExoAnchor.

The password is read from stdin and is never accepted as a command-line
argument.  This tool validates the wire contract, steady-state frame cadence,
device-side H.264 resource/chain telemetry, UVC error deltas, restore state, and
optionally the complete Annex-B stream with ffprobe/ffmpeg.  Browser rendering
and glass-to-glass latency remain separate acceptance gates.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import requests


MOTION_SAMPLE_FPS = 5
MOTION_SAMPLE_WIDTH = 160
MOTION_SAMPLE_HEIGHT = 90
MOTION_CHANGED_PIXEL_DELTA = 8
MOTION_MIN_COMPARISONS = 5
MAX_FPS_RATIO = 1.06

# These cumulative source-side counters are evidence for cadence diagnosis,
# not lifecycle error counters.  A positive phase delta alone is not yet a
# classified failure: in particular, ``frames_dropped`` aggregates several
# paths that the H.264-specific counters do not distinguish.  Keep the deltas
# visible in reports and let the classified transport/runtime counters gate.
NON_ERROR_COUNTER_KEYS = frozenset(
    {
        "frames_captured",
        "frames_encoded",
        "frames_dropped",
        "uvc_callbacks",
        "h264_pressure_coalesces",
    }
)


@dataclass(frozen=True)
class Mode:
    width: int
    height: int
    capture_fps: float

    @property
    def promised_h264_fps(self) -> float:
        # The validated Rev3 contract is 1080p25 and 720p30. Keep the raw
        # bitstream timing gate aligned with the firmware's mode policy.
        ceiling = 30.0 if self.width <= 1280 and self.height <= 720 else 25.0
        nominal = float(int(self.capture_fps + 0.5))
        return min(nominal, ceiling)

    @property
    def h264_supported(self) -> bool:
        ceiling = 30.0 if self.width <= 1280 and self.height <= 720 else 25.0
        return (
            80 <= self.width <= 1920
            and 80 <= self.height <= 2032
            and self.capture_fps <= ceiling
        )

    @property
    def label(self) -> str:
        fps = int(self.capture_fps) if self.capture_fps.is_integer() else self.capture_fps
        return f"{self.width}x{self.height}@{fps}"


class BufferedSocket:
    def __init__(self, sock: socket.socket, initial: bytes = b"") -> None:
        self.sock = sock
        self.buffer = bytearray(initial)

    def read_exact(self, count: int) -> bytes:
        while len(self.buffer) < count:
            chunk = self.sock.recv(max(4096, count - len(self.buffer)))
            if not chunk:
                raise EOFError("websocket closed")
            self.buffer.extend(chunk)
        result = bytes(self.buffer[:count])
        del self.buffer[:count]
        return result

    def read_frame(self) -> tuple[int, bytes]:
        first, second = self.read_exact(2)
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self.read_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.read_exact(8))[0]
        mask = self.read_exact(4) if masked else None
        payload = self.read_exact(length)
        if mask:
            payload = bytes(
                value ^ mask[index & 3] for index, value in enumerate(payload)
            )
        return opcode, payload


def parse_mode(raw: str) -> Mode:
    try:
        dimensions, fps_text = raw.lower().split("@", 1)
        width_text, height_text = dimensions.split("x", 1)
        mode = Mode(int(width_text), int(height_text), float(fps_text))
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError(
            "mode must be WIDTHxHEIGHT@FPS, for example 1920x1080@25"
        ) from error
    if mode.width < 80 or mode.height < 80 or not 1 <= mode.capture_fps <= 60:
        raise argparse.ArgumentTypeError("mode is outside the supported test range")
    return mode


def parse_hid_point(raw: str) -> tuple[int, int]:
    try:
        x_text, y_text = raw.split(",", 1)
        point = (int(x_text), int(y_text))
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError(
            "HID point must be X,Y using absolute values from 0 to 32767"
        ) from error
    if not all(0 <= value <= 32767 for value in point):
        raise argparse.ArgumentTypeError(
            "HID point values must be between 0 and 32767"
        )
    return point


def response_json(response: requests.Response) -> dict[str, Any]:
    try:
        data = response.json()
    except ValueError as error:
        raise RuntimeError(
            f"{response.request.method} {response.url} returned non-JSON "
            f"HTTP {response.status_code}"
        ) from error
    if not isinstance(data, dict):
        raise RuntimeError(f"{response.url} returned a non-object JSON response")
    return data


def login(host: str, username: str, password: str) -> requests.Session:
    session = requests.Session()
    response = session.post(
        f"http://{host}/api/auth/login",
        json={
            "username": username,
            "password": password,
            "request_id": uuid.uuid4().hex[:24],
        },
        timeout=5,
    )
    data = response_json(response)
    while response.status_code == 202 and data.get("pending"):
        time.sleep(max(0.05, float(data.get("poll_after_ms", 100)) / 1000.0))
        response = session.get(
            f"http://{host}/api/auth/login/status",
            params={"job_id": data["job_id"]},
            timeout=5,
        )
        data = response_json(response)
    response.raise_for_status()
    return session


def status(session: requests.Session, host: str) -> dict[str, Any]:
    response = session.get(f"http://{host}/api/status", timeout=5)
    response.raise_for_status()
    return response_json(response)


def video_status(payload: dict[str, Any]) -> dict[str, Any]:
    video = payload.get("video", payload)
    return video if isinstance(video, dict) else {}


def set_mode(session: requests.Session, host: str, mode: Mode) -> None:
    response = session.post(
        f"http://{host}/api/video/resolution",
        json={
            "pixel_format": "MJPEG",
            "width": mode.width,
            "height": mode.height,
            "fps_x100": round(mode.capture_fps * 100),
        },
        timeout=8,
    )
    response.raise_for_status()


def mode_observation(payload: dict[str, Any], mode: Mode) -> dict[str, Any]:
    """Return only the existing video-status fields needed for mode gating."""
    video = video_status(payload)
    actual_width = int(video.get("width", 0) or 0)
    actual_height = int(video.get("height", 0) or 0)
    target_width = int(video.get("target_width", 0) or 0)
    target_height = int(video.get("target_height", 0) or 0)
    target_fps_x100 = int(video.get("target_fps_x100", 0) or 0)
    expected_fps_x100 = round(mode.capture_fps * 100)
    checks = {
        "actual_dimensions": (actual_width, actual_height)
        == (mode.width, mode.height),
        "target_dimensions": (target_width, target_height)
        == (mode.width, mode.height),
        # UVC frame intervals are represented at 0.01 FPS precision, while the
        # driver accepts a 0.5 FPS tolerance when matching advertised modes.
        "target_fps": abs(target_fps_x100 - expected_fps_x100) <= 50,
        "mjpeg_capture": str(video.get("pixel_format", "")).upper() == "MJPEG",
        "device_connected": bool(video.get("device_connected", video.get("connected"))),
        "frame_ready": bool(video.get("frame_ready")),
        "no_video_error": not str(video.get("last_error", "") or ""),
    }
    return {
        "actual": {
            "width": actual_width,
            "height": actual_height,
            "fps": float(video.get("fps", 0) or 0),
            "frame_ready": bool(video.get("frame_ready")),
        },
        "target": {
            "width": target_width,
            "height": target_height,
            "fps_x100": target_fps_x100,
        },
        "pixel_format": video.get("pixel_format"),
        "last_error": video.get("last_error", ""),
        "checks": checks,
        "passed": all(checks.values()),
    }


def wait_for_mode(
    session: requests.Session, host: str, mode: Mode, timeout_s: float
) -> dict[str, Any]:
    started = time.monotonic()
    deadline = started + timeout_s
    latest: dict[str, Any] = {
        "checks": {},
        "passed": False,
    }
    while time.monotonic() < deadline:
        latest = mode_observation(status(session, host), mode)
        if latest["passed"]:
            break
        time.sleep(0.1)
    latest["elapsed_ms"] = round((time.monotonic() - started) * 1000)
    latest["timeout_s"] = timeout_s
    return latest


def wait_for_mode_lifecycle_settle(
    session: requests.Session,
    host: str,
    mode: Mode,
    timeout_s: float,
    *,
    minimum_observation_s: float = 1.0,
    counter_quiet_s: float = 0.5,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Wait past first-frame readiness for the UVC start lifecycle to settle.

    UVC start/format events are delivered asynchronously.  ``frame_ready`` can
    therefore become true before a startup underflow/overflow counter update is
    published.  This bounded quiet period assigns those expected transition
    events to mode activation while preserving a fresh baseline immediately
    before the H.264 WebSocket is opened.
    """
    started = time.monotonic()
    deadline = started + timeout_s
    latest_payload: dict[str, Any] = {}
    latest_observation: dict[str, Any] = {"passed": False, "checks": {}}
    previous_error_counters: dict[str, int] | None = None
    ready_since: float | None = None
    counters_quiet_since: float | None = None
    settled = False
    while time.monotonic() < deadline:
        latest_payload = status(session, host)
        latest_observation = mode_observation(latest_payload, mode)
        error_counters = error_counter_view(counter_snapshot(latest_payload))
        now = time.monotonic()
        if latest_observation["passed"]:
            if ready_since is None:
                ready_since = now
            if (
                previous_error_counters is None
                or error_counters != previous_error_counters
            ):
                counters_quiet_since = now
            elif counters_quiet_since is None:
                counters_quiet_since = now
            if (
                now - ready_since >= minimum_observation_s
                and counters_quiet_since is not None
                and now - counters_quiet_since >= counter_quiet_s
            ):
                settled = True
                break
        else:
            ready_since = None
            counters_quiet_since = None
        previous_error_counters = error_counters
        time.sleep(0.1)
    finished = time.monotonic()
    return latest_payload, {
        "settled": settled,
        "elapsed_ms": round((finished - started) * 1000),
        "timeout_s": timeout_s,
        "minimum_observation_ms": round(minimum_observation_s * 1000),
        "counter_quiet_ms": round(counter_quiet_s * 1000),
        "observation": latest_observation,
    }


def set_lease(
    session: requests.Session,
    host: str,
    stream_id: int,
    active: bool,
    *,
    claim: bool = False,
    force: bool = False,
) -> None:
    if force and (not claim or not active):
        raise ValueError("force is valid only for the initial lease claim")
    payload: dict[str, Any] = {
        "owner": "kvm",
        "active": active,
        "stream_id": stream_id,
    }
    if claim:
        payload.update({"claim": True, "previous_stream_id": 0})
        if force:
            payload["force"] = True
    response = session.post(
        f"http://{host}/api/video/lease", json=payload, timeout=8
    )
    response.raise_for_status()


def activate_capture_mode(
    session: requests.Session,
    host: str,
    mode: Mode,
    timeout_s: float,
    *,
    force_lease: bool = False,
) -> tuple[int, dict[str, Any]]:
    """Acquire the KVM lease that the following H.264 cycle will keep using.

    Releasing a short-lived probe lease here used to stop UVC capture between
    mode activation and the measured session.  That control-plane transition
    could then land inside the H.264 counter window as an unrelated UVC
    underflow/restart.  A successful probe therefore transfers its lease to
    ``run_cycle``; only a failed probe releases it locally.
    """
    lifecycle_before = status(session, host)
    lifecycle_before_counters = counter_snapshot(lifecycle_before)
    stream_id = int.from_bytes(os.urandom(4), "little") or 1
    set_lease(
        session,
        host,
        stream_id,
        True,
        claim=True,
        force=force_lease,
    )
    try:
        result = wait_for_mode(session, host, mode, timeout_s)
        result["probe_stream_id"] = stream_id
        result["lease_claim"] = {
            "force": force_lease,
            "initial_claim_only": True,
        }
        if result["passed"]:
            lifecycle_after, settle = wait_for_mode_lifecycle_settle(
                session, host, mode, timeout_s
            )
            result["lifecycle_settle"] = settle
            lifecycle_counter_delta = counter_delta(
                lifecycle_before_counters,
                counter_snapshot(lifecycle_after),
            )
            lifecycle_runtime_errors = runtime_error_snapshot(lifecycle_after)
            lifecycle_runtime_error_checks = (
                mode_activation_runtime_error_checks(lifecycle_runtime_errors)
            )
            lifecycle_h264 = video_status(lifecycle_after).get(
                "h264_runtime", {}
            )
            reference_workspace, reference_workspace_gate = (
                reference_workspace_checks(lifecycle_h264)
            )
            lifecycle_checks = {
                "settled": bool(settle["settled"]),
                "error_counters": error_counters_clean(
                    lifecycle_counter_delta
                ),
                "runtime_errors": all(
                    lifecycle_runtime_error_checks.values()
                ),
                "reference_workspace": all(
                    reference_workspace_gate.values()
                ),
            }
            result["lifecycle_counter_delta"] = lifecycle_counter_delta
            result["lifecycle_runtime_errors"] = lifecycle_runtime_errors
            result["lifecycle_runtime_error_checks"] = (
                lifecycle_runtime_error_checks
            )
            result["reference_workspace"] = reference_workspace
            result["reference_workspace_checks"] = (
                reference_workspace_gate
            )
            result["lifecycle_checks"] = lifecycle_checks
            result["passed"] = bool(
                result["passed"] and all(lifecycle_checks.values())
            )
            if result["passed"]:
                return stream_id, result
        set_lease(session, host, stream_id, False)
        return 0, result
    except BaseException:
        set_lease(session, host, stream_id, False)
        raise


def websocket_upgrade(
    host: str, port: int, path: str, cookie: str
) -> tuple[socket.socket, BufferedSocket]:
    sock = socket.create_connection((host, port), timeout=8)
    sock.settimeout(8)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Cookie: EA_SESSION={cookie}\r\n\r\n"
    ).encode("ascii")
    sock.sendall(request)
    received = bytearray()
    while b"\r\n\r\n" not in received:
        chunk = sock.recv(4096)
        if not chunk:
            raise EOFError("websocket closed during upgrade")
        received.extend(chunk)
        if len(received) > 16384:
            raise RuntimeError("oversized websocket upgrade response")
    header, remainder = bytes(received).split(b"\r\n\r\n", 1)
    status_line = header.split(b"\r\n", 1)[0]
    if b" 101 " not in status_line:
        sock.close()
        raise RuntimeError(f"websocket upgrade failed: {status_line!r}")
    return sock, BufferedSocket(sock, remainder)


def websocket_connect(
    host: str, cookie: str, stream_id: int
) -> tuple[socket.socket, BufferedSocket]:
    return websocket_upgrade(
        host,
        81,
        f"/api/ws/video/h264?stream_id={stream_id}",
        cookie,
    )


def hid_websocket_connect(
    host: str, cookie: str, stream_id: int
) -> socket.socket:
    sock, _reader = websocket_upgrade(
        host,
        80,
        f"/api/ws/hid?stream_id={stream_id}",
        cookie,
    )
    return sock


def websocket_send_client_frame(
    sock: socket.socket, opcode: int, payload: bytes
) -> None:
    if len(payload) > 0x7FFFFFFFFFFFFFFF:
        raise ValueError("websocket payload is too large")
    header = bytearray((0x80 | (opcode & 0x0F),))
    if len(payload) < 126:
        header.append(0x80 | len(payload))
    elif len(payload) <= 0xFFFF:
        header.append(0x80 | 126)
        header.extend(struct.pack("!H", len(payload)))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack("!Q", len(payload)))
    mask = os.urandom(4)
    masked = bytes(
        value ^ mask[index & 3] for index, value in enumerate(payload)
    )
    sock.sendall(bytes(header) + mask + masked)


def websocket_send_text(sock: socket.socket, payload: dict[str, Any]) -> None:
    websocket_send_client_frame(
        sock,
        1,
        json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode(
            "ascii"
        ),
    )


def websocket_send_close(sock: socket.socket, code: int = 1000) -> None:
    """Send a standards-compliant client CLOSE without tearing down TCP yet.

    Browser clients mask their control frames and let the server observe CLOSE
    before the socket disappears.  A raw ``sock.close()`` instead looks like a
    transport failure to the device and can race its final queued access unit,
    incorrectly incrementing the session/send failure counters in this test.
    The caller keeps TCP open while polling for device-side restore completion.
    """
    websocket_send_client_frame(sock, 8, struct.pack("!H", code))


def websocket_wait_for_peer_close(
    sock: socket.socket, reader: BufferedSocket, timeout_s: float = 1.5
) -> dict[str, Any]:
    """Best-effort bounded observation of the server's close/EOF response."""
    started = time.monotonic()
    previous_timeout = sock.gettimeout()
    observed = False
    response = "timeout"
    try:
        sock.settimeout(timeout_s)
        while time.monotonic() - started < timeout_s:
            try:
                opcode, _payload = reader.read_frame()
            except EOFError:
                observed = True
                response = "eof"
                break
            except ConnectionError:
                observed = True
                response = "connection_closed"
                break
            except socket.timeout:
                break
            if opcode == 8:
                observed = True
                response = "close"
                break
    finally:
        sock.settimeout(previous_timeout)
    return {
        "close_frame_sent": True,
        "peer_close_observed": observed,
        "peer_response": response,
        "elapsed_ms": round((time.monotonic() - started) * 1000),
    }


def hid_drag_worker(
    sock: socket.socket,
    start: tuple[int, int],
    end: tuple[int, int],
    duration_s: float,
    step_ms: int,
    stream_started: threading.Event,
    stop_requested: threading.Event,
    result: dict[str, Any],
) -> None:
    """Drive the same stream-bound HID socket used by the browser KVM.

    The video lease already owns ``stream_id``.  A second Agent/MCP control
    lease would correctly conflict with it, so this worker deliberately uses
    ``/api/ws/hid?stream_id=...`` and never calls ``/api/control/lease``.
    Every exit path attempts both mouse-up and release-all before returning.
    """
    held = False
    started_at = 0.0
    actions_sent = 0
    cleanup_errors: list[str] = []
    try:
        while not stream_started.wait(0.1):
            if stop_requested.is_set():
                result["error"] = "H.264 stream ended before the first access unit"
                return

        websocket_send_text(sock, {"type": "absmove", "x": start[0], "y": start[1]})
        actions_sent += 1
        websocket_send_text(sock, {"type": "absmousedown", "button": 0})
        actions_sent += 1
        held = True
        started_at = time.monotonic()
        result["started"] = True

        corners = (
            (end[0], start[1]),
            end,
            (start[0], end[1]),
            start,
        )
        segment_steps = max(2, round(500 / step_ms))
        point = start
        corner_index = 0
        next_step_at = started_at
        while time.monotonic() - started_at < duration_s:
            if stop_requested.is_set():
                result["error"] = "H.264 stream ended before HID drag completed"
                break
            target = corners[corner_index % len(corners)]
            for step in range(1, segment_steps + 1):
                if stop_requested.is_set():
                    result["error"] = "H.264 stream ended before HID drag completed"
                    break
                fraction = step / segment_steps
                x = round(point[0] + (target[0] - point[0]) * fraction)
                y = round(point[1] + (target[1] - point[1]) * fraction)
                websocket_send_text(sock, {"type": "absmove", "x": x, "y": y})
                actions_sent += 1
                next_step_at += step_ms / 1000.0
                remaining = next_step_at - time.monotonic()
                if remaining > 0 and stop_requested.wait(remaining):
                    result["error"] = "H.264 stream ended before HID drag completed"
                    break
                if time.monotonic() - started_at >= duration_s:
                    break
            else:
                point = target
                corner_index += 1
                continue
            break
        if time.monotonic() - started_at >= duration_s and not result.get("error"):
            result["completed"] = True
    except (OSError, ValueError) as error:
        result["error"] = f"HID drag failed: {error}"
    finally:
        if held:
            try:
                websocket_send_text(sock, {"type": "absmouseup", "button": 0})
                actions_sent += 1
            except OSError as error:
                cleanup_errors.append(f"mouseup failed: {error}")
        try:
            websocket_send_text(sock, {"type": "releaseall"})
            actions_sent += 1
        except OSError as error:
            cleanup_errors.append(f"releaseall failed: {error}")
        result["actions_sent"] = actions_sent
        result["elapsed_s"] = (
            round(time.monotonic() - started_at, 3) if started_at else 0.0
        )
        result["cleanup_errors"] = cleanup_errors
        result["cleanup_ok"] = held and not cleanup_errors


def nal_types(annex_b: bytes) -> set[int]:
    found: set[int] = set()
    index = 0
    while index + 3 <= len(annex_b):
        if annex_b[index : index + 4] == b"\x00\x00\x00\x01":
            start = index + 4
            index = start
        elif annex_b[index : index + 3] == b"\x00\x00\x01":
            start = index + 3
            index = start
        else:
            index += 1
            continue
        if start < len(annex_b):
            found.add(annex_b[start] & 0x1F)
    return found


def counter_snapshot(payload: dict[str, Any]) -> dict[str, int]:
    video = video_status(payload)
    pipeline = video.get("pipeline_stats", {})
    h264 = video.get("h264_runtime", {})
    chain = h264.get("chain_metrics", {}) if isinstance(h264, dict) else {}
    hid = payload.get("hid", {})
    hid_stats = hid.get("stats", {}) if isinstance(hid, dict) else {}
    return {
        "frames_captured": int(video.get("frames_captured", 0) or 0),
        "frames_encoded": int(video.get("frames_encoded", 0) or 0),
        "frames_dropped": int(video.get("frames_dropped", 0) or 0),
        "uvc_callbacks": int(pipeline.get("uvc_callbacks", 0) or 0),
        "uvc_queue_drops": int(pipeline.get("queue_drops", 0) or 0),
        "h264_pressure_coalesces": int(
            pipeline.get("h264_pressure_coalesces", 0) or 0
        ),
        "uvc_buffer_underflows": int(pipeline.get("buffer_underflows", 0) or 0),
        "uvc_buffer_overflows": int(pipeline.get("buffer_overflows", 0) or 0),
        "uvc_return_failures": int(pipeline.get("return_failures", 0) or 0),
        "h264_backpressure_drops": int(
            chain.get("backpressure_drops_total", 0) or 0
        ),
        "h264_decode_failures": int(chain.get("decode_failures_total", 0) or 0),
        "h264_encode_failures": int(chain.get("encode_failures_total", 0) or 0),
        "h264_send_failures": int(chain.get("send_failures_total", 0) or 0),
        "h264_sessions_failed": int(chain.get("sessions_failed_total", 0) or 0),
        "h264_overlap_wait_timeouts": int(
            chain.get("overlap_wait_timeouts", 0) or 0
        ),
        # A successful WebSocket write only proves that the command reached
        # the device.  TinyUSB can still reject the actual report while its
        # endpoint is busy, so dynamic KVM acceptance must gate the device-side
        # failure counter as well as the transport and video counters.
        "hid_failed_messages": int(
            hid_stats.get("failed_messages", 0) or 0
        ),
    }


def counter_delta(before: dict[str, int], after: dict[str, int]) -> dict[str, int]:
    return {key: after.get(key, 0) - before.get(key, 0) for key in before}


def error_counter_view(counters: dict[str, int]) -> dict[str, int]:
    """Return only classified counters that participate in error gating."""
    return {
        key: value
        for key, value in counters.items()
        if key not in NON_ERROR_COUNTER_KEYS
    }


def error_counters_clean(delta: dict[str, int]) -> bool:
    """Gate classified failures while retaining source counters as evidence."""
    return all(value == 0 for value in error_counter_view(delta).values())


def runtime_error_snapshot(payload: dict[str, Any]) -> dict[str, Any]:
    video = video_status(payload)
    h264 = video.get("h264_runtime", {})
    if not isinstance(h264, dict):
        h264 = {}
    return {
        "video_last_error": str(video.get("last_error", "") or ""),
        "resource_error": str(h264.get("resource_error", "") or ""),
        "restore_error": str(h264.get("restore_error", "") or ""),
        "teardown_poisoned": bool(h264.get("teardown_poisoned")),
    }


def runtime_error_checks(errors: dict[str, Any]) -> dict[str, bool]:
    return {
        "no_video_error": not errors.get("video_last_error"),
        "resource_ok": errors.get("resource_error") == "ESP_OK",
        "restore_ok": errors.get("restore_error") == "ESP_OK",
        "teardown_clean": not bool(errors.get("teardown_poisoned")),
    }


def mode_activation_runtime_error_checks(
    errors: dict[str, Any],
) -> dict[str, bool]:
    """Validate runtime state before the first H.264 resource probe.

    ``ESP_ERR_NOT_FINISHED`` is the H.264 backend's documented initial
    resource state: mode activation deliberately happens before opening the
    H.264 WebSocket.  Any actual resource-acquisition failure, UVC error,
    restore error, or poisoned teardown remains fatal.
    """
    return {
        "no_video_error": not errors.get("video_last_error"),
        "resource_not_failed": errors.get("resource_error")
        in {"ESP_OK", "ESP_ERR_NOT_FINISHED"},
        "restore_ok": errors.get("restore_error") == "ESP_OK",
        "teardown_clean": not bool(errors.get("teardown_poisoned")),
    }


def reference_workspace_checks(
    h264: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, bool]]:
    """Require the latency-critical encoder reference workspace in SRAM.

    A PSRAM fallback can still produce valid Annex-B video, but it is the
    known degraded path under investigation and must never satisfy the Stable
    H.264 acceptance gate.
    """
    resources = h264.get("resources", {}) if isinstance(h264, dict) else {}
    if not isinstance(resources, dict):
        resources = {}
    required = parse_positive_int(
        resources.get("reference_workspace_required_bytes")
    )
    capacity = parse_positive_int(
        resources.get("reference_workspace_capacity_bytes")
    )
    evidence = {
        "reserve_attempted": bool(
            resources.get("reference_workspace_reserve_attempted")
        ),
        "reserved": bool(resources.get("reference_workspace_reserved")),
        "residency": str(
            resources.get("reference_workspace_residency", "none") or "none"
        ),
        "required_bytes": required,
        "capacity_bytes": capacity,
        "error": str(
            resources.get("reference_workspace_error", "") or ""
        ),
    }
    checks = {
        "reserve_attempted": evidence["reserve_attempted"],
        "reserved": evidence["reserved"],
        "internal": evidence["residency"] == "internal",
        "capacity": required is not None
        and required > 0
        and capacity is not None
        and capacity >= required,
        "error": evidence["error"] == "ESP_OK",
    }
    return evidence, checks


def post_release_runtime_error_checks(
    payload: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, bool]]:
    """Validate runtime errors after releasing the measured KVM lease.

    A product configured to stop capture when no owner remains reports the
    exact informational state ``video capture idle``.  It is accepted only
    when control and video state prove that the lease release actually stopped
    capture.  Other messages (including UVC underflow/return failures) are
    never exempted, and their counters are gated independently by the caller.
    """
    video = video_status(payload)
    control = video.get("control", {})
    if not isinstance(control, dict):
        control = {}
    errors = runtime_error_snapshot(payload)
    expected_idle_stop = (
        not bool(control.get("kvm_active"))
        and not bool(control.get("active_enabled"))
        and not bool(video.get("capture_enabled"))
        and not bool(video.get("streaming"))
        and not bool(video.get("frame_ready"))
    )
    checks = runtime_error_checks(errors)
    checks["no_video_error"] = (
        not errors["video_last_error"]
        or (
            errors["video_last_error"] == "video capture idle"
            and expected_idle_stop
        )
    )
    checks["idle_error_matches_stopped_capture"] = (
        errors["video_last_error"] != "video capture idle"
        or expected_idle_stop
    )
    return errors, checks


def memory_snapshot(payload: dict[str, Any]) -> dict[str, dict[str, int]]:
    performance = payload.get("performance", {})
    memory = performance.get("memory", {}) if isinstance(performance, dict) else {}
    if not isinstance(memory, dict):
        return {}
    result: dict[str, dict[str, int]] = {}
    for region in ("heap", "internal", "psram"):
        source = memory.get(region, {})
        if not isinstance(source, dict):
            continue
        values: dict[str, int] = {}
        for field in ("free_bytes", "minimum_free_bytes", "largest_free_block"):
            value = source.get(field)
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                values[field] = int(value)
        if values:
            result[region] = values
    return result


def memory_drift(
    baseline: dict[str, dict[str, int]], sample: dict[str, dict[str, int]]
) -> dict[str, dict[str, int]]:
    """Return sample-minus-baseline deltas for fields supplied by /api/status."""
    result: dict[str, dict[str, int]] = {}
    for region, baseline_fields in baseline.items():
        sample_fields = sample.get(region, {})
        deltas = {
            field: sample_fields[field] - baseline_value
            for field, baseline_value in baseline_fields.items()
            if field in sample_fields
        }
        if deltas:
            result[region] = deltas
    return result


def uint32_forward_delta(previous: int, current: int) -> int | None:
    delta = (current - previous) & 0xFFFFFFFF
    if delta == 0 or delta > 0x7FFFFFFF:
        return None
    return delta


def cadence_summary(deltas_ms: list[int]) -> dict[str, Any]:
    if not deltas_ms:
        return {
            "samples": 0,
            "mean_delta_ms": 0.0,
            "median_delta_ms": 0.0,
            "p95_delta_ms": 0,
            "max_delta_ms": 0,
            "fps": 0.0,
        }
    ordered = sorted(deltas_ms)
    p95_index = min(len(ordered) - 1, int((len(ordered) - 1) * 0.95 + 0.5))
    mean_delta = statistics.fmean(deltas_ms)
    return {
        "samples": len(deltas_ms),
        "mean_delta_ms": round(mean_delta, 3),
        "median_delta_ms": round(statistics.median(deltas_ms), 3),
        "p95_delta_ms": ordered[p95_index],
        "max_delta_ms": max(deltas_ms),
        "fps": round(1000.0 / mean_delta, 3) if mean_delta > 0 else 0.0,
    }


def fixed_window_fps(arrivals: list[float], window_s: float = 2.0) -> list[float]:
    if len(arrivals) < 2:
        return []
    first = arrivals[0]
    last = arrivals[-1]
    windows: list[float] = []
    start = first
    while start + window_s <= last:
        count = sum(start <= arrival < start + window_s for arrival in arrivals)
        windows.append(count / window_s)
        start += window_s
    return windows


def parse_positive_int(value: Any) -> int | None:
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return None
    return parsed if parsed >= 0 else None


def parse_frame_rate(value: Any) -> float | None:
    """Parse ffprobe's rational frame-rate string without accepting zero/NaN."""
    try:
        numerator_text, denominator_text = str(value).split("/", 1)
        numerator = float(numerator_text)
        denominator = float(denominator_text)
        rate = numerator / denominator
    except (TypeError, ValueError, ZeroDivisionError):
        return None
    return rate if rate > 0 and rate < float("inf") else None


def parse_vui_frame_rates(trace_output: str) -> tuple[list[float], list[int]]:
    """Extract fixed progressive frame rates from FFmpeg ``trace_headers``.

    Raw Annex-B H.264 has no container timestamps.  FFprobe therefore reports
    its demuxer default (commonly ``avg_frame_rate=25/1``), even when the SPS
    VUI correctly declares another cadence.  The H.264 timing contract is
    ``fps = time_scale / (2 * num_units_in_tick)`` for the progressive,
    fixed-frame-rate streams emitted by ExoAnchor, so validate those SPS fields
    directly instead of treating the demuxer default as bitstream metadata.
    """
    num_units_in_tick: int | None = None
    time_scale: int | None = None
    rates: list[float] = []
    fixed_flags: list[int] = []

    for line in trace_output.splitlines():
        try:
            value = int(line.rsplit("=", 1)[1].strip())
        except (IndexError, ValueError):
            continue
        if "num_units_in_tick" in line:
            num_units_in_tick = value
        elif "time_scale" in line:
            time_scale = value
        elif "fixed_frame_rate_flag" in line:
            fixed_flags.append(value)
            if num_units_in_tick and time_scale:
                rates.append(time_scale / (2.0 * num_units_in_tick))
            num_units_in_tick = None
            time_scale = None

    return rates, fixed_flags


def decode_annex_b(
    path: Path, expected: Mode, expected_frames: int, timeout: float
) -> dict[str, Any]:
    ffmpeg = shutil.which("ffmpeg")
    ffprobe = shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        return {"available": False, "passed": False, "error": "ffmpeg/ffprobe unavailable"}
    try:
        probe = subprocess.run(
            [
                ffprobe,
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-count_frames",
                "-show_entries",
                "stream=codec_name,width,height,r_frame_rate,avg_frame_rate,nb_read_frames",
                "-of",
                "json",
                str(path),
            ],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        return {
            "available": True,
            "passed": False,
            "error": f"ffprobe timed out after {error.timeout}s",
        }
    streams: list[dict[str, Any]] = []
    if probe.returncode == 0:
        try:
            streams = json.loads(probe.stdout).get("streams", [])
        except (ValueError, AttributeError):
            streams = []
    try:
        decode = subprocess.run(
            [
                ffmpeg,
                "-v",
                "error",
                "-nostats",
                "-progress",
                "pipe:1",
                "-f",
                "h264",
                "-i",
                str(path),
                "-map",
                "0:v:0",
                "-f",
                "null",
                os.devnull,
            ],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        return {
            "available": True,
            "passed": False,
            "error": f"ffmpeg timed out after {error.timeout}s",
            "probe_error": probe.stderr.strip()[-500:],
        }
    try:
        headers = subprocess.run(
            [
                ffmpeg,
                "-hide_banner",
                "-v",
                "verbose",
                "-f",
                "h264",
                "-i",
                str(path),
                "-map",
                "0:v:0",
                "-c:v",
                "copy",
                "-frames:v",
                "1",
                "-bsf:v",
                "trace_headers",
                "-f",
                "null",
                os.devnull,
            ],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        return {
            "available": True,
            "passed": False,
            "error": f"ffmpeg trace_headers timed out after {error.timeout}s",
            "probe_error": probe.stderr.strip()[-500:],
            "decode_error": decode.stderr.strip()[-500:],
        }
    stream = streams[0] if streams else {}
    ffprobe_frames = parse_positive_int(stream.get("nb_read_frames"))
    vui_rates, vui_fixed_flags = parse_vui_frame_rates(headers.stderr)
    unique_vui_rates = sorted({round(rate, 6) for rate in vui_rates})
    declared_fps = unique_vui_rates[0] if len(unique_vui_rates) == 1 else None
    vui_fixed = bool(vui_fixed_flags) and all(flag == 1 for flag in vui_fixed_flags)
    expected_fps = expected.promised_h264_fps
    progress_frames = [
        parse_positive_int(line.split("=", 1)[1])
        for line in decode.stdout.splitlines()
        if line.startswith("frame=")
    ]
    ffmpeg_frames = next(
        (value for value in reversed(progress_frames) if value is not None), None
    )
    passed = (
        probe.returncode == 0
        and decode.returncode == 0
        and headers.returncode == 0
        and stream.get("codec_name") == "h264"
        and int(stream.get("width", 0)) == expected.width
        and int(stream.get("height", 0)) == expected.height
        and declared_fps is not None
        and vui_fixed
        and abs(declared_fps - expected_fps) <= 0.01
        and ffprobe_frames == expected_frames
        and ffmpeg_frames == expected_frames
    )
    return {
        "available": True,
        "passed": passed,
        "codec": stream.get("codec_name"),
        "width": stream.get("width"),
        "height": stream.get("height"),
        "r_frame_rate": stream.get("r_frame_rate"),
        # Diagnostic only: for raw Annex-B this is the demuxer's input default,
        # not the SPS/VUI timing declaration used by the pass/fail gate.
        "avg_frame_rate": stream.get("avg_frame_rate"),
        "raw_demuxer_avg_frame_rate": stream.get("avg_frame_rate"),
        "declared_fps": round(declared_fps, 3) if declared_fps is not None else None,
        "vui_frame_rates": unique_vui_rates,
        "vui_fixed_frame_rate_flags": vui_fixed_flags,
        "expected_fps": expected_fps,
        "expected_frames": expected_frames,
        "ffprobe_frames": ffprobe_frames,
        "ffmpeg_frames": ffmpeg_frames,
        "probe_error": probe.stderr.strip()[-500:],
        "decode_error": decode.stderr.strip()[-500:],
        "headers_error": headers.stderr.strip()[-500:],
    }


def analyze_dynamic_source(
    path: Path,
    timeout: float,
    min_mean_mad: float,
    min_changed_pixels_percent: float,
) -> dict[str, Any]:
    """Prove that a HID-drag capture contains sustained scene motion.

    Transport cadence alone cannot distinguish a real window drag from a
    stationary desktop whose only changes are the mouse pointer, clock, or a
    selection rectangle.  Decode the already captured Annex-B stream to a
    bounded 5 FPS grayscale thumbnail sequence, then measure consecutive-frame
    mean absolute difference (MAD) and the percentage of pixels whose luma
    changes by at least ``MOTION_CHANGED_PIXEL_DELTA``.  Both mean thresholds
    must pass so one small but high-contrast UI element cannot create a false
    dynamic-source result.
    """
    evidence: dict[str, Any] = {
        "enabled": True,
        "available": False,
        "passed": False,
        "sample_fps": MOTION_SAMPLE_FPS,
        "sample_width": MOTION_SAMPLE_WIDTH,
        "sample_height": MOTION_SAMPLE_HEIGHT,
        "changed_pixel_delta": MOTION_CHANGED_PIXEL_DELTA,
        "minimum_comparisons": MOTION_MIN_COMPARISONS,
        "thresholds": {
            "min_mean_mad": min_mean_mad,
            "min_mean_changed_pixels_percent": min_changed_pixels_percent,
        },
    }
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        evidence["error"] = "ffmpeg unavailable for dynamic-source analysis"
        return evidence
    filter_graph = (
        f"fps={MOTION_SAMPLE_FPS},"
        f"scale={MOTION_SAMPLE_WIDTH}:{MOTION_SAMPLE_HEIGHT}:flags=area,"
        "format=gray"
    )
    try:
        decode = subprocess.run(
            [
                ffmpeg,
                "-v",
                "error",
                "-nostdin",
                "-f",
                "h264",
                "-i",
                str(path),
                "-an",
                "-sn",
                "-dn",
                "-vf",
                filter_graph,
                "-pix_fmt",
                "gray",
                "-f",
                "rawvideo",
                "pipe:1",
            ],
            capture_output=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        evidence["available"] = True
        evidence["error"] = (
            f"dynamic-source ffmpeg timed out after {error.timeout}s"
        )
        return evidence
    raw = decode.stdout if isinstance(decode.stdout, bytes) else b""
    stderr = decode.stderr
    if isinstance(stderr, bytes):
        stderr_text = stderr.decode("utf-8", errors="replace")
    else:
        stderr_text = str(stderr or "")
    frame_bytes = MOTION_SAMPLE_WIDTH * MOTION_SAMPLE_HEIGHT
    frame_count, trailing_bytes = divmod(len(raw), frame_bytes)
    comparisons = max(0, frame_count - 1)
    mad_values: list[float] = []
    changed_values: list[float] = []
    for index in range(comparisons):
        previous_start = index * frame_bytes
        current_start = previous_start + frame_bytes
        previous = raw[previous_start:current_start]
        current = raw[current_start:current_start + frame_bytes]
        absolute_differences = [
            abs(current_value - previous_value)
            for previous_value, current_value in zip(previous, current)
        ]
        mad_values.append(sum(absolute_differences) / frame_bytes)
        changed_values.append(
            100.0
            * sum(
                difference >= MOTION_CHANGED_PIXEL_DELTA
                for difference in absolute_differences
            )
            / frame_bytes
        )

    def summarize(values: list[float]) -> dict[str, float]:
        if not values:
            return {"mean": 0.0, "p95": 0.0, "max": 0.0}
        ordered = sorted(values)
        p95_index = min(
            len(ordered) - 1,
            int((len(ordered) - 1) * 0.95 + 0.5),
        )
        return {
            "mean": round(statistics.fmean(values), 6),
            "p95": round(ordered[p95_index], 6),
            "max": round(ordered[-1], 6),
        }

    mad = summarize(mad_values)
    changed = summarize(changed_values)
    checks = {
        "decoder_exit": decode.returncode == 0,
        "frame_alignment": trailing_bytes == 0,
        "comparisons": comparisons >= MOTION_MIN_COMPARISONS,
        "mean_mad": mad["mean"] >= min_mean_mad,
        "mean_changed_pixels": (
            changed["mean"] >= min_changed_pixels_percent
        ),
    }
    evidence.update(
        {
            "available": True,
            "decoded_frames": frame_count,
            "comparisons": comparisons,
            "trailing_bytes": trailing_bytes,
            "mad": mad,
            "changed_pixels_percent": changed,
            "decode_error": stderr_text.strip()[-500:],
            "checks": checks,
            "passed": all(checks.values()),
        }
    )
    return evidence


def wait_for_restore(
    session: requests.Session, host: str, timeout_s: float
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    latest: dict[str, Any] = {}
    while time.monotonic() < deadline:
        latest = status(session, host)
        h264 = video_status(latest).get("h264_runtime", {})
        if not h264.get("session_active") and not h264.get("restore_pending"):
            return latest
        time.sleep(0.1)
    return latest


def wait_for_post_release_lifecycle(
    session: requests.Session, host: str, timeout_s: float
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Observe the independent UVC lifecycle after the measured lease ends.

    Depending on product settings, release may leave capture on for
    preview/always-on, switch it to another profile, or stop it as idle.  The
    caller gates the settled state, error counters, and context-aware runtime
    errors so this transition cannot hide a UVC failure.
    """
    transient_errors = {
        "UVC stream restarting",
        "UVC transport restarting",
        "switching video capture",
    }
    started = time.monotonic()
    deadline = started + timeout_s
    latest: dict[str, Any] = {}
    settled = False
    stable_samples = 0
    while time.monotonic() < deadline:
        latest = status(session, host)
        video = video_status(latest)
        control = video.get("control", {})
        if not isinstance(control, dict):
            control = {}
        error = str(video.get("last_error", "") or "")
        active_enabled = bool(control.get("active_enabled"))
        active_owner = str(control.get("active_owner", "") or "")
        if active_enabled:
            lifecycle_matches = (
                not bool(control.get("kvm_active"))
                and bool(video.get("capture_enabled"))
                and bool(video.get("streaming"))
                and bool(video.get("frame_ready"))
                and str(video.get("capture_owner", "") or "") == active_owner
                and not error
            )
        else:
            lifecycle_matches = (
                not bool(control.get("kvm_active"))
                and not bool(video.get("capture_enabled"))
                and not bool(video.get("streaming"))
                and error in {"", "video capture idle"}
            )
        # Observe long enough to include the supervisor's deliberate UVC stop
        # or profile switch, then require two consecutive settled samples.
        if (
            time.monotonic() - started >= 0.75
            and error not in transient_errors
            and lifecycle_matches
        ):
            stable_samples += 1
        else:
            stable_samples = 0
        if stable_samples >= 2:
            settled = True
            break
        time.sleep(0.1)
    return latest, {
        "settled": settled,
        "elapsed_ms": round((time.monotonic() - started) * 1000),
        "timeout_s": timeout_s,
    }


def run_cycle(
    session: requests.Session,
    host: str,
    cookie: str,
    stream_id: int,
    mode: Mode,
    duration: float,
    min_mean_ratio: float,
    min_window_ratio: float,
    first_au_max_ms: int,
    require_ffmpeg: bool,
    restore_timeout: float,
    post_release_timeout: float,
    hid_drag_start: tuple[int, int] | None,
    hid_drag_end: tuple[int, int] | None,
    hid_drag_seconds: float,
    hid_drag_step_ms: int,
    motion_min_mean_mad: float,
    motion_min_changed_pixels_percent: float,
    capture_path: Path | None,
) -> dict[str, Any]:
    before_payload = status(session, host)
    before = counter_snapshot(before_payload)
    before_memory = memory_snapshot(before_payload)
    baseline_captured_at = time.monotonic()
    lease_active = True
    sock: socket.socket | None = None
    reader: BufferedSocket | None = None
    annex_path: Path | None = None
    active_payload: dict[str, Any] = {}
    restored_payload: dict[str, Any] = {}
    post_release_payload: dict[str, Any] = {}
    post_release_lifecycle: dict[str, Any] = {}
    close_handshake: dict[str, Any] = {}
    hid_sock: socket.socket | None = None
    hid_thread: threading.Thread | None = None
    hid_stream_started = threading.Event()
    hid_stop_requested = threading.Event()
    hid_drag_result: dict[str, Any] = {
        "enabled": hid_drag_start is not None and hid_drag_end is not None,
        "start": list(hid_drag_start) if hid_drag_start else None,
        "end": list(hid_drag_end) if hid_drag_end else None,
        "requested_seconds": hid_drag_seconds,
        "step_ms": hid_drag_step_ms,
        "connected": False,
        "started": False,
        "completed": False,
        "actions_sent": 0,
        "cleanup_ok": False,
        "cleanup_errors": [],
    }
    started = time.monotonic()
    try:
        websocket_connect_started_at = time.monotonic()
        sock, reader = websocket_connect(host, cookie, stream_id)
        if hid_drag_result["enabled"]:
            try:
                hid_sock = hid_websocket_connect(host, cookie, stream_id)
                hid_drag_result["connected"] = True
                assert hid_drag_start is not None and hid_drag_end is not None
                hid_thread = threading.Thread(
                    target=hid_drag_worker,
                    name=f"h264-hid-drag-{stream_id}",
                    args=(
                        hid_sock,
                        hid_drag_start,
                        hid_drag_end,
                        hid_drag_seconds,
                        hid_drag_step_ms,
                        hid_stream_started,
                        hid_stop_requested,
                        hid_drag_result,
                    ),
                    daemon=True,
                )
                hid_thread.start()
            except (OSError, RuntimeError) as error:
                hid_drag_result["error"] = f"HID WebSocket setup failed: {error}"
        deadline = time.monotonic() + duration
        next_heartbeat = time.monotonic() + 2.0
        frames = 0
        total_bytes = 0
        first_au_ms: int | None = None
        first_sequence: int | None = None
        previous_sequence: int | None = None
        sequence_gaps = 0
        sequence_discontinuities = 0
        types: set[int] = set()
        timestamps: list[int] = []
        pts_deltas_ms: list[int] = []
        pts_non_monotonic = 0
        previous_pts_ms: int | None = None
        arrivals: list[float] = []
        observed_sizes: set[tuple[int, int]] = set()
        max_au_bytes = 0
        with tempfile.NamedTemporaryFile(
            prefix="exoanchor-h264-", suffix=".h264", delete=False
        ) as annex_file:
            annex_path = Path(annex_file.name)
            while time.monotonic() < deadline:
                if time.monotonic() >= next_heartbeat:
                    set_lease(session, host, stream_id, True)
                    next_heartbeat = time.monotonic() + 2.0
                opcode, packet = reader.read_frame()
                if opcode == 8:
                    break
                if opcode != 2 or len(packet) < 24 or packet[:4] != b"EAH1":
                    continue
                (
                    _magic,
                    version,
                    _flags,
                    header_size,
                    width,
                    height,
                    sequence,
                    pts_ms,
                    payload_len,
                ) = struct.unpack_from("<4sBBHHHIII", packet, 0)
                if version != 1 or header_size != 24 or payload_len != len(packet) - 24:
                    raise RuntimeError("invalid EAH1 framing")
                if previous_sequence is not None:
                    sequence_delta = uint32_forward_delta(previous_sequence, sequence)
                    if sequence_delta is None:
                        sequence_discontinuities += 1
                    elif sequence_delta != 1:
                        sequence_gaps += sequence_delta - 1
                if first_sequence is None:
                    first_sequence = sequence
                previous_sequence = sequence
                if previous_pts_ms is not None:
                    pts_delta = uint32_forward_delta(previous_pts_ms, pts_ms)
                    if pts_delta is None:
                        pts_non_monotonic += 1
                    else:
                        pts_deltas_ms.append(pts_delta)
                previous_pts_ms = pts_ms
                payload = packet[24:]
                annex_file.write(payload)
                arrival = time.monotonic()
                arrivals.append(arrival)
                hid_stream_started.set()
                frames += 1
                total_bytes += payload_len
                max_au_bytes = max(max_au_bytes, payload_len)
                timestamps.append(pts_ms)
                observed_sizes.add((width, height))
                types.update(nal_types(payload))
                if first_au_ms is None:
                    first_au_ms = int((arrival - started) * 1000)
        if hid_thread is not None:
            hid_stop_requested.set()
            hid_thread.join(timeout=max(2.0, hid_drag_step_ms / 1000.0 + 1.0))
            if hid_thread.is_alive():
                hid_drag_result["error"] = "HID drag worker did not stop"
                hid_drag_result["cleanup_ok"] = False
        if hid_sock is not None:
            try:
                websocket_send_close(hid_sock)
            except OSError as error:
                hid_drag_result.setdefault("close_error", str(error))
            hid_sock.close()
            hid_sock = None
        active_payload = status(session, host)
        websocket_send_close(sock)
        close_handshake = websocket_wait_for_peer_close(sock, reader)
        restored_payload = wait_for_restore(session, host, restore_timeout)
        if sock is not None:
            sock.close()
            sock = None
        set_lease(session, host, stream_id, False)
        lease_active = False
        post_release_payload, post_release_lifecycle = (
            wait_for_post_release_lifecycle(
                session, host, post_release_timeout
            )
        )
        steady_elapsed = arrivals[-1] - arrivals[0] if len(arrivals) > 1 else 0.0
        steady_fps = (len(arrivals) - 1) / steady_elapsed if steady_elapsed > 0 else 0.0
        window_fps = fixed_window_fps(arrivals)
        target_fps = mode.promised_h264_fps
        pts_cadence = cadence_summary(pts_deltas_ms)
        active_video = video_status(active_payload)
        h264 = active_video.get("h264_runtime", {})
        resources = h264.get("resources", {}) if isinstance(h264, dict) else {}
        chain = h264.get("chain_metrics", {}) if isinstance(h264, dict) else {}
        ffmpeg_result = decode_annex_b(
            annex_path, mode, frames, max(30.0, duration * 3.0)
        ) if annex_path else {"available": False, "passed": False}
        dynamic_source = {
            "enabled": False,
            "available": False,
            "passed": True,
            "reason": "HID drag disabled",
        }
        if hid_drag_result["enabled"]:
            dynamic_source = (
                analyze_dynamic_source(
                    annex_path,
                    max(30.0, duration * 3.0),
                    motion_min_mean_mad,
                    motion_min_changed_pixels_percent,
                )
                if annex_path
                else {
                    "enabled": True,
                    "available": False,
                    "passed": False,
                    "error": "captured Annex-B stream unavailable",
                }
            )
        active_errors = runtime_error_snapshot(active_payload)
        active_error_checks = runtime_error_checks(active_errors)
        active_memory = memory_snapshot(active_payload)
        reference_workspace, reference_workspace_gate = (
            reference_workspace_checks(h264)
        )
        checks = {
            "wire_size": observed_sizes == {(mode.width, mode.height)},
            "nal_contract": {1, 5, 7, 8}.issubset(types) and 0 not in types,
            "sequence": sequence_gaps == 0 and sequence_discontinuities == 0,
            "pts_monotonic": pts_non_monotonic == 0
            and len(pts_deltas_ms) == max(0, frames - 1),
            "pts_cadence": pts_cadence["fps"]
            >= target_fps * min_mean_ratio
            and pts_cadence["fps"] <= target_fps * MAX_FPS_RATIO,
            "pts_arrival_agreement": steady_fps > 0
            and steady_fps * min_window_ratio
            <= pts_cadence["fps"]
            <= steady_fps / min_window_ratio,
            "first_au": first_au_ms is not None and first_au_ms <= first_au_max_ms,
            "mean_fps": steady_fps >= target_fps * min_mean_ratio
            and steady_fps <= target_fps * MAX_FPS_RATIO,
            "window_fps": bool(window_fps)
            and min(window_fps) >= target_fps * min_window_ratio
            and max(window_fps) <= target_fps * MAX_FPS_RATIO,
            "runtime_available": bool(h264.get("available")),
            "runtime_active": bool(h264.get("session_active")),
            "pipeline_ready": bool(h264.get("pipeline_ready")),
            "runtime_target_fps": int(h264.get("target_fps", 0) or 0)
            == int(target_fps),
            "reference_workspace": all(reference_workspace_gate.values()),
            "dual_yuv": int(resources.get("yuv_surfaces", 0) or 0) >= 2,
            "dual_au": int(resources.get("au_slots", 0) or 0) >= 2,
            "overlap": bool(resources.get("overlap_enabled")),
            "no_serial_fallback": not bool(resources.get("serial_fallback", True)),
            "chain_metrics": bool(chain.get("valid")),
            "runtime_errors": all(active_error_checks.values()),
            "ffmpeg": bool(ffmpeg_result.get("passed"))
            if require_ffmpeg
            else (not ffmpeg_result.get("available") or bool(ffmpeg_result.get("passed"))),
        }
        if hid_drag_result["enabled"]:
            hid_drag_checks = {
                "connected": bool(hid_drag_result.get("connected")),
                "started": bool(hid_drag_result.get("started")),
                "completed": bool(hid_drag_result.get("completed")),
                "actions_sent": int(hid_drag_result.get("actions_sent", 0)) >= 4,
                "cleanup": bool(hid_drag_result.get("cleanup_ok")),
                "no_error": not str(hid_drag_result.get("error", "") or ""),
            }
            hid_drag_result["checks"] = hid_drag_checks
            hid_drag_result["passed"] = all(hid_drag_checks.values())
            checks["hid_drag"] = hid_drag_result["passed"]
            checks["dynamic_source"] = bool(dynamic_source.get("passed"))
        return {
            "stream_id": stream_id,
            "mode": mode.label,
            "active_baseline": {
                "phase": "after_mode_lifecycle_settle_before_websocket",
                "counters": before,
                "to_websocket_connect_ms": round(
                    (websocket_connect_started_at - baseline_captured_at) * 1000
                ),
            },
            "target_h264_fps": target_fps,
            "frames": frames,
            "steady_elapsed_s": round(steady_elapsed, 3),
            "steady_fps": round(steady_fps, 3),
            "window_fps": [round(value, 3) for value in window_fps],
            "min_window_fps": round(min(window_fps), 3) if window_fps else 0.0,
            "mbps": round(
                total_bytes * 8 / steady_elapsed / 1_000_000, 3
            ) if steady_elapsed else 0.0,
            "first_au_ms": first_au_ms,
            "sequence_gaps": sequence_gaps,
            "sequence_discontinuities": sequence_discontinuities,
            "sequence_first": first_sequence,
            "sequence_last": previous_sequence,
            "nal_types": sorted(types),
            "timestamp_first_ms": timestamps[0] if timestamps else None,
            "timestamp_last_ms": timestamps[-1] if timestamps else None,
            "timestamp_span_ms": sum(pts_deltas_ms),
            "pts_non_monotonic": pts_non_monotonic,
            "pts_cadence": pts_cadence,
            "max_au_bytes": max_au_bytes,
            "observed_sizes": sorted(f"{width}x{height}" for width, height in observed_sizes),
            "resources": resources,
            "reference_workspace": reference_workspace,
            "reference_workspace_checks": reference_workspace_gate,
            "chain_metrics": chain,
            "runtime_errors": active_errors,
            "runtime_error_checks": active_error_checks,
            "close_handshake": close_handshake,
            "hid_drag": hid_drag_result,
            "dynamic_source": dynamic_source,
            "annex_b_path": str(capture_path) if capture_path is not None else None,
            "memory": {
                "baseline": before_memory,
                "active": active_memory,
                "active_drift": memory_drift(before_memory, active_memory),
            },
            "ffmpeg": ffmpeg_result,
            "checks": checks,
            "passed": all(checks.values()),
            "_before": before,
            "_before_memory": before_memory,
            "_active_payload": active_payload,
            "_restored": restored_payload,
            "_post_release": post_release_payload,
            "_post_release_lifecycle": post_release_lifecycle,
        }
    finally:
        hid_stop_requested.set()
        if hid_thread is not None and hid_thread.is_alive():
            hid_thread.join(timeout=max(2.0, hid_drag_step_ms / 1000.0 + 1.0))
        if hid_sock is not None:
            # The worker owns the normal mouse-up/release-all path.  If it did
            # not finish, make one final best-effort release on the exact
            # stream-bound HID socket before closing it and releasing video.
            try:
                websocket_send_text(hid_sock, {"type": "absmouseup", "button": 0})
                websocket_send_text(hid_sock, {"type": "releaseall"})
            except OSError:
                pass
            try:
                websocket_send_close(hid_sock)
            except OSError:
                pass
            hid_sock.close()
        if sock is not None:
            sock.close()
        if lease_active:
            try:
                set_lease(session, host, stream_id, False)
            except requests.RequestException:
                pass
        if annex_path is not None:
            if capture_path is not None and annex_path.exists():
                capture_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(annex_path, capture_path)
            annex_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True, help="exact target IPv4 address")
    parser.add_argument("--username", default="admin")
    parser.add_argument(
        "--mode",
        action="append",
        type=parse_mode,
        default=[],
        help="repeatable capture mode WIDTHxHEIGHT@FPS",
    )
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--min-mean-ratio", type=float, default=0.94)
    parser.add_argument("--min-window-ratio", type=float, default=0.80)
    parser.add_argument("--first-au-max-ms", type=int, default=6500)
    parser.add_argument("--mode-timeout", type=float, default=12.0)
    parser.add_argument("--restore-timeout", type=float, default=8.0)
    parser.add_argument("--post-release-timeout", type=float, default=4.0)
    parser.add_argument("--require-ffmpeg", action="store_true")
    parser.add_argument(
        "--force-lease",
        action="store_true",
        help=(
            "force only the initial KVM lease claim when a controlled HIL run "
            "must replace an existing browser lease"
        ),
    )
    parser.add_argument(
        "--hid-drag-start",
        type=parse_hid_point,
        help="optional KVM title-bar HID point X,Y in the 0..32767 range",
    )
    parser.add_argument(
        "--hid-drag-end",
        type=parse_hid_point,
        help="opposite HID point X,Y for the bounded drag rectangle",
    )
    parser.add_argument("--hid-drag-seconds", type=float, default=45.0)
    parser.add_argument("--hid-drag-step-ms", type=int, default=32)
    parser.add_argument(
        "--motion-min-mean-mad",
        type=float,
        default=0.5,
        help=(
            "minimum mean consecutive-frame grayscale MAD required when HID "
            "drag is enabled"
        ),
    )
    parser.add_argument(
        "--motion-min-changed-pct",
        type=float,
        default=1.0,
        help=(
            "minimum mean percentage of changed grayscale pixels required "
            "when HID drag is enabled"
        ),
    )
    parser.add_argument(
        "--capture-dir",
        type=Path,
        help="optional directory for preserving each received Annex-B stream",
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.cycles < 1 or args.duration < 6:
        parser.error("cycles must be >=1 and duration must be >=6 seconds")
    if not 0 < args.min_mean_ratio <= 1 or not 0 < args.min_window_ratio <= 1:
        parser.error("FPS ratios must be in (0, 1]")
    if (
        args.mode_timeout <= 0
        or args.restore_timeout <= 0
        or args.post_release_timeout <= 0
    ):
        parser.error("mode, restore and post-release timeouts must be positive")
    if (args.hid_drag_start is None) != (args.hid_drag_end is None):
        parser.error("--hid-drag-start and --hid-drag-end must be used together")
    if args.hid_drag_start is not None:
        if args.hid_drag_start == args.hid_drag_end:
            parser.error("HID drag start and end must differ")
        if args.hid_drag_seconds <= 0 or args.hid_drag_seconds > args.duration - 2:
            parser.error("HID drag seconds must be positive and leave 2 seconds in the cycle")
        if not 10 <= args.hid_drag_step_ms <= 1000:
            parser.error("HID drag step must be between 10 and 1000 ms")
        if args.motion_min_mean_mad <= 0:
            parser.error("motion MAD threshold must be positive")
        if not 0 < args.motion_min_changed_pct <= 100:
            parser.error("motion changed-pixel threshold must be in (0, 100]")
    modes = args.mode or [Mode(1920, 1080, 25.0), Mode(1280, 720, 30.0)]
    unsupported_modes = [mode.label for mode in modes if not mode.h264_supported]
    if unsupported_modes:
        parser.error(
            "H.264 validation supports only 1080p25 / 720p30 or lower: "
            + ", ".join(unsupported_modes)
        )

    password = sys.stdin.readline().rstrip("\r\n")
    if not password:
        raise SystemExit("password must be supplied on stdin")
    session = login(args.host, args.username, password)
    password = ""
    cookie = session.cookies.get("EA_SESSION")
    if not cookie:
        raise RuntimeError("login did not return EA_SESSION")

    report: dict[str, Any] = {
        "schema": "exoanchor.h264_acceptance.v1",
        "host": args.host,
        "started_unix": int(time.time()),
        "thresholds": {
            "min_mean_ratio": args.min_mean_ratio,
            "min_window_ratio": args.min_window_ratio,
            "max_fps_ratio": MAX_FPS_RATIO,
            "first_au_max_ms": args.first_au_max_ms,
            "mode_timeout_s": args.mode_timeout,
            "restore_timeout_s": args.restore_timeout,
            "post_release_timeout_s": args.post_release_timeout,
            "require_ffmpeg": args.require_ffmpeg,
            "force_lease": args.force_lease,
            "hid_drag_enabled": args.hid_drag_start is not None,
            "hid_drag_seconds": args.hid_drag_seconds,
            "hid_drag_step_ms": args.hid_drag_step_ms,
            "motion_min_mean_mad": args.motion_min_mean_mad,
            "motion_min_changed_pixels_percent": args.motion_min_changed_pct,
        },
        "cycles": [],
    }
    all_passed = True
    for mode in modes:
        set_mode(session, args.host, mode)
        for cycle_number in range(1, args.cycles + 1):
            stream_id, mode_activation = activate_capture_mode(
                session,
                args.host,
                mode,
                args.mode_timeout,
                force_lease=args.force_lease,
            )
            if not mode_activation["passed"]:
                result = {
                    "mode": mode.label,
                    "cycle": cycle_number,
                    "mode_activation": mode_activation,
                    "failure": "capture mode did not become active",
                    "passed": False,
                }
                report["cycles"].append(result)
                all_passed = False
                print(
                    json.dumps(result, ensure_ascii=False, sort_keys=True),
                    flush=True,
                )
                break
            result = run_cycle(
                session,
                args.host,
                cookie,
                stream_id,
                mode,
                args.duration,
                args.min_mean_ratio,
                args.min_window_ratio,
                args.first_au_max_ms,
                args.require_ffmpeg,
                args.restore_timeout,
                args.post_release_timeout,
                args.hid_drag_start,
                args.hid_drag_end,
                args.hid_drag_seconds,
                args.hid_drag_step_ms,
                args.motion_min_mean_mad,
                args.motion_min_changed_pct,
                (
                    args.capture_dir
                    / f"{mode.width}x{mode.height}-{mode.capture_fps:g}-cycle{cycle_number}.h264"
                    if args.capture_dir is not None
                    else None
                ),
            )
            before = result.pop("_before")
            before_memory = result.pop("_before_memory")
            active_payload = result.pop("_active_payload")
            restored = result.pop("_restored")
            post_release = result.pop("_post_release")
            post_release_lifecycle = result.pop("_post_release_lifecycle")
            active_counters = counter_snapshot(active_payload)
            active_delta = counter_delta(before, active_counters)
            after = counter_snapshot(restored)
            delta = counter_delta(before, after)
            restore_delta = counter_delta(active_counters, after)
            h264_after = video_status(restored).get("h264_runtime", {})
            restore_errors = runtime_error_snapshot(restored)
            restore_error_state = runtime_error_checks(restore_errors)
            restored_memory = memory_snapshot(restored)
            restore_checks = {
                "restored": not bool(h264_after.get("restore_pending")),
                "session_closed": not bool(h264_after.get("session_active")),
                "teardown_clean": not bool(h264_after.get("teardown_poisoned")),
                "error_counters": error_counters_clean(restore_delta),
                "runtime_errors": all(restore_error_state.values()),
            }
            active_counter_check = error_counters_clean(active_delta)
            post_release_counters = counter_snapshot(post_release)
            post_release_counter_delta = counter_delta(
                after, post_release_counters
            )
            post_release_errors, post_release_error_state = (
                post_release_runtime_error_checks(post_release)
            )
            post_release_checks = {
                "settled": bool(post_release_lifecycle.get("settled")),
                "error_counters": error_counters_clean(
                    post_release_counter_delta
                ),
                "runtime_errors": all(post_release_error_state.values()),
            }
            post_release_video = video_status(post_release)
            post_release_control = post_release_video.get("control", {})
            if not isinstance(post_release_control, dict):
                post_release_control = {}
            result["mode_activation"] = mode_activation
            result["active_counter_delta"] = active_delta
            result["checks"]["error_counters"] = active_counter_check
            result["counter_delta"] = delta
            result["restore_counter_delta"] = restore_delta
            result["runtime_errors_after_restore"] = restore_errors
            result["runtime_error_checks_after_restore"] = restore_error_state
            result["memory"]["restored"] = restored_memory
            result["memory"]["restore_drift"] = memory_drift(
                before_memory, restored_memory
            )
            result["restore_checks"] = restore_checks
            result["post_release_checks"] = post_release_checks
            result["post_release_lifecycle"] = {
                **post_release_lifecycle,
                "counter_delta": post_release_counter_delta,
                "runtime_errors": post_release_errors,
                "runtime_error_checks": post_release_error_state,
                "checks": post_release_checks,
                "passed": all(post_release_checks.values()),
                "control": {
                    "kvm_active": bool(
                        post_release_control.get("kvm_active")
                    ),
                    "active_enabled": bool(
                        post_release_control.get("active_enabled")
                    ),
                    "active_owner": str(
                        post_release_control.get("active_owner", "") or ""
                    ),
                },
                "video": {
                    "capture_enabled": bool(
                        post_release_video.get("capture_enabled")
                    ),
                    "streaming": bool(post_release_video.get("streaming")),
                    "frame_ready": bool(post_release_video.get("frame_ready")),
                    "capture_owner": str(
                        post_release_video.get("capture_owner", "") or ""
                    ),
                    "last_error": str(
                        post_release_video.get("last_error", "") or ""
                    ),
                },
            }
            result["passed"] = bool(
                result["passed"]
                and active_counter_check
                and all(restore_checks.values())
                and all(post_release_checks.values())
            )
            result["cycle"] = cycle_number
            report["cycles"].append(result)
            all_passed = all_passed and result["passed"]
            print(json.dumps(result, ensure_ascii=False, sort_keys=True), flush=True)
            time.sleep(0.25)
    report["passed"] = all_passed
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    print("PASS" if all_passed else "FAIL")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
