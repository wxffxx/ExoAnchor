#!/usr/bin/env python3
"""Guard ownership and cancellation boundaries in the rebuilt video path.

These checks intentionally focus on lifecycle ordering.  They prevent a
future throughput change from reintroducing the two classes of failures that
made the previous path non-deterministic: freeing a UVC object while callbacks
still own frames, and allowing an obsolete browser transport to renew or emit
against a replacement KVM session.
"""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile


PROJECT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (PROJECT / relative).read_text(encoding="utf-8")


def section(text: str, start: str, end: str) -> str:
    first = text.find(start)
    if first < 0:
        return ""
    last = text.find(end, first + len(start))
    return text[first:] if last < 0 else text[first:last]


def ordered(body: str, *markers: str) -> bool:
    cursor = -1
    for marker in markers:
        cursor = body.find(marker, cursor + 1)
        if cursor < 0:
            return False
    return True


def function_section(text: str, name: str) -> str:
    """Return one top-level named JavaScript function through the next one.

    The production page intentionally keeps these functions compact.  Splitting
    at the next named function is more stable than trying to parse nested arrow
    callbacks with a brace counter, while still keeping every contract check
    scoped to the implementation that owns it.
    """
    start = text.find(f"function {name}(")
    if start < 0:
        return ""
    next_function = re.search(r"function\s+[A-Za-z_$][\w$]*\s*\(",
                              text[start + 1:])
    if next_function is None:
        return text[start:]
    return text[start:start + 1 + next_function.start()]


def check_h264_mse_latency_contract(kvm_html: str,
                                    failures: list[str]) -> None:
    """Guard the MSE fallback against content-dependent latency growth.

    Byte and frame counts are not media time: a moving desktop can change H.264
    access-unit sizes by an order of magnitude without changing the amount of
    video waiting to be rendered.  These checks therefore require an explicit
    media-duration budget plus the already-buffered browser lag.
    """
    expected_limits = {
        "H264_MSE_TARGET_LAG_MS": 150,
        "H264_MSE_SOFT_LAG_MS": 250,
        "H264_MSE_HARD_LAG_MS": 500,
        "H264_MSE_SEEK_COOLDOWN_MS": 250,
        "H264_MSE_STARTUP_GRACE_MS": 1500,
        "H264_MSE_TRIM_AFTER_MS": 10000,
        "H264_MSE_TRIM_INTERVAL_MS": 5000,
        "H264_MSE_RETAIN_HISTORY_MS": 3000,
    }
    missing_limits = [
        f"{name}={value}"
        for name, value in expected_limits.items()
        if not re.search(rf"\b{re.escape(name)}\s*=\s*{value}\b", kvm_html)
    ]
    if missing_limits:
        failures.append(
            "H.264 MSE low-latency limits must be explicit: "
            + ", ".join(missing_limits)
        )

    start_mse = function_section(kvm_html, "startMse")
    missing_state = [field for field in (
        "queueDurationMs",
        "inflightDurationMs",
        "inflightBytes",
        "lastSeekAt",
        "lastTrimAt",
        "rendered",
        "latencyArmed",
        "latencyGraceUntil",
        "lastRenderedTime",
    ) if not re.search(rf"\b{field}\s*:\s*(?:0|false)\b", start_mse)]
    if missing_state:
        failures.append(
            "H.264 MSE state must initialize latency fields: "
            + ", ".join(missing_state)
        )
    if not re.search(r'\bsourceOp\s*:\s*"idle"', start_mse):
        failures.append(
            'H.264 MSE state must initialize sourceOp as "idle"'
        )

    lag_snapshot = function_section(kvm_html, "mseLagSnapshot")
    snapshot_markers = (
        "state.queueDurationMs",
        "state.inflightDurationMs",
        "h264MseVideo.currentTime",
        "bufferedLagMs",
        "totalLagMs",
        "oldestAgeMs",
        "enqueuedAt",
        "state.queueBytes",
        "state.inflightBytes",
        "totalBytes",
    )
    missing_snapshot = [m for m in snapshot_markers if m not in lag_snapshot]
    if missing_snapshot:
        failures.append(
            "H.264 MSE lag snapshot must combine queued, in-flight, buffered, "
            "and oldest media time: " + ", ".join(missing_snapshot)
        )
    if not ordered(lag_snapshot, "try{", "ranges.start(", "ranges.end(",
                   "}catch"):
        failures.append(
            "H.264 MSE TimeRanges reads must be guarded against transient exceptions"
        )
    compact_snapshot = re.sub(r"\s+", "", lag_snapshot)
    if "totalBytes=Math.max(0,state.queueBytes+state.inflightBytes)" not in compact_snapshot:
        failures.append(
            "H.264 MSE byte hard limit must include queued plus in-flight bytes"
        )

    enforce_latency = function_section(kvm_html, "mseEnforceLatency")
    enforcement_markers = (
        "mseLagSnapshot(state)",
        "H264_MSE_HARD_LAG_MS",
        "H264_MSE_SOFT_LAG_MS",
        "H264_MSE_SEEK_COOLDOWN_MS",
        "snapshot.totalLagMs",
        "snapshot.oldestAgeMs",
        "h264MseVideo.seeking",
        "state.lastSeekAt",
        "state.rendered",
        "snapshot.totalBytes",
        "H264_MSE_HARD_BYTES",
        "state.latencyArmed",
        "state.latencyGraceUntil",
        "recoverH264(expectedStreamId",
    )
    missing_enforcement = [
        m for m in enforcement_markers if m not in enforce_latency
    ]
    if missing_enforcement:
        failures.append(
            "H.264 MSE hard recovery and cooldown seek contract missing: "
            + ", ".join(missing_enforcement)
        )
    if not ordered(
        enforce_latency,
        "snapshot.totalBytes>H264_MSE_HARD_BYTES",
        "recoverH264(expectedStreamId",
        "if(!state.rendered)return true",
        "const startupGrace=",
        "if(!startupGrace&&(",
        "snapshot.totalLagMs>H264_MSE_HARD_LAG_MS",
    ):
        failures.append(
            "H.264 MSE byte overflow must remain active before rendering/grace, "
            "while time-lag recovery must wait until startup grace is armed"
        )
    for marker in (
        "seekForStartup=startupGrace",
        "seekForSoft=!startupGrace",
    ):
        if marker not in enforce_latency:
            failures.append(
                f"H.264 MSE startup/steady live-edge seek split missing: {marker}"
            )
    compact_enforcement = re.sub(r"\s+", "", enforce_latency)
    if (
        "h264MseVideo.currentTime=" not in compact_enforcement
        or "H264_MSE_TARGET_LAG_MS/1000" not in compact_enforcement
    ):
        failures.append(
            "H.264 MSE live-edge seek must target the explicit 150 ms delay"
        )

    mse_source = function_section(kvm_html, "mseEnsureSource")
    init_markers = (
        "data:init",
        "durationMs:0",
        "media:false",
    )
    missing_init = [m for m in init_markers if m not in mse_source]
    if missing_init:
        failures.append(
            "H.264 MSE init segment must use typed queue metadata: "
            + ", ".join(missing_init)
        )

    mse_consume = function_section(kvm_html, "mseConsume")
    media_markers = (
        "data:fragment",
        "durationMs:duration",
        "wireTs:timestamp",
        "enqueuedAt:now",
        "key",
        "media:true",
        "state.queueDurationMs+=duration",
        "mseEnforceLatency(state,expectedStreamId)",
    )
    missing_media = [m for m in media_markers if m not in mse_consume]
    if missing_media:
        failures.append(
            "H.264 MSE media queue must carry and enforce media duration: "
            + ", ".join(missing_media)
        )
    if "state.queue.push(fragment)" in mse_consume:
        failures.append(
            "H.264 MSE media fragments must not bypass duration-aware queue metadata"
        )

    mse_pump = function_section(kvm_html, "msePump")
    pump_markers = (
        "item.data",
        "item.durationMs",
        "state.queueDurationMs",
        "state.inflightDurationMs",
        "state.inflightBytes",
    )
    missing_pump = [m for m in pump_markers if m not in mse_pump]
    if missing_pump:
        failures.append(
            "H.264 MSE pump must preserve queued/in-flight duration accounting: "
            + ", ".join(missing_pump)
        )
    if "appendBuffer(item.data)" not in mse_pump:
        failures.append(
            "H.264 MSE pump must append the typed queue item's data payload"
        )
    if not ordered(
        mse_pump,
        "state.source.updating",
        "if(!state.queue.length){",
        "state.rendered&&state.latencyArmed",
        "now>=state.latencyGraceUntil",
        "snapshot.totalLagMs<H264_MSE_SOFT_LAG_MS",
        "now-state.lastTrimAt>=H264_MSE_TRIM_INTERVAL_MS",
        "historyMs>H264_MSE_TRIM_AFTER_MS",
        "h264MseVideo.currentTime-H264_MSE_RETAIN_HISTORY_MS/1000",
        'state.sourceOp="remove"',
        "state.lastTrimAt=now",
        "state.source.remove(",
    ):
        failures.append(
            "H.264 MSE history trim must require an empty/low-lag queue, wait "
            "10 s and 5 s between trims, and retain the newest 3 s"
        )
    if not ordered(
        mse_pump,
        "try{",
        'state.sourceOp="remove"',
        "state.source.remove(",
        '}catch(e){state.sourceOp="idle"',
    ):
        failures.append(
            "H.264 MSE history removal must recover sourceOp after SourceBuffer exceptions"
        )
    if not ordered(
        mse_pump,
        "state.inflightBytes=item.data.length",
        'state.sourceOp="append"',
        "state.source.appendBuffer(item.data)",
    ):
        failures.append(
            "H.264 MSE append must account in-flight bytes before SourceBuffer I/O"
        )

    mse_render = function_section(kvm_html, "noteMseRenderedFrame")
    if not ordered(
        mse_render,
        "currentTime=h264MseVideo.currentTime",
        "if(!state.rendered){",
        "state.rendered=true",
        "state.latencyGraceUntil=now+H264_MSE_STARTUP_GRACE_MS",
        "state.lastRenderedTime=currentTime",
        "currentTime>state.lastRenderedTime",
        "state.latencyArmed=true",
        "mseEnforceLatency(state,expectedStreamId)",
    ):
        failures.append(
            "H.264 MSE startup must establish grace on the first rendered frame "
            "and arm latency recovery only after media time advances"
        )
    compact_render = re.sub(r"\s+", "", mse_render)
    if not re.search(
        r"if\(currentTime>state\.lastRenderedTime[^)]*\)"
        r"state\.latencyArmed=true",
        compact_render,
    ):
        failures.append(
            "H.264 MSE latencyArmed must be assigned only by an advancing "
            "rendered media time"
        )

    mse_source_lifecycle = function_section(kvm_html, "mseEnsureSource")
    if not ordered(
        mse_source_lifecycle,
        "const completedOp=state.sourceOp",
        'state.sourceOp="idle"',
        'if(completedOp==="append"){',
        "state.inflightBytes=0",
        "state.inflightDurationMs=0",
        "state.inflightEnqueuedAt=0",
    ):
        failures.append(
            "H.264 MSE updateend must distinguish append from remove before "
            "clearing in-flight accounting"
        )

    # Fragment batching is deliberately not required here.  A per-frame fMP4
    # fragment keeps latency bounded; if batching is introduced later it needs
    # its own duration cap rather than a frame-count-only flush rule.


def check_h264_mock_runtime(failures: list[str]) -> None:
    """Execute the fixture and session-ownership guards, not just grep them."""
    module_path = PROJECT / "tools/serve-ui.py"
    spec = importlib.util.spec_from_file_location(
        "exoanchor_video_fixture_contract", module_path
    )
    if spec is None or spec.loader is None:
        failures.append("could not load real-bitstream browser fixture module")
        return
    module = importlib.util.module_from_spec(spec)
    previous_dont_write_bytecode = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous_dont_write_bytecode

    state = module.MockState
    old_generation = state.begin_h264_session(1.0)
    new_generation = state.begin_h264_session(2.0)
    if state.end_h264_session(old_generation) or not state.h264_session_active:
        failures.append("an obsolete mock H.264 socket can clear the newest session")
    if not state.end_h264_session(new_generation) or state.h264_session_active:
        failures.append("the newest mock H.264 socket does not clear its session")

    start = b"\x00\x00\x00\x01"
    aud = start + b"\x09\xf0"
    sps = start + b"\x67\x42\x00\x1f"
    pps = start + b"\x68\xce\x06\xe2"
    idr = start + b"\x65\x88\x84"
    with tempfile.TemporaryDirectory(prefix="exoanchor-h264-contract-") as tmp:
        tmp_path = Path(tmp)
        valid = tmp_path / "valid.h264"
        valid.write_bytes(aud + sps + pps + idr)
        try:
            module._parse_h264_fixture(f"1280x720@25={valid}")
        except argparse.ArgumentTypeError as error:
            failures.append(f"valid first-IDR fixture was rejected: {error}")

        missing_headers = tmp_path / "missing-headers.h264"
        missing_headers.write_bytes(aud + sps + pps + aud + idr)
        try:
            module._parse_h264_fixture(f"1280x720@25={missing_headers}")
        except argparse.ArgumentTypeError:
            pass
        else:
            failures.append(
                "fixture accepted a first IDR access unit without SPS/PPS"
            )


