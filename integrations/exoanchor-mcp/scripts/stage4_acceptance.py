#!/usr/bin/env python3
"""Run the non-mutating Stage 4 acceptance sequence against one dev firmware."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

PACKAGE_ROOT = Path(__file__).resolve().parents[1]
if str(PACKAGE_ROOT) not in sys.path:
    sys.path.insert(0, str(PACKAGE_ROOT))

from exoanchor_mcp import __version__
from exoanchor_mcp.observations import utc_now
from exoanchor_mcp.server import ExoAnchorClient, ExoAnchorConfig, ExoAnchorError, ToolRuntime


def check(name, function):
    try:
        value = function()
    except ExoAnchorError as exc:
        return {"name": name, "status": "failed", "error": str(exc)}
    return {"name": name, "status": "passed", "evidence": value}


def observation_evidence(observation):
    return {
        "observation_id": observation.get("observation_id"),
        "kind": observation.get("kind"),
        "captured_at": observation.get("captured_at"),
        "content_sha256": observation.get("content_sha256"),
    }


def run_read_only(runtime):
    checks = []
    blockers = []

    capability_check = check(
        "capabilities",
        lambda: observation_evidence(
            runtime.call("exoanchor_capabilities", {})["text"]
        ),
    )
    checks.append(capability_check)

    status_payload = None
    status_check = check(
        "status",
        lambda: runtime.call("exoanchor_status", {"include_system": True})["text"],
    )
    if status_check["status"] == "passed":
        status_payload = status_check["evidence"]
        conditions = status_payload.get("derived", {}).get("conditions", {})
        status_check["evidence"] = {
            **observation_evidence(status_payload),
            "conditions": conditions,
        }
    checks.append(status_check)

    logs_check = check(
        "logs",
        lambda: runtime.call("exoanchor_logs", {"limit": 20})["text"],
    )
    if logs_check["status"] == "passed":
        logs = logs_check["evidence"]
        logs_check["evidence"] = {
            **observation_evidence(logs),
            "returned": len(logs.get("data", {}).get("logs", [])),
        }
    checks.append(logs_check)

    video_ready = bool(
        status_payload
        and status_payload.get("derived", {}).get("conditions", {}).get("video_connected")
    )
    if video_ready:
        snapshot_check = check(
            "snapshot",
            lambda: runtime.call("exoanchor_snapshot", {})["text"],
        )
        if snapshot_check["status"] == "passed":
            snapshot = snapshot_check["evidence"]
            snapshot_check["evidence"] = {
                **observation_evidence(snapshot),
                "frame_id": snapshot.get("data", {}).get("frame_id"),
                "width": snapshot.get("data", {}).get("width"),
                "height": snapshot.get("data", {}).get("height"),
                "bytes": snapshot.get("data", {}).get("bytes"),
            }
        checks.append(snapshot_check)
    else:
        checks.append({
            "name": "snapshot",
            "status": "blocked",
            "reason": "video_connected condition is false; read-only acceptance does not acquire a video lease",
        })
        blockers.append("snapshot requires an already active video capture path")

    try:
        runtime.call("exoanchor_power_action", {"action": "locator_on"})
    except ExoAnchorError as exc:
        checks.append({
            "name": "write_gate",
            "status": "passed" if "EXOANCHOR_ALLOW_WRITE=1" in str(exc) else "failed",
            "evidence": "mutating call rejected before device access",
        })
    else:
        checks.append({
            "name": "write_gate",
            "status": "failed",
            "error": "mutating call unexpectedly passed",
        })

    passed = sum(item["status"] == "passed" for item in checks)
    failed = sum(item["status"] == "failed" for item in checks)
    blocked = sum(item["status"] == "blocked" for item in checks)
    return {
        "schema": "exoanchor.mcp.stage4.acceptance.v1",
        "generated_at": utc_now(),
        "bridge_version": __version__,
        "mode": "live_read_only",
        "device_id": runtime.device_id,
        "safety": {
            "allow_write": False,
            "firmware_flash_performed": False,
            "power_or_reset_performed": False,
            "ssh_command_performed": False,
            "hid_action_performed": False,
        },
        "summary": {
            "status": "failed" if failed else ("partial" if blocked else "passed"),
            "passed": passed,
            "failed": failed,
            "blocked": blocked,
        },
        "checks": checks,
        "blockers": blockers,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", help="Optional JSON report path")
    args = parser.parse_args()
    config = ExoAnchorConfig.from_env()
    if config.allow_write:
        raise SystemExit(
            "Stage 4 read-only acceptance refuses to run while EXOANCHOR_ALLOW_WRITE is enabled"
        )
    report = run_read_only(ToolRuntime(ExoAnchorClient(config)))
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text, encoding="utf-8")
    print(text, end="")
    raise SystemExit(1 if report["summary"]["status"] == "failed" else 0)


if __name__ == "__main__":
    main()
