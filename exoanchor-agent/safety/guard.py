"""
Safety Guard — Prevents the agent from damaging the target machine.

Checks:
  - Step count limits
  - Execution duration limits
  - Screen error detection (BSOD, kernel panic)
  - Loop detection
  - Human confirmation for dangerous operations
"""

import logging
from typing import Optional

from ..core.models import ScreenState

logger = logging.getLogger("exoanchor.safety")


class SafetyGuard:
    """
    Safety mechanism that runs before each agent action.
    Returns abort=True if execution should stop.
    """

    def __init__(self, config: dict):
        self.max_steps = config.get("max_steps", 200)
        self.max_duration = config.get("max_duration", 600)
        self.require_confirmation = config.get("require_confirmation", True)
        self.auto_abort_on_bsod = config.get("auto_abort_on_bsod", True)
        self._confirmation_callback = None

    def set_confirmation_callback(self, callback):
        """Set a callback for requesting human confirmation."""
        self._confirmation_callback = callback

    async def check(self, screen: ScreenState, context) -> dict:
        """
        Run all safety checks.
        Returns {"abort": bool, "reason": str}
        """
        # Step limit
        if context.current_step >= self.max_steps:
            return {"abort": True, "reason": f"Max steps exceeded ({self.max_steps})"}

        # Duration limit
        if context.elapsed > self.max_duration:
            return {"abort": True, "reason": f"Timeout ({self.max_duration}s)"}

        # Screen error (BSOD / kernel panic)
        if self.auto_abort_on_bsod and screen.type == "error":
            return {"abort": True, "reason": f"Screen error: {screen.error or screen.description}"}

        # Loop detection
        if context.detect_loop(window=5):
            return {"abort": True, "reason": "Action loop detected"}

        return {"abort": False, "reason": ""}

    async def request_confirmation(self, action_description: str) -> bool:
        """Request human confirmation for a dangerous action."""
        if not self.require_confirmation:
            return True

        if self._confirmation_callback:
            return await self._confirmation_callback(action_description)

        # Default: allow (if no callback is set, assume running in auto mode)
        logger.warning(f"No confirmation callback set, auto-allowing: {action_description}")
        return True
