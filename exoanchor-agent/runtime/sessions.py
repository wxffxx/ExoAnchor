"""
Durable agent sessions for server-owned ask/execute workflows.
"""

from __future__ import annotations

import json
import logging
import os
import time
import uuid
from enum import Enum
from typing import Any, Optional

from pydantic import BaseModel, Field

from .events import EventHub, RuntimeEvent, build_snapshot_event

logger = logging.getLogger("exoanchor.runtime.sessions")


class SessionState(str, Enum):
    PENDING = "pending"
    PARSING = "parsing"
    WAITING_INPUT = "waiting_input"
    DISPATCHING = "dispatching"
    RUNNING = "running"
    WAITING_CONFIRMATION = "waiting_confirmation"
    PAUSED = "paused"
    COMPLETED = "completed"
    FAILED = "failed"
    ABORTED = "aborted"


class AgentSession(BaseModel):
    """Durable record for one natural-language request."""

    session_id: str = Field(default_factory=lambda: str(uuid.uuid4())[:8])
    request: str
    conversation_id: str = ""
    model: str = ""
    force_plan: bool = False
    dry_run: bool = False
    state: SessionState = SessionState.PENDING
    result_type: str = ""
    execution_kind: str = ""
    summary: str = ""
    message: str = ""
    parsed_result: dict[str, Any] = Field(default_factory=dict)
    run_id: str = ""
    task_id: str = ""
    command: str = ""
    output: str = ""
    success: Optional[bool] = None
    error: Optional[str] = None
    metadata: dict[str, Any] = Field(default_factory=dict)
    created_at: float = Field(default_factory=time.time)
    updated_at: float = Field(default_factory=time.time)
    completed_at: Optional[float] = None


class SessionStore:
    """JSON-backed persistence for agent sessions."""

    def __init__(self, directory: str):
        self.directory = os.path.abspath(os.path.expanduser(directory))
        os.makedirs(self.directory, exist_ok=True)

    def _path_for(self, session_id: str) -> str:
        return os.path.join(self.directory, f"{session_id}.json")

    def save(self, session: AgentSession) -> None:
        path = self._path_for(session.session_id)
        tmp_path = f"{path}.tmp"
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(session.model_dump(), f, indent=2, ensure_ascii=False)
        os.replace(tmp_path, path)

    def load(self, session_id: str) -> Optional[AgentSession]:
        path = self._path_for(session_id)
        if not os.path.exists(path):
            return None
        try:
            with open(path, "r", encoding="utf-8") as f:
                return AgentSession(**json.load(f))
        except Exception as exc:
            logger.warning("Failed to load session %s: %s", session_id, exc)
            return None

    def list(self, limit: Optional[int] = 20) -> list[AgentSession]:
        items: list[AgentSession] = []
        for name in os.listdir(self.directory):
            if not name.endswith(".json"):
                continue
            path = os.path.join(self.directory, name)
            try:
                with open(path, "r", encoding="utf-8") as f:
                    items.append(AgentSession(**json.load(f)))
            except Exception as exc:
                logger.warning("Failed to parse session snapshot %s: %s", path, exc)
        items.sort(key=lambda item: item.updated_at, reverse=True)
        if limit is None:
            return items
        return items[:limit]