def check_h264_acceptance_gate_runtime(failures: list[str]) -> None:
    """Execute lifecycle gate helpers against expected and fatal states."""
    module_path = PROJECT / "tools/h264-accept.py"
    module_name = "exoanchor_h264_acceptance_gate_contract"
    spec = importlib.util.spec_from_file_location(module_name, module_path)
    if spec is None or spec.loader is None:
        failures.append("could not load H.264 acceptance gate module")
        return
    module = importlib.util.module_from_spec(spec)
    previous_module = sys.modules.get(module_name)
    previous_dont_write_bytecode = sys.dont_write_bytecode
    sys.modules[module_name] = module
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous_dont_write_bytecode
        if previous_module is None:
            sys.modules.pop(module_name, None)
        else:
            sys.modules[module_name] = previous_module

    expected_mode_rates = {
        module.Mode(1920, 1080, 30.0): (25.0, False),
        module.Mode(1920, 1080, 25.0): (25.0, True),
        module.Mode(1280, 720, 60.0): (30.0, False),
        module.Mode(1280, 720, 30.0): (30.0, True),
        module.Mode(1280, 720, 29.97): (30.0, True),
        module.Mode(1280, 720, 20.0): (20.0, True),
    }
    for mode, (expected_fps, expected_supported) in expected_mode_rates.items():
        if mode.promised_h264_fps != expected_fps:
            failures.append(
                f"H.264 acceptance mode policy mismatch for {mode.label}: "
                f"{mode.promised_h264_fps} != {expected_fps}"
            )
        if mode.h264_supported is not expected_supported:
            failures.append(
                f"H.264 acceptance support policy mismatch for {mode.label}: "
                f"{mode.h264_supported} != {expected_supported}"
            )

    for raw_mode in ("1920x1080@30", "1280x720@60"):
        rejected = subprocess.run(
            [
                sys.executable,
                "-B",
                str(module_path),
                "--host",
                "127.0.0.1",
                "--mode",
                raw_mode,
            ],
            input="",
            capture_output=True,
            text=True,
            check=False,
        )
        if rejected.returncode != 2 or "1080p25 / 720p30 or lower" not in rejected.stderr:
            failures.append(
                f"H.264 acceptance CLI did not reject unsupported mode {raw_mode} before login"
            )
    for raw_mode in ("1920x1080@25", "1280x720@30", "1280x720@29.97"):
        accepted = subprocess.run(
            [
                sys.executable,
                "-B",
                str(module_path),
                "--host",
                "127.0.0.1",
                "--mode",
                raw_mode,
            ],
            input="",
            capture_output=True,
            text=True,
            check=False,
        )
        if accepted.returncode != 1 or "password must be supplied on stdin" not in accepted.stderr:
            failures.append(
                f"H.264 acceptance CLI rejected supported mode {raw_mode}"
            )

    clean_delta = {
        "frames_captured": 500,
        "frames_encoded": 480,
        "frames_dropped": 20,
        "uvc_callbacks": 500,
        "uvc_queue_drops": 0,
        "h264_pressure_coalesces": 1,
        "uvc_buffer_underflows": 0,
        "uvc_buffer_overflows": 0,
        "uvc_return_failures": 0,
        "hid_failed_messages": 0,
    }
    if not module.error_counters_clean(clean_delta):
        failures.append(
            "H.264 source telemetry and pressure coalescing must remain observable non-error counters"
        )
    for fatal_counter in (
        "uvc_buffer_underflows",
        "uvc_return_failures",
        "hid_failed_messages",
    ):
        fatal_delta = dict(clean_delta)
        fatal_delta[fatal_counter] = 1
        if module.error_counters_clean(fatal_delta):
            failures.append(
                f"H.264 lifecycle gate incorrectly exempts {fatal_counter}"
            )

    counter_payload = {
        "hid": {"stats": {"failed_messages": 7}},
        "video": {
            "frames_captured": 101,
            "frames_encoded": 97,
            "frames_dropped": 4,
            "pipeline_stats": {"uvc_callbacks": 103},
            "h264_runtime": {},
        },
    }
    snapshot = module.counter_snapshot(counter_payload)
    expected_observation_counters = {
        "frames_captured": 101,
        "frames_encoded": 97,
        "frames_dropped": 4,
        "uvc_callbacks": 103,
    }
    if any(
        snapshot.get(key) != value
        for key, value in expected_observation_counters.items()
    ):
        failures.append(
            "H.264 acceptance report does not retain source-side video counter evidence"
        )
    if snapshot.get("hid_failed_messages") != 7:
        failures.append(
            "dynamic H.264 gate does not read device-side HID failed_messages"
        )

    class LifecycleClock:
        def __init__(self) -> None:
            self.now = 0.0

        def monotonic(self) -> float:
            return self.now

        def sleep(self, seconds: float) -> None:
            self.now += seconds

    def lifecycle_payload(sample: int, *, late_error: bool) -> dict[str, object]:
        return {
            "video": {
                "width": 1280,
                "height": 720,
                "target_width": 1280,
                "target_height": 720,
                "target_fps_x100": 3000,
                "fps": 30.0,
                "pixel_format": "MJPEG",
                "device_connected": True,
                "frame_ready": True,
                "last_error": "",
                "frames_captured": sample,
                "frames_encoded": sample,
                "frames_dropped": sample // 7,
                "pipeline_stats": {
                    "uvc_callbacks": sample,
                    "buffer_underflows": 1 if late_error else 0,
                },
                "h264_runtime": {},
            },
            "hid": {"stats": {}},
        }

    original_status = module.status
    original_time = module.time
    lifecycle_mode = module.Mode(1280, 720, 30.0)
    steady_clock = LifecycleClock()
    steady_samples = 0

    def growing_source_status(_session: object, _host: str) -> dict[str, object]:
        nonlocal steady_samples
        steady_samples += 1
        return lifecycle_payload(steady_samples, late_error=False)

    module.status = growing_source_status
    module.time = steady_clock
    try:
        _payload, steady_settle = module.wait_for_mode_lifecycle_settle(
            object(), "device.local", lifecycle_mode, 1.3
        )
    finally:
        module.status = original_status
        module.time = original_time
    if (
        not steady_settle.get("settled")
        or not 1000 <= int(steady_settle.get("elapsed_ms", 0)) <= 1200
    ):
        failures.append(
            "growing source counters must not prevent the lifecycle gate from settling after about one second"
        )

    delayed_clock = LifecycleClock()
    delayed_samples = 0

    def delayed_error_status(_session: object, _host: str) -> dict[str, object]:
        nonlocal delayed_samples
        delayed_samples += 1
        return lifecycle_payload(
            delayed_samples,
            late_error=delayed_clock.now >= 0.8,
        )

    module.status = delayed_error_status
    module.time = delayed_clock
    try:
        _payload, delayed_settle = module.wait_for_mode_lifecycle_settle(
            object(), "device.local", lifecycle_mode, 1.2
        )
    finally:
        module.status = original_status
        module.time = original_time
    if delayed_settle.get("settled"):
        failures.append(
            "a delayed classified lifecycle error must reset the quiet period and fail a bounded settle"
        )

    class LeaseResponse:
        def raise_for_status(self) -> None:
            return None

    class LeaseSession:
        def __init__(self) -> None:
            self.posts: list[tuple[str, dict[str, object], int]] = []

        def post(
            self, url: str, *, json: dict[str, object], timeout: int
        ) -> LeaseResponse:
            self.posts.append((url, dict(json), timeout))
            return LeaseResponse()

    lease_session = LeaseSession()
    module.set_lease(
        lease_session,
        "device.local",
        37,
        True,
        claim=True,
        force=True,
    )
    module.set_lease(lease_session, "device.local", 37, True)
    module.set_lease(lease_session, "device.local", 37, False)
    lease_payloads = [payload for _url, payload, _timeout in lease_session.posts]
    if lease_payloads != [
        {
            "owner": "kvm",
            "active": True,
            "stream_id": 37,
            "claim": True,
            "previous_stream_id": 0,
            "force": True,
        },
        {"owner": "kvm", "active": True, "stream_id": 37},
        {"owner": "kvm", "active": False, "stream_id": 37},
    ]:
        failures.append(
            "forced HIL lease must set force only on the initial claim, never heartbeat/release"
        )
    post_count_before_invalid_force = len(lease_session.posts)
    try:
        module.set_lease(
            lease_session,
            "device.local",
            37,
            True,
            force=True,
        )
    except ValueError:
        pass
    else:
        failures.append("non-claim HIL lease incorrectly accepted force=True")
    if len(lease_session.posts) != post_count_before_invalid_force:
        failures.append("invalid forced heartbeat reached the device HTTP API")
    try:
        module.set_lease(
            lease_session,
            "device.local",
            37,
            False,
            claim=True,
            force=True,
        )
    except ValueError:
        pass
    else:
        failures.append("lease release incorrectly accepted force=True")
    if len(lease_session.posts) != post_count_before_invalid_force:
        failures.append("invalid forced release reached the device HTTP API")

    activation_calls: list[tuple[bool, bool, bool]] = []
    original_status = module.status
    original_counter_snapshot = module.counter_snapshot
    original_set_lease = module.set_lease
    original_wait_for_mode = module.wait_for_mode
    original_wait_for_mode_lifecycle_settle = (
        module.wait_for_mode_lifecycle_settle
    )
    original_counter_delta = module.counter_delta
    original_runtime_error_snapshot = module.runtime_error_snapshot
    original_mode_activation_runtime_error_checks = (
        module.mode_activation_runtime_error_checks
    )
    original_reference_workspace_checks = module.reference_workspace_checks
    original_error_counters_clean = module.error_counters_clean

    def record_activation_lease(
        _session: object,
        _host: str,
        _stream_id: int,
        active: bool,
        *,
        claim: bool = False,
        force: bool = False,
    ) -> None:
        activation_calls.append((active, claim, force))

    module.status = lambda _session, _host: {}
    module.counter_snapshot = lambda _payload: {}
    module.set_lease = record_activation_lease
    module.wait_for_mode = lambda *_args: {"passed": True}
    module.wait_for_mode_lifecycle_settle = (
        lambda *_args: ({}, {"settled": True})
    )
    module.counter_delta = lambda _before, _after: {}
    module.runtime_error_snapshot = lambda _payload: {}
    module.mode_activation_runtime_error_checks = (
        lambda _errors: {"runtime": True}
    )
    module.reference_workspace_checks = (
        lambda _runtime: ({}, {"workspace": True})
    )
    module.error_counters_clean = lambda _delta: True
    try:
        _stream_id, forced_activation = module.activate_capture_mode(
            object(),
            "device.local",
            module.Mode(1280, 720, 30.0),
            1.0,
            force_lease=True,
        )
        forced_activation_calls = list(activation_calls)
        activation_calls.clear()
        _stream_id, default_activation = module.activate_capture_mode(
            object(),
            "device.local",
            module.Mode(1280, 720, 30.0),
            1.0,
        )
        default_activation_calls = list(activation_calls)
        activation_calls.clear()
        module.wait_for_mode = lambda *_args: {"passed": False}
        failed_stream_id, failed_activation = module.activate_capture_mode(
            object(),
            "device.local",
            module.Mode(1280, 720, 30.0),
            1.0,
            force_lease=True,
        )
        failed_activation_calls = list(activation_calls)
    finally:
        module.status = original_status
        module.counter_snapshot = original_counter_snapshot
        module.set_lease = original_set_lease
        module.wait_for_mode = original_wait_for_mode
        module.wait_for_mode_lifecycle_settle = (
            original_wait_for_mode_lifecycle_settle
        )
        module.counter_delta = original_counter_delta
        module.runtime_error_snapshot = original_runtime_error_snapshot
        module.mode_activation_runtime_error_checks = (
            original_mode_activation_runtime_error_checks
        )
        module.reference_workspace_checks = original_reference_workspace_checks
        module.error_counters_clean = original_error_counters_clean
    if forced_activation_calls != [(True, True, True)]:
        failures.append(
            "--force-lease did not flow exclusively to capture activation's initial claim"
        )
    if forced_activation.get("lease_claim") != {
        "force": True,
        "initial_claim_only": True,
    }:
        failures.append("forced lease choice is missing from per-cycle evidence")
    if default_activation_calls != [(True, True, False)]:
        failures.append("default HIL mode activation unexpectedly forced the lease")
    if default_activation.get("lease_claim") != {
        "force": False,
        "initial_claim_only": True,
    }:
        failures.append("default lease choice is missing from per-cycle evidence")
    if failed_stream_id != 0 or failed_activation_calls != [
        (True, True, True),
        (False, False, False),
    ]:
        failures.append(
            "failed forced activation must release normally without repeating force"
        )
    if failed_activation.get("lease_claim", {}).get("force") is not True:
        failures.append("failed forced activation lost its lease evidence")

    class RecordingSocket:
        def __init__(self) -> None:
            self.writes: list[bytes] = []

        def sendall(self, payload: bytes) -> None:
            self.writes.append(payload)

    def decode_client_frame(wire: bytes) -> tuple[int, bool, bytes]:
        if len(wire) < 6:
            raise ValueError("short client WebSocket frame")
        opcode = wire[0] & 0x0F
        masked = bool(wire[1] & 0x80)
        length = wire[1] & 0x7F
        cursor = 2
        if length == 126:
            length = int.from_bytes(wire[cursor:cursor + 2], "big")
            cursor += 2
        elif length == 127:
            length = int.from_bytes(wire[cursor:cursor + 8], "big")
            cursor += 8
        mask = wire[cursor:cursor + 4]
        cursor += 4
        if len(mask) != 4 or len(wire) != cursor + length:
            raise ValueError("malformed client WebSocket frame")
        payload = bytes(
            value ^ mask[index & 3]
            for index, value in enumerate(wire[cursor:])
        )
        return opcode, masked, payload

    protocol_socket = RecordingSocket()
    module.websocket_send_text(protocol_socket, {"type": "releaseall"})
    module.websocket_send_close(protocol_socket)
    try:
        text_opcode, text_masked, text_payload = decode_client_frame(
            protocol_socket.writes[0]
        )
        close_opcode, close_masked, close_payload = decode_client_frame(
            protocol_socket.writes[1]
        )
    except (IndexError, ValueError) as error:
        failures.append(f"dynamic HID client frame is malformed: {error}")
    else:
        if (
            text_opcode != 1
            or not text_masked
            or module.json.loads(text_payload) != {"type": "releaseall"}
        ):
            failures.append(
                "dynamic HID text commands must be masked RFC 6455 client frames"
            )
        if close_opcode != 8 or not close_masked or close_payload != b"\x03\xe8":
            failures.append(
                "dynamic HID close must be a masked RFC 6455 client frame"
            )

    upgrade_calls: list[tuple[str, int, str, str]] = []
    original_upgrade = module.websocket_upgrade

    def record_upgrade(host: str, port: int, path: str, cookie: str):
        upgrade_calls.append((host, port, path, cookie))
        return RecordingSocket(), object()

    module.websocket_upgrade = record_upgrade
    try:
        stream_id = 0x1234ABCD
        module.websocket_connect("device.local", "session", stream_id)
        module.hid_websocket_connect("device.local", "session", stream_id)
    finally:
        module.websocket_upgrade = original_upgrade
    expected_upgrade_calls = [
        (
            "device.local",
            81,
            "/api/ws/video/h264?stream_id=305441741",
            "session",
        ),
        (
            "device.local",
            80,
            "/api/ws/hid?stream_id=305441741",
            "session",
        ),
    ]
    if upgrade_calls != expected_upgrade_calls:
        failures.append(
            "dynamic HID must bind to the exact H.264 stream_id on the main HTTP port"
        )

    class FirstAuGate:
        def __init__(self, events: list[tuple[str, object]]) -> None:
            self.events = events

        def wait(self, timeout: float) -> bool:
            self.events.append(("first_au_wait", timeout))
            return True

    class ClearStop:
        def is_set(self) -> bool:
            return False

        def wait(self, _timeout: float) -> bool:
            return False

    drag_events: list[tuple[str, object]] = []
    original_send_text = module.websocket_send_text

    def record_drag(_sock: object, payload: dict[str, object]) -> None:
        drag_events.append(("send", dict(payload)))

    module.websocket_send_text = record_drag
    clean_drag_result: dict[str, object] = {}
    try:
        module.hid_drag_worker(
            object(),
            (100, 200),
            (300, 400),
            0.0,
            10,
            FirstAuGate(drag_events),
            ClearStop(),
            clean_drag_result,
        )
    finally:
        module.websocket_send_text = original_send_text
    drag_types = [
        payload.get("type")
        for event, payload in drag_events
        if event == "send" and isinstance(payload, dict)
    ]
    if not drag_events or drag_events[0][0] != "first_au_wait":
        failures.append("dynamic HID emitted input before the first H.264 AU gate")
    if drag_types != ["absmove", "absmousedown", "absmouseup", "releaseall"]:
        failures.append(
            "dynamic HID clean path must press only after first AU and finish with mouseup/releaseall"
        )
    if not clean_drag_result.get("cleanup_ok"):
        failures.append("dynamic HID clean path did not report successful input cleanup")

    class MissingFirstAuGate:
        def wait(self, _timeout: float) -> bool:
            return False

    class StopBeforeFirstAu(ClearStop):
        def is_set(self) -> bool:
            return True

    pre_au_messages: list[dict[str, object]] = []

    def record_pre_au(_sock: object, payload: dict[str, object]) -> None:
        pre_au_messages.append(dict(payload))

    module.websocket_send_text = record_pre_au
    pre_au_result: dict[str, object] = {}
    try:
        module.hid_drag_worker(
            object(),
            (100, 200),
            (300, 400),
            0.1,
            10,
            MissingFirstAuGate(),
            StopBeforeFirstAu(),
            pre_au_result,
        )
    finally:
        module.websocket_send_text = original_send_text
    if [message.get("type") for message in pre_au_messages] != ["releaseall"]:
        failures.append(
            "dynamic HID must not move or press when video ends before first AU"
        )
    if pre_au_result.get("error") != (
        "H.264 stream ended before the first access unit"
    ):
        failures.append("dynamic HID did not report a missing first H.264 AU")

    failing_drag_messages: list[dict[str, object]] = []

    def fail_during_held_drag(
        _sock: object, payload: dict[str, object]
    ) -> None:
        failing_drag_messages.append(dict(payload))
        if len(failing_drag_messages) == 3:
            raise OSError("injected movement failure")

    module.websocket_send_text = fail_during_held_drag
    failed_drag_result: dict[str, object] = {}
    try:
        module.hid_drag_worker(
            object(),
            (100, 200),
            (300, 400),
            0.1,
            10,
            FirstAuGate([]),
            ClearStop(),
            failed_drag_result,
        )
    finally:
        module.websocket_send_text = original_send_text
    failed_drag_types = [message.get("type") for message in failing_drag_messages]
    if failed_drag_types[-2:] != ["absmouseup", "releaseall"]:
        failures.append(
            "dynamic HID exception path must finally send mouseup then releaseall"
        )
    if not str(failed_drag_result.get("error", "")).startswith(
        "HID drag failed:"
    ):
        failures.append("dynamic HID movement failure was not surfaced by the worker")

    initial_errors = {
        "video_last_error": "",
        "resource_error": "ESP_ERR_NOT_FINISHED",
        "restore_error": "ESP_OK",
        "teardown_poisoned": False,
    }
    if not all(
        module.mode_activation_runtime_error_checks(initial_errors).values()
    ):
        failures.append(
            "mode activation rejects the expected pre-probe H.264 resource state"
        )
    failed_resource = dict(initial_errors, resource_error="ESP_ERR_NO_MEM")
    if all(module.mode_activation_runtime_error_checks(failed_resource).values()):
        failures.append("mode activation accepts a real H.264 resource failure")

    idle_payload = {
        "video": {
            "last_error": "video capture idle",
            "capture_enabled": False,
            "streaming": False,
            "frame_ready": False,
            "control": {"kvm_active": False, "active_enabled": False},
            "h264_runtime": {
                "resource_error": "ESP_OK",
                "restore_error": "ESP_OK",
                "teardown_poisoned": False,
            },
        }
    }
    _errors, idle_checks = module.post_release_runtime_error_checks(idle_payload)
    if not all(idle_checks.values()):
        failures.append(
            "post-release gate rejects an exact idle message for stopped capture"
        )
    active_idle_payload = {
        "video": {
            **idle_payload["video"],
            "capture_enabled": True,
            "streaming": True,
            "frame_ready": True,
            "control": {"kvm_active": False, "active_enabled": True},
        }
    }
    _errors, active_idle_checks = module.post_release_runtime_error_checks(
        active_idle_payload
    )
    if all(active_idle_checks.values()):
        failures.append(
            "post-release gate exempts an idle error while capture is active"
        )
    underflow_payload = {
        "video": {
            **idle_payload["video"],
            "last_error": "UVC frame buffer underflow",
        }
    }
    _errors, underflow_checks = module.post_release_runtime_error_checks(
        underflow_payload
    )
    if all(underflow_checks.values()):
        failures.append("post-release gate exempts a real UVC underflow error")

    class FakeProcess:
        def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
            self.returncode = returncode
            self.stdout = stdout
            self.stderr = stderr

    original_which = module.shutil.which
    original_run = module.subprocess.run
    header_result = 1
    header_output = "Invalid value at num_units_in_tick: bitstream ended"
    probe_width = 1920
    probe_height = 1080

    def fake_which(_name: str) -> str:
        return "/fake/ffmpeg"

    def fake_run(command: list[str], **_kwargs: object) -> FakeProcess:
        if "-show_entries" in command:
            return FakeProcess(
                0,
                module.json.dumps(
                    {
                        "streams": [
                            {
                                "codec_name": "h264",
                                "width": probe_width,
                                "height": probe_height,
                                "r_frame_rate": "50/1",
                                "avg_frame_rate": "25/1",
                                "nb_read_frames": "1",
                            }
                        ]
                    }
                ),
            )
        if "trace_headers" in command:
            return FakeProcess(
                header_result,
                stderr=header_output,
            )
        return FakeProcess(0, stdout="frame=1\n")

    module.shutil.which = fake_which
    module.subprocess.run = fake_run
    try:
        mode = module.Mode(1920, 1080, 25.0)
        malformed = module.decode_annex_b(Path("malformed.h264"), mode, 1, 1.0)
        if malformed.get("passed"):
            failures.append(
                "H.264 acceptance ignores a trace_headers SPS/VUI parse failure"
            )
        if "num_units_in_tick" not in str(malformed.get("headers_error", "")):
            failures.append(
                "H.264 acceptance does not retain the SPS/VUI parser error"
            )
        header_result = 0
        header_output = "\n".join(
            [
                "[trace_headers] num_units_in_tick = 1",
                "[trace_headers] time_scale = 50",
                "[trace_headers] fixed_frame_rate_flag = 1",
            ]
        )
        valid = module.decode_annex_b(Path("valid.h264"), mode, 1, 1.0)
        if not valid.get("passed"):
            failures.append(
                "H.264 acceptance rejects an otherwise valid stream after trace_headers passes"
            )
        if valid.get("declared_fps") != 25.0:
            failures.append("H.264 acceptance does not retain declared VUI cadence")

        probe_width = 1280
        probe_height = 720
        header_output = "\n".join(
            [
                "[trace_headers] num_units_in_tick = 1",
                "[trace_headers] time_scale = 60",
                "[trace_headers] fixed_frame_rate_flag = 1",
            ]
        )
        valid_720 = module.decode_annex_b(
            Path("valid-720p30.h264"), module.Mode(1280, 720, 30.0), 1, 1.0
        )
        if not valid_720.get("passed") or valid_720.get("declared_fps") != 30.0:
            failures.append(
                "H.264 acceptance trusts the raw demuxer 25fps default over 720p30 VUI timing"
            )

        header_output = "\n".join(
            [
                "[trace_headers] num_units_in_tick = 1",
                "[trace_headers] time_scale = 58",
                "[trace_headers] fixed_frame_rate_flag = 1",
            ]
        )
        wrong_rate = module.decode_annex_b(
            Path("wrong-rate.h264"), module.Mode(1280, 720, 30.0), 1, 1.0
        )
        if wrong_rate.get("passed"):
            failures.append("H.264 acceptance ignores a mismatched SPS/VUI cadence")

        header_output = "\n".join(
            [
                "[trace_headers] num_units_in_tick = 1",
                "[trace_headers] time_scale = 60",
                "[trace_headers] fixed_frame_rate_flag = 0",
            ]
        )
        variable_rate = module.decode_annex_b(
            Path("variable-rate.h264"), module.Mode(1280, 720, 30.0), 1, 1.0
        )
        if variable_rate.get("passed"):
            failures.append(
                "H.264 acceptance ignores a cleared fixed_frame_rate_flag"
            )
    finally:
        module.shutil.which = original_which
        module.subprocess.run = original_run

    class MotionProcess:
        def __init__(
            self,
            returncode: int,
            stdout: bytes = b"",
            stderr: bytes = b"",
        ) -> None:
            self.returncode = returncode
            self.stdout = stdout
            self.stderr = stderr

    frame_bytes = module.MOTION_SAMPLE_WIDTH * module.MOTION_SAMPLE_HEIGHT
    small_change_frames: list[bytes] = []
    for index in range(module.MOTION_MIN_COMPARISONS + 1):
        frame = bytearray(frame_bytes)
        if index % 2:
            frame[:16] = b"\xff" * 16
        small_change_frames.append(bytes(frame))
    real_motion_frames = [
        bytes([0 if index % 2 == 0 else 24]) * frame_bytes
        for index in range(module.MOTION_MIN_COMPARISONS + 1)
    ]
    motion_raw = b"".join(small_change_frames)
    motion_commands: list[list[str]] = []

    def fake_motion_run(
        command: list[str], **_kwargs: object
    ) -> MotionProcess:
        motion_commands.append(list(command))
        return MotionProcess(0, stdout=motion_raw)

    module.shutil.which = fake_which
    module.subprocess.run = fake_motion_run
    try:
        false_positive = module.analyze_dynamic_source(
            Path("pointer-clock-selection.h264"),
            1.0,
            0.5,
            1.0,
        )
        if false_positive.get("passed"):
            failures.append(
                "H.264 dynamic-source gate accepts pointer/clock-sized changes"
            )
        false_checks = false_positive.get("checks", {})
        if (
            false_checks.get("mean_mad") is not False
            or false_checks.get("mean_changed_pixels") is not False
        ):
            failures.append(
                "H.264 dynamic-source evidence does not identify both weak-motion metrics"
            )
        motion_raw = b"".join(real_motion_frames)
        real_motion = module.analyze_dynamic_source(
            Path("real-window-drag.h264"),
            1.0,
            0.5,
            1.0,
        )
        if not real_motion.get("passed"):
            failures.append("H.264 dynamic-source gate rejects sustained scene motion")
        if real_motion.get("comparisons") != module.MOTION_MIN_COMPARISONS:
            failures.append("H.264 dynamic-source gate lost frame-pair evidence")
        if (
            real_motion.get("mad", {}).get("mean", 0) < 20
            or real_motion.get("changed_pixels_percent", {}).get("mean", 0)
            < 99
        ):
            failures.append("H.264 dynamic-source metrics are not reported in expected units")
        filter_graph = (
            f"fps={module.MOTION_SAMPLE_FPS},"
            f"scale={module.MOTION_SAMPLE_WIDTH}:"
            f"{module.MOTION_SAMPLE_HEIGHT}:flags=area,format=gray"
        )
        if not motion_commands or not all(
            filter_graph in command and "pipe:1" in command
            for command in motion_commands
        ):
            failures.append(
                "H.264 dynamic-source gate does not use bounded 5 FPS grayscale ffmpeg decoding"
            )
    finally:
        module.shutil.which = original_which
        module.subprocess.run = original_run


