"""
Plan Executor — Backend runtime for durable multi-step plan execution.
"""

import asyncio
import logging
import time
import uuid
from typing import Any, Awaitable, Callable, Optional

from ..memory.run_memory import RunMemory
from ..safety import PolicyAction, PolicyDecision, PolicyEngine
from .models import (
    PlanRunStatus,
    PlanStepState,
    PlanStepStatus,
    RunState,
    ToolObservation,
)
from ..tools import ToolExecutor

logger = logging.getLogger("exoanchor.plan_executor")


class _AbortRequested(Exception):
    """Internal control-flow exception for aborting a run."""


class PlanExecutor:
    """Execute backend plan runs and persist their state."""

    TERMINAL_STEP_STATES = {
        PlanStepState.DONE,
        PlanStepState.FAILED,
        PlanStepState.SKIPPED,
    }

    def __init__(
        self,
        ssh_manager=None,
        tool_executor: Optional[ToolExecutor] = None,
        run_memory: Optional[RunMemory] = None,
        policy_engine: Optional[PolicyEngine] = None,
        run_store=None,
        step_evaluator: Optional[Callable[[dict], Awaitable[dict]]] = None,
    ):
        self.ssh = ssh_manager
        self.tools = tool_executor or (ToolExecutor(None, ssh_manager) if ssh_manager is not None else None)
        self.memory = run_memory
        self.policy = policy_engine
        self.run_store = run_store
        self.step_evaluator = step_evaluator

        self._current_run: Optional[PlanRunStatus] = None
        self._run_task: Optional[asyncio.Task] = None
        self._abort_event = asyncio.Event()
        self._paused = False
        self._confirmation_future: Optional[asyncio.Future] = None
        self._lock = asyncio.Lock()
        self._ws_callbacks: list[Callable] = []
        self.run_history: list[PlanRunStatus] = []

        if self.run_store:
            self.run_history = self.run_store.list_runs(limit=50)

    @property
    def has_active_run(self) -> bool:
        if self._current_run is None:
            return False
        return self._current_run.state not in {
            RunState.COMPLETED,
            RunState.FAILED,
            RunState.ABORTED,
        }

    def set_step_evaluator(self, step_evaluator: Optional[Callable[[dict], Awaitable[dict]]]) -> None:
        self.step_evaluator = step_evaluator

    async def start_plan(
        self,
        goal: str,
        steps: list[dict],
        supervised: bool = False,
        react_mode: str = "on_fail",
        model: str = "",
        source: str = "llm",
        metadata: Optional[dict[str, Any]] = None,
    ) -> PlanRunStatus:
        async with self._lock:
            if self.has_active_run:
                raise RuntimeError(f"Plan already running: {self._current_run.run_id}")

            now = time.time()
            run = PlanRunStatus(
                run_id=str(uuid.uuid4())[:8],
                goal=goal or "执行计划",
                state=RunState.RUNNING,
                steps=[self._normalize_step(step, idx) for idx, step in enumerate(steps)],
                created_at=now,
                started_at=now,
                updated_at=now,
                supervised=supervised,
                react_mode=react_mode or "on_fail",
                model=model or "",
                source=source or "llm",
                metadata=metadata or {},
            )
            self._refresh_run_counters(run)
            self._current_run = run
            self._paused = False
            self._abort_event.clear()
            await self._emit_run("created", run)
            self._run_task = asyncio.create_task(self._run_current_plan())
            return run

    async def resume_saved_run(self, run_id: str) -> PlanRunStatus:
        async with self._lock:
            if self.has_active_run:
                raise RuntimeError(f"Plan already running: {self._current_run.run_id}")
            if self.run_store is None:
                raise RuntimeError("Run store not configured")

            saved = self.run_store.load_run(run_id)
            if saved is None:
                raise RuntimeError("Run not found")
            if not saved.metadata.get("recoverable", False):
                raise RuntimeError("Run is not marked recoverable")

            resume_from = int(saved.metadata.get("resume_from_step", saved.current_step_index or 0) or 0)
            now = time.time()
            resumed = PlanRunStatus(**saved.model_dump())
            resumed.run_id = str(uuid.uuid4())[:8]
            resumed.state = RunState.RUNNING
            resumed.error = None
            resumed.created_at = now
            resumed.started_at = now
            resumed.updated_at = now
            resumed.completed_at = None
            resumed.waiting_step_id = None
            resumed.current_step_index = max(0, min(resume_from, len(resumed.steps)))
            resumed.metadata = dict(saved.metadata)
            resumed.metadata["resumed_from"] = saved.run_id
            resumed.metadata["recoverable"] = False
            resumed.metadata["resume_from_step"] = resumed.current_step_index

            for idx, step in enumerate(resumed.steps):
                if idx < resumed.current_step_index and step.status == PlanStepState.DONE:
                    continue
                if step.status != PlanStepState.SKIPPED:
                    step.status = PlanStepState.PENDING
                step.started_at = None
                step.finished_at = None
                step.output = ""
                step.error = None
                step.exit_status = None
                step.observation = None
                step.eval_action = None
                step.eval_reason = None

            self._refresh_run_counters(resumed)
            self._current_run = resumed
            self._paused = False
            self._abort_event.clear()
            await self._emit_run("created", resumed)
            self._run_task = asyncio.create_task(self._run_current_plan())
            return resumed

    def get_current_run(self) -> Optional[PlanRunStatus]:
        return self._current_run

    def get_run(self, run_id: str) -> Optional[PlanRunStatus]:
        if self._current_run and self._current_run.run_id == run_id:
            return self._current_run
        if self.run_store:
            return self.run_store.load_run(run_id)
        return next((run for run in self.run_history if run.run_id == run_id), None)

    def list_runs(self, limit: int = 20) -> list[PlanRunStatus]:
        runs: list[PlanRunStatus] = []
        seen: set[str] = set()

        if self._current_run:
            runs.append(self._current_run)
            seen.add(self._current_run.run_id)

        source_runs = self.run_store.list_runs(limit=max(limit, 50)) if self.run_store else self.run_history
        for run in source_runs:
            if run.run_id in seen:
                continue
            runs.append(run)
            seen.add(run.run_id)
            if len(runs) >= limit:
                break
        return runs[:limit]

    async def pause_run(self, run_id: str) -> PlanRunStatus:
        run = self._require_current_run(run_id)
        if run.state == RunState.RUNNING:
            self._paused = True
            run.state = RunState.PAUSED
            await self._emit_run("updated", run)
        return run

    async def resume_run(self, run_id: str) -> PlanRunStatus:
        run = self._require_current_run(run_id)
        if run.state == RunState.PAUSED:
            self._paused = False
            run.state = RunState.RUNNING
            await self._emit_run("updated", run)
        return run

    async def abort_run(self, run_id: str) -> Optional[PlanRunStatus]:
        run = self._require_current_run(run_id)
        self._abort_event.set()
        self._paused = False
        if self._confirmation_future and not self._confirmation_future.done():
            self._confirmation_future.set_result("__abort__")
        return run

    async def confirm_step(self, run_id: str, approved: bool) -> PlanRunStatus:
        run = self._require_current_run(run_id)
        if run.state != RunState.WAITING_CONFIRMATION or self._confirmation_future is None:
            raise RuntimeError("Run is not waiting for confirmation")
        if not self._confirmation_future.done():
            self._confirmation_future.set_result(bool(approved))
        return run

    def add_ws_callback(self, callback: Callable):
        self._ws_callbacks.append(callback)

    def remove_ws_callback(self, callback: Callable):
        if callback in self._ws_callbacks:
            self._ws_callbacks.remove(callback)

    async def _run_current_plan(self) -> None:
        run = self._current_run
        if run is None:
            return

        try:
            index = 0
            while index < len(run.steps):
                if self._abort_event.is_set():
                    raise _AbortRequested()

                await self._wait_if_paused(run)

                step = run.steps[index]
                run.current_step_index = index

                if step.status in self.TERMINAL_STEP_STATES:
                    index += 1
                    continue

                await self._prepare_step(run, step)
                decision = self._evaluate_policy(run, step)

                if decision.action == PolicyAction.DENY:
                    step.status = PlanStepState.FAILED
                    step.error = f"Policy denied step: {decision.reason}"
                    step.finished_at = time.time()
                    run.state = RunState.FAILED
                    run.error = step.error
                    run.completed_at = time.time()
                    await self._emit_run("finished", run)
                    return

                requires_confirmation = (run.supervised and step.dangerous) or decision.action == PolicyAction.CONFIRM
                confirmation_reason = decision.reason if decision.action == PolicyAction.CONFIRM else "用户确认危险步骤"

                if requires_confirmation:
                    approved = await self._request_confirmation(run, step)
                    if approved == "__abort__":
                        raise _AbortRequested()
                    if not approved:
                        step.status = PlanStepState.SKIPPED
                        step.eval_action = "reject" if decision.action != PolicyAction.CONFIRM else "policy_reject"
                        step.eval_reason = confirmation_reason
                        step.finished_at = time.time()
                        run.state = RunState.RUNNING
                        run.waiting_step_id = None
                        await self._emit_run("updated", run)
                        index += 1
                        continue

                if self.tools is None:
                    raise RuntimeError("Tool executor not configured")

                observation = await self.tools.execute(step.tool, step.args)
                await self._apply_tool_result(step, observation)

                if self.step_evaluator and (run.react_mode == "always" or not observation.success):
                    evaluation = await self._evaluate_step(run, index, step, observation)
                    should_stop, next_index = await self._apply_evaluation(run, index, step, evaluation)
                    if should_stop:
                        return
                    index = next_index
                    continue

                await self._emit_run("updated", run)
                if not observation.success:
                    run.state = RunState.FAILED
                    run.error = step.error or f"步骤 {step.id} 执行失败"
                    run.completed_at = time.time()
                    await self._emit_run("finished", run)
                    return

                index += 1

            run.current_step_index = len(run.steps)
            run.state = RunState.COMPLETED
            run.completed_at = time.time()
            await self._emit_run("finished", run)

        except _AbortRequested:
            run.state = RunState.ABORTED
            run.error = "Aborted by user"
            run.completed_at = time.time()
            await self._emit_run("finished", run)
        except asyncio.CancelledError:
            run.state = RunState.ABORTED
            run.error = "Aborted by user"
            run.completed_at = time.time()
            await self._emit_run("finished", run)
        except Exception as exc:
            logger.exception("Plan execution failed")
            run.state = RunState.FAILED
            run.error = str(exc)
            run.completed_at = time.time()
            await self._emit_run("finished", run)
        finally:
            self._paused = False
            self._abort_event.clear()
            self._confirmation_future = None
            self._remember_run(run)
            self._current_run = None
            self._run_task = None

    def _normalize_step(self, step: Any, idx: int) -> PlanStepStatus:
        raw_id = None
        if isinstance(step, dict):
            raw_id = step.get("id")
            description = step.get("description") or f"步骤 {idx + 1}"
            tool = ToolExecutor.normalize_tool_name(step.get("tool") or "shell.exec")
            args = step.get("args") if isinstance(step.get("args"), dict) else {}
            command = step.get("command") or ""
            dangerous = bool(step.get("dangerous", False))
        else:
            raw_id = getattr(step, "id", None)
            description = getattr(step, "description", f"步骤 {idx + 1}")
            tool = ToolExecutor.normalize_tool_name(getattr(step, "tool", "shell.exec"))
            args = getattr(step, "args", {}) if isinstance(getattr(step, "args", {}), dict) else {}
            command = getattr(step, "command", "")
            dangerous = bool(getattr(step, "dangerous", False))

        args = dict(args)
        if command and "command" not in args:
            args["command"] = command
        command = command or ToolExecutor.describe_tool_call(tool, args)

        if raw_id in (None, ""):
            raw_id = idx + 1

        return PlanStepStatus(
            id=str(raw_id),
            description=str(description).strip(),
            tool=tool,
            args=args,
            command=str(command).strip(),
            dangerous=dangerous,
        )

    async def _prepare_step(self, run: PlanRunStatus, step: PlanStepStatus) -> None:
        run.state = RunState.RUNNING
        run.waiting_step_id = None
        step.status = PlanStepState.RUNNING
        step.started_at = step.started_at or time.time()
        step.finished_at = None
        step.output = ""
        step.error = None
        step.exit_status = None
        step.observation = None
        await self._emit_run("updated", run)

    async def _apply_tool_result(self, step: PlanStepStatus, observation: ToolObservation) -> None:
        step.observation = observation
        step.output = str(observation.output or observation.stdout or observation.stderr or "").strip()
        step.exit_status = observation.exit_status
        step.finished_at = time.time()

        if observation.success:
            step.status = PlanStepState.DONE
            step.error = None
        else:
            step.status = PlanStepState.FAILED
            step.error = observation.error or step.output or f"Tool failed with status {step.exit_status}"

    async def _evaluate_step(
        self,
        run: PlanRunStatus,
        index: int,
        step: PlanStepStatus,
        observation: ToolObservation,
    ) -> dict:
        remaining = [
            {
                "id": future_step.id,
                "description": future_step.description,
                "tool": future_step.tool,
            }
            for future_step in run.steps[index + 1:]
        ]

        payload = {
            "goal": run.goal,
            "step_id": step.id,
            "total": len(run.steps),
            "description": step.description,
            "tool": step.tool,
            "args": step.args,
            "command": step.command,
            "output": (step.output or step.error or "")[:2000],
            "observation": observation.model_dump(),
            "success": bool(observation.success),
            "remaining": remaining,
            "model": run.model,
        }

        try:
            response = await self.step_evaluator(payload)
            return response or {"action": "continue"}
        except Exception as exc:
            logger.warning(f"Plan step evaluator failed: {exc}")
            return {"action": "continue", "_error": str(exc)}

    async def _apply_evaluation(
        self,
        run: PlanRunStatus,
        index: int,
        step: PlanStepStatus,
        evaluation: dict,
    ) -> tuple[bool, int]:
        action = str(evaluation.get("action", "continue")).strip() or "continue"
        reason = str(evaluation.get("reason") or evaluation.get("message") or "").strip()
        step.eval_action = action
        step.eval_reason = reason or None

        if action == "abort":
            run.state = RunState.FAILED
            run.error = reason or step.error or f"步骤 {step.id} 执行失败"
            run.completed_at = time.time()
            await self._emit_run("finished", run)
            return True, index

        if action == "modify":
            target_index = self._find_step_index(run, evaluation.get("replace_step_id"))
            new_command = str(evaluation.get("new_command") or "").strip()
            raw_tool = str(evaluation.get("tool") or "").strip()
            new_tool = ToolExecutor.normalize_tool_name(raw_tool) if raw_tool else ""
            new_args = evaluation.get("args") if isinstance(evaluation.get("args"), dict) else {}
            if target_index is not None and (new_command or new_tool):
                target_step = run.steps[target_index]
                if new_command:
                    target_step.command = new_command
                    target_step.tool = "shell.exec"
                    target_step.args = {"command": new_command}
                elif new_tool:
                    target_step.tool = new_tool
                    target_step.args = new_args
                    target_step.command = ToolExecutor.describe_tool_call(new_tool, new_args)
                if evaluation.get("description"):
                    target_step.description = str(evaluation.get("description")).strip()

        if action == "add_step":
            new_command = str(evaluation.get("command") or "").strip()
            new_tool = ToolExecutor.normalize_tool_name(evaluation.get("tool") or "shell.exec")
            new_args = evaluation.get("args") if isinstance(evaluation.get("args"), dict) else {}
            if new_command:
                new_args = dict(new_args)
                new_args.setdefault("command", new_command)
            if new_command or new_args:
                new_step = PlanStepStatus(
                    id=str(evaluation.get("id") or f"extra-{int(time.time() * 1000)}"),
                    description=str(evaluation.get("description") or "额外步骤").strip(),
                    tool=new_tool,
                    args=new_args,
                    command=ToolExecutor.describe_tool_call(new_tool, new_args),
                    dangerous=bool(evaluation.get("dangerous", False)),
                )
                run.steps.insert(index + 1, new_step)

        if action == "skip":
            target_index = self._find_step_index(run, evaluation.get("next_step_id"))
            if target_index is not None and target_index > index + 1:
                for skipped in run.steps[index + 1:target_index]:
                    skipped.status = PlanStepState.SKIPPED
                    skipped.eval_action = "skip"
                    skipped.eval_reason = reason or "AI 建议跳过"
                    skipped.finished_at = time.time()
                await self._emit_run("updated", run)
                return False, target_index

            if index + 1 < len(run.steps):
                skipped = run.steps[index + 1]
                skipped.status = PlanStepState.SKIPPED
                skipped.eval_action = "skip"
                skipped.eval_reason = reason or "AI 建议跳过"
                skipped.finished_at = time.time()
                await self._emit_run("updated", run)
                return False, index + 2

        await self._emit_run("updated", run)
        return False, index + 1

    async def _request_confirmation(self, run: PlanRunStatus, step: PlanStepStatus):
        run.state = RunState.WAITING_CONFIRMATION
        run.waiting_step_id = step.id
        step.status = PlanStepState.WAITING_CONFIRMATION
        self._confirmation_future = asyncio.get_event_loop().create_future()
        await self._emit_run("confirmation_requested", run)
        result = await self._confirmation_future
        self._confirmation_future = None
        run.waiting_step_id = None
        if result == "__abort__":
            return result
        run.state = RunState.RUNNING
        step.status = PlanStepState.RUNNING
        await self._emit_run("updated", run)
        return bool(result)

    async def _wait_if_paused(self, run: PlanRunStatus) -> None:
        while self._paused and not self._abort_event.is_set():
            run.state = RunState.PAUSED
            await self._emit_run("updated", run)
            await asyncio.sleep(0.2)
        if self._abort_event.is_set():
            raise _AbortRequested()
        if run.state == RunState.PAUSED:
            run.state = RunState.RUNNING
            await self._emit_run("updated", run)

    def _find_step_index(self, run: PlanRunStatus, step_id: Any) -> Optional[int]:
        if step_id in (None, ""):
            return None
        needle = str(step_id)
        for idx, step in enumerate(run.steps):
            if step.id == needle:
                return idx
        return None

    def _refresh_run_counters(self, run: PlanRunStatus) -> None:
        run.total_steps = len(run.steps)
        run.completed_steps = sum(1 for step in run.steps if step.status in self.TERMINAL_STEP_STATES)
        run.updated_at = time.time()

    def _remember_run(self, run: PlanRunStatus) -> None:
        snapshot = PlanRunStatus(**run.model_dump())
        self.run_history = [item for item in self.run_history if item.run_id != snapshot.run_id]
        self.run_history.insert(0, snapshot)
        self.run_history = self.run_history[:50]

    async def _emit_run(self, event: str, run: PlanRunStatus) -> None:
        self._refresh_run_counters(run)
        if self.run_store:
            self.run_store.save_run(run)
        if self.memory:
            try:
                self.memory.capture_plan_run(run)
                if self.run_store:
                    self.run_store.save_run(run)
            except Exception as exc:
                logger.warning(f"Failed to capture plan memory for {run.run_id}: {exc}")

        payload = {
            "type": "plan_run",
            "event": event,
            "run": run.model_dump(),
        }
        for cb in self._ws_callbacks:
            try:
                if asyncio.iscoroutinefunction(cb):
                    await cb(payload)
                else:
                    cb(payload)
            except Exception as exc:
                logger.debug(f"Plan WS callback failed: {exc}")

    def _require_current_run(self, run_id: str) -> PlanRunStatus:
        if self._current_run is None or self._current_run.run_id != run_id:
            raise RuntimeError("Run not active")
        return self._current_run

    def _evaluate_policy(self, run: PlanRunStatus, step: PlanStepStatus) -> PolicyDecision:
        if self.policy is None:
            return PolicyDecision(tool_name=step.tool, command=step.command, args=step.args)

        policy_context = dict(run.metadata.get("policy_context", {}))
        source_type = policy_context.get("source_type", "manual_plan")
        agent_mode = policy_context.get("agent_mode", "manual")
        decision = self.policy.evaluate_tool_call(
            step.tool,
            step.args,
            source_type=source_type,
            agent_mode=agent_mode,
            metadata={
                "run_id": run.run_id,
                "step_id": step.id,
                "goal": run.goal,
            },
        )
        self.policy.audit(
            decision,
            source_type=source_type,
            agent_mode=agent_mode,
            metadata={
                "run_id": run.run_id,
                "step_id": step.id,
                "goal": run.goal,
            },
        )
        return decision
