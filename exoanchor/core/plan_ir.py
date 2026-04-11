"""
Plan IR — Unified internal representation for executable plans.
"""

from typing import Any
from typing import Optional

from pydantic import BaseModel, Field

from ..tools import ToolExecutor


class ExecutableStep(BaseModel):
    """Normalized executable step used by plan and scripted skill runtimes."""

    id: str
    description: str
    tool: str = "shell.exec"
    args: dict[str, Any] = Field(default_factory=dict)
    dangerous: bool = False
    expect: Optional[str] = None
    wait: float = 0.0
    retry: int = 1
    retry_delay: float = 1.0

    @property
    def command(self) -> str:
        return ToolExecutor.describe_tool_call(self.tool, self.args)


class ExecutablePlan(BaseModel):
    """Normalized plan IR shared by LLM plans and scripted YAML skills."""

    goal: str
    steps: list[ExecutableStep] = Field(default_factory=list)
    source: str = "unknown"
    metadata: dict[str, Any] = Field(default_factory=dict)


def _substitute_str(text: str, params: dict[str, Any]) -> str:
    result = text
    for key, value in (params or {}).items():
        result = result.replace(f"{{{key}}}", str(value))
    return result


def _substitute_obj(value: Any, params: dict[str, Any]) -> Any:
    if isinstance(value, str):
        return _substitute_str(value, params)
    if isinstance(value, dict):
        return {k: _substitute_obj(v, params) for k, v in value.items()}
    if isinstance(value, list):
        return [_substitute_obj(v, params) for v in value]
    return value


def plan_from_llm(goal: str, steps: list[dict]) -> ExecutablePlan:
    normalized = []
    for idx, step in enumerate(steps):
        tool = ToolExecutor.normalize_tool_name(step.get("tool") or "shell.exec")
        args = step.get("args") if isinstance(step.get("args"), dict) else {}
        raw_command = step.get("command")
        command = "" if raw_command in (None, "") else str(raw_command).strip()
        if command and "command" not in args:
            args["command"] = command
        if tool == "shell.exec" and not str(args.get("command", "")).strip():
            continue
        normalized.append(ExecutableStep(
            id=str(step.get("id", idx + 1)),
            description=str(step.get("description") or f"步骤 {idx + 1}").strip(),
            tool=tool,
            args=args,
            dangerous=bool(step.get("dangerous", False)),
        ))
    return ExecutablePlan(goal=goal or "执行计划", steps=normalized, source="llm")


def plan_from_scripted_skill(skill, params: dict[str, Any]) -> ExecutablePlan:
    steps = []
    for idx, raw_step in enumerate(skill.steps):
        step_def = _substitute_obj(raw_step, params)
        action = step_def.get("action", {}) if isinstance(step_def.get("action", {}), dict) else {}
        args = step_def.get("args") if isinstance(step_def.get("args"), dict) else {}
        if not args:
            args = dict(action)
        tool = ToolExecutor.normalize_tool_name(step_def.get("tool") or action.get("type") or "shell.exec")
        dangerous = bool(step_def.get("dangerous", False))
        command = str(args.get("command", "")).strip()
        if tool == "shell.exec" and not command:
            continue
        if not dangerous and tool == "shell.exec":
            dangerous = any(token in command for token in ("sudo ", "rm ", "chmod ", "chown ", "systemctl restart", "apt ", "yum "))

        steps.append(ExecutableStep(
            id=str(step_def.get("id", idx + 1)),
            description=str(step_def.get("description") or step_def.get("id") or f"步骤 {idx + 1}").strip(),
            tool=tool,
            args=args,
            dangerous=dangerous,
            expect=step_def.get("expect"),
            wait=float(step_def.get("wait", 0.0) or 0.0),
            retry=int(step_def.get("retry", 1) or 1),
            retry_delay=float(step_def.get("retry_delay", 1.0) or 1.0),
        ))

    return ExecutablePlan(
        goal=getattr(skill, "goal", "") or getattr(skill, "description", "") or getattr(skill, "name", "执行技能"),
        steps=steps,
        source=f"skill:{getattr(skill, 'name', 'unknown')}",
        metadata={"skill_name": getattr(skill, "name", "unknown")},
    )
