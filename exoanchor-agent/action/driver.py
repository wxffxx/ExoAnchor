"""
Action Driver — Multi-channel operation dispatcher.

Routes actions to the optimal channel:
  - HID: always available (keyboard, mouse via USB)
  - SSH: after OS boots (fast command execution)
  - Fallback: SSH commands degrade to HID typing
"""

import asyncio
import logging
from typing import Optional

from .adapters import HIDAdapterInterface, GPIOAdapterInterface

logger = logging.getLogger("exoanchor.action")


class Action:
    """Represents a single operation to execute on the target machine."""

    def __init__(self, type: str, **kwargs):
        self.type = type
        for k, v in kwargs.items():
            setattr(self, k, v)

    def __repr__(self):
        attrs = {k: v for k, v in self.__dict__.items() if k != "type"}
        return f"Action({self.type}, {attrs})"


class ActionResult:
    """Result of an executed action."""

    def __init__(self, success: bool = True, output: str = "", error: str = "", note: str = ""):
        self.success = success
        self.output = output
        self.error = error
        self.note = note

    def __repr__(self):
        if self.success:
            return f"ActionResult(ok, output='{self.output[:50]}')"
        return f"ActionResult(FAIL, error='{self.error}')"


class ActionDriver:
    """
    Multi-channel action dispatcher.

    Routes operations to the best available channel:
      - HID actions always go through the HID adapter
      - Shell commands use SSH if available, otherwise fall back to HID typing
      - Power actions go through GPIO adapter
    """

    def __init__(
        self,
        hid_adapter: HIDAdapterInterface,
        gpio_adapter: GPIOAdapterInterface,
        ssh_manager=None,
    ):
        self.hid = hid_adapter
        self.gpio = gpio_adapter
        self.ssh = ssh_manager  # Set later via set_ssh_manager()

    def set_ssh_manager(self, ssh_manager):
        """Inject SSH manager (avoids circular dependency on init)."""
        self.ssh = ssh_manager

    @property
    def has_shell(self) -> bool:
        """Whether a shell channel (SSH) is available."""
        return self.ssh is not None and self.ssh.has_shell

    async def execute(self, action: Action) -> ActionResult:
        """Execute an action, routing to the optimal channel."""
        try:
            handler = self._get_handler(action.type)
            if handler is None:
                return ActionResult(success=False, error=f"Unknown action type: {action.type}")
            return await handler(action)
        except Exception as e:
            logger.error(f"Action execution failed: {action} — {e}")
            return ActionResult(success=False, error=str(e))

    def _get_handler(self, action_type: str):
        """Get the handler method for an action type."""
        handlers = {
            # HID
            "key_press": self._handle_key_press,
            "key_sequence": self._handle_key_sequence,
            "type_text": self._handle_type_text,
            "mouse_move": self._handle_mouse_move,
            "mouse_click": self._handle_mouse_click,
            "release_all": self._handle_release_all,
            # Power
            "power": self._handle_power,
            # Shell (auto-channel)
            "shell": self._handle_shell,
            # File transfer
            "upload": self._handle_upload,
            "download": self._handle_download,
            # Waiting
            "wait": self._handle_wait,
        }
        return handlers.get(action_type)

    # ── HID Handlers ────────────────────────────────────────────

    async def _handle_key_press(self, action: Action) -> ActionResult:
        modifiers = getattr(action, "modifiers", None)
        await self.hid.send_key(action.key, modifiers)
        return ActionResult(success=True)

    async def _handle_key_sequence(self, action: Action) -> ActionResult:
        interval = getattr(action, "interval", 0.05)
        for key in action.keys:
            await self.hid.send_key(key)
            await asyncio.sleep(interval)
        return ActionResult(success=True)

    async def _handle_type_text(self, action: Action) -> ActionResult:
        interval = getattr(action, "interval", 0.05)
        await self.hid.type_string(action.text, interval=interval)
        return ActionResult(success=True)

    async def _handle_mouse_move(self, action: Action) -> ActionResult:
        absolute = getattr(action, "absolute", False)
        await self.hid.move_mouse(action.x, action.y, absolute=absolute)
        return ActionResult(success=True)

    async def _handle_mouse_click(self, action: Action) -> ActionResult:
        button = getattr(action, "button", "left")
        x = getattr(action, "x", None)
        y = getattr(action, "y", None)
        await self.hid.click(button, x, y)
        return ActionResult(success=True)

    async def _handle_release_all(self, action: Action) -> ActionResult:
        await self.hid.release_all()
        return ActionResult(success=True)

    # ── Power Handler ───────────────────────────────────────────

    async def _handle_power(self, action: Action) -> ActionResult:
        result = await self.gpio.power_action(action.power_action)
        return ActionResult(success=result.get("ok", True), output=str(result))

    # ── Shell Handler (SSH with HID fallback) ───────────────────

    async def _handle_shell(self, action: Action) -> ActionResult:
        command = action.command
        timeout = getattr(action, "timeout", 30)

        if self.has_shell:
            # Fast path: SSH
            try:
                output = await self.ssh.run(command, timeout=timeout)
                return ActionResult(success=True, output=output)
            except Exception as e:
                logger.warning(f"SSH command failed, falling back to HID: {e}")

        # Fallback: type command via HID
        await self.hid.type_string(command)
        await self.hid.send_key("Enter")
        wait_time = getattr(action, "wait", 2)
        await asyncio.sleep(wait_time)
        return ActionResult(success=True, note="executed_via_hid")

    # ── File Transfer Handlers ──────────────────────────────────

    async def _handle_upload(self, action: Action) -> ActionResult:
        if not self.has_shell:
            return ActionResult(success=False, error="Upload requires SSH connection")
        await self.ssh.upload(action.local_path, action.remote_path)
        return ActionResult(success=True)

    async def _handle_download(self, action: Action) -> ActionResult:
        if not self.has_shell:
            return ActionResult(success=False, error="Download requires SSH connection")
        await self.ssh.download(action.remote_path, action.local_path)
        return ActionResult(success=True)

    # ── Wait Handler ────────────────────────────────────────────

    async def _handle_wait(self, action: Action) -> ActionResult:
        duration = getattr(action, "duration", 1.0)
        await asyncio.sleep(duration)
        return ActionResult(success=True)
