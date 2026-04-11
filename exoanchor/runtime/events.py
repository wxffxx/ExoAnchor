"""
Unified runtime event stream for plans, tasks, and headless clients.
"""

from __future__ import annotations

import asyncio
import json
import time
import uuid
from typing import Any, Callable, Optional

from pydantic import BaseModel, Field


class RuntimeEvent(BaseModel):
    """Normalized event envelope shared by CLI, UI, and automations."""

    event_id: str = Field(default_factory=lambda: str(uuid.uuid4())[:8])
    timestamp: float = Field(default_factory=time.time)
    stream: str
    event: str
    entity_kind: str
    entity_id: str = ""
    state: str = ""
    summary: str = ""
    payload: dict[str, Any] = Field(default_factory=dict)


def _safe_summary(raw: dict[str, Any]) -> str:
    if raw.get("type") == "plan_run":
        run = raw.get("run") or {}
        event = str(raw.get("event") or "")
        goal = str(run.get("goal") or "").strip()
        if event == "confirmation_requested":
            waiting_step_id = str(run.get("waiting_step_id") or "").strip()
            steps = run.get("steps") or []
            step = next((item for item in steps if str(item.get("id")) == waiting_step_id), {}) if waiting_step_id else {}
            return f"Waiting for confirmation: {step.get('description') or waiting_step_id or goal}"
        if event == "finished":
            return f"{run.get('state', '')}: {goal}".strip(": ")
        return goal

    event_type = str(raw.get("type") or "")
    if event_type == "task_start":
        return f"Task started: {raw.get('skill', '')}"
    if event_type == "task_end":
        status = raw.get("status") or {}
        return f"Task {status.get('state', '')}: {status.get('skill_name', '')}"
    if event_type == "step":
        return str(raw.get("step_id") or raw.get("action_detail") or raw.get("screen_state") or "step")
    return event_type or "event"


def normalize_runtime_event(raw: dict[str, Any]) -> RuntimeEvent:
    """Convert the existing task/plan callback payloads into one shared envelope."""
    raw = dict(raw or {})
    raw_type = str(raw.get("type") or "").strip()

    if raw_type == "plan_run":
        run = raw.get("run") or {}
        return RuntimeEvent(
            stream="plan_run",
            event=str(raw.get("event") or "updated"),
            entity_kind="plan_run",
            entity_id=str(run.get("run_id") or ""),
            state=str(run.get("state") or ""),
            summary=_safe_summary(raw),
            payload=raw,
        )

    if raw_type in {"task_start", "task_end", "step"}:
        task_id = str(raw.get("task_id") or "")
        status = raw.get("status") or {}
        state = str(status.get("state") or "")
        if raw_type == "step" and not state:
            state = "running"
        return RuntimeEvent(
            stream="task",
            event=raw_type,
            entity_kind="task",
            entity_id=task_id,
            state=state,
            summary=_safe_summary(raw),
            payload=raw,
        )

    return RuntimeEvent(
        stream="misc",
        event=raw_type or "event",
        entity_kind="event",
        entity_id=str(raw.get("id") or ""),
        state=str(raw.get("state") or ""),
        summary=_safe_summary(raw),
        payload=raw,
    )


def build_snapshot_event(stream: str, entity_kind: str, entity_id: str, payload: dict[str, Any], state: str = "", summary: str = "") -> RuntimeEvent:
    """Create a synthetic snapshot event for newly attached clients."""
    return RuntimeEvent(
        stream=stream,
        event="snapshot",
        entity_kind=entity_kind,
        entity_id=entity_id,
        state=state,
        summary=summary,
        payload=payload,
    )


class EventHub:
    """Async pub/sub hub with a small in-memory replay buffer."""

    def __init__(self, history_limit: int = 500):
        self.history_limit = history_limit
        self._history: list[RuntimeEvent] = []
        self._subscribers: dict[str, tuple[asyncio.Queue, Optional[Callable[[RuntimeEvent], bool]]]] = {}

    async def publish(self, event: RuntimeEvent) -> None:
        self._history.append(event)
        if len(self._history) > self.history_limit:
            self._history = self._history[-self.history_limit:]

        stale: list[str] = []
        for subscriber_id, (queue, matcher) in list(self._subscribers.items()):
            try:
                if matcher is not None and not matcher(event):
                    continue
                await queue.put(event)
            except Exception:
                stale.append(subscriber_id)

        for subscriber_id in stale:
            self._subscribers.pop(subscriber_id, None)

    async def publish_raw(self, payload: dict[str, Any]) -> RuntimeEvent:
        event = normalize_runtime_event(payload)
        await self.publish(event)
        return event

    def recent(self, limit: int = 50, matcher: Optional[Callable[[RuntimeEvent], bool]] = None) -> list[RuntimeEvent]:
        items = self._history[-limit:] if limit > 0 else list(self._history)
        if matcher is None:
            return list(items)
        return [item for item in items if matcher(item)]

    def subscribe(self, matcher: Optional[Callable[[RuntimeEvent], bool]] = None) -> tuple[str, asyncio.Queue]:
        subscriber_id = str(uuid.uuid4())
        queue: asyncio.Queue = asyncio.Queue()
        self._subscribers[subscriber_id] = (queue, matcher)
        return subscriber_id, queue

    def unsubscribe(self, subscriber_id: str) -> None:
        self._subscribers.pop(subscriber_id, None)

    def encode(self, event: RuntimeEvent) -> str:
        return json.dumps(event.model_dump(), ensure_ascii=False) + "\n"
