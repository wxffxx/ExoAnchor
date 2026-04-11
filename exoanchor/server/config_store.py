"""
Persistent JSON-backed config storage for the local test server.
"""

from __future__ import annotations

import json
import os
from typing import Any


class JSONConfigStore:
    """Load/save config with legacy fallback paths."""

    def __init__(self, primary_path: str, legacy_paths: list[str] | None = None):
        self.primary_path = os.path.abspath(primary_path)
        self.legacy_paths = [os.path.abspath(path) for path in (legacy_paths or [])]

    def load(self) -> dict[str, Any] | None:
        for path in [self.primary_path, *self.legacy_paths]:
            if not os.path.exists(path):
                continue
            try:
                with open(path, "r", encoding="utf-8") as handle:
                    return json.load(handle)
            except Exception:
                continue
        return None

    def save(self, config: dict[str, Any]) -> None:
        with open(self.primary_path, "w", encoding="utf-8") as handle:
            json.dump(config, handle, indent=2, ensure_ascii=False)
