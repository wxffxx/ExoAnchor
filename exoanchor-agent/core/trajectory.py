"""
Trajectory export helpers for plan runs.

The exported schema is intentionally plain JSON so it can feed benchmarks,
postmortems, imitation learning, and offline RL pipelines without importing
ExoAnchor internals.
"""

from __future__ import annotations

import re
from typing import Any

from .models import PlanRunStatus


TRAJECTORY_SCHEMA_VERSION = "exoanchor.trajectory.v1"

SECRET_KEY_PARTS = (
    "password",
    "api_key",
    "token",
    "secret",
    "authorization",
    "bearer",
)

SECRET_PATTERNS = [
    re.compile(r"\bsk-[A-Za-z0-9][A-Za-z0-9_-]{12,}\b"),
    re.compile(r"(?i)\bBearer\s+[A-Za-z0-9._~+/=-]+"),
    re.compile(r"(?i)\b(api[_-]?key|token|password|secret)=([^\s&;]+)"),
]


def plan_run_to_trajectory(run: PlanRunStatus, *, include_sensitive: bool = False) -> dict[str, Any]:
    """Convert a plan run snapshot into a benchmark/research trajectory."""
    duration = None
    if run.started_at is not None and run.completed_at is not None:
        duration = max(0.0, run.completed_at - run.started_at)

    transitions = []
    for index, step in enumerate(run.steps):
        observation = step.observation.model_dump() if step.observation is not None else None
        step_duration = None
        if step.started_at is not None and step.finished_at is not None:
            step_duration = max(0.0, step.finished_at - step.started_at)

        transitions.append(_redact({
            "index": index,
            "step_id": step.id,
            "description": step.description,
            "action": {
                "tool": step.tool,
                "args": step.args,
                "command": step.command,
                "dangerous": step.dangerous,
            },
            "observation": observation,
            "policy_decision": {
                "action": step.policy_action,
                "risk_level": step.policy_risk_level,
                "reason": step.policy_reason,
                "matched_rules": step.policy_rules,
            },
            "outcome": {
                "status": step.status.value if hasattr(step.status, "value") else str(step.status),
                "success": step.error is None and str(step.status) in {"done", "PlanStepState.DONE"},
                "exit_status": step.exit_status,
                "output": step.output,
                "error": step.error,
                "eval_action": step.eval_action,
                "eval_reason": step.eval_reason,
            },
            "timing": {
                "started_at": step.started_at,
                "finished_at": step.finished_at,
                "duration": step_duration,
            },
            "channel": observation.get("channel") if isinstance(observation, dict) else "",
        }, include_sensitive=include_sensitive))

    return _redact({
        "schema_version": TRAJECTORY_SCHEMA_VERSION,
        "type": "plan_run_trajectory",
        "run": {
            "run_id": run.run_id,
            "goal": run.goal,
            "state": run.state.value if hasattr(run.state, "value") else str(run.state),
            "source": run.source,
            "model": run.model,
            "supervised": run.supervised,
            "react_mode": run.react_mode,
            "created_at": run.created_at,
            "started_at": run.started_at,
            "completed_at": run.completed_at,
            "duration": duration,
            "total_steps": run.total_steps,
            "completed_steps": run.completed_steps,
            "error": run.error,
        },
        "policy_context": (run.metadata or {}).get("policy_context", {}),
        "metadata": run.metadata,
        "summary": {
            "success": str(run.state) in {"completed", "RunState.COMPLETED"},
            "step_count": len(run.steps),
            "failed_steps": sum(1 for step in run.steps if step.error),
            "channels": sorted({
                str(step.observation.channel)
                for step in run.steps
                if step.observation is not None and step.observation.channel
            }),
            "risk_levels": sorted({
                str(step.policy_risk_level)
                for step in run.steps
                if step.policy_risk_level
            }),
            "policy_actions": sorted({
                str(step.policy_action)
                for step in run.steps
                if step.policy_action
            }),
        },
        "transitions": transitions,
    }, include_sensitive=include_sensitive)


def _redact(value: Any, *, include_sensitive: bool = False) -> Any:
    if include_sensitive:
        return value

    if isinstance(value, dict):
        redacted = {}
        for key, item in value.items():
            if _is_secret_key(str(key)):
                redacted[key] = "[REDACTED]"
            else:
                redacted[key] = _redact(item, include_sensitive=include_sensitive)
        return redacted

    if isinstance(value, list):
        return [_redact(item, include_sensitive=include_sensitive) for item in value]

    if isinstance(value, str):
        return _redact_string(value)

    return value


def _is_secret_key(key: str) -> bool:
    normalized = key.lower().replace("-", "_")
    return any(part in normalized for part in SECRET_KEY_PARTS)


def _redact_string(value: str) -> str:
    text = value
    for pattern in SECRET_PATTERNS:
        text = pattern.sub(lambda match: f"{match.group(1)}=[REDACTED]" if match.lastindex else "[REDACTED]", text)
    return text
