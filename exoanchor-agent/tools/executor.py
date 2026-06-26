"""
Structured tool execution layer shared by plan and skill runtimes.
"""

import json
import logging
import re
import shlex
from typing import Any, Optional

from ..action.driver import Action, ActionDriver, ActionResult
from ..core.models import ToolObservation

logger = logging.getLogger("exoanchor.tools")


class ToolExecutor:
    """Execute structured tools and return normalized observations."""

    TOOL_ALIASES = {
        "shell": "shell.exec",
        "ssh": "shell.exec",
        "ssh.exec": "shell.exec",
        "wait": "wait.sleep",
        "power": "power.exec",
        "key_press": "hid.key_press",
        "key_sequence": "hid.key_sequence",
        "type_text": "hid.type_text",
        "mouse_move": "hid.mouse_move",
        "mouse_click": "hid.mouse_click",
        "release_all": "hid.release_all",
        "upload": "ssh.upload",
        "download": "ssh.download",
    }

    ACTION_TOOLS = {
        "hid.key_press": "key_press",
        "hid.key_sequence": "key_sequence",
        "hid.type_text": "type_text",
        "hid.mouse_move": "mouse_move",
        "hid.mouse_click": "mouse_click",
        "hid.release_all": "release_all",
        "ssh.upload": "upload",
        "ssh.download": "download",
        "wait.sleep": "wait",
        "power.exec": "power",
    }

    def __init__(self, action_driver: Optional[ActionDriver], ssh_manager=None, vision_backend=None):
        self.action = action_driver
        self.ssh = ssh_manager or getattr(action_driver, "ssh", None)
        self.vision = vision_backend

    def _rewrite_noninteractive_sudo(self, command: str) -> str:
        """Inject sudo -S when we have a known SSH password and are running non-interactively."""
        cmd = str(command or "").strip()
        password = str(getattr(self.ssh, "password", "") or "").strip()
        if not cmd or not password:
            return cmd

        safe_password = shlex.quote(password)

        def repl(match: re.Match[str]) -> str:
            matched = match.group(0)
            if re.search(r"\bsudo\s+-S\b", matched):
                return matched
            return matched.replace("sudo", f"printf '%s\\n' {safe_password} | sudo -S", 1)

        return re.sub(r"\bsudo\b(?!\s+-S\b)", repl, cmd)

    @classmethod
    def normalize_tool_name(cls, tool_name: str) -> str:
        raw = str(tool_name or "shell.exec").strip()
        if not raw:
            return "shell.exec"
        return cls.TOOL_ALIASES.get(raw, raw)

    @classmethod
    def describe_tool_call(cls, tool_name: str, args: Optional[dict[str, Any]] = None) -> str:
        tool = cls.normalize_tool_name(tool_name)
        params = dict(args or {})

        if tool == "shell.exec":
            return str(params.get("command", "")).strip()

        if tool == "systemd.status":
            unit = params.get("unit") or params.get("service") or ""
            return f"systemd.status {unit}".strip()

        if tool == "systemd.restart":
            unit = params.get("unit") or params.get("service") or ""
            prefix = "sudo " if params.get("sudo", True) else ""
            return f"{prefix}systemctl restart {unit}".strip()

        if tool == "docker.ps":
            return "docker ps"

        if tool in cls.ACTION_TOOLS:
            try:
                rendered = json.dumps(params, ensure_ascii=False, sort_keys=True)
            except TypeError:
                rendered = str(params)
            return f"{tool} {rendered}".strip()

        try:
            rendered = json.dumps(params, ensure_ascii=False, sort_keys=True)
        except TypeError:
            rendered = str(params)
        return f"{tool} {rendered}".strip()

    async def execute(self, tool_name: str, args: Optional[dict[str, Any]] = None) -> ToolObservation:
        tool = self.normalize_tool_name(tool_name)
        params = dict(args or {})

        if tool == "shell.exec":
            return await self._run_shell(tool, params)
        if tool == "systemd.status":
            return await self._run_systemd_status(params)
        if tool == "systemd.restart":
            return await self._run_systemd_restart(params)
        if tool == "docker.ps":
            return await self._run_docker_ps(params)
        if tool.startswith("vision."):
            return await self._run_vision_tool(tool, params)
        if tool in self.ACTION_TOOLS:
            return await self._run_action_tool(tool, params)
        if tool.startswith("action."):
            return await self._run_action_type(tool.split(".", 1)[1], params, tool_name=tool)

        return ToolObservation(
            tool_name=tool,
            success=False,
            output=f"Unknown tool: {tool}",
            stderr=f"Unknown tool: {tool}",
            error=f"Unknown tool: {tool}",
            parsed={"error_type": "unknown_tool"},
        )

    async def _run_shell(self, tool_name: str, args: dict[str, Any]) -> ToolObservation:
        command = str(args.get("command", "")).strip()
        timeout = int(args.get("timeout", 300) or 300)
        if not command:
            return ToolObservation(
                tool_name=tool_name,
                success=False,
                output="Missing shell command",
                stderr="Missing shell command",
                error="Missing shell command",
                parsed={"error_type": "invalid_args"},
            )

        effective_command = self._rewrite_noninteractive_sudo(command)

        if self.ssh and self.ssh.has_shell:
            result = await self.ssh.run_with_status(effective_command, timeout=timeout)
            stdout = str(result.get("stdout") or "")
            stderr = str(result.get("stderr") or "")
            output = str(result.get("output") or stdout or stderr or "")
            parsed = {
                "command": effective_command,
                "original_command": command,
                "timeout": timeout,
            }
            error_type = self._detect_error_type(stderr or output)
            if error_type:
                parsed["error_type"] = error_type
            return ToolObservation(
                tool_name=tool_name,
                success=bool(result.get("success", False)),
                exit_status=result.get("exit_status"),
                stdout=stdout,
                stderr=stderr,
                output=output,
                parsed=parsed,
                channel="ssh",
                error=stderr.strip() or None if not result.get("success", False) else None,
            )

        fallback_args = dict(args)
        fallback_args["command"] = effective_command
        result = await self.action.execute(Action(type="shell", **fallback_args))
        parsed = {
            "command": effective_command,
            "original_command": command,
            "timeout": timeout,
            "fallback": "hid",
        }
        if result.note:
            parsed["note"] = result.note
        return ToolObservation(
            tool_name=tool_name,
            success=result.success,
            stdout=str(result.output or ""),
            stderr=str(result.error or ""),
            output=str(result.output or result.error or ""),
            parsed=parsed,
            channel="hid",
            error=result.error or None,
        )

    async def _run_systemd_status(self, args: dict[str, Any]) -> ToolObservation:
        unit = str(args.get("unit") or args.get("service") or "").strip()
        timeout = int(args.get("timeout", 30) or 30)
        if not unit:
            return ToolObservation(
                tool_name="systemd.status",
                success=False,
                output="Missing systemd unit name",
                stderr="Missing systemd unit name",
                error="Missing systemd unit name",
                parsed={"error_type": "invalid_args"},
            )

        command = (
            "systemctl show "
            f"{shlex.quote(unit)} "
            "--no-pager "
            "--property=Id,LoadState,ActiveState,SubState,UnitFileState,Description,FragmentPath"
        )
        shell_obs = await self._run_shell("shell.exec", {"command": command, "timeout": timeout})
        parsed = dict(shell_obs.parsed)
        parsed.update(self._parse_key_value_output(shell_obs.stdout or shell_obs.output))
        parsed["unit"] = unit
        parsed["is_active"] = parsed.get("ActiveState") == "active"
        error_type = self._detect_error_type(shell_obs.stderr or shell_obs.output)
        if error_type:
            parsed["error_type"] = error_type

        return ToolObservation(
            tool_name="systemd.status",
            success=shell_obs.success,
            exit_status=shell_obs.exit_status,
            stdout=shell_obs.stdout,
            stderr=shell_obs.stderr,
            output=shell_obs.output,
            parsed=parsed,
            artifacts=shell_obs.artifacts,
            channel=shell_obs.channel,
            error=shell_obs.error,
        )

    async def _run_systemd_restart(self, args: dict[str, Any]) -> ToolObservation:
        unit = str(args.get("unit") or args.get("service") or "").strip()
        timeout = int(args.get("timeout", 60) or 60)
        use_sudo = bool(args.get("sudo", True))
        if not unit:
            return ToolObservation(
                tool_name="systemd.restart",
                success=False,
                output="Missing systemd unit name",
                stderr="Missing systemd unit name",
                error="Missing systemd unit name",
                parsed={"error_type": "invalid_args"},
            )

        prefix = "sudo " if use_sudo else ""
        command = f"{prefix}systemctl restart {shlex.quote(unit)}"
        restart_obs = await self._run_shell("shell.exec", {"command": command, "timeout": timeout})
        status_obs = None
        if restart_obs.success:
            status_obs = await self._run_systemd_status({"unit": unit, "timeout": timeout})

        parsed = dict(restart_obs.parsed)
        parsed.update({
            "unit": unit,
            "sudo": use_sudo,
            "restart_requested": True,
        })
        artifacts: dict[str, Any] = {}
        if status_obs is not None:
            parsed["status"] = status_obs.parsed
            artifacts["status_observation"] = status_obs.model_dump()

        output = restart_obs.output
        if not output and status_obs is not None:
            output = status_obs.output

        return ToolObservation(
            tool_name="systemd.restart",
            success=restart_obs.success,
            exit_status=restart_obs.exit_status,
            stdout=restart_obs.stdout,
            stderr=restart_obs.stderr,
            output=output,
            parsed=parsed,
            artifacts=artifacts,
            channel=restart_obs.channel,
            error=restart_obs.error,
        )

    async def _run_docker_ps(self, args: dict[str, Any]) -> ToolObservation:
        timeout = int(args.get("timeout", 30) or 30)
        command = "docker ps --format '{{json .}}'"
        shell_obs = await self._run_shell("shell.exec", {"command": command, "timeout": timeout})
        containers = []
        for line in (shell_obs.stdout or "").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                containers.append(json.loads(line))
            except json.JSONDecodeError:
                logger.debug(f"Skipping non-JSON docker ps line: {line[:120]}")
        parsed = dict(shell_obs.parsed)
        parsed["containers"] = containers
        parsed["count"] = len(containers)
        return ToolObservation(
            tool_name="docker.ps",
            success=shell_obs.success,
            exit_status=shell_obs.exit_status,
            stdout=shell_obs.stdout,
            stderr=shell_obs.stderr,
            output=shell_obs.output,
            parsed=parsed,
            artifacts=shell_obs.artifacts,
            channel=shell_obs.channel,
            error=shell_obs.error,
        )

    async def _run_vision_tool(self, tool_name: str, args: dict[str, Any]) -> ToolObservation:
        if self.vision is None:
            return ToolObservation(
                tool_name=tool_name,
                success=False,
                output="Vision backend not configured",
                stderr="Vision backend not configured",
                error="Vision backend not configured",
                parsed={"error_type": "vision_unavailable"},
            )

        if tool_name == "vision.analyze":
            frame = await self.vision.capture()
            analysis = await self.vision.analyze(frame, args.get("context"), args.get("goal", ""), args.get("checkpoints", []))
            return ToolObservation(
                tool_name=tool_name,
                success=True,
                output=getattr(analysis, "description", "") or "",
                parsed={
                    "screen_state": getattr(analysis, "type", "unknown"),
                    "elements": list(getattr(analysis, "elements", []) or []),
                },
                artifacts={"screen_state": analysis.model_dump() if hasattr(analysis, "model_dump") else {}},
                channel="vision",
            )

        return ToolObservation(
            tool_name=tool_name,
            success=False,
            output=f"Unsupported vision tool: {tool_name}",
            stderr=f"Unsupported vision tool: {tool_name}",
            error=f"Unsupported vision tool: {tool_name}",
            parsed={"error_type": "unknown_tool"},
        )

    async def _run_action_tool(self, tool_name: str, args: dict[str, Any]) -> ToolObservation:
        action_type = self.ACTION_TOOLS[tool_name]
        params = dict(args)
        if tool_name == "wait.sleep":
            duration = params.pop("duration", params.pop("seconds", 1.0))
            params["duration"] = duration
        if tool_name == "power.exec" and "action" in params and "power_action" not in params:
            params["power_action"] = params.pop("action")
        return await self._run_action_type(action_type, params, tool_name=tool_name)

    async def _run_action_type(
        self,
        action_type: str,
        args: dict[str, Any],
        *,
        tool_name: Optional[str] = None,
    ) -> ToolObservation:
        if self.action is None:
            return ToolObservation(
                tool_name=tool_name or f"action.{action_type}",
                success=False,
                output="Action driver not configured",
                stderr="Action driver not configured",
                error="Action driver not configured",
                parsed={"error_type": "action_unavailable"},
            )
        result = await self.action.execute(Action(type=action_type, **args))
        channel = self._channel_for_action(action_type, result)
        parsed: dict[str, Any] = {}
        if result.note:
            parsed["note"] = result.note
        return ToolObservation(
            tool_name=tool_name or f"action.{action_type}",
            success=result.success,
            stdout=str(result.output or ""),
            stderr=str(result.error or ""),
            output=str(result.output or result.error or ""),
            parsed=parsed,
            channel=channel,
            error=result.error or None,
        )

    def _channel_for_action(self, action_type: str, result: ActionResult) -> str:
        if action_type in ("key_press", "key_sequence", "type_text", "mouse_move", "mouse_click", "release_all"):
            return "hid"
        if action_type in ("upload", "download"):
            return "ssh"
        if action_type == "power":
            return "gpio"
        if action_type == "wait":
            return "local"
        if action_type == "shell":
            return "hid" if result.note == "executed_via_hid" else "ssh"
        return ""

    def _parse_key_value_output(self, text: str) -> dict[str, Any]:
        parsed: dict[str, Any] = {}
        for line in str(text or "").splitlines():
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            parsed[key.strip()] = value.strip()
        return parsed

    def _detect_error_type(self, text: str) -> Optional[str]:
        lower = str(text or "").lower()
        if not lower:
            return None
        if "permission denied" in lower:
            return "permission_denied"
        if "unit " in lower and "could not be found" in lower:
            return "unit_not_found"
        if "not-found" in lower and "loadstate" in lower:
            return "unit_not_found"
        if "command not found" in lower:
            return "command_not_found"
        if "no such file" in lower or "cannot access" in lower:
            return "file_not_found"
        if "address already in use" in lower or "port is already allocated" in lower:
            return "port_in_use"
        if "timed out" in lower or "timeout" in lower:
            return "timeout"
        return None
