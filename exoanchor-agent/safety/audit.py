"""
Audit Log — Append-only audit trail for policy decisions.
"""

import json
import os
import time
import uuid
from typing import Any, Optional

from pydantic import BaseModel, Field


class AuditEvent(BaseModel):
    event_id: str = Field(default_factory=lambda: str(uuid.uuid4())[:12])
    timestamp: float = Field(default_factory=time.time)
    source_type: str = ""
    agent_mode: str = ""
    tool_name: str = ""
    risk_level: str = "low"
    action: str = "allow"
    allowed: bool = True
    requires_confirmation: bool = False
    reason: str = ""
    matched_rules: list[str] = Field(default_factory=list)
    command: str = ""
    args: dict[str, Any] = Field(default_factory=dict)
    metadata: dict[str, Any] = Field(default_factory=dict)


class AuditLogStore:
    """Simple JSONL-backed audit log."""

    def __init__(self, path: str):
        self.path = os.path.abspath(os.path.expanduser(path))
        os.makedirs(os.path.dirname(self.path), exist_ok=True)

    def record(self, event: AuditEvent) -> None:
        with open(self.path, "a", encoding="utf-8") as f:
            f.write(json.dumps(event.model_dump(), ensure_ascii=False) + "\n")

    def list_events(self, limit: int = 50) -> list[AuditEvent]:
        if not os.path.exists(self.path):
            return []

        lines = []
        with open(self.path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                lines.append(line)

        events = []
        for line in reversed(lines):
            try:
                events.append(AuditEvent(**json.loads(line)))
            except Exception:
                continue
            if len(events) >= limit:
                break
        return events
