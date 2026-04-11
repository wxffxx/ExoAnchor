"""
Run Memory — Durable task snapshots, learned facts, and recovery helpers.
"""

import json
import os
import time
from typing import Any, Optional

from pydantic import BaseModel, Field

from ..core.context import ExecutionContext
from ..core.models import PlanRunStatus, PlanStepState, RunState, StepRecord, TaskState
from ..core.run_store import RunStore
from .artifact_store import ArtifactStore
from .fact_store import FactStore


class TaskSnapshot(BaseModel):
    task_id: str
    skill_name: str
    mode: str = "guided"
    params: dict[str, Any] = Field(default_factory=dict)
    state: str
    progress: float = 0.0
    current_step: int = 0
    total_steps: int = 0
    current_checkpoint: Optional[str] = None
    checkpoints_reached: list[str] = Field(default_factory=list)
    started_at: float = 0.0
    completed_at: Optional[float] = None
    updated_at: float = Field(default_factory=time.time)
    elapsed: float = 0.0
    error: Optional[str] = None
    result: Optional[dict[str, Any]] = None
    history: list[StepRecord] = Field(default_factory=list)
    artifacts: list[dict[str, Any]] = Field(default_factory=list)
    metadata: dict[str, Any] = Field(default_factory=dict)
    recoverable: bool = False