def cmake_bracket_block(text: str, variable: str) -> str:
    match = re.search(
        rf"set\({re.escape(variable)} \[=\[(.*?)\n\]=\]\)",
        text,
        re.DOTALL,
    )
    return match.group(1) if match else ""


def main() -> int:
    failures: list[str] = []
    check_h264_mock_runtime(failures)
    check_h264_acceptance_gate_runtime(failures)
    video_input = read("main/drivers/video_input.c")
    video_input_header = read("main/drivers/video_input.h")
    project_cmake = read("CMakeLists.txt")
    status_ws = read("main/services/status_ws.c")
    web_server = read("main/services/web_server.c")
    app_main = read("main/app/app_main.c")
    device_http = read("main/services/device_http.c")
    device_observation = read("main/application/device_observation_service.c")
    device_observation_header = read(
        "main/application/device_observation_service.h")
    video_http = read("main/services/web/video_http_module.inc")
    h264 = read("main/services/video_h264_stream.c")
    h264_header = read("main/services/video_h264_stream.h")
    video_control = read("main/application/video_control.c")
    control_lease = read("main/application/control_lease.c")
    kvm_html = read("main/www/kvm.html")
    h264_accept = read("tools/h264-accept.py")
    serve_ui = read("tools/serve-ui.py")
    agent_engine = read("main/services/web/agent_run_engine_module.inc")
    agent_dispatch = read("main/services/web/agent_tool_dispatch_module.inc")
    agent_runtime_types = read(
        "main/services/web/agent_runtime_types_module.inc")
    agent_run_task = read("main/services/web/agent_run_task_module.inc")
    check_h264_mse_latency_contract(kvm_html, failures)

    if 'note_drop_locked(publish_ret == ESP_ERR_NOT_FINISHED ?' in video_input:
        failures.append(
            "normal H.264 latest-frame backpressure still poisons video last_error"
        )
    release_body = section(
        video_input,
        "esp_err_t si_video_release_h264_jpeg(",
        "esp_err_t si_video_flush_h264_jpeg(",
    )
    if not ordered(
        release_body,
        "return_retained_uvc_frame(&item);",
        "memset(view, 0, sizeof(*view));",
        "return ESP_OK;",
    ):
        failures.append(
            "direct H.264 UVC release must clear the returned lease view"
        )
    detach_body = section(
        h264,
        "static void stream_detach_internal(uint32_t session, int fd,",
        "static void stream_detach(uint32_t session, int fd)",
    )
    if not ordered(
        detach_body,
        "pdMS_TO_TICKS(SI_H264_STREAM_LOCK_TIMEOUT_MS)",
        "s_stream.session == session && s_stream.fd == fd",
        "server = s_stream.server;",
        "s_stream.server = NULL;",
        "s_restore_pending, true",
        "xSemaphoreGive(s_stream.lock);",
        "httpd_sess_trigger_close(server, fd)",
    ):
        failures.append(
            "H.264 detach must bound the lock, retain restore state, and close only a matched failed session"
        )
    if "portMAX_DELAY" in h264:
        failures.append("H.264 backend retains an unbounded FreeRTOS wait")

    uvc_commit = cmake_bracket_block(project_cmake, "si_uvc_commit_fixed")
    for marker in (
        "const uint16_t frame_interval_hint = (1U << 0);",
        "const uint16_t compression_quality_hint = (1U << 3);",
        "vs_result.bmHint &=",
        "if (vs_format->fps > 0.0f)",
        "vs_result.bmHint |= frame_interval_hint;",
        "vs_result.wCompQuality = 0;",
        "vs_min.wCompQuality > 0",
        "vs_max.wCompQuality >= vs_min.wCompQuality",
    ):
        if marker not in uvc_commit:
            failures.append(
                f"UVC Probe must prefer explicit FPS and gate JPEG quality support: {marker}"
            )
    if not ordered(
        uvc_commit,
        "vs_result.bmHint &=",
        "if (vs_format->fps > 0.0f)",
        "vs_result.bmHint |= frame_interval_hint;",
        "vs_result.wCompQuality = 0;",
        "const bool quality_supported =",
        "if (quality_supported)",
        "vs_result.bmHint |= compression_quality_hint;",
    ):
        failures.append(
            "UVC Probe hint ordering must keep FPS ahead of supported compression quality"
        )
    for guessed_range in (
        "uint16_t quality_min = 1",
        "uint16_t quality_max = 10000",
    ):
        if guessed_range in uvc_commit:
            failures.append(
                f"UVC Probe must not invent support for a zero wCompQuality range: {guessed_range}"
            )
    if uvc_commit.count("if (quality_supported) {") < 1:
        failures.append(
            "UVC quality request must require proven device support"
        )
    if '"UVC Probe accepted: interval=%"' not in uvc_commit:
        failures.append(
            "UVC Probe must log the device-returned interval, hints, quality, frame, and payload"
        )
    if '"MJPEG quality accepted:' in uvc_commit:
        failures.append(
            "generic UVC Probe evidence must not claim unsupported JPEG quality was accepted"
        )

    callback = section(
        video_input, "static bool uvc_frame_cb(",
        "static void uvc_ingest_task(")
    reserve = section(
        video_input, "static bool try_reserve_uvc_frame(void)",
        "static bool try_enable_uvc_retention_cap(void)")
    if not ordered(
        reserve,
        "SI_CFG_VIDEO_FRAME_BUFFERS - 1U",
        "__atomic_load_n(&s_uvc_retention_state",
        "state & SI_UVC_RETENTION_COUNT_MASK",
        "state & SI_UVC_RETENTION_CAP_BIT",
        "assert(retained < SI_UVC_RETENTION_COUNT_MASK)",
        "__atomic_compare_exchange_n(&s_uvc_retention_state",
        "return true;",
    ) or "xSemaphoreTake(" in reserve or "uvc_host_frame_return(" in reserve:
        failures.append(
            "H.264 UVC admission must keep one physical driver buffer with "
            "a transition-safe packed-atomic reservation"
        )
    retention_release = section(
        video_input, "static void release_uvc_frame_retention(void)",
        "static void return_retained_uvc_frame(")
    if not ordered(
        retention_release,
        "state & SI_UVC_RETENTION_COUNT_MASK",
        "assert(count > 0U);",
        "state & SI_UVC_RETENTION_CAP_BIT",
        "__atomic_compare_exchange_n(&s_uvc_retention_state",
    ):
        failures.append(
            "UVC retention release must assert ownership and preserve the "
            "packed cap bit while decrementing"
        )
    cap_enable_helper = section(
        video_input, "static bool try_enable_uvc_retention_cap(void)",
        "static void disable_uvc_retention_cap(void)")
    cap_disable_helper = section(
        video_input, "static void disable_uvc_retention_cap(void)",
        "static bool uvc_frame_cb(")
    if not ordered(
        cap_enable_helper,
        "state & SI_UVC_RETENTION_COUNT_MASK",
        "if (retained > limit)",
        "state | SI_UVC_RETENTION_CAP_BIT",
        "__atomic_compare_exchange_n(&s_uvc_retention_state",
        "return true;",
    ) or "__atomic_fetch_and(&s_uvc_retention_state" not in cap_disable_helper:
        failures.append(
            "H.264 retention cap transition must share the reservation CAS "
            "word and preserve its retained-count bits"
        )
    if not ordered(
        callback,
        "s_uvc_accept_frames",
        "try_reserve_uvc_frame()",
        "__atomic_add_fetch(&s_h264_pressure_coalesces",
        "return true;",
        "xQueueSend(s_uvc_frame_queue, &item, 0)",
        "release_uvc_frame_retention()",
        "return true;",
        "return false;",
    ):
        failures.append(
            "UVC callback must reserve one UVC driver buffer during H.264, "
            "record intentional pressure separately, and roll back retention "
            "before returning a queue-rejected frame"
        )
    transport = section(
        video_input,
        "esp_err_t si_video_set_h264_transport_active(bool active)",
        "size_t si_video_jpeg_capacity(void)",
    )
    active_branch = section(transport, "if (active) {", "} else {")
    inactive_branch = transport[transport.find("} else {") :]
    if not ordered(
        active_branch,
        "while (!try_enable_uvc_retention_cap())",
        "__atomic_store_n(&s_h264_transport_active, true",
    ):
        failures.append(
            "H.264 enable must settle the independent retention cap before "
            "publishing the transport active state"
        )
    if not ordered(
        inactive_branch,
        "__atomic_store_n(&s_h264_transport_active, false",
        "disable_uvc_retention_cap()",
    ):
        failures.append(
            "H.264 disable must publish transport inactive before releasing "
            "the UVC retention cap"
        )
    for forbidden in (
        "uvc_host_frame_return(",
        "si_mjpeg_validate(",
        "si_video_frame_store_publish(",
        "memcpy(",
        "xSemaphoreTake(",
    ):
        if forbidden in callback:
            failures.append(
                f"UVC callback is no longer a bounded ownership handoff: {forbidden}"
            )
    for body, marker, description in (
        (
            video_input_header,
            "uint32_t h264_pressure_coalesces;",
            "video status must expose intentional H.264 UVC pressure coalesces",
        ),
        (
            device_observation_header,
            "uint32_t h264_pressure_coalesces;",
            "device observation must carry H.264 UVC pressure coalesces",
        ),
        (
            device_observation,
            "out->h264_pressure_coalesces = status.h264_pressure_coalesces;",
            "device observation must copy H.264 UVC pressure coalesces",
        ),
        (
            device_http,
            '"h264_pressure_coalesces"',
            "video status API must publish H.264 UVC pressure coalesces",
        ),
    ):
        if marker not in body:
            failures.append(description)

    ingest = section(
        video_input, "static void uvc_ingest_task(",
        "static void uvc_stream_event_cb(")
    if not ordered(
        ingest,
        "const int64_t ingest_started_us",
        "s_h264_transport_active",
        "si_mjpeg_validate(",
        "s_mjpeg_snapshot_request_generation",
        "si_video_frame_store_publish(item.frame->data",
        "esp_err_t publish_ret = h264_route ?",
        "handoff_h264_uvc_frame(&item, &frame_id)",
        "if (!h264_owns_frame)",
        "return_retained_uvc_frame(&item)",
    ):
        failures.append(
            "ingest must route validated H.264 frames to the direct lease queue "
            "and return every frame not transferred to that queue"
        )
    ingest_owned = section(
        ingest, "const int64_t ingest_started_us",
        "if (validate_us > 0U || measured_publish)")
    if "continue;" in ingest_owned:
        failures.append(
            "ingest has an early exit that can leak a retained UVC frame"
        )
    if not ordered(
        ingest,
        "publish_ret == ESP_ERR_NOT_FINISHED",
        "s_status.frames_dropped++;",
    ):
        failures.append(
            "packed MJPEG arena coalescing must count a drop without publishing a UVC corruption error"
        )
    if "si_video_frame_store_init(s_uvc_frame_capacity)" not in video_input:
        failures.append(
            "MJPEG frame store must share the full negotiated UVC frame ceiling"
        )

    retained_return = section(
        video_input, "static void return_retained_uvc_frame(",
        "static void uvc_return_task(")
    if not ordered(
        retained_return,
        "uvc_host_frame_return(",
        "if (ret == ESP_OK)",
        "release_uvc_frame_retention()",
        "__atomic_add_fetch(&s_uvc_return_pending",
        "xQueueSend(s_uvc_return_queue, &pending, 0)",
    ):
        failures.append(
            "UVC lease returns must synchronously refill the physical ring "
            "before decrementing retention, with a bounded recovery fallback"
        )
    return_worker = section(
        video_input, "static void uvc_return_task(",
        "static bool take_queued_h264_frame_locked(")
    if not ordered(
        return_worker,
        "uvc_host_frame_return(",
        "__atomic_sub_fetch(&s_uvc_return_pending",
        "release_uvc_frame_retention()",
    ):
        failures.append(
            "the asynchronous UVC return worker must retain stream ownership "
            "until the host frame return succeeds"
        )
    for marker in (
        "xQueueCreate(SI_CFG_VIDEO_FRAME_BUFFERS + 1U",
        'uvc_return_task, "si_uvc_return"',
        "s_uvc_return_retries",
        "s_uvc_return_failures",
    ):
        if marker not in video_input:
            failures.append(f"UVC return recovery contract missing: {marker}")

    set_capture = section(
        video_input, "esp_err_t si_video_set_capture(",
        "esp_err_t si_video_validate_mode(")
    if not ordered(
        set_capture,
        "select_stored_mode_locked(",
        "if (unavailable)",
        "return ESP_ERR_NOT_FOUND;",
        "s_capture_enabled = true;",
        "s_uvc_selection = selected;",
    ):
        failures.append(
            "exact UVC mode changes must reject unavailable modes before mutating the working capture selection"
        )
    mode_transaction = section(
        video_control, "esp_err_t si_video_control_change_kvm_mode(",
        "esp_err_t si_video_control_get_kvm_mode(")
    if not ordered(
        mode_transaction,
        "si_video_validate_mode(",
        "s_kvm_width = width;",
        "si_video_set_capture(",
        "s_kvm_width = previous_width;",
    ):
        failures.append(
            "KVM mode control must validate, apply, and roll back as one serialized transaction"
        )
    mode_handler = section(
        video_http, "static esp_err_t video_resolution_handler(",
        "static esp_err_t video_lease_handler(")
    if not ordered(
        mode_handler,
        "si_video_control_change_kvm_mode(",
        "si_video_control_get_kvm_mode(",
        "si_video_control_save_kvm_mode(",
    ):
        failures.append(
            "video mode HTTP persistence must happen only after transactional validation/application"
        )

    if not all(marker in video_input for marker in (
        "si_video_request_jpeg_snapshot(void)",
        "s_mjpeg_snapshot_request_generation",
        "s_mjpeg_snapshot_completed_generation",
        "si_video_frame_store_clear();",
    )) or "si_video_request_jpeg_snapshot()" not in video_http:
        failures.append(
            "snapshot requests must clear stale output-mode frames and copy one validated H.264 input frame on demand"
        )
    stale_borrow = section(
        video_input, "esp_err_t si_video_borrow_h264_jpeg_if_new(",
        "esp_err_t si_video_release_h264_jpeg(")
    if not ordered(
        stale_borrow,
        "if (!current ||",
        "xSemaphoreGive(s_h264_frame_lock)",
        "return_retained_uvc_frame(&item)",
    ):
        failures.append(
            "a rejected direct UVC lease must be returned after releasing "
            "the H.264 lease mutex"
        )
    input_wait = section(
        video_input, "esp_err_t si_video_wait_h264_jpeg_ready(",
        "esp_err_t si_video_borrow_h264_jpeg_if_new(")
    if not ordered(
        input_wait,
        "s_h264_transport_active",
        "xQueuePeek(s_h264_frame_queue",
        "ESP_ERR_TIMEOUT",
    ) or "xSemaphoreTake(s_h264_frame_lock" in input_wait:
        failures.append(
            "H.264 input readiness must use the frame queue as an event "
            "without waiting under the lease mutex"
        )
    for marker in (
        "xQueueCreate(1U, sizeof(si_uvc_frame_item_t))",
        "si_video_borrow_h264_jpeg_if_new(",
        "si_video_release_h264_jpeg(",
        "si_video_flush_h264_jpeg(",
        "invalidate_h264_frame_queue();",
    ):
        if marker not in video_input:
            failures.append(f"dedicated H.264 UVC lease boundary missing: {marker}")
    if "#if SI_CFG_VIDEO_H264_ENABLED && SI_CFG_VIDEO_FRAME_BUFFERS < 3" not in video_input:
        failures.append(
            "H.264 triple-buffer compile guard must use the defined feature macro"
        )
    init_uvc = section(video_input, "static esp_err_t init_uvc(void)",
                       "static void set_last_error_locked(")
    if not ordered(
        init_uvc,
        "#if SI_CFG_VIDEO_H264_ENABLED",
        "s_h264_frame_queue = xQueueCreate(",
        "s_h264_frame_lock = xSemaphoreCreateMutex();",
        "#endif",
    ):
        failures.append(
            "Stable must not allocate the disabled H.264 UVC queue or mutex"
        )

    handoff = section(
        video_input, "static esp_err_t handoff_h264_uvc_frame(",
        "static bool uvc_frame_cb(")
    if "uxQueueMessagesWaiting(s_h264_frame_queue) != 0U" not in handoff:
        failures.append("direct H.264 handoff must keep a one-frame pending boundary")
    if "s_h264_borrowed.active" in handoff:
        failures.append(
            "direct H.264 handoff must allow one pending frame while the "
            "decoder owns the current frame"
        )

    session_boundary = section(
        h264, "if (session != local_session)",
        "httpd_ws_client_info_t client")
    if "si_video_flush_h264_jpeg();" not in session_boundary:
        failures.append(
            "every H.264 browser session boundary must discard pending "
            "direct UVC input"
        )
    input_empty = section(
        h264,
        "if (err == ESP_ERR_NOT_FINISHED || err == ESP_ERR_NOT_FOUND)",
        "if (err != ESP_OK)")
    if input_empty.count("si_video_wait_h264_jpeg_ready(") < 2 or \
            "vTaskDelay(pdMS_TO_TICKS(5))" in input_empty:
        failures.append(
            "H.264 empty-input handling must wait on queue readiness rather "
            "than drift on a fixed polling delay"
        )

    drain = section(
        video_input, "static bool wait_for_ingest_drain(",
        "static void invalidate_stream_generation(")
    if "uvc_retained_frame_count()" not in drain:
        failures.append("UVC drain must wait on the end-to-end retained count")
    for stale_counter in (
        "uxQueueMessagesWaiting",
        "s_uvc_ingest_inflight",
    ):
        if stale_counter in drain:
            failures.append(
                f"UVC drain contains a dequeue/in-flight observation gap: {stale_counter}"
            )

    stop = section(
        video_input, "static bool stop_uvc_stream(void)",
        "static bool close_uvc_stream(")
    stop_active = section(
        stop, "invalidate_stream_generation();",
        "static bool close_uvc_stream(")
    if not ordered(
        stop_active,
        "uvc_host_stream_stop(stream)",
        "s_uvc_session_state = SI_UVC_SESSION_DRAINING;",
        "wait_for_ingest_drain(",
        "s_uvc_session_state = SI_UVC_SESSION_OPEN_STOPPED;",
    ):
        failures.append(
            "UVC stop must invalidate old work, stop production, and drain all retained frames"
        )
    for destructive in (
        "uvc_host_stream_close(",
        "__atomic_store_n(&s_uvc_stream, NULL",
        "usb_host_lib_set_root_port_power(",
    ):
        if destructive in stop:
            failures.append(
                f"ordinary UVC stop must retain the session and URB ring: {destructive}"
            )
    if not ordered(
        stop,
        "if (!wait_for_ingest_drain(",
        "s_uvc_session_state = SI_UVC_SESSION_DRAINING;",
        "s_uvc_stop_failures++;",
        "return false;",
        "s_uvc_session_state = SI_UVC_SESSION_OPEN_STOPPED;",
    ):
        failures.append(
            "a UVC drain timeout must remain retryable and must not publish a stopped state"
        )

    close = section(
        video_input, "static bool close_uvc_stream(",
        "static void uvc_supervisor_task(")
    if not ordered(
        close,
        "if (stop_first && !stop_uvc_stream())",
        "return false;",
        "wait_for_ingest_drain(",
        "uvc_host_stream_close(stream)",
        "if (close_ret != ESP_OK)",
        "return false;",
        "__atomic_store_n(&s_uvc_stream, NULL",
    ):
        failures.append(
            "UVC close may clear ownership before stop, drain, and driver close all succeed"
        )
    if "usb_host_lib_set_root_port_power(" in close:
        failures.append(
            "UVC close must not power-cycle a root port while a live handle may remain"
        )

    supervisor = section(
        video_input, "static void uvc_supervisor_task(",
        "static esp_err_t init_uvc(")
    selection = section(
        video_input, "static esp_err_t get_uvc_selection(",
        "static esp_err_t reserve_uvc_dma_budget(")
    enumeration = section(
        video_input, "static void uvc_driver_event_cb(",
        "static void usb_host_task(")
    if not ordered(
        enumeration,
        "store_uvc_modes_locked(",
        "has_selection = select_stored_mode_locked(",
        "s_uvc_selection = selected;",
        "if (has_selection && s_uvc_supervisor_task)",
        "xTaskNotifyGive(s_uvc_supervisor_task);",
    ):
        failures.append(
            "UVC enumeration must publish a physical mode and wake pre-open even while capture is idle"
        )
    select_gate = section(
        enumeration, "store_uvc_modes_locked(",
        "has_selection = select_stored_mode_locked(")
    if "if (s_capture_enabled)" in select_gate:
        failures.append(
            "UVC physical mode selection is still gated by browser capture demand"
        )
    if not ordered(
        selection,
        "bool *selection_valid",
        "bool *capture_requested",
        "const bool valid = s_uvc_selection.valid;",
        "*selection_valid = valid;",
        "*selection = s_uvc_selection;",
        "*capture_requested = s_capture_enabled;",
        "return ESP_OK;",
    ):
        failures.append(
            "physical UVC selection must be returned independently from capture demand"
        )
    if not ordered(
        selection,
        "xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE",
        "return ESP_ERR_TIMEOUT;",
    ):
        failures.append(
            "a busy UVC selection snapshot must be distinguishable from an invalid physical selection"
        )
    idle = section(
        supervisor, "if (!capture_requested)",
        "if (s_uvc_session_state == SI_UVC_SESSION_DRAINING ||")
    if "stop_uvc_stream()" not in idle:
        failures.append("idle video demand must stop but retain the UVC session")
    if "close_uvc_stream(" in idle:
        failures.append("ordinary idle still destroys the retained UVC session")
    if not ordered(
        supervisor,
        "__atomic_exchange_n(&s_uvc_device_gone_pending",
        "consume_uvc_device_gone();",
        "if (s_uvc_close_pending)",
        "close_uvc_stream(true);",
        "__atomic_exchange_n(&s_uvc_transport_fault",
        "s_uvc_close_pending = true;",
        "continue;",
        "selection_ret = get_uvc_selection(",
        "if (selection_ret != ESP_OK)",
        "continue;",
        "if (!selection_valid)",
        "if (!opened)",
        "uvc_host_stream_open(",
        "if (!capture_requested)",
    ):
        failures.append(
            "fatal/close-pending state must be consumed before physical selection, pre-open, and idle handling"
        )
    if not ordered(
        supervisor,
        "selection_ret = get_uvc_selection(",
        "if (selection_ret != ESP_OK)",
        "continue;",
        "if (!selection_valid)",
        "if (!opened)",
        "uvc_host_stream_open(",
        "__atomic_store_n(&s_uvc_stream, opened, __ATOMIC_RELEASE);",
        "s_uvc_session_state = SI_UVC_SESSION_OPEN_STOPPED;",
        "if (!capture_requested)",
    ):
        failures.append(
            "enumeration must pre-open one retained UVC session before capture demand is evaluated"
        )
    no_selection = section(
        supervisor, "if (!selection_valid)", "if (!opened)")
    if not ordered(
        no_selection,
        "if (s_uvc_session_state != SI_UVC_SESSION_OPEN_STOPPED",
        "stop_uvc_stream()",
        "if (!capture_requested && s_lock",
        'reset_frame_state_locked("video capture idle")',
    ):
        failures.append(
            "a released final video lease must settle back to idle even when the retained UVC stream is already stopped"
        )
    active_selection_refresh = section(
        supervisor, "bool no_frame_timeout = false;",
        "if (no_frame_timeout && stopped")
    if not ordered(
        active_selection_refresh,
        "requested_ret = get_uvc_selection(",
        "if (requested_ret != ESP_OK)",
        "xTaskNotifyGive(s_uvc_supervisor_task);",
        "continue;",
        "if (requested_valid && requested_capture",
        "idle_requested = !requested_capture;",
    ):
        failures.append(
            "a transient UVC selection lock timeout must preserve the wakeup, and an explicit stop must be remembered through unwind"
        )
    if not ordered(
        active_selection_refresh,
        "bool idle_requested = false;",
        "idle_requested = !requested_capture;",
        "stopped = stop_uvc_stream();",
        "idle_requested && stopped ?",
        '"video capture idle"',
        '"UVC stream restarting"',
    ):
        failures.append(
            "an explicit final-lease stop must publish idle immediately after a successful UVC unwind"
        )
    if not ordered(
        supervisor,
        "if (s_uvc_session_state == SI_UVC_SESSION_DRAINING ||",
        "if (!stop_uvc_stream())",
        "if (s_uvc_session_state == SI_UVC_SESSION_OPEN_STOPPED)",
        "uvc_host_stream_start(opened)",
    ):
        failures.append(
            "UVC restart must finish a pending drain and start only from OPEN_STOPPED"
        )
    for marker in (
        "SI_UVC_HEALTHY_RESET_CALLBACKS",
        "SI_UVC_HEALTHY_RESET_MS",
        "healthy_callbacks",
        "healthy_since",
    ):
        if marker not in supervisor:
            failures.append(
                f"UVC no-frame recovery lacks a sustained-health reset guard: {marker}"
            )
    callback_change = section(
        supervisor, "if (callbacks != last_callbacks)",
        "if ((int32_t)(xTaskGetTickCount() - no_frame_since)")
    reset_at = callback_change.find("s_no_frame_reopen_count = 0U;")
    threshold_at = callback_change.find("SI_UVC_HEALTHY_RESET_CALLBACKS")
    duration_at = callback_change.find("SI_UVC_HEALTHY_RESET_MS")
    if reset_at < 0 or not (0 <= threshold_at < reset_at and
                            0 <= duration_at < reset_at):
        failures.append(
            "one callback can still erase accumulated UVC reopen failures"
        )

    dma_reserve = section(
        video_input, "static esp_err_t reserve_uvc_dma_budget(",
        "static void release_uvc_dma_budget_for_open(")
    dma_release = section(
        video_input, "static void release_uvc_dma_budget_for_open(",
        "static esp_err_t reserve_uvc_frame_arena(")
    video_init = section(
        video_input, "esp_err_t si_video_init(void)",
        "void si_video_get_status(")
    if "#define SI_UVC_DMA_RESERVE_BYTES (64U * 1024U)" not in video_input:
        failures.append("UVC pre-open internal-DMA reserve must remain 64 KiB")
    if not ordered(
        dma_reserve,
        "if (s_uvc_dma_reserve)",
        "heap_caps_aligned_alloc(",
        "512U, SI_UVC_DMA_RESERVE_BYTES",
        "MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT",
    ):
        failures.append(
            "UVC DMA reserve must be a single 512-byte-aligned internal-DMA allocation"
        )
    if not ordered(
        dma_release,
        "if (!s_uvc_dma_reserve)",
        "return;",
        "heap_caps_free(s_uvc_dma_reserve);",
        "s_uvc_dma_reserve = NULL;",
    ):
        failures.append(
            "first-open DMA reserve release must be idempotent and clear ownership"
        )
    if not ordered(
        close,
        "uvc_host_stream_close(stream)",
        "__atomic_store_n(&s_uvc_stream, NULL",
        "return true;",
    ):
        failures.append(
            "successful UVC close must clear the application handle after the component retains its stopped ring"
        )
    for forbidden in (
        "rearm_uvc_dma_budget_after_close",
        "reserve UVC DMA reopen budget",
    ):
        if forbidden in video_input:
            failures.append(
                f"UVC restart must not depend on reallocating a fragmented DMA reserve: {forbidden}"
            )
    if not ordered(
        video_init,
        "si_video_reserve_boot_memory()",
        "init_uvc()",
    ):
        failures.append(
            "video initialization must complete the UVC boot-memory barrier before enumeration/open"
        )
    first_open = section(supervisor, "if (!opened)", "} else if (!uvc_transport_matches")
    if not ordered(
        first_open,
        "release_uvc_dma_budget_for_open();",
        "uvc_host_stream_config_t config",
        "uvc_host_stream_open(",
    ):
        failures.append(
            "the first UVC open must release the boot reserve immediately before opening the reusable ring"
        )
    if video_input.count("release_uvc_dma_budget_for_open();") != 1:
        failures.append(
            "UVC DMA reserve may only be released by the retained first-open path"
        )

    release_patch = section(
        project_cmake, "set(si_uvc_release_fixed",
        "string(FIND \"${si_uvc_host_patched}\" \"${si_uvc_release_upstream}\"")
    if not ordered(
        release_patch,
        "usb_host_interface_release(",
        "if (release_ret != ESP_OK)",
        "ret = release_ret;",
        "goto exit;",
    ):
        failures.append(
            "usb_host_uvc release failure must return without removing or freeing the stream"
        )
    for unsafe in (
        "release_ret != ESP_ERR_INVALID_STATE",
        "release_ret != ESP_ERR_NOT_FOUND",
        "uvc_device_remove(",
        "usb_host_transfer_free(",
    ):
        if unsafe in release_patch:
            failures.append(
                f"usb_host_uvc release failure can still fall through to free: {unsafe}"
            )

    for marker in (
        "uvc_transfer_cache_t",
        "static void uvc_transfer_cache_free(void)",
        "static bool uvc_transfer_cache_matches(",
        "static void uvc_transfers_retain(",
        "Reusing %u stopped USB transfers",
        "uvc_device_remove(uvc_stream, false)",
        "uvc_device_remove(uvc_stream, true)",
        "uvc_transfer_cache_free();",
    ):
        if marker not in project_cmake:
            failures.append(
                f"guarded usb_host_uvc patch lacks retained-ring lifecycle marker: {marker}"
            )
    if not ordered(
        project_cmake,
        "if (uvc_transfer_cache_matches(",
        "uvc_stream->constant.xfers = s_uvc_transfer_cache.xfers;",
        "memset(&s_uvc_transfer_cache, 0",
        "if (uvc_stream->constant.xfers[i] == NULL)",
    ):
        failures.append(
            "usb_host_uvc must consume an exact cached topology before allocating replacement URBs"
        )

    loss_patch = cmake_bracket_block(project_cmake, "si_uvc_loss_fixed")
    for marker in (
        "case USB_TRANSFER_STATUS_TIMED_OUT:",
        "case USB_TRANSFER_STATUS_SKIPPED:",
        "uvc_host_frame_t *loss_frame",
        "const bool completed_mjpeg_waiting_for_fid",
        "loss_frame->data_len >= 2",
        "loss_frame->data[loss_frame->data_len - 2] == JPEG_MARKER",
        "loss_frame->data[loss_frame->data_len - 1] == 0xD9U",
        "if (loss_frame && !completed_mjpeg_waiting_for_fid)",
        "uvc_stream->single_thread.skip_current_frame = true;",
    ):
        if marker not in loss_patch:
            failures.append(
                f"MS2109 packet-loss/terminal-EOI guard missing: {marker}"
            )
    if loss_patch and not ordered(
        loss_patch,
        "uvc_host_frame_t *loss_frame",
        "const bool completed_mjpeg_waiting_for_fid",
        "loss_frame->data_len >= 2",
        "loss_frame->data[loss_frame->data_len - 2] == JPEG_MARKER",
        "loss_frame->data[loss_frame->data_len - 1] == 0xD9U",
        "if (loss_frame && !completed_mjpeg_waiting_for_fid)",
        "uvc_stream->single_thread.skip_current_frame = true;",
    ):
        failures.append(
            "MS2109 loss handling taints a complete EOI frame while it waits for the next FID"
        )

    hid_guard = section(status_ws, "static bool hid_ws_handshake_auth_guard(",
                        "static esp_err_t hid_ws_reject_after_send(")
    for marker in (
        "si_auth_session_get_live_context(",
        "ctx->session_id",
        "ctx->auth_generation",
        "SI_PRINCIPAL_BROWSER",
        "SI_CAPABILITY_HID",
        "si_control_lease_kvm_hid_owner_is_current(",
        "ctx->stream_owner_epoch",
    ):
        if marker not in hid_guard:
            failures.append(f"KVM HID live owner guard missing: {marker}")
    hid_pre_handshake = section(
        status_ws, "esp_err_t hid_ws_pre_handshake(",
        "esp_err_t hid_ws_handler(")
    hid_handler = section(status_ws, "esp_err_t hid_ws_handler(", "#endif")
    if not ordered(
        hid_pre_handshake,
        "hid_ws_stream_id(req, &stream_id, &stream_id_present)",
        "if (!stream_id_present)",
        "si_http_require_capability(req, SI_CAPABILITY_HID, &session)",
        "si_control_lease_claim_kvm_hid_owner(",
        "req->sess_ctx = ctx;",
        "req->free_ctx = hid_ws_ctx_free;",
    ) or not ordered(
        hid_handler,
        "hid_ws_ctx_t *ctx = (hid_ws_ctx_t *)req->sess_ctx;",
        "si_hid_json_execute_owned(",
        "root, &ctx->owner, hid_ws_live_guard, ctx",
    ):
        failures.append(
            "HID frames must use the immutable auth/stream owner token from sess_ctx"
        )
    frame_path = section(
        hid_handler,
        "hid_ws_ctx_t *ctx = (hid_ws_ctx_t *)req->sess_ctx;",
        "#endif",
    )
    if "hid_ws_stream_id(req" in frame_path:
        failures.append(
            "HID data frames must not reparse an unreliable WebSocket request query"
        )
    for direct_touch in (
        "si_control_lease_touch_kvm(",
        "si_video_control_touch_kvm(",
    ):
        if direct_touch in hid_handler:
            failures.append(
                f"HID handler bypasses tagged passive validation: {direct_touch}"
            )
    hid_ctx_free = section(status_ws, "static void hid_ws_ctx_free(",
                           "static void *status_calloc(")
    if not ordered(
        hid_ctx_free,
        "si_hid_owner_release_if_current(&ctx->owner);",
        "free(ctx);",
    ):
        failures.append(
            "HID socket teardown must release only its exact server-issued owner token"
        )
    if not ordered(
        hid_pre_handshake,
        "req->sess_ctx = ctx;",
        "req->free_ctx = hid_ws_ctx_free;",
    ) or not ordered(
        hid_handler,
        "HTTPD_WS_TYPE_CLOSE",
        "hid_ws_ctx_free(ctx);",
    ):
        failures.append(
            "HID WebSocket must install and execute a disconnect cleanup context"
        )
    if not ordered(
        web_server,
        "cfg.ws_pre_handshake_cb = handler == hid_ws_handler",
        "? hid_ws_pre_handshake",
        ": ws_auth_pre_handshake;",
    ):
        failures.append(
            "HID session context must be installed by the ESP-IDF pre-handshake callback"
        )
    if ("return send_ret == ESP_OK ? ESP_ERR_INVALID_STATE : send_ret;" not in
            section(status_ws, "static esp_err_t hid_ws_reject_after_send(",
                    "esp_err_t overall_status_handler(")):
        failures.append(
            "a transmitted HID pre-handshake denial must still stop the WebSocket upgrade"
        )

    desired = section(video_control, "static void desired_locked(",
                      "static bool request_equal(")
    if not ordered(desired, "if (kvm_active_locked(now_ms))",
                   "if (agent_takeover_active_locked(now_ms))"):
        failures.append(
            "a visible Stable KVM mode must outrank the Agent preview capture mode"
        )
    acquire = section(video_control,
                      "static bool kvm_transition_under_hid_gate(",
                      "static bool run_kvm_transition(")
    for marker in (
        "KVM_OP_ACQUIRE",
        "bool expired = context->claim && !was_kvm_active && !canceled;",
        "bool live_heartbeat = heartbeat && was_kvm_active &&",
        "s_kvm_auth_generation == context->auth_generation;",
        "KVM_OP_RELEASE",
        "KVM_OP_WS_CLAIM",
        "KVM_OP_EXPIRE",
        "transition->replace = true;",
        "current->claim.kind == SI_HID_OWNER_CONTROL_LEASE",
    ):
        if marker not in acquire:
            failures.append(
                f"expired KVM claims and delayed heartbeats are not separated: {marker}"
            )
    acquire_case = section(acquire, "case KVM_OP_ACQUIRE:",
                           "case KVM_OP_RELEASE:")
    if "agent_takeover_active_locked" in acquire_case:
        failures.append(
            "Agent input ownership must not reject a new or renewed KVM video observer"
        )
    touch_agent = section(video_control,
                          "void si_video_control_touch_agent(bool takeover)",
                          "void si_video_control_keep_agent_alive(")
    for forbidden in ("s_kvm_stream_id = 0U", "s_kvm_seen = false",
                      "remember_canceled_stream_locked"):
        if forbidden in touch_agent:
            failures.append(
                f"Agent input takeover still destroys the observer video lease: {forbidden}"
            )
    if "si_video_control_kvm_hid_owner_is_current(" not in control_lease:
        failures.append(
            "HID authorization must be separate from video stream observation"
        )
    lease_desired = section(
        control_lease, "static void desired_owner_locked(",
        "typedef enum {")
    if not ordered(
        lease_desired,
        'if (active_locked(now_ms) && strcasecmp(s_mode, "observe") != 0)',
        "if (kvm_view_active && current &&",
    ):
        failures.append(
            "Agent/MCP HID ownership must outrank passive KVM video presence"
        )
    if "SI_CONTROL_LEASE_UPDATE_KVM_ACTIVE" in control_lease:
        failures.append(
            "control lease still treats a visible KVM page as an input conflict"
        )
    for marker in (
        "status->kvm_view_active",
        "status->input_control_active",
        "hid_owner.claim.kind == SI_HID_OWNER_KVM_STREAM",
        "status->can_request = !raw_agent_active",
    ):
        if marker not in control_lease:
            failures.append(
                f"control lease observer/input split missing {marker}"
            )
    if not ordered(
        hid_pre_handshake,
        "si_control_lease_get_status(&lease);",
        "if (lease.input_control_active)",
        "si_control_lease_claim_kvm_hid_owner(",
    ):
        failures.append(
            "passive KVM HID reconnect can still steal Agent/MCP input control"
        )

    if "let videoClaimed=true,videoLeaseHeld=false;" not in kvm_html:
        failures.append(
            "browser video demand and confirmed lease ownership must be separate states"
        )
    for marker in (
        "let msVideoPowerKnown=false,msVideoPowerOff=false;",
        "function suspendKvmForMsPowerOff()",
        "function resumeKvmAfterMsPowerOn()",
        "function syncMsVideoPowerState(statusValue)",
        'showVideoRecovery("MS2109 电源已关闭")',
        "if(!msVideoPowerKnown||msVideoPowerOff||!kvmPageActive",
        "if(kvmLeaseRetryTimer||msVideoPowerOff||kvmLeaving",
    ):
        if marker not in kvm_html:
            failures.append(
                f"KVM MS2109-off connection suppression missing {marker}"
            )
    for source, marker in (
        (device_observation_header, "si_device_ms2109_power_observation_t"),
        (device_observation, "si_device_observation_get_ms2109_power("),
        (device_http, "si_device_http_add_ms2109_power_json("),
        (status_ws, 'cJSON_AddObjectToObject(root, "ms2109")'),
    ):
        if marker not in source:
            failures.append(
                f"shared status lacks MS2109 power observation {marker}"
            )
    readonly = section(kvm_html, "function setKvmReadOnly(on)",
                       "async function reclaimKvmControl(")
    if "releaseKvmLease()" in readonly:
        failures.append(
            "Agent input takeover must not release the browser video lease"
        )
    release_all = section(kvm_html, "function releaseAll()",
                          "release.onclick=releaseAll")
    for marker in (
        "cancelPendingInputActions();",
        "stopTouch(null,false);",
        "stopDirectDrag(null,false);",
        'API.send({type:"releaseall"})',
    ):
        if marker not in release_all:
            failures.append(
                f"cancellation-safe browser input release is incomplete: {marker}"
            )
    if "/agent video takeover|409|preempt/i" in kvm_html:
        failures.append(
            "generic HTTP 409 must not be presented as Agent takeover"
        )
    for marker in (
        "function renderAgentPresence(agentViewing,inputControlActive,controller,browserHealthy)",
        'lease.agent_owner==="mcp"?"MCP":"Agent"',
        '`${controller} 正在控制输入`',
        "只读观察 · 视频共享",
        "人工输入不受影响",
        "KVM 视频继续共享",
        "可随时终止自动控制",
        '"/api/control/lease",{active:false,force:true',
        ">终止自动控制</button>",
    ):
        if marker not in kvm_html:
            failures.append(
                f"KVM Agent observation/takeover disclosure missing: {marker}"
            )
    if '"human KVM video is active"' in video_http:
        failures.append(
            "Agent read-only observation must share video with an active KVM"
        )
    if "if(selectedOutputMode===\"h264\")h264ReconnectAttempts=0" in kvm_html:
        failures.append(
            "one decoded H.264 frame must not reset the bounded reconnect budget"
        )
    if "h264StableTimer=UI.lifecycle.timeout" not in kvm_html:
        failures.append(
            "H.264 reconnect budget needs a sustained-health reset timer"
        )
    h264_packet = section(kvm_html, "function h264Packet(",
                          "function startH264Transport(")
    for marker in (
        "h264TransportCurrent(generation,expectedStreamId)",
        "h264SpecMatches(width,height)",
        "rememberH264Parameters(nalus)",
    ):
        if marker not in h264_packet:
            failures.append(
                f"H.264 selected-mode/render contract missing: {marker}"
            )
    webcodecs_submit = section(
        kvm_html, "function submitH264WebCodecsPacket(",
        "function finishH264BackendProbe(")
    for marker in (
        "h264SpecMatches(w,h)",
        "h264LastFrameAt=performance.now();markVideoReady()",
        "annexBWithParameters(packet.nalus,sets)",
        "switchH264ToMse(generation,expectedStreamId,`WebCodecs 解码失败",
        'switchH264ToMse(generation,expectedStreamId,"WebCodecs 配置失败',
        'switchH264ToMse(generation,expectedStreamId,"WebCodecs 帧提交失败',
    ):
        if marker not in webcodecs_submit:
            failures.append(
                f"H.264 WebCodecs render/fallback contract missing: {marker}"
            )
    backend_probe = section(
        kvm_html, "function finishH264BackendProbe(",
        "function h264Packet(")
    for marker in (
        "VideoDecoder.isConfigSupported(config)",
        "result?.supported===true",
        "result?.config||config",
        "switchH264ToMse(probe.generation,probe.expectedStreamId",
        'h264Mode="webcodecs"',
    ):
        if marker not in backend_probe:
            failures.append(
                f"H.264 capability probe/fallback contract missing: {marker}"
            )
    mse_fallback = section(
        kvm_html, "function switchH264ToMse(",
        "function h264WebCodecsConfig(")
    for marker in (
        'h264Mode="mse"',
        "startMse(generation,expectedStreamId)",
        "seedH264MseState(h264Mse)",
    ):
        if marker not in mse_fallback:
            failures.append(
                f"H.264 same-session MSE fallback missing: {marker}"
            )
    for forbidden in (
        "closeH264Transport()",
        "startMjpegTransport(",
        'selectedOutputMode="mjpeg"',
    ):
        if forbidden in mse_fallback:
            failures.append(
                f"H.264 decoder fallback must keep the same H.264 session/selection: {forbidden}"
            )
    packet_before_decoder = section(h264_packet,
                                    "function h264Packet(",
                                    "if(h264Mode===\"mse\")")
    if "h264LastFrameAt=performance.now()" in packet_before_decoder:
        failures.append(
            "receiving an H.264 packet must not count as a rendered healthy frame"
        )
    start_h264 = section(kvm_html, "function startH264Transport(",
                         "function stopVideoForPeer(")
    for marker in (
        "const selected=selectedCaptureSpec()",
        "if(!h264CaptureSpecSupported(selected))return false",
        "h264TransportSpec={...selected,streamId:expectedStreamId,generation,modeGeneration:videoModeSwitchGeneration}",
        "h264ParameterSets={sps:null,pps:null,codec:\"\",injected:false}",
        'typeof VideoDecoder.isConfigSupported==="function"',
        'h264Mode=webCodecs?"webcodecs-probe":"mse"',
    ):
        if marker not in start_h264:
            failures.append(
                f"H.264 transport is not bound to the selected mode/generation: {marker}"
            )
    for marker in (
        "function h264CaptureSpecSupported(spec)",
        "spec.width<=1280&&spec.height<=720?3000:2500",
        "当前采集帧率超出 H.264 验证范围（1080p25 / 720p30）",
    ):
        if marker not in kvm_html:
            failures.append(
                f"KVM must reject unvalidated H.264 capture rates explicitly: {marker}"
            )
    mse_render = section(kvm_html, "function noteMseRenderedFrame(",
                         "function startMse(")
    if not ordered(mse_render, "h264MseVideo.videoWidth",
                   "h264SpecMatches(width,height)",
                   "h264LastFrameAt=performance.now();", "markVideoReady()"):
        failures.append(
            "MSE health must be published only after a matching rendered frame"
        )
    if kvm_html.count("restartH264Transport(expectedStreamId") != 2:
        failures.append(
            "all H.264 transport restarts must pass through the bounded recoverH264 budget"
        )
    if "function recoverH264(expectedStreamId,message){if(h264RestartTimer||" not in kvm_html:
        failures.append(
            "concurrent H.264 errors can consume retry budget without starting a retry"
        )
    if ("outputModeField.hidden=!h264OutputAvailable" not in kvm_html or
            ".side-field[hidden]{display:none!important}" not in kvm_html):
        failures.append(
            "Stable must hide the unavailable experimental output-mode selector"
        )
    if 'option.textContent="H.264（实验，1080p25 / 720p30）"' not in kvm_html:
        failures.append(
            "experimental H.264 option must state its validated mode rates"
        )
    if "Number(metrics.stream_id)===videoStreamId" not in kvm_html:
        failures.append(
            "KVM metrics must be scoped to the currently claimed stream id"
        )
    hid_connect = section(kvm_html, "function hidMayConnect()",
                          "function disconnectHidWs()")
    for marker in (
        "!kvmReadOnly",
        "!videoPeerBlocked",
        "hidConnectGeneration",
        "hidConnectPendingGeneration",
        "hidSocketStreamId===videoStreamId",
        "const generation=hidConnectGeneration,streamId=videoStreamId",
        "streamId!==videoStreamId",
        "scheduleHidReconnect",
    ):
        if marker not in hid_connect:
            failures.append(
                f"HID reconnect ownership/cancellation contract missing: {marker}"
            )
    focus_resume = section(kvm_html, "function clearFocusRefreshPending()",
                           "function cancelBootSequenceOnLeave()")
    for marker in (
        "windowHasFocus()",
        "videoModeSwitching",
        "kvmWindowBlurred",
        "hidFocusBlocked=true",
        "function finishKvmFocusRefresh()",
        "hidFocusBlocked=false",
        "clearFocusRefreshPending()",
    ):
        if marker not in focus_resume:
            failures.append(
                f"focus refresh cancellation contract missing: {marker}"
            )
    hid_gesture = section(kvm_html, "function showHidGestureBlocked()",
                          "function directMoveTo(")
    for marker in (
        'setKeyboardCaptureMsg("HID 正在连接，请重试","warn")',
        "if(hidSocketIsCurrent())return true",
        "if(hidMayConnect())void API.connect()",
        "return false",
    ):
        if marker not in hid_gesture:
            failures.append(
                f"HID first-action reconnect guard missing: {marker}"
            )
    for marker in (
        "beginVideoTransition(",
        "waitForVideoTransitionReady(",
        "restartVideoForTransition(",
        "finishVideoTransition(",
    ):
        if kvm_html.count(marker) < 2:
            failures.append(
                f"mode/output/reload transitions are not serialized: {marker}"
            )

    if "include_screenshot && route.needs_control_lease" in agent_engine:
        failures.append(
            "Agent screenshot observation must not preempt human HID before any write action"
        )
    acquire_helper = section(agent_engine,
                             "static esp_err_t agent_run_acquire_kvm_control(",
                             "static esp_err_t agent_run_execute_to_json(")
    if not ordered(acquire_helper, "si_video_control_touch_agent(true);",
                   "si_video_control_apply();",
                   "si_control_lease_touch_agent_owned(",
                   "s_agent_run_job.hid_owner = owner;"):
        failures.append(
            "Agent ownership transition must mint and retain an exact producer token"
        )
    if "agent_run_acquire_kvm_control(\n            job_id, control_error" not in agent_dispatch:
        failures.append(
            "approved HID actions must acquire input ownership at execution time"
        )

    mjpeg = section(
        video_http, "static esp_err_t stream_handler(",
        "static int httpd_client_count(")
    for marker in (
        "stream_id_present",
        "si_video_control_kvm_stream_is_current(stream_id)",
        '"stream_id is required for revocable KVM video"',
    ):
        if marker not in mjpeg:
            failures.append(
                f"MJPEG tagged fail-closed lease boundary missing: {marker}"
            )
    if "si_video_control_touch_kvm(" in mjpeg:
        failures.append(
            "an identityless MJPEG transport must not renew KVM HID authority"
        )
    if mjpeg.count("si_video_control_kvm_stream_is_current(stream_id)") < 2:
        failures.append(
            "MJPEG must reject a stale tagged stream both before and during frame delivery"
        )

    send_worker = section(
        h264, "static void h264_httpd_send_work(",
        "static esp_err_t pipeline_start_egress(")
    if not ordered(
        send_worker,
        "send_stopping, __ATOMIC_ACQUIRE",
        "xSemaphoreTake(s_stream.lock, pdMS_TO_TICKS(50))",
        "s_stream.server == job.server",
        "s_stream.session == job.session",
        "s_stream.fd == job.fd",
        "s_stream.stream_id == job.stream_id",
        "xSemaphoreGive(s_stream.lock)",
        "si_video_control_kvm_stream_is_current(job.stream_id)",
        "client_before =",
        "httpd_ws_get_fd_info(job.server, job.fd)",
        "client_before == HTTPD_WS_CLIENT_WEBSOCKET",
        "send_attempted = true",
        "httpd_ws_send_frame_async(job.server, job.fd, &frame)",
        "httpd_sess_update_lru_counter(job.server, job.fd)",
        "if (send_attempted && err != ESP_OK)",
        "client_after =",
        "client_after == HTTPD_WS_CLIENT_WEBSOCKET",
        "stream_still_current(job.session, job.fd)",
        "if (active_send_failure)",
        "pipeline_return_au_slot(pipeline, job.slot_id)",
    ):
        failures.append(
            "H.264 HTTPD work must revalidate the final tuple and return its AU slot"
        )
    if "if (send_attempted && (err == ESP_OK || active_send_failure))" not in send_worker:
        failures.append(
            "H.264 HTTPD work must not publish a CLOSE-raced send error as the pipeline result"
        )
    if not ordered(
        send_worker,
        "if (active_send_failure)",
        "__atomic_fetch_add(&s_send_failures_total",
        "stream_fail(job.session, job.fd);",
    ) or send_worker.count("__atomic_fetch_add(&s_send_failures_total") != 1:
        failures.append(
            "H.264 HTTPD work must count and fail only a post-revalidated live-WebSocket send error"
        )
    if send_worker.count("pipeline_return_au_slot(pipeline, job.slot_id);") != 1:
        failures.append(
            "H.264 HTTPD work must return each queued AU exactly once across send and cancellation paths"
        )
    for marker in (
        ".slot_id = send_slot_id",
        ".payload = wire_packet",
        "pipeline_return_au_slot(pipeline, job.slot_id);",
        "stream_fail(job.session, job.fd);",
        "httpd_queue_work(server, h264_httpd_send_work, work)",
    ):
        if marker not in h264:
            failures.append(f"H.264 dual-AU sender ownership missing: {marker}")
    if h264.count("httpd_ws_send_frame_async(") != 1 or \
            "static void h264_send_task(" in h264:
        failures.append(
            "H.264 socket I/O must run only inside one queued HTTPD work callback"
        )

    encode_send = section(
        h264, "static esp_err_t encode_and_send_frame(",
        "static bool flush_decoded_frame(")
    initialize_h264 = section(
        h264, "static esp_err_t h264_runtime_reserve_internal(",
        "static esp_err_t h264_runtime_reserve(")
    for marker in (
        "#define SI_H264_AU_SLOT_COUNT 2U",
        "#define SI_H264_PRIMARY_AU_SLOT 0U",
        "#define SI_H264_SMALL_EGRESS_SLOT 1U",
        "#define SI_H264_SMALL_EGRESS_PAYLOAD_SIZE (1024U * 1024U)",
        "si_h264_au_slot_t au_slots[SI_H264_AU_SLOT_COUNT]",
        "uint8_t *s_preallocated_au[SI_H264_AU_SLOT_COUNT]",
        "if (primary_au_payload_size < SI_CFG_VIDEO_H264_MAX_ACCESS_UNIT)",
        "const size_t storage_sizes[SI_H264_AU_SLOT_COUNT]",
        "[SI_H264_PRIMARY_AU_SLOT] =",
        "[SI_H264_SMALL_EGRESS_SLOT] =",
        "s_preallocated_au[slot_id] = esp_h264_aligned_calloc(",
    ):
        if marker not in h264:
            failures.append(f"H.264 bounded AU reserve missing: {marker}")
    if not ordered(
        initialize_h264,
        "const bool au_reserve_complete =",
        "s_preallocated_au[SI_H264_PRIMARY_AU_SLOT]",
        "s_preallocated_au[SI_H264_SMALL_EGRESS_SLOT]",
        "if (!s_preallocated_yuv420 || !au_reserve_complete)",
        "startup codec buffer reserve failed",
    ):
        failures.append(
            "H.264 must require both bounded egress slots or preserve MJPEG-only availability"
        )
    for marker in (
        "pipeline_claim_au_slot(pipeline, primary_slot_id)",
        "return ESP_ERR_NOT_FINISHED;",
        "primary_slot->storage + SI_H264_PAYLOAD_OFFSET",
        "output.length <= small_payload_capacity",
        "pipeline_claim_au_slot(pipeline, SI_H264_SMALL_EGRESS_SLOT)",
        "memcpy(small_slot->storage + SI_H264_PAYLOAD_OFFSET",
        "pipeline_return_au_slot(pipeline, primary_slot_id);",
        ".slot_id = send_slot_id",
        "pipeline_return_au_slot(pipeline, send_slot_id);",
        "s_small_egress_access_units_total",
        "s_primary_egress_access_units_total",
        "__atomic_store_n(&pipeline->encoder_reset_required, true",
        "httpd_queue_work(server, h264_httpd_send_work, work)",
    ):
        if marker not in encode_send:
            failures.append(
                f"H.264 dual-AU encode/send boundary missing: {marker}"
            )
    if not ordered(
        encode_send,
        "pipeline_claim_au_slot(pipeline, primary_slot_id)",
        "esp_h264_enc_process(pipeline->h264_encoder, &input, &output)",
        "output.length <= small_payload_capacity",
        "pipeline_claim_au_slot(pipeline, SI_H264_SMALL_EGRESS_SLOT)",
        "memcpy(small_slot->storage + SI_H264_PAYLOAD_OFFSET",
        "send_slot_id = SI_H264_SMALL_EGRESS_SLOT;",
        "pipeline_return_au_slot(pipeline, primary_slot_id);",
        ".slot_id = send_slot_id",
        "httpd_queue_work(server, h264_httpd_send_work, work)",
    ):
        failures.append(
            "H.264 must encode only into the primary slot, then copy a known-small AU before releasing it"
        )
    if ".buffer = small_slot->storage" in encode_send:
        failures.append(
            "H.264 hardware encoder must never target the bounded small-egress slot"
        )
    for forbidden in (
        "SI_H264_ASYNC_SEND_STAGING_BYTES",
        "send_packet",
        "send_available",
        "httpd_ws_send_data(",
    ):
        if forbidden in h264:
            failures.append(
                f"obsolete H.264 staging/synchronous egress remains: {forbidden}"
            )

    decode_worker = section(
        h264, "static void jpeg_decode_worker_task(",
        "static esp_err_t pipeline_start_decode_worker(")
    release_source = section(
        h264, "static void decode_job_release_source(",
        "static size_t h264_encoder_output_capacity(")
    if not ordered(
        release_source,
        "if (job->h264_uvc_lease)",
        "si_video_release_h264_jpeg(&job->jpeg_view)",
        "si_video_release_jpeg(&job->jpeg_view)",
    ):
        failures.append(
            "H.264 decode jobs must distinguish direct UVC leases from frame-store views"
        )
    if not ordered(
        decode_worker,
        "h264_jpeg_decoder_process(",
        "decode_job_release_source(&job);",
        "xQueueSend(worker->results, &job,",
        "pdMS_TO_TICKS(SI_H264_WORKER_IO_TIMEOUT_MS)",
    ):
        failures.append(
            "the JPEG worker must return its retained UVC source only after DMA decode completes"
        )

    stream_loop = section(
        h264, "static void h264_stream_task(",
        "static esp_err_t ensure_stream_task(")
    transient_input = section(
        stream_loop,
        "if (err == ESP_ERR_NOT_FINISHED || err == ESP_ERR_NOT_FOUND)",
        "if (err != ESP_OK)")
    if not ordered(
        transient_input,
        "if (overlap && decoded_ready)",
        "overlap_wait_timeout_us(&pipeline)",
        "si_video_wait_h264_jpeg_ready(wait_ms);",
        "continue;",
        "chain.overlap_wait_timeouts++;",
        "flush_decoded_frame(",
    ):
        failures.append(
            "an overlap-capable sender must retain decoded A while it waits boundedly for B"
        )
    first_transient_flush = transient_input.find("flush_decoded_frame(")
    timeout_marker = transient_input.find("chain.overlap_wait_timeouts++;")
    if first_transient_flush < 0 or first_transient_flush < timeout_marker:
        failures.append(
            "decoded A is still flushed before the bounded next-frame wait expires"
        )

    overlap_submit = section(
        stream_loop,
        "const uint32_t paired_decode_frame =",
        "completed.decode_started_us = esp_timer_get_time();")
    if not ordered(
        overlap_submit,
        "decoded_ready ? jpeg_frame : 0U",
        "xQueueSend(pipeline.decode_worker.requests, &completed",
        "decode_inflight = true;",
        "flush_decoded_frame(",
        "paired_decode_frame",
    ):
        failures.append(
            "D(B) must be queued before E(A), with the pair identity carried into timing"
        )

    overlap_result = section(
        stream_loop, "if (overlap && decode_inflight)",
        "} else if (!overlap && decoded_ready")
    if not ordered(
        overlap_result,
        "xQueueReceive(pipeline.decode_worker.results, &completed",
        "chain_metrics_record_overlap(&chain, &overlap_probe, &completed);",
        "if (completed.result == ESP_OK)",
    ):
        failures.append(
            "D(B)/E(A) overlap timing must be recorded before the decode result is consumed"
        )
    for marker in (
        "#define SI_H264_OVERLAP_WAIT_PERIODS 2U",
        "#define SI_H264_OVERLAP_WAIT_SLACK_MS 10U",
        "overlap pairs=%u miss=%u avg=%u span=%u/%u wait=%u/%u timeout=%u",
    ):
        if marker not in h264:
            failures.append(f"H.264 overlap observability contract missing: {marker}")

    restart_encoder = section(
        h264, "static esp_err_t pipeline_restart_encoder_for_session(",
        "static esp_err_t encode_and_send_frame(")
    session_change = section(
        h264, "if (session != local_session)",
        "httpd_ws_client_info_t client")
    if not ordered(
        restart_encoder,
        "esp_h264_enc_close(",
        "pipeline->h264_open = false;",
        "esp_h264_enc_open(",
        "pipeline->pts_origin_valid = false;",
        "encoder_reset_required, false",
    ):
        failures.append(
            "every H.264 browser session must close/open encoder state before reuse"
        )
    if not ordered(
        session_change,
        "sequence = 0;",
        "pipeline_stop_decode_worker(&pipeline);",
        "pipeline_restart_encoder_for_session(&pipeline);",
    ):
        failures.append(
            "H.264 session generation change must reset sequence and restart encoder for an IDR"
        )

    live_selftest = section(
        h264, "esp_err_t si_h264_stream_self_test(void)",
        "esp_err_t si_h264_stream_self_test_file(")
    if "si_video_borrow_jpeg_if_new(UINT32_MAX, &jpeg_view)" not in live_selftest:
        failures.append(
            "live H.264 self-test must borrow the immutable JPEG view"
        )
    if "si_video_acquire_jpeg(" in live_selftest:
        failures.append(
            "live H.264 self-test still allocates and copies the captured JPEG"
        )
    file_selftest = section(
        h264, "esp_err_t si_h264_stream_self_test_file(",
        "static bool stream_snapshot("
    )
    if not ordered(
        file_selftest,
        "stat(path, &file_info)",
        "return ESP_ERR_INVALID_SIZE;",
        "h264_stream_claim_h264_resources(&resource_token)",
        "h264_runtime_reserve(resource_token)",
        "heap_caps_aligned_alloc(",
    ):
        failures.append(
            "file H.264 self-test must validate the bounded regular file before reserving boot-lifetime codec resources"
        )

    send_stop = section(
        h264, "static esp_err_t pipeline_stop_egress(",
        "static esp_err_t pipeline_release_internal(")
    pipeline_release = section(
        h264, "static esp_err_t pipeline_release_internal(",
        "static esp_err_t pipeline_release(")
    pipeline_release_wrapper = section(
        h264, "static esp_err_t pipeline_release(",
        "static esp_err_t pipeline_recreate_jpeg_decoder(")
    if not ordered(
        send_stop,
        "send_stopping, true",
        "SI_H264_EGRESS_STOP_TIMEOUT_MS",
        "free_au_mask, __ATOMIC_ACQUIRE",
        "all_free_mask",
        "pipeline->teardown_poisoned = true;",
        "return ESP_ERR_TIMEOUT;",
        "vTaskDelay(1)",
    ):
        failures.append(
            "H.264 cleanup must wait for every queued HTTPD work item before free"
        )
    if not ordered(
        pipeline_release,
        "worker_err = pipeline_stop_decode_worker(pipeline);",
        "egress_err = pipeline_stop_egress(pipeline);",
        "pipeline->teardown_poisoned = true;",
        "return worker_err != ESP_OK ? worker_err : egress_err;",
        "esp_h264_enc_close(",
        "jpeg_del_decoder_engine(",
        "s_codec_pipeline_claimed, false",
        "memset(pipeline, 0, sizeof(*pipeline));",
    ):
        failures.append(
            "H.264 pipeline cleanup must join workers before releasing codec-owned memory"
        )
    if "return pipeline_release_internal(pipeline, true);" not in pipeline_release_wrapper:
        failures.append(
            "ordinary H.264 pipeline cleanup must release the global codec gate"
        )

    pipeline_configure = section(
        h264, "static esp_err_t pipeline_configure(",
        "static esp_err_t h264_runtime_reserve_internal(void)")
    fps_policy = section(
        h264, "static uint32_t h264_output_fps_cap(",
        "static esp_err_t pipeline_stop_decode_worker(")
    for marker in (
        "width <= 1280U && height <= 720U",
        "SI_CFG_VIDEO_H264_720P_MAX_FPS",
        "SI_CFG_VIDEO_H264_HIGH_RES_MAX_FPS",
        "static bool h264_output_mode_supported(",
        "width >= 80U && width <= 1920U",
        "height >= 80U && height <= 2032U",
        "requested_fps_x100 >= 100U",
        "requested_fps_x100 <= h264_output_fps_cap(width, height) * 100U",
        "(requested_fps_x100 + 50U) / 100U",
    ):
        if marker not in fps_policy:
            failures.append(
                f"H.264 per-resolution frame-rate policy missing: {marker}"
            )
    if h264.count("h264_output_fps_for_mode(") != 3:
        failures.append(
            "live and self-test H.264 paths must share one frame-rate policy"
        )
    for marker in (
        "#define SI_CFG_VIDEO_H264_HIGH_RES_MAX_FPS 25U",
        "#define SI_CFG_VIDEO_H264_720P_MAX_FPS 30U",
    ):
        if marker not in read("main/config/app_config.h"):
            failures.append(
                f"H.264 validated frame-rate contract must be compile-time fixed: {marker}"
            )
    self_test = section(
        h264, "esp_err_t si_h264_stream_self_test(void)",
        "esp_err_t si_h264_stream_self_test_file(")
    if not ordered(
        self_test,
        "si_video_get_status(&initial_video)",
        "h264_output_mode_supported(",
        "return ESP_ERR_NOT_SUPPORTED;",
        "h264_stream_claim_h264_resources(&resource_token)",
        "h264_runtime_reserve(resource_token)",
        "h264_stream_self_test_jpeg(",
        "initial_video.target_fps_x100",
        "resource_token",
        "h264_stream_release_h264_resources(resource_token)",
    ):
        failures.append(
            "live H.264 self-test must reject unsupported modes before reserving boot-lifetime codec resources"
        )
    post_handshake = section(
        h264, "esp_err_t si_h264_stream_ws_post_handshake(",
        "esp_err_t si_h264_stream_ws_handler(")
    if not ordered(
        post_handshake,
        "si_h264_stream_initialize()",
        "si_video_get_status(&video)",
        "h264_output_mode_supported(",
        "return ESP_ERR_NOT_SUPPORTED;",
        "ensure_stream_task()",
    ):
        failures.append(
            "H.264 WebSocket must reject unvalidated capture rates before starting its worker"
        )
    stream_loop = section(
        h264, "static void h264_stream_task(void *arg)",
        "static esp_err_t ensure_stream_task(void)")
    if not ordered(
        stream_loop,
        "jpeg_decoder_get_info(",
        "h264_output_mode_supported(info.width, info.height,",
        "si_video_release_h264_jpeg(&jpeg_view)",
        "stream_fail(session, fd)",
        "h264_output_fps_for_mode(",
    ):
        failures.append(
            "an active H.264 session must fail closed if a mode switch leaves the validated rate contract"
        )
    for marker in (
        "unsupported_modes = [mode.label for mode in modes if not mode.h264_supported]",
        "parser.error(",
        "H.264 validation supports only 1080p25 / 720p30 or lower",
        "pts_cadence[\"fps\"] <= target_fps * MAX_FPS_RATIO",
        "steady_fps <= target_fps * MAX_FPS_RATIO",
        "max(window_fps) <= target_fps * MAX_FPS_RATIO",
        'int(h264.get("target_fps", 0) or 0)',
    ):
        if marker not in h264_accept:
            failures.append(
                f"H.264 acceptance tool must reject unvalidated rates: {marker}"
            )
    if "fps_value > SI_CFG_VIDEO_FPS" in h264:
        failures.append(
            "H.264 still applies the 1080p default FPS cap to every resolution"
        )
    if not ordered(
        pipeline_configure,
        "uint64_t primary_au_payload_size_64 = raw_size_64;",
        "primary_au_payload_size_64 < SI_CFG_VIDEO_H264_MAX_ACCESS_UNIT",
        "primary_au_payload_size_64 = SI_CFG_VIDEO_H264_MAX_ACCESS_UNIT;",
        "primary_au_payload_size_64 + SI_H264_PAYLOAD_OFFSET",
        "SI_H264_SMALL_EGRESS_PAYLOAD_SIZE + SI_H264_PAYLOAD_OFFSET",
    ):
        failures.append(
            "H.264 per-mode resources must retain a worst-case primary AU and bounded small egress"
        )
    if not ordered(
        pipeline_configure,
        "if (!pipeline->owns_codec_gate)",
        "__atomic_compare_exchange_n(&s_codec_pipeline_claimed",
        "pipeline->owns_codec_gate = true;",
        "pipeline_release_internal(pipeline, false);",
    ):
        failures.append(
            "H.264 reconfiguration must retain or acquire one global hardware-codec owner gate before rebuild"
        )
    if not ordered(
        pipeline_configure,
        "esp_h264_enc_del(s_preallocated_encoder)",
        "s_preallocated_encoder = NULL;",
        "esp_h264_enc_hw_new(&h264_cfg, &s_preallocated_encoder)",
    ):
        failures.append(
            "resolution changes must transactionally replace the single global H.264 handle"
        )

    for marker in (
        "si_h264_137_param_sha256",
        "si_h264_137_rc_sha256",
        "si_h264_137_nal_sha256",
        "7cb83c1331903ad482c784a3567e8253ff2ebb31d6eb9828e64ba538f64749f2",
        "si_h264_sps_escape_fixed",
        "si_h264_patched_nal",
        "build_parameter_nals",
        "si_h264_set_fps_fixed",
        "si_h264_initial_nal_fixed",
        "si_h264_reference_reserve_upstream",
        "esp_h264_exoanchor_reserve_ref_workspace",
        "esp_h264_aligned_malloc",
        "ESP_H264_MEM_INTERNAL",
        "esp_ptr_internal(reserved)",
        "== 138247",
        "if(NOT CONFIG_CACHE_L1_CACHE_LINE_SIZE EQUAL 64)",
        "ALIGN_UP(required, CONFIG_CACHE_L1_CACHE_LINE_SIZE)",
        "esp_h264_exoanchor_release_cached_workspaces",
        "uint16_t mb_width = (width + 15U) >> 4;",
        "max_refame_buffer_size((int16_t)mb_width)",
        "s_cached_ref_workspace",
        "s_cached_db_workspace",
        "take_cached_workspace",
        "cache_largest_workspace",
        "param->ref_capacity = actual_size;",
        "Rejecting non-internal reference workspace from cache",
        "Rejecting non-internal reference workspace allocation",
        "param->db_capacity = actual_size;",
        "Reusing cached %s workspace",
    ):
        if marker not in project_cmake:
            failures.append(
                f"H.264 max-workspace reuse override missing: {marker}"
            )
    sps_escape_override = section(
        project_cmake,
        "set(si_h264_sps_escape_fixed",
        "string(FIND \"${si_h264_nal_source}\"",
    )
    if not ordered(
        sps_escape_override,
        "bs_rbsp_trailing(&bs)",
        "rbsp_bytes",
        "index = 1",
        "zero_run >= 2U && value <= 3U",
        "escaped_bytes + 5U > len",
        "return 0;",
        "memmove(",
        "buffer[4U + index] = 0x03",
        "escaped_bytes + 4U",
    ):
        failures.append(
            "esp_h264 SPS VUI must insert bounded emulation-prevention bytes after the NAL header"
        )
    cache_guard = cmake_bracket_block(
        project_cmake, "si_h264_cache_guard_fixed")
    if not ordered(
        cache_guard,
        '"soc/soc_caps.h"',
        "esp_h264_cache_range_is_known_uncached_internal",
        "if (!addr || length == 0U)",
        "__builtin_add_overflow",
        "SOC_RTC_FAST_MEM_SUPPORTED",
        "esp_ptr_in_rtc_dram_fast(addr)",
        "esp_ptr_in_rtc_dram_fast((const void *)last)",
        "SOC_MEM_SPM_SUPPORTED",
        "esp_ptr_in_spm(addr)",
        "esp_ptr_in_spm((const void *)last)",
        "return false;",
        "esp_h264_cache_check_and_writeback",
        "if (esp_h264_cache_range_is_known_uncached_internal(addr, length))",
        "return;",
        "esp_cache_msync(addr, length",
        "esp_h264_cache_check_and_invalidate",
        "if (esp_h264_cache_range_is_known_uncached_internal(addr, length))",
        "return;",
        "esp_cache_msync(addr, length",
    ):
        failures.append(
            "esp_h264 cache sync must skip only complete known-uncached SPM/RTCRAM ranges and preserve IDF validation for every other address"
        )
    if "cache_hal_vaddr_to_cache_level_id" in cache_guard:
        failures.append(
            "esp_h264 cache guard must not silently swallow arbitrary addresses rejected by the cache HAL"
        )
    for marker in (
        'set(si_h264_137_cache_sha256',
        '"ad3a681f1ba9c97e82ac01d9deb3eef3e8a42eecf9965af325ecd940255f8d48"',
        'file(SHA256 "${si_h264_upstream_cache}"',
        'file(WRITE "${si_h264_patched_cache}" "${si_h264_cache_guard_fixed}")',
        'list(REMOVE_ITEM si_h264_sources "${si_h264_upstream_cache}")',
    ):
        if marker not in project_cmake:
            failures.append(
                f"esp_h264 cache-address override is not hash-guarded and source-replaced: {marker}"
            )
    cache_source_replacement = section(
        project_cmake,
        'list(FIND si_h264_sources "${si_h264_upstream_cache}"',
        'set_property(TARGET ${si_h264_component} PROPERTY SOURCES',
    )
    if not ordered(
        cache_source_replacement,
        "si_h264_cache_source_index",
        "if(si_h264_cache_source_index EQUAL -1)",
        'message(FATAL_ERROR "Could not replace esp_h264 cache wrapper source")',
        'list(REMOVE_ITEM si_h264_sources "${si_h264_upstream_cache}")',
        "list(APPEND si_h264_sources",
        '"${si_h264_patched_cache}")',
    ):
        failures.append(
            "esp_h264 cache source replacement must fail closed and append the generated wrapper"
        )
    set_fps_override = section(
        project_cmake,
        "set(si_h264_set_fps_fixed",
        "string(FIND \"${si_h264_param_patched}\" \"${si_h264_set_fps_upstream}\"",
    )
    if not ordered(
        set_fps_override,
        "next_nal[SPS_PPS_BUF_SIZE]",
        "build_parameter_nals(",
        "if (ret != ESP_H264_ERR_OK)",
        "memcpy(param->nal_buf",
        "param->nal_bit_len = next_nal_bit_len",
        "param->fps = fps",
        "esp_h264_enc_hw_rc_set_bt_fps",
    ):
        failures.append(
            "esp_h264 FPS changes must transactionally rebuild and validate SPS/PPS before commit"
        )
    reference_helper = section(
        project_cmake, "set(si_h264_reference_reserve_fixed",
        "string(FIND \"${si_h264_param_patched}\"")
    if not ordered(
        reference_helper,
        "== 138247",
        "ALIGN_UP(required, CONFIG_CACHE_L1_CACHE_LINE_SIZE)",
        "esp_h264_aligned_malloc(",
        "ESP_H264_MEM_INTERNAL",
        "esp_ptr_internal(reserved)",
        "s_cached_ref_workspace = reserved;",
        "s_cached_ref_capacity = actual_size;",
    ):
        failures.append(
            "H.264 early reference reserve must prove exact size, cache alignment, internal residency, and cache ownership"
        )
    workspace_reuse = section(
        project_cmake,
        "static uint8_t *take_cached_workspace(",
        "static void cache_largest_workspace(",
    )
    if "memset(workspace" in workspace_reuse:
        failures.append(
            "H.264 cached workspace reuse must preserve esp_h264 1.3.7 non-zeroing malloc semantics"
        )
    if "esp_h264_cache_check_and_writeback(workspace, *actual_size);" not in workspace_reuse:
        failures.append(
            "H.264 cached workspace reuse must retain explicit cache-ownership transfer"
        )
    workspace_alloc_override = section(
        project_cmake, "set(si_h264_workspace_alloc_fixed",
        "string(FIND \"${si_h264_param_patched}\" \"${si_h264_workspace_alloc_upstream}\"")
    if not ordered(
        workspace_alloc_override,
        "take_cached_workspace(&s_cached_ref_workspace",
        "esp_h264_aligned_malloc(",
        "ESP_H264_MEM_INTERNAL",
        "!esp_ptr_internal(param->ref)",
        "param->ref_capacity = actual_size;",
    ):
        failures.append(
            "esp_h264_enc_hw_new parameter allocation must consume the cached internal reference or fail strict-internal allocation"
        )
    if "esp_h264_enc_dual_hw_new(" in h264:
        failures.append(
            "the product pipeline must not use the simultaneous dual-stream API as a mode selector"
        )

    idle_cleanup = section(
        h264, "if (now_us - inactive_since_us >=",
        "vTaskDelay(pdMS_TO_TICKS(20));")
    if not ordered(
        idle_cleanup,
        "s_stream.teardown = true;",
        "xSemaphoreGive(s_stream.lock);",
        "cleanup_err = pipeline_release(&pipeline);",
        "si_video_control_set_h264_transport(false);",
        "s_restore_pending, false",
        "restore_complete = true;",
        "s_restore_pending, true",
        "s_restore_error, cleanup_err",
        "restore_retry_not_before_us",
    ):
        failures.append(
            "idle H.264 cleanup must retain failed restore state and retry after bounded backoff"
        )

    service_init = section(
        h264, "esp_err_t si_h264_stream_initialize(void)",
        "void si_h264_stream_get_status(")
    handshake = section(
        h264, "esp_err_t si_h264_stream_ws_post_handshake(",
        "esp_err_t si_h264_stream_ws_handler(")
    if "esp_h264_enc_hw_new(" in service_init or \
            "h264_runtime_reserve()" in service_init or \
            "h264_runtime_reserve_internal()" in service_init or \
            "jpeg_alloc_decoder_mem(" in service_init or \
            "esp_h264_aligned_calloc(" in service_init:
        failures.append(
            "H.264 service initialization may reserve only the patched internal reference workspace"
        )
    if not ordered(
        service_init,
        "__atomic_compare_exchange_n(",
        "s_reference_workspace_reserve_claimed",
        "if (!si_video_boot_memory_ready())",
        "s_reference_workspace_reserve_attempted, true",
        "return ESP_ERR_INVALID_STATE;",
        "early H.264 reference reserve heap",
        "esp_h264_exoanchor_reserve_ref_workspace(",
        "SI_CFG_VIDEO_WIDTH, &required_bytes, &capacity_bytes",
        "s_reference_workspace_reserved",
        "s_reference_workspace_internal",
        "if (reserve_error != ESP_OK)",
        "esp_h264_exoanchor_release_cached_workspaces();",
        "s_resource_error, reserve_error",
        "return reserve_error;",
        "s_service_initialized, true",
    ):
        failures.append(
            "H.264 initialization must reserve the configured-width internal reference workspace before publishing availability"
        )
    video_boot_reserve = section(
        video_input, "esp_err_t si_video_reserve_boot_memory(void)",
        "bool si_video_boot_memory_ready(void)")
    for forbidden in (
        "xTaskCreate", "usb_host_install(", "uvc_host_install(",
        "init_uvc()", "xQueueCreate(", "xSemaphoreCreate",
    ):
        if forbidden in video_boot_reserve:
            failures.append(
                f"UVC boot-memory barrier must not start runtime resources: {forbidden}"
            )
    if not ordered(
        video_boot_reserve,
        "__atomic_compare_exchange_n(",
        "&s_boot_memory_reserve_claimed",
        "reserve_uvc_frame_arena()",
        "reserve_uvc_dma_budget()",
        "s_boot_memory_reserved, err == ESP_OK",
        "s_boot_memory_reserve_claimed, false",
    ) or "si_video_reserve_boot_memory()" not in section(
        video_input, "esp_err_t si_video_init(void)",
        "void si_video_get_status("):
        failures.append(
            "video init must reuse the idempotent arena and DMA boot-memory barrier"
        )
    if not ordered(
        app_main,
        "#if CONFIG_SI_VIDEO_H264_EXPERIMENT",
        "si_video_reserve_boot_memory();",
        "si_h264_stream_initialize();",
        "si_target_uart_init();",
        "si_video_init();",
    ):
        failures.append(
            "H.264 builds must reserve UVC boot memory and reference SRAM before target-UART or video tasks"
        )
    runtime_reserve = section(
        h264, "static esp_err_t h264_runtime_reserve(",
        "esp_err_t si_h264_stream_initialize(void)")
    if not ordered(
        runtime_reserve,
        "s_reference_workspace_reserved",
        "s_reference_workspace_internal",
        "h264_resource_owner_is(resource_token,",
        "SI_H264_RESOURCE_OWNER_H264",
        "h264_runtime_gate_take(SI_H264_RESOURCE_RESERVE_WAIT_MS)",
        "h264_resource_owner_is(resource_token,",
        "s_runtime_available",
        "s_runtime_reserve_terminal_failure",
        "h264_runtime_reserve_internal(resource_token)",
        "s_runtime_available, err == ESP_OK",
        "const bool invariant_failure =",
        "err == ESP_ERR_INVALID_ARG",
        "err == ESP_ERR_INVALID_SIZE",
        "err == ESP_ERR_NOT_SUPPORTED",
        "err != ESP_OK && invariant_failure",
        "s_runtime_reserve_terminal_failure, true",
        "h264_runtime_gate_give();",
    ):
        failures.append(
            "H.264 lazy reserve must hold one exact operation token and terminalize only deterministic invariants"
        )
    terminal_decision = section(
        runtime_reserve, "const bool invariant_failure =",
        "h264_runtime_gate_give();")
    for forbidden in (
        "ESP_ERR_NO_MEM",
        "ESP_ERR_TIMEOUT",
        "esp_h264_exoanchor_release_cached_workspaces",
    ):
        if forbidden in terminal_decision or (
                forbidden == "esp_h264_exoanchor_release_cached_workspaces" and
                forbidden in runtime_reserve):
            failures.append(
                f"retryable H.264 reserve must not terminalize or release the boot reference cache on {forbidden}"
            )

    for marker in (
        "typedef uint32_t si_h264_resource_token_t;",
        "si_h264_resource_token_t *token_out",
        "si_h264_resource_token_t token",
    ):
        if marker not in h264_header:
            failures.append(
                f"public generation-bound Agent/H.264 token contract missing: {marker}"
            )
    if "s_agent_resources_claimed" in h264:
        failures.append(
            "Agent and H.264 must share one owner token, not an independent Agent boolean"
        )
    token_generation = section(
        h264, "static si_h264_resource_token_t h264_resource_next_token(",
        "static bool h264_resource_owner_is(")
    owner_check = section(
        h264, "static bool h264_resource_owner_is(",
        "static esp_err_t h264_resource_claim_locked(")
    owner_claim = section(
        h264, "static esp_err_t h264_resource_claim_locked(",
        "static bool h264_resource_release_exact(")
    exact_release = section(
        h264, "static bool h264_resource_release_exact(",
        "static esp_err_t h264_stream_claim_h264_resources(")
    if not ordered(
        h264,
        "SI_H264_RESOURCE_OWNER_NONE = 0U",
        "SI_H264_RESOURCE_OWNER_AGENT = 1U",
        "SI_H264_RESOURCE_OWNER_H264 = 2U",
        "SI_H264_RESOURCE_OWNER_KIND_MASK = 3U",
    ) or not ordered(
        token_generation,
        "s_resource_owner_epoch",
        "epoch = 1U",
        "return (epoch << 2U) | owner_kind;",
    ) or not ordered(
        owner_check,
        "token != 0U",
        "token & SI_H264_RESOURCE_OWNER_KIND_MASK",
        "s_resource_owner_token",
        "token;",
    ) or not ordered(
        owner_claim,
        "s_resource_owner_token",
        "!= 0U",
        "h264_resource_next_token(owner_kind)",
        "s_resource_owner_token, token",
        "*token_out = token;",
    ) or not ordered(
        exact_release,
        "uint32_t expected = token;",
        "__atomic_compare_exchange_n(",
        "&s_resource_owner_token, &expected, 0U",
    ):
        failures.append(
            "Agent/H.264 arbitration must encode owner kind plus generation and compare-release one exact token"
        )

    agent_resource_claim = section(
        h264, "esp_err_t si_h264_stream_claim_agent_resources(",
        "bool si_h264_stream_release_agent_resources(")
    if not ordered(
        agent_resource_claim,
        "*token_out = 0U;",
        "h264_runtime_gate_take(SI_H264_RESOURCE_CLAIM_WAIT_MS)",
        "h264_resource_claim_locked(",
        "SI_H264_RESOURCE_OWNER_AGENT",
        "s_session_active",
        "s_codec_pipeline_claimed",
        "s_preallocated_encoder_claimed",
        "s_preallocated_buffers_claimed",
        "h264_resource_release_exact(",
        "esp_h264_enc_del(s_preallocated_encoder)",
        "s_preallocated_encoder = NULL",
        "s_runtime_available, false",
        "SI_AGENT_EXECUTOR_INTERNAL_MIN_FREE",
        "SI_AGENT_EXECUTOR_INTERNAL_MIN_LARGEST",
        "return ESP_ERR_NO_MEM;",
        "h264_runtime_gate_give();",
        "*token_out = token;",
    ):
        failures.append(
            "Agent must claim one exact token, suspend only an idle encoder, and pass the token to its executor"
        )
    if agent_resource_claim.count("h264_resource_release_exact(") < 3:
        failures.append(
            "every failed Agent admission path must exact-release its generation token"
        )
    agent_resource_release = section(
        h264, "bool si_h264_stream_release_agent_resources(",
        "/* Build-time patched into esp_h264_enc_hw_param.c.")
    if not ordered(
        agent_resource_release,
        "return h264_resource_release_exact(token,",
        "SI_H264_RESOURCE_OWNER_AGENT",
    ):
        failures.append(
            "Agent cleanup must compare-release only its exact generation token"
        )
    for marker in (
        "#define SI_H264_ENCODER_INTERNAL_MIN_FREE (24U * 1024U)",
        "#define SI_H264_ENCODER_INTERNAL_MIN_LARGEST (18U * 1024U)",
        "#define SI_AGENT_EXECUTOR_INTERNAL_MIN_FREE (48U * 1024U)",
        "#define SI_AGENT_EXECUTOR_INTERNAL_MIN_LARGEST (26U * 1024U)",
    ):
        if marker not in h264:
            failures.append(f"peak internal-memory admission gate missing: {marker}")

    agent_arg_free = section(
        agent_run_task, "static void agent_run_task_arg_free(",
        "static bool agent_result_requires_independent_verification(")
    agent_launch = section(
        agent_run_task, "static esp_err_t agent_run_launch_execution(",
        "static void __attribute__((unused)) agent_run_start_next_queued(")
    agent_worker = section(
        agent_run_task, "static void agent_run_task(void *arg)",
        "static bool agent_run_terminalize_launch_failure_locked(")
    if "uint32_t h264_resource_token;" not in agent_runtime_types or not ordered(
        agent_launch,
        "si_h264_stream_claim_agent_resources(&resource_token)",
        "task_arg->h264_resource_token = resource_token;",
        "xTaskCreateWithCaps(",
    ) or not ordered(
        agent_arg_free,
        "run->h264_resource_token != 0U",
        "resource_token =",
        "run->h264_resource_token;",
        "run->h264_resource_token = 0U;",
        "si_h264_stream_release_agent_resources(resource_token)",
    ) or not ordered(
        agent_worker,
        "agent_run_task_arg_free(run);",
        "vTaskDeleteWithCaps(NULL);",
    ):
        failures.append(
            "Agent executor must retain its exact resource token through task completion and release it exactly once"
        )

    runtime_internal = section(
        h264, "static esp_err_t h264_runtime_reserve_internal(",
        "static esp_err_t h264_runtime_reserve(")
    if not ordered(
        runtime_internal,
        "h264_resource_owner_is(resource_token,",
        "SI_H264_RESOURCE_OWNER_H264",
        "if (!s_preallocated_encoder)",
        "heap_caps_get_free_size(",
        "heap_caps_get_largest_free_block(",
        "SI_H264_ENCODER_INTERNAL_MIN_FREE",
        "SI_H264_ENCODER_INTERNAL_MIN_LARGEST",
        "return ESP_ERR_INVALID_STATE;",
        "esp_h264_enc_hw_new(",
        "buffers_already_reserved",
    ):
        failures.append(
            "H.264 encoder recreation must fail retryably on Agent/heap pressure and reuse PSRAM buffers"
        )
    vendor_error_map = section(
        h264, "static esp_err_t h264_vendor_error_to_esp(",
        "static esp_err_t pipeline_configure(")
    if not ordered(
        vendor_error_map,
        "case ESP_H264_ERR_MEM:",
        "return ESP_ERR_NO_MEM;",
        "case ESP_H264_ERR_TIMEOUT:",
        "return ESP_ERR_TIMEOUT;",
    ):
        failures.append(
            "vendor allocation and timeout pressure must remain distinguishable retryable errors"
        )

    h264_operation_claim = section(
        h264, "static esp_err_t h264_stream_claim_h264_resources(",
        "static bool h264_stream_release_h264_resources(")
    h264_operation_release = section(
        h264, "static bool h264_stream_release_h264_resources(",
        "esp_err_t si_h264_stream_claim_agent_resources(")
    if not ordered(
        h264_operation_claim,
        "*token_out = 0U;",
        "h264_runtime_gate_take(SI_H264_RESOURCE_CLAIM_WAIT_MS)",
        "h264_resource_claim_locked(",
        "SI_H264_RESOURCE_OWNER_H264",
        "h264_runtime_gate_give();",
    ) or not ordered(
        h264_operation_release,
        "s_session_active",
        "s_pipeline_ready",
        "s_codec_pipeline_claimed",
        "s_preallocated_encoder_claimed",
        "s_preallocated_buffers_claimed",
        "s_teardown_poisoned",
        "return false;",
        "h264_resource_release_exact(token,",
        "SI_H264_RESOURCE_OWNER_H264",
    ):
        failures.append(
            "each H.264 operation must own one token and retain it until codec/session teardown is complete"
        )

    selftest_worker = section(
        h264, "static esp_err_t h264_stream_self_test_jpeg(",
        "esp_err_t si_h264_stream_self_test(void)")
    if not ordered(
        selftest_worker,
        "si_h264_resource_token_t resource_token",
        "pipeline_configure(&pipeline,",
        "buffers_preclaimed, resource_token",
        "pipeline_release(&pipeline);",
    ) or not ordered(
        file_selftest,
        "h264_stream_claim_h264_resources(&resource_token)",
        "h264_runtime_reserve(resource_token)",
        "h264_stream_self_test_jpeg(",
        "0U, resource_token",
        "h264_stream_release_h264_resources(resource_token)",
    ):
        failures.append(
            "live and file self-tests must carry one H.264 token from reserve through pipeline teardown"
        )

    stream_state = section(
        h264, "typedef struct {\n    SemaphoreHandle_t lock;",
        "} si_h264_stream_state_t;")
    stream_snapshot_token = section(
        h264, "static bool stream_snapshot(",
        "static bool stream_still_current(")
    stream_task_token = section(
        h264, "static void h264_stream_task(void *arg)",
        "static esp_err_t ensure_stream_task(void)")
    websocket_token = section(
        h264, "esp_err_t si_h264_stream_ws_post_handshake(",
        "esp_err_t si_h264_stream_ws_handler(")
    websocket_teardown = section(
        stream_task_token, "if (cleanup_claimed)",
        "if (session != local_session)")
    if "si_h264_resource_token_t resource_token;" not in stream_state or not ordered(
        stream_snapshot_token,
        "si_h264_resource_token_t *resource_token",
        "*resource_token = s_stream.resource_token;",
    ) or not ordered(
        stream_task_token,
        "si_h264_resource_token_t resource_token = 0U;",
        "stream_snapshot(&server, &fd, &session, &stream_id,",
        "&resource_token)",
        "pipeline_configure(&pipeline,",
        "resource_token",
    ) or not ordered(
        websocket_token,
        "resource_token = s_stream.resource_token",
        "if (resource_token == 0U)",
        "h264_stream_claim_h264_resources(&resource_token)",
        "h264_resource_owner_is(",
        "h264_runtime_reserve(resource_token)",
        "s_stream.resource_token = resource_token;",
        "s_session_active, true",
    ) or not ordered(
        websocket_teardown,
        "pipeline_release(&pipeline)",
        "si_video_control_set_h264_transport(false)",
        "resource_token =",
        "s_stream.resource_token",
        "h264_stream_release_h264_resources(",
        "resource_token",
        "s_stream.resource_token = 0U;",
        "s_restore_pending, false",
    ):
        failures.append(
            "H.264 WebSocket must carry its operation token from reserve through deferred pipeline/MJPEG teardown"
        )
    if not ordered(
        app_main,
        "esp_err_t h264_ret = si_h264_stream_initialize();",
        "if (h264_ret == ESP_OK)",
        "continuing with MJPEG",
    ) or "ESP_ERROR_CHECK(si_h264_stream_initialize())" in app_main:
        failures.append(
            "optional H.264 initialization must be nonfatal to the Stable MJPEG baseline"
        )
    for marker in (
        "resource_probe_attempted",
        "reference_workspace_reserve_attempted",
        "reference_workspace_reserved",
        "reference_workspace_internal",
        "reference_workspace_required_bytes",
        "reference_workspace_capacity_bytes",
        "reference_workspace_error",
        "available",
        "session_active",
        "pipeline_ready",
        "restore_pending",
        "teardown_poisoned",
        "overlap_enabled",
        "serial_fallback",
        "metrics_valid",
        "source_fps_x100",
        "output_fps_x100",
        "backpressure_drops_total",
        "encode_failures_total",
        "primary_au_capacity_bytes",
        "small_egress_capacity_bytes",
        "primary_egress_access_units_total",
        "small_egress_access_units_total",
        "resource_error",
        "restore_error",
    ):
        if marker not in h264_header or marker not in device_http:
            failures.append(
                f"runtime H.264 capability/status field is not exposed: {marker}"
            )
    for marker in (
        '"schema": "exoanchor.h264_acceptance.v1"',
        'parser.add_argument("--host", required=True',
        "password must be supplied on stdin",
        "fixed_window_fps(arrivals)",
        "counter_delta(before, after)",
        "decode_annex_b(",
        '"-bsf:v"',
        '"trace_headers"',
        "headers.returncode == 0",
        '"headers_error"',
        "websocket_send_close(",
        "websocket_wait_for_peer_close(",
        "websocket_send_client_frame(sock, 8, struct.pack(\"!H\", code))",
        '"lifecycle_counter_delta"',
        '"lifecycle_checks"',
        "reference_workspace_checks(",
        '"reference_workspace_residency"',
        '"reference_workspace_required_bytes"',
        '"reference_workspace_capacity_bytes"',
        'evidence["residency"] == "internal"',
        'capacity >= required',
        '"active_baseline"',
        '"active_counter_delta"',
        '"restore_counter_delta"',
        '"post_release_lifecycle"',
        '"post_release_checks"',
        '"dual_yuv"',
        '"dual_au"',
        '"no_serial_fallback"',
        '"restore_checks"',
        '"h264_pressure_coalesces"',
        '"frames_captured": int(video.get("frames_captured", 0) or 0)',
        '"frames_encoded": int(video.get("frames_encoded", 0) or 0)',
        '"frames_dropped": int(video.get("frames_dropped", 0) or 0)',
        '"uvc_callbacks": int(pipeline.get("uvc_callbacks", 0) or 0)',
        "NON_ERROR_COUNTER_KEYS",
        "error_counter_view(counter_snapshot(latest_payload))",
        "if key not in NON_ERROR_COUNTER_KEYS",
        '"--force-lease"',
        '"force_lease": args.force_lease',
        '"lease_claim"',
        '"initial_claim_only": True',
        '"--hid-drag-start"',
        '"--hid-drag-end"',
        '"--motion-min-mean-mad"',
        '"--motion-min-changed-pct"',
        "def hid_websocket_connect(",
        "def analyze_dynamic_source(",
        '"changed_pixels_percent"',
        'checks["dynamic_source"]',
        'f"/api/ws/hid?stream_id={stream_id}"',
        '"type": "absmousedown"',
        '"type": "absmouseup"',
        '"type": "releaseall"',
        "hid_stream_started.set()",
    ):
        if marker not in h264_accept:
            failures.append(
                f"repeatable H.264 acceptance tool is missing contract: {marker}"
            )
    mode_activation = section(
        h264_accept, "def activate_capture_mode(", "def websocket_connect("
    )
    if not ordered(
        mode_activation,
        "lifecycle_before = status(session, host)",
        "set_lease(",
        "claim=True,",
        "force=force_lease,",
        "wait_for_mode(session, host, mode, timeout_s)",
        "wait_for_mode_lifecycle_settle(",
        "lifecycle_counter_delta = counter_delta(",
        "mode_activation_runtime_error_checks(",
        '"error_counters": error_counters_clean(',
        'result["passed"] and all(lifecycle_checks.values())',
        "return stream_id, result",
    ):
        failures.append(
            "H.264 acceptance must settle and record mode-start lifecycle before transferring the KVM lease"
        )
    acceptance_cycle = section(h264_accept, "def run_cycle(", "def main()")
    if not ordered(
        acceptance_cycle,
        "before_payload = status(session, host)",
        "before = counter_snapshot(before_payload)",
        "websocket_connect_started_at = time.monotonic()",
        "websocket_connect(host, cookie, stream_id)",
    ):
        failures.append(
            "H.264 active baseline must be captured after mode settle and immediately before WebSocket connect"
        )
    if not ordered(
        acceptance_cycle,
        "websocket_send_close(sock)",
        "websocket_wait_for_peer_close(sock, reader)",
        "wait_for_restore(session, host, restore_timeout)",
        "set_lease(session, host, stream_id, False)",
        "wait_for_post_release_lifecycle(",
    ):
        failures.append(
            "H.264 acceptance must close WebSocket and verify restore before releasing the measured KVM lease"
        )
    hid_connector = section(
        h264_accept, "def hid_websocket_connect(", "def websocket_send_client_frame("
    )
    if not ordered(
        hid_connector,
        "websocket_upgrade(",
        "host,",
        "80,",
        'f"/api/ws/hid?stream_id={stream_id}"',
    ):
        failures.append(
            "dynamic H.264 acceptance must attach HID to the exact KVM stream on the main HTTP port"
        )
    hid_drag = section(h264_accept, "def hid_drag_worker(", "def nal_types(")
    if not ordered(
        hid_drag,
        '"type": "absmove"',
        '"type": "absmousedown"',
        '"type": "absmove"',
        '"type": "absmouseup"',
        '"type": "releaseall"',
    ):
        failures.append(
            "dynamic H.264 acceptance must drag through stream-bound HID and release every held input"
        )
    if "set_lease(" in hid_drag or "session.post(" in hid_drag:
        failures.append(
            "stream-bound HID must not acquire a competing Agent/MCP control lease"
        )
    if not ordered(
        acceptance_cycle,
        "hid_websocket_connect(host, cookie, stream_id)",
        "hid_thread.start()",
        "hid_stream_started.set()",
        "hid_stop_requested.set()",
        'websocket_send_text(hid_sock, {"type": "absmouseup", "button": 0})',
        'websocket_send_text(hid_sock, {"type": "releaseall"})',
        "set_lease(session, host, stream_id, False)",
    ):
        failures.append(
            "dynamic H.264 acceptance must start after a real AU and release HID before the KVM video lease"
        )
    dynamic_source_gate = section(
        acceptance_cycle,
        'dynamic_source = {',
        "active_errors = runtime_error_snapshot(active_payload)",
    )
    if not ordered(
        dynamic_source_gate,
        '"enabled": False',
        'if hid_drag_result["enabled"]:',
        "analyze_dynamic_source(",
        "motion_min_mean_mad,",
        "motion_min_changed_pixels_percent,",
    ):
        failures.append(
            "offline motion analysis must run only for an enabled HID drag"
        )
    if not ordered(
        acceptance_cycle,
        'checks["hid_drag"] = hid_drag_result["passed"]',
        'checks["dynamic_source"] = bool(dynamic_source.get("passed"))',
        '"dynamic_source": dynamic_source,',
    ):
        failures.append(
            "HID-drag acceptance must gate and retain offline dynamic-source evidence"
        )
    post_release_runtime = section(
        h264_accept,
        "def post_release_runtime_error_checks(",
        "def memory_snapshot(",
    )
    for marker in (
        'errors["video_last_error"] == "video capture idle"',
        'not bool(control.get("active_enabled"))',
        'not bool(video.get("capture_enabled"))',
        'not bool(video.get("streaming"))',
        'not bool(video.get("frame_ready"))',
    ):
        if marker not in post_release_runtime:
            failures.append(
                f"post-release idle exception lacks state proof: {marker}"
            )
    acceptance_main = section(
        h264_accept, "def main() -> int:", 'if __name__ == "__main__":'
    )
    force_lease_flag = section(
        acceptance_main,
        'parser.add_argument(\n        "--force-lease",',
        'parser.add_argument(\n        "--hid-drag-start",',
    )
    if (
        'action="store_true"' not in force_lease_flag
        or "default=True" in force_lease_flag
    ):
        failures.append("--force-lease must be opt-in and default false")
    if not ordered(
        acceptance_main,
        '"force_lease": args.force_lease,',
        '"motion_min_mean_mad": args.motion_min_mean_mad,',
        '"motion_min_changed_pixels_percent": args.motion_min_changed_pct,',
        "activate_capture_mode(",
        "force_lease=args.force_lease,",
    ):
        failures.append(
            "--force-lease must be recorded in thresholds and passed only to mode activation"
        )
    if not ordered(
        acceptance_main,
        "post_release_counter_delta = counter_delta(",
        "post_release_runtime_error_checks(post_release)",
        '"settled": bool(post_release_lifecycle.get("settled"))',
        '"error_counters": error_counters_clean(',
        '"runtime_errors": all(post_release_error_state.values())',
        'and all(post_release_checks.values())',
    ):
        failures.append(
            "H.264 acceptance must gate post-release settle, counters, and context-aware runtime errors"
        )
    for marker in (
        '"--h264-fixture"',
        "_annex_b_access_units(",
        "_handle_h264_websocket(",
        '"<4sBBHHHIII",',
        'b"EAH1", 1, flags, 24',
        '"h264WsPort": MockState.h264_ws_port',
        '"h264ForceMse": MockState.h264_force_mse',
    ):
        if marker not in serve_ui:
            failures.append(
                f"real-bitstream browser fixture is missing contract: {marker}"
            )
    if "window.ExoAnchorBuild?.h264WsPort" not in kvm_html:
        failures.append(
            "H.264 browser tests must be able to select the mock WebSocket port without changing the production port"
        )
    if (
        '"--h264-force-mse"' not in serve_ui
        or "window.ExoAnchorBuild?.h264ForceMse!==true" not in kvm_html
    ):
        failures.append(
            "real-bitstream browser fixtures must be able to exercise the MSE fallback without changing production defaults"
        )
    if not ordered(
        handshake,
        "if (s_stream.teardown)",
        "h264_stream_claim_h264_resources(&resource_token)",
        "h264_runtime_reserve(resource_token)",
        "si_video_control_set_h264_transport(true)",
        "s_stream.resource_token = resource_token;",
        "s_restore_pending, false",
    ) or "refusing H.264 handshake while MJPEG restore is pending" in handshake:
        failures.append(
            "H.264 handshake must let a grace-period reconnect cancel pending restore, while refusing active teardown"
        )

    send_all = section(
        h264, "static int h264_socket_send_all(",
        "static esp_h264_enc_handle_t s_preallocated_encoder")
    if not ordered(
        send_all,
        "SI_H264_SEND_TIMEOUT_MS",
        "esp_timer_get_time() >= deadline_us",
        "flags | MSG_DONTWAIT",
        "errno == EAGAIN || errno == EWOULDBLOCK",
        "SI_H264_SEND_EAGAIN_RETRY_MS",
        "vTaskDelay(",
    ):
        failures.append("H.264 send-all must bound transient EAGAIN retries")

    ws_handler = section(
        h264, "esp_err_t si_h264_stream_ws_handler(",
        "/* end of H.264 WebSocket handler */")
    if not ordered(
        ws_handler,
        "frame.type == HTTPD_WS_TYPE_CLOSE",
        "stream_finish_fd(fd, ret != ESP_OK);",
    ) or not ordered(
        h264,
        "static void stream_detach(uint32_t session, int fd)",
        "stream_detach_internal(session, fd, false);",
        "static void stream_fail(uint32_t session, int fd)",
        "stream_detach_internal(session, fd, true);",
    ):
        failures.append(
            "normal WebSocket CLOSE must only detach while backend errors trigger a session close"
        )

    if failures:
        for failure in failures:
            print(f"video pipeline contract failure: {failure}", file=sys.stderr)
        return 1
    print("video pipeline contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
