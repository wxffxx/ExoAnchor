"""
Token Usage Store — Persist LLM API token usage statistics.
"""
from __future__ import annotations

import json
import os
import time
from typing import Any


class TokenStore:
    """JSON-backed store for LLM token usage statistics."""

    MAX_HISTORY = 200

    def __init__(self, path: str):
        self.path = os.path.abspath(os.path.expanduser(path))
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        self._data: dict[str, Any] = {"totals": {}, "history": []}
        self._load()

    def _load(self):
        if os.path.exists(self.path):
            try:
                with open(self.path) as f:
                    self._data = json.load(f)
                self._data.setdefault("totals", {})
                self._data.setdefault("history", [])
            except Exception:
                self._data = {"totals": {}, "history": []}

    def _save(self):
        with open(self.path, "w") as f:
            json.dump(self._data, f, indent=2)

    def record(
        self,
        *,
        provider: str,
        model: str,
        input_tokens: int,
        output_tokens: int,
        call_type: str = "intent",
    ):
        """Record a single LLM API call's token usage."""
        key = f"{provider}/{model}"
        totals = self._data["totals"]
        if key not in totals:
            totals[key] = {
                "provider": provider,
                "model": model,
                "input_tokens": 0,
                "output_tokens": 0,
                "calls": 0,
            }
        totals[key]["input_tokens"] += input_tokens
        totals[key]["output_tokens"] += output_tokens
        totals[key]["calls"] += 1

        self._data["history"].append({
            "provider": provider,
            "model": model,
            "input_tokens": input_tokens,
            "output_tokens": output_tokens,
            "call_type": call_type,
            "timestamp": time.time(),
        })
        if len(self._data["history"]) > self.MAX_HISTORY:
            self._data["history"] = self._data["history"][-self.MAX_HISTORY:]

        self._save()

    def get_summary(self) -> dict[str, Any]:
        """Return aggregated stats and recent call history."""
        totals = self._data["totals"]
        grand_input = sum(v["input_tokens"] for v in totals.values())
        grand_output = sum(v["output_tokens"] for v in totals.values())
        grand_calls = sum(v["calls"] for v in totals.values())
        return {
            "grand_total": {
                "input_tokens": grand_input,
                "output_tokens": grand_output,
                "total_tokens": grand_input + grand_output,
                "calls": grand_calls,
            },
            "by_model": list(totals.values()),
            "recent": self._data["history"][-20:],
        }

    def reset(self):
        """Clear all recorded usage data."""
        self._data = {"totals": {}, "history": []}
        self._save()