class RunMemory:
    """Coordinates durable task snapshots, facts, artifacts, and recovery."""

    TASK_TERMINAL_STATES = {
        TaskState.COMPLETED.value,
        TaskState.FAILED.value,
        TaskState.ABORTED.value,
    }

    RUN_TERMINAL_STATES = {
        RunState.COMPLETED.value,
        RunState.FAILED.value,
        RunState.ABORTED.value,
    }

    def __init__(
        self,
        tasks_dir: str,
        artifact_store: ArtifactStore,
        fact_store: FactStore,
    ):
        self.tasks_dir = os.path.abspath(os.path.expanduser(tasks_dir))
        self.artifacts = artifact_store
        self.facts = fact_store
        os.makedirs(self.tasks_dir, exist_ok=True)

    def save_task_context(
        self,
        ctx: ExecutionContext,
        *,
        metadata: Optional[dict[str, Any]] = None,
    ) -> TaskSnapshot:
        snapshot = TaskSnapshot(
            task_id=ctx.task.task_id,
            skill_name=ctx.task.skill_name,
            mode=ctx.task.mode,
            params=dict(ctx.task.params or {}),
            state=ctx.task.state.value,
            progress=ctx.progress,
            current_step=ctx.current_step,
            total_steps=max(ctx.current_step, len(ctx.history)),
            current_checkpoint=ctx.current_checkpoint,
            checkpoints_reached=sorted(ctx.checkpoints_reached),
            started_at=ctx.task.started_at,
            completed_at=ctx.task.completed_at or None,
            updated_at=time.time(),
            elapsed=ctx.elapsed,
            error=ctx.task.error,
            result=ctx.task.result,
            history=[StepRecord(**step.model_dump()) for step in ctx.history],
            metadata={**dict(getattr(ctx.task, "metadata", {}) or {}), **(metadata or {})},
            recoverable=ctx.task.state.value not in self.TASK_TERMINAL_STATES,
        )

        if snapshot.state in self.TASK_TERMINAL_STATES and snapshot.history:
            artifact = self.artifacts.save_json(
                "task-history",
                [step.model_dump() for step in snapshot.history],
                source_id=snapshot.task_id,
                metadata={
                    "skill_name": snapshot.skill_name,
                    "state": snapshot.state,
                },
            )
            snapshot.artifacts = [artifact]

        self._save_task(snapshot)
        self._learn_from_task(snapshot)
        return snapshot

    def load_task(self, task_id: str) -> Optional[TaskSnapshot]:
        path = self._task_path(task_id)
        if not os.path.exists(path):
            return None
        try:
            with open(path, "r", encoding="utf-8") as f:
                return TaskSnapshot(**json.load(f))
        except Exception:
            return None

    def list_tasks(self, limit: int = 20) -> list[TaskSnapshot]:
        items = []
        for name in os.listdir(self.tasks_dir):
            if not name.endswith(".json"):
                continue
            path = os.path.join(self.tasks_dir, name)
            try:
                with open(path, "r", encoding="utf-8") as f:
                    items.append(TaskSnapshot(**json.load(f)))
            except Exception:
                continue
        items.sort(key=lambda item: item.updated_at, reverse=True)
        return items[:limit]

    def capture_plan_run(self, run: PlanRunStatus) -> None:
        self._learn_from_plan_run(run)

        if run.state.value in self.RUN_TERMINAL_STATES:
            artifact = self.artifacts.save_json(
                "plan-run",
                run.model_dump(),
                source_id=run.run_id,
                metadata={
                    "goal": run.goal,
                    "state": run.state.value,
                },
            )
            if artifact and run.metadata.get("terminal_artifact_id") != artifact["artifact_id"]:
                run.metadata["terminal_artifact_id"] = artifact["artifact_id"]

    def recover_stale_state(self, run_store: Optional[RunStore] = None) -> dict[str, int]:
        recovered_tasks = self._recover_stale_tasks()
        recovered_runs = self._recover_stale_runs(run_store) if run_store else 0
        return {
            "recovered_tasks": recovered_tasks,
            "recovered_runs": recovered_runs,
        }

    def _task_path(self, task_id: str) -> str:
        return os.path.join(self.tasks_dir, f"{task_id}.json")

    def _save_task(self, snapshot: TaskSnapshot) -> None:
        path = self._task_path(snapshot.task_id)
        tmp_path = f"{path}.tmp"
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(snapshot.model_dump(), f, indent=2, ensure_ascii=False)
        os.replace(tmp_path, path)

    def _recover_stale_tasks(self) -> int:
        recovered = 0
        for snapshot in self.list_tasks(limit=500):
            if snapshot.state in self.TASK_TERMINAL_STATES:
                continue
            snapshot.state = TaskState.ABORTED.value
            snapshot.completed_at = snapshot.completed_at or time.time()
            snapshot.updated_at = time.time()
            snapshot.error = snapshot.error or "Service restarted before task completed"
            snapshot.recoverable = True
            snapshot.metadata["recovery_reason"] = "service_restart"
            self._save_task(snapshot)
            self.facts.record_failure(
                "task",
                snapshot.task_id,
                snapshot.error,
                state=snapshot.state,
                details={
                    "skill_name": snapshot.skill_name,
                    "recoverable": True,
                },
            )
            recovered += 1
        return recovered

    def _recover_stale_runs(self, run_store: RunStore) -> int:
        recovered = 0
        for run in run_store.list_runs(limit=None):
            if run.state.value in self.RUN_TERMINAL_STATES:
                continue
            run.state = RunState.ABORTED
            run.error = run.error or "Service restarted before run completed"
            run.completed_at = run.completed_at or time.time()
            run.updated_at = time.time()
            run.metadata["recoverable"] = True
            run.metadata["resume_from_step"] = run.current_step_index
            run.metadata["recovery_reason"] = "service_restart"

            if 0 <= run.current_step_index < len(run.steps):
                step = run.steps[run.current_step_index]
                if step.status in {PlanStepState.RUNNING, PlanStepState.WAITING_CONFIRMATION}:
                    step.status = PlanStepState.PENDING
                    step.started_at = None
                    step.finished_at = None
                    step.output = ""
                    step.error = None
                    step.exit_status = None
                    step.observation = None

            run_store.save_run(run)
            self.facts.record_failure(
                "plan",
                run.run_id,
                run.error,
                state=run.state.value,
                step_id=run.steps[run.current_step_index].id if 0 <= run.current_step_index < len(run.steps) else None,
                details={
                    "goal": run.goal,
                    "recoverable": True,
                },
            )
            recovered += 1
        return recovered

    def _learn_from_task(self, snapshot: TaskSnapshot) -> None:
        self.facts.upsert(
            "tasks.latest",
            {
                "task_id": snapshot.task_id,
                "skill_name": snapshot.skill_name,
                "state": snapshot.state,
                "updated_at": snapshot.updated_at,
            },
            category="task",
            source=snapshot.task_id,
        )

        for step in snapshot.history:
            observation = step.observation
            if observation is None:
                continue
            self._learn_from_observation(
                observation.model_dump(),
                source=f"task:{snapshot.task_id}",
            )

        if snapshot.state in {TaskState.FAILED.value, TaskState.ABORTED.value} and snapshot.error:
            self.facts.record_failure(
                "task",
                snapshot.task_id,
                snapshot.error,
                state=snapshot.state,
                details={
                    "skill_name": snapshot.skill_name,
                    "checkpoint": snapshot.current_checkpoint,
                },
            )

    def _learn_from_plan_run(self, run: PlanRunStatus) -> None:
        self.facts.upsert(
            "plans.latest",
            {
                "run_id": run.run_id,
                "goal": run.goal,
                "state": run.state.value,
                "updated_at": run.updated_at,
            },
            category="plan",
            source=run.run_id,
        )

        for step in run.steps:
            if step.observation is None:
                continue
            self._learn_from_observation(
                step.observation.model_dump(),
                source=f"plan:{run.run_id}",
            )

        if run.state in {RunState.FAILED, RunState.ABORTED} and run.error:
            step_id = run.steps[run.current_step_index].id if 0 <= run.current_step_index < len(run.steps) else None
            self.facts.record_failure(
                "plan",
                run.run_id,
                run.error,
                state=run.state.value,
                step_id=step_id,
                details={"goal": run.goal},
            )

    def _learn_from_observation(self, observation: dict[str, Any], *, source: str) -> None:
        tool_name = str(observation.get("tool_name") or "")
        parsed = observation.get("parsed") or {}

        if tool_name == "systemd.status":
            unit = parsed.get("unit") or parsed.get("Id") or ""
            if unit:
                self.facts.upsert(
                    f"service.{unit}.active_state",
                    parsed.get("ActiveState"),
                    category="service",
                    source=source,
                    details={"parsed": parsed},
                )
                self.facts.upsert(
                    f"service.{unit}.sub_state",
                    parsed.get("SubState"),
                    category="service",
                    source=source,
                )

        if tool_name == "systemd.restart":
            unit = parsed.get("unit") or ""
            if unit:
                self.facts.upsert(
                    f"service.{unit}.last_restart_request",
                    observation.get("timestamp"),
                    category="service",
                    source=source,
                    details={"parsed": parsed},
                )

        if tool_name == "docker.ps":
            self.facts.upsert(
                "docker.containers.count",
                parsed.get("count", 0),
                category="docker",
                source=source,
                details={"containers": parsed.get("containers", [])},
            )

        if tool_name == "shell.exec":
            command = str(parsed.get("command") or "")
            output = str(observation.get("output") or "")
            if "uname -a" in command:
                self.facts.upsert(
                    "system.uname",
                    output,
                    category="system",
                    source=source,
                )
