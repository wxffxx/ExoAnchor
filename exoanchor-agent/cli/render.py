"""
Output helpers for the ExoAnchor CLI.
"""

from __future__ import annotations

import json
from typing import Any


def print_json(data: Any) -> None:
    print(json.dumps(data, ensure_ascii=False, indent=2))


def print_step(prefix: str, text: str) -> None:
    print(f"{prefix} {text}", flush=True)


def print_jsonl(event: dict[str, Any]) -> None:
    print(json.dumps(event, ensure_ascii=False), flush=True)


def normalize_state(value: Any) -> str:
    text = str(value or "").strip().lower()
    if "." in text:
        text = text.split(".")[-1]
    return text