class SessionRuntime:
    """Owns session snapshots and mirrors child task/plan events into session events."""

    TERMINAL_STATES = {
        SessionState.COMPLETED,
        SessionState.FAILED,
        SessionState.ABORTED,
    }

    def __init__(self, store: SessionStore, event_hub: EventHub):
        self.store = store
        self.event_hub = event_hub
        self._sessions_by_run: dict[str, set[str]] = {}
        self._sessions_by_task: dict[str, set[str]] = {}

    async def create(
        self,
        *,
        request: str,
        conversation_id: str = "",
        model: str = "",
        force_plan: bool = False,
        dry_run: bool = False,
        metadata: Optional[dict[str, Any]] = None,
    ) -> AgentSession:
        session = AgentSession(
            request=request,
            conversation_id=conversation_id,
            model=model,
            force_plan=force_plan,
            dry_run=dry_run,
            summary=request[:120],
            metadata=dict(metadata or {}),
        )
        self.store.save(session)
        await self._publish("created", session)
        return session

    def get(self, session_id: str) -> Optional[AgentSession]:
        return self.store.load(session_id)

    def list(self, limit: int = 20) -> list[AgentSession]:
        return self.store.list(limit=limit)

    def bind_run(self, session_id: str, run_id: str) -> None:
        if not session_id or not run_id:
            return
        self._sessions_by_run.setdefault(run_id, set()).add(session_id)

    def bind_task(self, session_id: str, task_id: str) -> None:
        if not session_id or not task_id:
            return
        self._sessions_by_task.setdefault(task_id, set()).add(session_id)

    async def update(
        self,
        session: AgentSession,
        *,
        event: str = "updated",
        state: Optional[SessionState] = None,
        payload: Optional[dict[str, Any]] = None,
        **changes: Any,
    ) -> AgentSession:
        current = self.store.load(session.session_id) or session
        for key, value in changes.items():
            setattr(current, key, value)
        if state is not None:
            current.state = state
        current.updated_at = time.time()
        if current.state in self.TERMINAL_STATES and current.completed_at is None:
            current.completed_at = current.updated_at
        self.store.save(current)
        await self._publish(event, current, payload=payload)
        return current

    def snapshot_event(self, session_id: str) -> Optional[RuntimeEvent]:
        session = self.store.load(session_id)
        if session is None:
            return None
        return build_snapshot_event(
            stream="session",
            entity_kind="session",
            entity_id=session.session_id,
            state=session.state.value,
            summary=session.summary,
            payload={"session": session.model_dump()},
        )

    async def sync_child_event(self, event: RuntimeEvent) -> None:
        if event.entity_kind == "plan_run":
            targets = list(self._sessions_by_run.get(event.entity_id, set()))
        elif event.entity_kind == "task":
            targets = list(self._sessions_by_task.get(event.entity_id, set()))
        else:
            return

        for session_id in targets:
            session = self.store.load(session_id)
            if session is None:
                continue

            next_state = self._map_child_state(event)
            changes: dict[str, Any] = {}
            if event.entity_kind == "plan_run":
                changes["run_id"] = event.entity_id
                changes["execution_kind"] = "plan_run"
                if not session.result_type:
                    changes["result_type"] = "plan"
            elif event.entity_kind == "task":
                changes["task_id"] = event.entity_id
                changes["execution_kind"] = "task"
                if not session.result_type:
                    changes["result_type"] = "skill_call"

            if next_state in self.TERMINAL_STATES:
                changes["success"] = next_state == SessionState.COMPLETED
                child_payload = event.payload or {}
                if event.entity_kind == "plan_run":
                    run = child_payload.get("run") or {}
                    changes["error"] = run.get("error") or session.error
                    if not session.summary:
                        changes["summary"] = run.get("goal") or session.summary
                else:
                    status = child_payload.get("status") or {}
                    snapshot = child_payload.get("snapshot") or {}
                    changes["error"] = status.get("error") or snapshot.get("error") or session.error
                    if not session.summary:
                        changes["summary"] = status.get("skill_name") or snapshot.get("skill_name") or session.summary

            await self.update(
                session,
                event="child_event",
                state=next_state,
                payload={"child_event": event.model_dump()},
                **changes,
            )

    async def _publish(self, event_name: str, session: AgentSession, payload: Optional[dict[str, Any]] = None) -> None:
        data = {"session": session.model_dump()}
        if payload:
            data.update(payload)
        await self.event_hub.publish(
            RuntimeEvent(
                stream="session",
                event=event_name,
                entity_kind="session",
                entity_id=session.session_id,
                state=session.state.value,
                summary=session.summary or session.request[:120],
                payload=data,
            )
        )

    def _map_child_state(self, event: RuntimeEvent) -> SessionState:
        state = str(event.state or "").strip().lower()
        if "." in state:
            state = state.split(".")[-1]
        if state == "waiting_confirmation":
            return SessionState.WAITING_CONFIRMATION
        if state == "paused":
            return SessionState.PAUSED
        if state == "completed":
            return SessionState.COMPLETED
        if state == "failed":
            return SessionState.FAILED
        if state == "aborted":
            return SessionState.ABORTED
        return SessionState.RUNNING
