"""
Execution Context — Tracks state during task execution.
"""

import time
import logging
from typing import Optional
from .models import Task, TaskState, StepRecord, ScreenState, ToolObservation

logger = logging.getLogger("exoanchor.context")


class ExecutionContext:
    """
    Tracks all state during the execution of a single task.

    Provides:
      - Step history recording
      - Progress tracking
      - Loop detection
      - Checkpoint management
      - Convenience accessors for the executor
    """

    def __init__(self, task: Task):
        self.task = task
        self.history: list[StepRecord] = []
        self.progress: float = 0.0
        self.current_step: int = 0
        self.current_checkpoint: Optional[str] = None
        self.checkpoints_reached: set[str] = set()
        self._start_time: float = time.time()
        self._is_complete: bool = False
        self._is_paused: bool = False
        self.action = None
        self.vision = None
        self.ssh = None
        self.executor = None
        self.skill = None
        self.tools = None

    @property
    def is_complete(self) -> bool:
        return self._is_complete

    @property
    def is_paused(self) -> bool:
        return self._is_paused

    @property
    def elapsed(self) -> float:
        """Seconds since execution started."""
        return time.time() - self._start_time

    def mark_complete(self, result: dict = None):
        """Mark task as completed."""
        self._is_complete = True
        self.progress = 1.0
        self.task.state = TaskState.COMPLETED
        self.task.completed_at = time.time()
        self.task.result = result

    def mark_failed(self, error: str):
        """Mark task as failed."""
        self._is_complete = True
        self.task.state = TaskState.FAILED
        self.task.completed_at = time.time()
        self.task.error = error

    def mark_aborted(self):
        """Mark task as aborted by user."""
        self._is_complete = True
        self.task.state = TaskState.ABORTED
        self.task.completed_at = time.time()
        self.task.error = "Aborted by user"

    def pause(self):
        self._is_paused = True
        self.task.state = TaskState.PAUSED

    def resume(self):
        self._is_paused = False
        self.task.state = TaskState.RUNNING

    def record_step(
        self,
        screen_state: Optional[ScreenState],
        action_type: str,
        action_detail: str,
        action_result: str,
        channel: str = "hid",
        observation: Optional[ToolObservation] = None,
        screenshot_path: str = None,
    ):
        """Record a step in execution history."""
        step = StepRecord(
            step_number=self.current_step,
            timestamp=time.time(),
            screen_state=screen_state,
            action_type=action_type,
            action_detail=action_detail,
            action_result=action_result or (observation.output if observation else ""),
            channel_used=channel or (observation.channel if observation else "hid"),
            observation=observation,
            screenshot_path=screenshot_path,
        )
        self.history.append(step)
        self.current_step += 1

    def update_progress(self, progress: float):
        """Update progress (0.0 - 1.0)."""
        self.progress = max(0.0, min(1.0, progress))

    def reach_checkpoint(self, name: str):
        """Mark a checkpoint as reached."""
        self.current_checkpoint = name
        self.checkpoints_reached.add(name)
        logger.info(f"Checkpoint reached: {name}")

    def has_reached(self, checkpoint: str) -> bool:
        """Whether a checkpoint has been reached."""
        return checkpoint in self.checkpoints_reached

    def detect_loop(self, window: int = 5) -> bool:
        """
        Detect if the agent is stuck in a loop.
        Checks repeated actions and repeated screen signatures.
        """
        if len(self.history) < window:
            return False

        recent = self.history[-window:]
        action_types = [s.action_type for s in recent]
        action_details = [s.action_detail for s in recent]

        # All same action type AND same detail = loop
        if len(set(action_types)) == 1 and len(set(action_details)) == 1:
            logger.warning(f"Loop detected: {action_types[0]}({action_details[0]}) repeated {window} times")
            return True

        screen_signatures = [
            s.screen_state.signature()
            for s in recent
            if s.screen_state is not None
        ]
        if len(screen_signatures) == window and len(set(screen_signatures)) == 1 and len(set(action_types)) <= 2:
            logger.warning(
                "Loop detected: screen signature repeated %s times with actions=%s",
                window,
                sorted(set(action_types)),
            )
            return True

        return False

    def get_recent_history(self, n: int = 10) -> list[dict]:
        """Get recent history as serializable dicts."""
        return [
            {
                "step": s.step_number,
                "action": f"{s.action_type}: {s.action_detail}",
                "result": s.action_result,
                "channel": s.channel_used,
            }
            for s in self.history[-n:]
        ]

    def bind_runtime(self, action=None, vision=None, ssh=None, executor=None, skill=None, tools=None):
        """Attach runtime capabilities so Python skills can use ctx directly."""
        self.action = action
        self.vision = vision
        self.ssh = ssh
        self.executor = executor
        self.skill = skill
        self.tools = tools
        return self

    async def run_shell(self, command: str, timeout: int = 30) -> dict:
        """Convenience helper for Python skills to run a shell command."""
        if self.ssh is None:
            raise RuntimeError("SSH manager not bound to execution context")
        return await self.ssh.run_with_status(command, timeout=timeout)

    async def run_tool(self, tool_name: str, args: Optional[dict] = None) -> ToolObservation:
        """Convenience helper for Python skills to execute a structured tool."""
        if self.tools is None:
            raise RuntimeError("Tool executor not bound to execution context")
        return await self.tools.execute(tool_name, args or {})
