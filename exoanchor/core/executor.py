"""
Semi-Active Executor — Runs Skills (playbooks) on-demand or triggered by passive mode.

This is the engine that executes Skill sequences using:
  - Vision (screen capture + LLM analysis) for guided mode
  - HID (keyboard/mouse) for target machine interaction
  - SSH for fast command execution
  - Safety checks at every step
"""

import asyncio
import time
import logging
import json
from typing import Optional, Callable, Awaitable

from .models import CandidateAction, Task, TaskState, ScreenState, TaskStatus, ToolObservation
from .context import ExecutionContext
from .plan_ir import plan_from_scripted_skill, ExecutablePlan, ExecutableStep
from ..memory.run_memory import RunMemory
from ..safety import PolicyAction, PolicyDecision, PolicyEngine
from ..action.driver import ActionDriver, Action
from ..tools import ToolExecutor

logger = logging.getLogger("exoanchor.executor")


class SemiActiveExecutor:
    """
    Executes Skills in the semi-active or manual mode.

    Both modes use the same execution engine — the difference is
    who triggers execution:
      - Manual mode: only user/API triggers
      - Semi-active mode: also triggered by passive monitor
    """

    def __init__(
        self,
        action_driver: ActionDriver,
        vision_backend=None,
        safety_guard=None,
        skill_store=None,
        tool_executor: Optional[ToolExecutor] = None,
        run_memory: Optional[RunMemory] = None,
        policy_engine: Optional[PolicyEngine] = None,
    ):
        self.action = action_driver
        self.vision = vision_backend
        self.guard = safety_guard
        self.skill_store = skill_store
        self.tools = tool_executor or ToolExecutor(action_driver, getattr(action_driver, "ssh", None), vision_backend)
        self.memory = run_memory
        self.policy = policy_engine

        # Current execution
        self._current_task: Optional[Task] = None
        self._current_context: Optional[ExecutionContext] = None
        self._execution_task: Optional[asyncio.Task] = None
        self._abort_event = asyncio.Event()

        # Task history
        self.task_history: list[Task] = []

        # WebSocket callbacks for real-time updates
        self._ws_callbacks: list[Callable] = []

    # ── Task Execution ──────────────────────────────────────────

    async def run_skill(self, skill_name: str, params: dict = None, metadata: dict = None) -> TaskStatus:
        """
        Execute a skill by name. Main entry point for both manual and triggered execution.

        Returns TaskStatus when complete (or failed/aborted).
        """
        task, skill, merged_params = self._prepare_skill_run(skill_name, params, metadata=metadata)
        return await self._execute_prepared_skill(task, skill, merged_params)

    def _prepare_skill_run(self, skill_name: str, params: dict = None, metadata: dict = None):
        if self._current_task and self._current_task.state == TaskState.RUNNING:
            raise RuntimeError(f"Task already running: {self._current_task.task_id}")

        if self.skill_store is None:
            raise RuntimeError("No skill store configured")

        skill = self.skill_store.get_skill(skill_name)
        if skill is None:
            raise ValueError(f"Skill not found: {skill_name}")

        raw_params = params or {}
        merged_params = skill.validate_params(raw_params) if hasattr(skill, "validate_params") else raw_params

        task = Task(skill_name=skill_name, params=merged_params, mode=skill.get("mode", "guided"), metadata=metadata or {})
        task.state = TaskState.RUNNING
        task.started_at = time.time()
        return task, skill, merged_params

    async def _execute_prepared_skill(self, task: Task, skill, merged_params: dict) -> TaskStatus:
        self._current_task = task
        self._abort_event.clear()

        ctx = ExecutionContext(task=task).bind_runtime(
            action=self.action,
            vision=self.vision,
            ssh=self.action.ssh if hasattr(self.action, "ssh") else None,
            executor=self,
            skill=skill,
            tools=self.tools,
        )
        self._current_context = ctx

        logger.info(f"Starting task {task.task_id}: skill={task.skill_name}, params={merged_params}")
        self._persist_context(ctx)
        await self._notify_ws({"type": "task_start", "task_id": task.task_id, "skill": task.skill_name})

        try:
            mode = skill.get("mode", "guided")

            if mode == "scripted":
                await self._run_scripted(skill, ctx)
            elif mode == "guided":
                await self._run_guided(skill, ctx)
            elif mode == "python":
                await self._run_python(skill, ctx)
            else:
                raise ValueError(f"Unknown skill mode: {mode}")

            if not ctx.is_complete:
                ctx.mark_complete()
            logger.info(f"Task {task.task_id} completed successfully")

        except asyncio.CancelledError:
            ctx.mark_aborted()
            logger.info(f"Task {task.task_id} aborted")
        except Exception as e:
            ctx.mark_failed(str(e))
            logger.error(f"Task {task.task_id} failed: {e}")

        # Save to history
        self.task_history.append(task)
        if len(self.task_history) > 50:
            self.task_history = self.task_history[-50:]

        status = ctx.task.to_status(ctx.progress, ctx.current_step, ctx.current_checkpoint)
        self._persist_context(ctx)
        await self._notify_ws({"type": "task_end", "task_id": task.task_id, "status": status.model_dump()})

        self._current_task = None
        self._current_context = None
        return status

    async def run_skill_async(self, skill_name: str, params: dict = None, metadata: dict = None) -> str:
        """Start a skill execution in background. Returns task_id."""
        task, skill, merged_params = self._prepare_skill_run(skill_name, params, metadata=metadata)
        self._current_task = task
        self._execution_task = asyncio.create_task(self._execute_prepared_skill(task, skill, merged_params))
        return task.task_id

    # ── Scripted Execution ──────────────────────────────────────

    async def _run_scripted(self, skill: dict, ctx: ExecutionContext):
        """Execute a scripted skill (fixed steps)."""
        plan = plan_from_scripted_skill(skill, ctx.task.params)
        await self._run_executable_plan(plan, ctx)

    async def _run_executable_plan(self, plan: ExecutablePlan, ctx: ExecutionContext):
        """Execute a normalized internal plan."""
        total = len(plan.steps)

        for i, step in enumerate(plan.steps):
            if self._abort_event.is_set():
                ctx.mark_aborted()
                return

            while ctx.is_paused:
                await asyncio.sleep(0.5)

            step_id = step.id
            wait_time = step.wait or 0.0
            expect = step.expect
            retry = int(step.retry or 1)
            retry_delay = float(step.retry_delay or 1.0)

            logger.info(f"Step {i+1}/{total}: {step_id}")

            for attempt in range(retry):
                # Safety check
                if self.guard:
                    screen = ScreenState(type="unknown")
                    safety = await self.guard.check(screen, ctx)
                    if safety.get("abort", False):
                        ctx.mark_failed(f"Safety abort: {safety.get('reason')}")
                        return

                # Execute tool
                decision = self._evaluate_policy(step.tool, step.args, ctx, {
                    "step_id": step_id,
                    "plan_goal": plan.goal,
                    "plan_source": plan.source,
                })
                if decision.action == PolicyAction.DENY:
                    ctx.mark_failed(f"Policy denied step {step_id}: {decision.reason}")
                    self._persist_context(ctx, {"policy_denied": True, "policy_reason": decision.reason})
                    return
                if decision.action == PolicyAction.CONFIRM:
                    ctx.mark_failed(f"Step {step_id} requires a supervised plan: {decision.reason}")
                    self._persist_context(ctx, {"policy_confirmation_required": True, "policy_reason": decision.reason})
                    return

                observation = await self.tools.execute(step.tool, step.args)

                # Record step
                ctx.record_step(
                    screen_state=None,
                    action_type=step.tool,
                    action_detail=self._format_step_detail(step),
                    action_result=observation.output or str(observation.success),
                    channel=observation.channel,
                    observation=observation,
                )
                self._persist_context(ctx, {"plan_goal": plan.goal, "plan_source": plan.source})

                # Wait
                if wait_time:
                    await asyncio.sleep(wait_time)

                if not observation.success:
                    if attempt < retry - 1:
                        logger.info(f"Step {step_id} action failed, retry {attempt+1}/{retry}: {observation.error or observation.output}")
                        await asyncio.sleep(retry_delay)
                        continue
                    ctx.mark_failed(f"Step {step_id} failed: {observation.error or observation.output or 'tool failed'}")
                    return

                # Verify expectation
                if expect:
                    if self._matches_expectation(expect, observation):
                        break  # Success
                    elif attempt < retry - 1:
                        logger.info(f"Step {step_id} retry {attempt+1}/{retry}")
                        await asyncio.sleep(retry_delay)
                        continue
                    else:
                        ctx.mark_failed(f"Step {step_id} failed: expected '{expect}', got '{observation.output}'")
                        return
                else:
                    break  # No expectation or no output to verify

            ctx.update_progress((i + 1) / total)
            await self._notify_ws({
                "type": "step",
                "task_id": ctx.task.task_id,
                "step": i + 1,
                "total": total,
                "step_id": step_id,
                "progress": ctx.progress,
            })

    async def _run_python(self, skill, ctx: ExecutionContext):
        """Execute a Python skill and optionally interpret a returned plan."""
        result = await skill.execute(ctx)

        if isinstance(result, ExecutablePlan):
            await self._run_executable_plan(result, ctx)
            return

        if isinstance(result, list):
            plan = ExecutablePlan(
                goal=getattr(skill, "description", "") or getattr(skill, "name", "执行技能"),
                steps=[ExecutableStep(**step) if isinstance(step, dict) else step for step in result],
                source=f"skill:{getattr(skill, 'name', 'unknown')}",
            )
            await self._run_executable_plan(plan, ctx)
            return

        if isinstance(result, dict):
            if result.get("type") == "plan" and isinstance(result.get("steps"), list):
                plan = ExecutablePlan(
                    goal=result.get("goal") or getattr(skill, "description", "") or getattr(skill, "name", "执行技能"),
                    steps=[ExecutableStep(**step) for step in result["steps"]],
                    source=f"skill:{getattr(skill, 'name', 'unknown')}",
                )
                await self._run_executable_plan(plan, ctx)
                return
            ctx.mark_complete(result=result)

    # ── Guided Execution ────────────────────────────────────────

    async def _run_guided(self, skill: dict, ctx: ExecutionContext):
        """
        Execute a guided skill (LLM-driven).
        The Vision API analyzes the screen and decides next actions.
        """
        if self.vision is None:
            raise RuntimeError("Guided mode requires a vision backend")

        goal = skill.get("goal", "")
        goal = self._substitute_params_str(goal, ctx.task.params)
        checkpoints = skill.get("checkpoints", [])
        safety_cfg = skill.get("safety", {})
        max_steps = safety_cfg.get("max_steps", 100)
        max_duration = safety_cfg.get("max_duration", 300)
        recovery = skill.get("recovery", {})
        recovery_attempts = 0
        recovery_limit = int((recovery or {}).get("max_attempts", 2) or 2) if isinstance(recovery, dict) else 2

        logger.info(f"Guided execution — Goal: {goal[:100]}...")

        while not ctx.is_complete:
            if self._abort_event.is_set():
                ctx.mark_aborted()
                return

            while ctx.is_paused:
                await asyncio.sleep(0.5)

            # Safety limits
            if ctx.current_step >= max_steps:
                ctx.mark_failed(f"Max steps exceeded ({max_steps})")
                return
            if ctx.elapsed > max_duration:
                ctx.mark_failed(f"Timeout ({max_duration}s)")
                return

            # Loop detection
            if ctx.detect_loop(window=5):
                if recovery_attempts < recovery_limit and await self._attempt_guided_recovery(ctx, recovery, "loop_detected"):
                    recovery_attempts += 1
                    self._persist_context(ctx, {"guided_recovery_attempts": recovery_attempts, "guided_recovery_reason": "loop_detected"})
                    await asyncio.sleep(1)
                    continue
                if recovery_attempts >= recovery_limit:
                    ctx.mark_failed(f"Agent stuck in loop after {recovery_attempts} recovery attempts")
                else:
                    ctx.mark_failed("Agent stuck in loop")
                    return

            # 1. Capture screen
            try:
                frame = await self.vision.capture()
                screen_state = await self.vision.analyze(frame, ctx, goal, checkpoints)
            except Exception as e:
                logger.error(f"Vision analysis failed: {e}")
                await asyncio.sleep(2)
                continue

            self._update_guided_checkpoint(screen_state, checkpoints, ctx)
            if screen_state.progress_hint is not None:
                ctx.update_progress(max(ctx.progress, max(0.0, min(1.0, float(screen_state.progress_hint)))))

            # 2. Check for completion
            if screen_state.type in {"task_complete", "done"}:
                ctx.mark_complete()
                return

            # 3. Safety check
            if self.guard:
                safety = await self.guard.check(screen_state, ctx)
                if safety.get("abort", False):
                    ctx.mark_failed(f"Safety abort: {safety.get('reason')}")
                    return

            # 4. Decide + Act (from vision analysis)
            candidate = self._select_guided_action(screen_state)
            if candidate is None:
                logger.info("Guided analysis returned no actionable candidate, waiting for next frame")
                await asyncio.sleep(1)
                continue

            action_outcome = await self._execute_guided_candidate(
                candidate,
                screen_state=screen_state,
                goal=goal,
                checkpoints=checkpoints,
                ctx=ctx,
            )
            if action_outcome == "complete":
                return
            if action_outcome == "failed":
                return

            # 6. Progress
            if checkpoints:
                ctx.update_progress(max(ctx.progress, len(ctx.checkpoints_reached) / len(checkpoints)))

            await self._notify_ws({
                "type": "step",
                "task_id": ctx.task.task_id,
                "step": ctx.current_step,
                "screen_state": screen_state.type,
                "progress": ctx.progress,
                "checkpoint": ctx.current_checkpoint,
                "focused_region": screen_state.focused_region,
                "confidence": screen_state.confidence,
            })

            # Wait for screen to stabilize
            await asyncio.sleep(1)

    # ── Task Control ────────────────────────────────────────────

    def abort_current(self):
        """Abort the currently running task."""
        self._abort_event.set()
        if self._execution_task:
            self._execution_task.cancel()

    def pause_current(self):
        """Pause the currently running task."""
        if self._current_context:
            self._current_context.pause()

    def resume_current(self):
        """Resume the currently paused task."""
        if self._current_context:
            self._current_context.resume()

    # ── Status ──────────────────────────────────────────────────

    def get_current_task(self) -> Optional[TaskStatus]:
        """Get current task status."""
        if self._current_context:
            ctx = self._current_context
            return ctx.task.to_status(ctx.progress, ctx.current_step, ctx.current_checkpoint)
        if self._current_task and self._current_task.state == TaskState.RUNNING:
            return self._current_task.to_status()
        return None

    def get_task_history(self, n: int = 10) -> list[dict]:
        """Get recent task history."""
        if self.memory is not None:
            return [snapshot.model_dump() for snapshot in self.memory.list_tasks(limit=n)]
        return [
            {
                "task_id": t.task_id,
                "skill": t.skill_name,
                "state": t.state.value,
                "error": t.error,
                "result": t.result,
            }
            for t in self.task_history[-n:]
        ]

    def get_task_snapshot(self, task_id: str):
        if self.memory is None:
            return None
        return self.memory.load_task(task_id)

    # ── Helpers ─────────────────────────────────────────────────

    def _substitute_params(self, obj: dict, params: dict) -> dict:
        """Substitute {param} placeholders in action definitions."""
        result = {}
        for k, v in obj.items():
            if isinstance(v, str):
                result[k] = self._substitute_params_str(v, params)
            elif isinstance(v, dict):
                result[k] = self._substitute_params(v, params)
            else:
                result[k] = v
        return result

    def _substitute_params_str(self, text: str, params: dict) -> str:
        """Substitute {param} in a string."""
        for key, value in params.items():
            text = text.replace(f"{{{key}}}", str(value))
        return text

    def _persist_context(self, ctx: ExecutionContext, metadata: Optional[dict] = None) -> None:
        if self.memory is None:
            return
        try:
            self.memory.save_task_context(ctx, metadata=metadata)
        except Exception as exc:
            logger.warning(f"Failed to persist task context {ctx.task.task_id}: {exc}")

    def _format_step_detail(self, step: ExecutableStep) -> str:
        return ToolExecutor.describe_tool_call(step.tool, step.args)

    def _evaluate_policy(self, tool_name: str, args: dict, ctx: ExecutionContext, metadata: Optional[dict] = None):
        if self.policy is None:
            return PolicyDecision(tool_name=tool_name, args=dict(args or {}), command=ToolExecutor.describe_tool_call(tool_name, args or {}))
        policy_context = dict(ctx.task.metadata.get("policy_context", {}))
        source_type = policy_context.get("source_type", "manual_task")
        agent_mode = policy_context.get("agent_mode", "manual")
        decision = self.policy.evaluate_tool_call(
            tool_name,
            args,
            source_type=source_type,
            agent_mode=agent_mode,
            metadata=metadata,
        )
        self.policy.audit(
            decision,
            source_type=source_type,
            agent_mode=agent_mode,
            metadata={
                "task_id": ctx.task.task_id,
                "skill_name": ctx.task.skill_name,
                **(metadata or {}),
            },
        )
        return decision

    def _matches_expectation(self, expect: str, observation: ToolObservation) -> bool:
        needle = str(expect).strip()
        if not needle:
            return True
        haystacks = [observation.output or "", observation.stdout or "", observation.stderr or ""]
        if any(needle in item for item in haystacks):
            return True
        lowered = needle.lower()
        for value in self._iter_observation_values(observation.parsed or {}):
            if isinstance(value, str) and lowered == value.lower():
                return True
            if isinstance(value, bool) and lowered in ("true", "false"):
                return value == (lowered == "true")
        return False

    def _safe_json(self, payload: dict) -> str:
        try:
            return json.dumps(payload, ensure_ascii=False, sort_keys=True)
        except TypeError:
            return str(payload)

    def _select_guided_action(self, screen_state: ScreenState) -> Optional[CandidateAction]:
        candidates = list(screen_state.candidate_actions or [])
        if not candidates and isinstance(screen_state.raw_response, dict):
            fallback = screen_state.raw_response.get("next_action")
            if isinstance(fallback, dict):
                try:
                    candidates = [CandidateAction(**fallback)]
                except Exception:
                    logger.debug("Failed to parse fallback next_action: %s", fallback)
        if not candidates:
            return None

        ranked = sorted(candidates, key=lambda item: float(item.confidence or 0.0), reverse=True)
        for candidate in ranked:
            allowed_states = set(candidate.precondition_screen_types or [])
            if allowed_states and screen_state.type not in allowed_states:
                continue
            return candidate
        return ranked[0]

    async def _execute_guided_candidate(
        self,
        candidate: CandidateAction,
        *,
        screen_state: ScreenState,
        goal: str,
        checkpoints: list,
        ctx: ExecutionContext,
    ) -> str:
        if candidate.type == "done":
            ctx.mark_complete()
            return "complete"
        if candidate.type == "error":
            ctx.mark_failed(candidate.reason or "Vision backend reported unrecoverable state")
            return "failed"

        if candidate.tool:
            tool_name = candidate.tool
            tool_args = dict(candidate.args or {})
            decision = self._evaluate_policy(tool_name, tool_args, ctx, {
                "guided_goal": goal,
                "screen_state": screen_state.type,
                "focused_region": screen_state.focused_region,
            })
            if decision.action == PolicyAction.DENY:
                ctx.mark_failed(f"Policy denied guided action: {decision.reason}")
                self._persist_context(ctx, {"policy_denied": True, "policy_reason": decision.reason})
                return "failed"
            if decision.action == PolicyAction.CONFIRM:
                ctx.mark_failed(f"Guided action requires human approval: {decision.reason}")
                self._persist_context(ctx, {"policy_confirmation_required": True, "policy_reason": decision.reason})
                return "failed"
            observation = await self.tools.execute(tool_name, tool_args)
            action_type = tool_name
            action_detail = self._safe_json(tool_args)
            action_result = observation.output or ""
            channel = observation.channel
        else:
            action_data = self._candidate_to_action_payload(candidate)
            action_type_name = f"action.{candidate.type}"
            decision = self._evaluate_policy(action_type_name, action_data, ctx, {
                "guided_goal": goal,
                "screen_state": screen_state.type,
                "focused_region": screen_state.focused_region,
            })
            if decision.action == PolicyAction.DENY:
                ctx.mark_failed(f"Policy denied guided action: {decision.reason}")
                self._persist_context(ctx, {"policy_denied": True, "policy_reason": decision.reason})
                return "failed"
            if decision.action == PolicyAction.CONFIRM:
                ctx.mark_failed(f"Guided action requires human approval: {decision.reason}")
                self._persist_context(ctx, {"policy_confirmation_required": True, "policy_reason": decision.reason})
                return "failed"
            action = Action(**action_data)
            result = await self.action.execute(action)
            observation = ToolObservation(
                tool_name=f"action.{action.type}",
                success=result.success,
                stdout=str(result.output or ""),
                stderr=str(result.error or ""),
                output=str(result.output or result.error or ""),
                parsed={"note": result.note} if result.note else {},
                channel="hid",
                error=result.error or None,
            )
            action_type = action.type
            action_detail = self._safe_json(action_data)
            action_result = str(result.output or result.error or "")
            channel = observation.channel

        postcondition, followup_state = await self._evaluate_guided_postcondition(
            candidate,
            ctx=ctx,
            goal=goal,
            checkpoints=checkpoints,
        )
        if postcondition:
            parsed = dict(observation.parsed or {})
            parsed["guided"] = postcondition
            observation.parsed = parsed
            if followup_state is not None:
                self._update_guided_checkpoint(followup_state, checkpoints, ctx)

        ctx.record_step(
            screen_state=screen_state,
            action_type=action_type,
            action_detail=action_detail,
            action_result=action_result,
            channel=channel,
            observation=observation,
        )
        self._persist_context(ctx, {"guided_goal": goal, "focused_region": screen_state.focused_region})

        if not observation.success:
            ctx.mark_failed(observation.error or observation.output or "Guided action failed")
            return "failed"

        if postcondition and not postcondition.get("ok", True):
            ctx.mark_failed(postcondition.get("reason") or "Guided action did not satisfy its postcondition")
            return "failed"

        return "continue"

    def _candidate_to_action_payload(self, candidate: CandidateAction) -> dict:
        payload = {"type": candidate.type}
        for field in ("key", "text", "x", "y", "button", "duration", "interval", "power_action"):
            value = getattr(candidate, field, None)
            if value is not None:
                payload[field] = value
        if candidate.keys:
            payload["keys"] = list(candidate.keys)
        if candidate.modifiers:
            payload["modifiers"] = list(candidate.modifiers)
        return payload

    def _update_guided_checkpoint(self, screen_state: ScreenState, checkpoints: list, ctx: ExecutionContext) -> None:
        if screen_state.checkpoint and screen_state.checkpoint not in ctx.checkpoints_reached:
            ctx.reach_checkpoint(screen_state.checkpoint)
            return

        if not checkpoints:
            return

        for cp in checkpoints:
            name = cp.get("name")
            if not name or name in ctx.checkpoints_reached:
                continue
            if self._screen_matches_checkpoint(screen_state, cp):
                ctx.reach_checkpoint(name)

    async def _evaluate_guided_postcondition(
        self,
        candidate: CandidateAction,
        *,
        ctx: ExecutionContext,
        goal: str,
        checkpoints: list,
    ) -> tuple[Optional[dict], Optional[ScreenState]]:
        needs_followup = any([
            candidate.wait_for_change,
            candidate.wait_for_stable,
            candidate.expected_screen_type,
            candidate.expected_checkpoint,
            candidate.expected_text,
        ])
        if not needs_followup or self.vision is None:
            return None, None

        outcome = {
            "ok": True,
            "wait_for_change": None,
            "wait_for_stable": None,
            "expected_screen_type": candidate.expected_screen_type,
            "expected_checkpoint": candidate.expected_checkpoint,
            "expected_text": candidate.expected_text,
        }

        if candidate.wait_for_change:
            outcome["wait_for_change"] = await self.vision.wait_for_change(timeout=max(float(candidate.duration or 0.0), 5.0))
            outcome["ok"] = outcome["ok"] and outcome["wait_for_change"]
        if candidate.wait_for_stable:
            outcome["wait_for_stable"] = await self.vision.wait_stable(timeout=max(float(candidate.duration or 0.0), 3.0))
            outcome["ok"] = outcome["ok"] and outcome["wait_for_stable"]

        followup_state = None
        if candidate.expected_screen_type or candidate.expected_checkpoint or candidate.expected_text:
            frame = await self.vision.capture()
            followup_state = await self.vision.analyze(frame, ctx, goal, checkpoints)
            outcome["followup_screen_type"] = followup_state.type
            outcome["followup_checkpoint"] = followup_state.checkpoint
            outcome["followup_description"] = followup_state.description

            if candidate.expected_screen_type and followup_state.type != candidate.expected_screen_type:
                outcome["ok"] = False
            if candidate.expected_checkpoint and not (
                followup_state.checkpoint == candidate.expected_checkpoint
                or any(
                    cp.get("name") == candidate.expected_checkpoint and self._screen_matches_checkpoint(followup_state, cp)
                    for cp in checkpoints
                )
            ):
                outcome["ok"] = False
            if candidate.expected_text:
                texts = " ".join([
                    followup_state.description,
                    followup_state.focused_region,
                    " ".join(followup_state.elements),
                    " ".join(
                        filter(
                            None,
                            [item.label or item.text for item in followup_state.ui_elements],
                        )
                    ),
                ]).lower()
                if candidate.expected_text.lower() not in texts:
                    outcome["ok"] = False

        if not outcome["ok"]:
            reason_bits = []
            if candidate.expected_screen_type and outcome.get("followup_screen_type") != candidate.expected_screen_type:
                reason_bits.append(f"expected screen '{candidate.expected_screen_type}', got '{outcome.get('followup_screen_type')}'")
            if candidate.expected_checkpoint and outcome.get("followup_checkpoint") != candidate.expected_checkpoint:
                reason_bits.append(f"expected checkpoint '{candidate.expected_checkpoint}', got '{outcome.get('followup_checkpoint')}'")
            if candidate.expected_text and candidate.expected_text.lower() not in str(outcome.get("followup_description", "")).lower():
                reason_bits.append(f"expected text '{candidate.expected_text}' not visible")
            if outcome.get("wait_for_change") is False:
                reason_bits.append("screen did not change in time")
            if outcome.get("wait_for_stable") is False:
                reason_bits.append("screen did not stabilize in time")
            outcome["reason"] = "; ".join(reason_bits) or "postcondition mismatch"

        return outcome, followup_state

    def _screen_matches_checkpoint(self, screen_state: ScreenState, checkpoint: dict) -> bool:
        hint = (checkpoint.get("visual_hint") or checkpoint.get("description") or "").strip().lower()
        if not hint:
            return False

        haystacks = [
            screen_state.description.lower(),
            screen_state.focused_region.lower(),
            " ".join(screen_state.elements).lower(),
            " ".join(
                filter(
                    None,
                    [item.label or item.text for item in screen_state.ui_elements],
                )
            ).lower(),
        ]
        return any(hint in haystack for haystack in haystacks if haystack)

    async def _attempt_guided_recovery(self, ctx: ExecutionContext, recovery_cfg, reason: str) -> bool:
        actions = self._recovery_actions(recovery_cfg)
        if not actions:
            return False

        logger.warning("Guided loop detected, attempting recovery: %s", reason)
        for raw_action in actions:
            if not isinstance(raw_action, dict):
                continue

            if raw_action.get("tool"):
                tool_name = raw_action["tool"]
                tool_args = dict(raw_action.get("args", {}))
                observation = await self.tools.execute(tool_name, tool_args)
                ctx.record_step(
                    screen_state=None,
                    action_type=f"recovery.{tool_name}",
                    action_detail=self._safe_json(tool_args),
                    action_result=observation.output or observation.error or "",
                    channel=observation.channel,
                    observation=observation,
                )
                continue

            action_payload = dict(raw_action)
            action_payload.setdefault("type", "wait")
            detail = self._safe_json(action_payload)
            action = Action(**action_payload)
            result = await self.action.execute(action)
            observation = ToolObservation(
                tool_name=f"action.{action.type}",
                success=result.success,
                stdout=str(result.output or ""),
                stderr=str(result.error or ""),
                output=str(result.output or result.error or ""),
                parsed={"recovery_reason": reason},
                channel="hid",
                error=result.error or None,
            )
            ctx.record_step(
                screen_state=None,
                action_type=f"recovery.{action.type}",
                action_detail=detail,
                action_result=observation.output or observation.error or "",
                channel=observation.channel,
                observation=observation,
            )

            if not result.success:
                logger.warning("Recovery action failed: %s", detail)
                return False

        return True

    def _recovery_actions(self, recovery_cfg) -> list[dict]:
        raw = recovery_cfg
        if isinstance(recovery_cfg, dict):
            raw = recovery_cfg.get("on_stuck")

        if isinstance(raw, list):
            return [item for item in raw if isinstance(item, dict)]
        if isinstance(raw, dict):
            return [raw]
        if isinstance(raw, str):
            lowered = raw.lower()
            actions: list[dict] = []
            if "ctrl+c" in lowered:
                actions.append({"type": "key_press", "key": "c", "modifiers": ["Ctrl"]})
            if "ctrl+alt+t" in lowered or "ctrl-alt-t" in lowered:
                actions.append({"type": "key_press", "key": "t", "modifiers": ["Ctrl", "Alt"]})
            if "escape" in lowered or "esc" in lowered:
                actions.append({"type": "key_press", "key": "Escape"})
            if "enter" in lowered:
                actions.append({"type": "key_press", "key": "Enter"})
            if not actions:
                actions = [
                    {"type": "key_press", "key": "Escape"},
                    {"type": "wait", "duration": 1.0},
                ]
            return actions
        return []

    def _iter_observation_values(self, value):
        if isinstance(value, dict):
            for item in value.values():
                yield from self._iter_observation_values(item)
            return
        if isinstance(value, list):
            for item in value:
                yield from self._iter_observation_values(item)
            return
        yield value

    # ── WebSocket Notifications ─────────────────────────────────

    def add_ws_callback(self, callback: Callable):
        self._ws_callbacks.append(callback)

    def remove_ws_callback(self, callback: Callable):
        if callback in self._ws_callbacks:
            self._ws_callbacks.remove(callback)

    async def _notify_ws(self, data: dict):
        """Notify all WebSocket listeners."""
        for cb in self._ws_callbacks:
            try:
                if asyncio.iscoroutinefunction(cb):
                    await cb(data)
                else:
                    cb(data)
            except Exception as e:
                logger.debug(f"WS callback error: {e}")
