"""Observation metadata and bounded in-process replay support.

Device responses and target-screen pixels are untrusted external observations.
This module adds provenance and stable hashes without claiming that the observed
content is true.
"""

from __future__ import annotations

import hashlib
import json
import threading
from collections import OrderedDict
from datetime import datetime, timezone
from typing import Any


JSON = dict[str, Any]


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def canonical_hash(value: Any) -> str:
    encoded = json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def bytes_hash(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def jpeg_dimensions(data: bytes) -> tuple[int, int]:
    """Read JPEG dimensions without adding an image-library dependency."""
    if len(data) < 4 or data[:2] != b"\xff\xd8":
        raise ValueError("snapshot is not a JPEG image")
    offset = 2
    sof_markers = {
        0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
        0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF,
    }
    while offset + 3 < len(data):
        if data[offset] != 0xFF:
            offset += 1
            continue
        while offset < len(data) and data[offset] == 0xFF:
            offset += 1
        if offset >= len(data):
            break
        marker = data[offset]
        offset += 1
        if marker in {0xD8, 0xD9}:
            continue
        if offset + 2 > len(data):
            break
        segment_length = int.from_bytes(data[offset:offset + 2], "big")
        if segment_length < 2 or offset + segment_length > len(data):
            break
        if marker in sof_markers and segment_length >= 7:
            height = int.from_bytes(data[offset + 3:offset + 5], "big")
            width = int.from_bytes(data[offset + 5:offset + 7], "big")
            if width > 0 and height > 0:
                return width, height
        offset += segment_length
    raise ValueError("JPEG dimensions are unavailable")


def make_observation(kind: str, source: str, data: Any, *,
                     captured_at: str | None = None,
                     derived: JSON | None = None) -> JSON:
    captured = captured_at or utc_now()
    content_sha256 = canonical_hash(data)
    identity = canonical_hash({
        "kind": kind,
        "source": source,
        "captured_at": captured,
        "content_sha256": content_sha256,
    })[:24]
    observation: JSON = {
        "observation_id": f"obs_{identity}",
        "kind": kind,
        "source": source,
        "captured_at": captured,
        "trust": "untrusted_external_observation",
        "content_sha256": content_sha256,
        "data": data,
    }
    if derived:
        observation["derived"] = derived
    return observation


class ObservationStore:
    """Keeps recent observations replayable for one MCP server lifetime."""

    def __init__(self, maximum: int = 128):
        self.maximum = maximum
        self._items: OrderedDict[str, JSON] = OrderedDict()
        self._lock = threading.Lock()

    def put(self, observation: JSON) -> JSON:
        observation_id = observation.get("observation_id")
        if not isinstance(observation_id, str):
            raise ValueError("observation_id is required")
        with self._lock:
            self._items[observation_id] = observation
            self._items.move_to_end(observation_id)
            while len(self._items) > self.maximum:
                self._items.popitem(last=False)
        return observation

    def get(self, observation_id: str) -> JSON | None:
        with self._lock:
            observation = self._items.get(observation_id)
            if observation is not None:
                self._items.move_to_end(observation_id)
            return observation

    def list_metadata(self) -> list[JSON]:
        with self._lock:
            return [
                {key: value for key, value in observation.items() if key != "data"}
                for observation in reversed(self._items.values())
            ]


def condition_value(status: JSON, condition: str) -> bool:
    video = status.get("video") if isinstance(status.get("video"), dict) else {}
    hid = status.get("hid") if isinstance(status.get("hid"), dict) else {}
    power = status.get("power") if isinstance(status.get("power"), dict) else {}
    ssh = status.get("ssh") if isinstance(status.get("ssh"), dict) else {}
    lease = status.get("control_lease") if isinstance(status.get("control_lease"), dict) else {}
    mcp = status.get("mcp") if isinstance(status.get("mcp"), dict) else {}
    power_detect = (
        power.get("detect", {}).get("power", {})
        if isinstance(power.get("detect"), dict) else {}
    )

    values = {
        "mcp_enabled": mcp.get("enabled") is not False,
        "video_connected": video.get("connected") is True,
        "video_disconnected": video.get("connected") is False,
        "hid_ready": hid.get("ready") is True or (
            hid.get("initialized") is True and hid.get("connected") is True
        ),
        "power_on": (
            power.get("power_on") is True
            or power.get("on") is True
            or power_detect.get("state") == "on"
        ),
        "power_off": (
            power.get("power_on") is False
            or power.get("on") is False
            or power_detect.get("state") == "off"
        ),
        "lease_available": lease.get("can_request") is True or lease.get("active") is False,
        "ssh_configured": (
            isinstance(ssh.get("target"), dict) and ssh["target"].get("configured") is True
        ),
        "ssh_host_verified": ssh.get("host_key_check") is True,
    }
    return bool(values.get(condition, False))


STATUS_CONDITIONS = {
    "mcp_enabled",
    "video_connected",
    "video_disconnected",
    "hid_ready",
    "power_on",
    "power_off",
    "lease_available",
    "ssh_configured",
    "ssh_host_verified",
}
