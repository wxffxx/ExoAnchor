"""
Skill — Data model for loadable operation playbooks.

Skills can be:
  - YAML files (declarative, for simple/scripted flows)
  - Python classes (for complex logic with loops, conditionals, data processing)
"""

import os
import logging
from typing import Optional, Any
from abc import ABC, abstractmethod

import yaml

logger = logging.getLogger("exoanchor.skills")


def validate_param_spec(params_spec: dict, user_params: dict) -> dict:
    """Validate and merge user-provided params with declared defaults."""
    merged = {}
    for key, spec in (params_spec or {}).items():
        if key in user_params:
            value = user_params[key]
            param_type = spec.get("type", "str")
            try:
                if param_type == "int":
                    value = int(value)
                elif param_type == "float":
                    value = float(value)
                elif param_type == "bool":
                    if isinstance(value, str):
                        value = value.lower() in ("1", "true", "yes", "on")
                    else:
                        value = bool(value)
            except (ValueError, TypeError):
                raise ValueError(f"Invalid type for param '{key}': expected {param_type}")

            if "range" in spec and spec["range"] is not None:
                lo, hi = spec["range"]
                if not (lo <= value <= hi):
                    raise ValueError(f"Param '{key}' out of range [{lo}, {hi}]: {value}")

            merged[key] = value
        elif spec.get("required", False):
            raise ValueError(f"Required param '{key}' not provided")
        else:
            merged[key] = spec.get("default")
    return merged


class Skill:
    """
    A loaded skill ready for execution.
    Wraps the parsed YAML data with convenience accessors.
    """

    def __init__(self, data: dict, source_path: str = ""):
        self._data = data.get("skill", data)
        self.source_path = source_path

    @property
    def name(self) -> str:
        return self._data.get("name", "unnamed")

    @property
    def description(self) -> str:
        return self._data.get("description", "")

    @property
    def mode(self) -> str:
        return self._data.get("mode", "guided")

    @property
    def tags(self) -> list[str]:
        return self._data.get("tags", [])

    @property
    def params(self) -> dict:
        return self._data.get("params", {})

    @property
    def steps(self) -> list[dict]:
        return self._data.get("steps", [])

    @property
    def goal(self) -> str:
        return self._data.get("goal", "")

    @property
    def checkpoints(self) -> list[dict]:
        return self._data.get("checkpoints", [])

    @property
    def safety(self) -> dict:
        return self._data.get("safety", {})

    @property
    def recovery(self) -> dict:
        return self._data.get("recovery", {})

    @property
    def is_builtin(self) -> bool:
        return self._data.get("builtin", False)

    def get(self, key: str, default=None):
        return self._data.get(key, default)

    def validate_params(self, user_params: dict) -> dict:
        """Validate and merge user-provided params with defaults."""
        return validate_param_spec(self.params, user_params)

    def to_dict(self) -> dict:
        """Serialize to dict (for API responses)."""
        return {
            "name": self.name,
            "description": self.description,
            "mode": self.mode,
            "tags": self.tags,
            "params": self.params,
            "builtin": self.is_builtin,
            "source": self.source_path,
        }

    @classmethod
    def from_yaml_file(cls, path: str) -> "Skill":
        """Load a Skill from a YAML file."""
        with open(path, "r") as f:
            data = yaml.safe_load(f)
        return cls(data, source_path=path)

    @classmethod
    def from_yaml_string(cls, content: str) -> "Skill":
        """Load a Skill from a YAML string."""
        data = yaml.safe_load(content)
        return cls(data)

    def __repr__(self):
        return f"Skill({self.name}, mode={self.mode})"


class SkillBase(ABC):
    """
    Base class for Python-based skills.

    Subclass this for complex skills that need loops, conditionals,
    or data processing beyond what YAML can express.
    """

    name: str = "unnamed"
    description: str = ""
    tags: list[str] = []
    params: dict[str, dict] = {}

    @abstractmethod
    async def execute(self, ctx) -> dict:
        """
        Execute the skill.
        
        Args:
            ctx: ExecutionContext with action, vision, ssh access
            
        Returns:
            Result dict
        """
        ...

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "description": self.description,
            "mode": "python",
            "tags": self.tags,
            "params": {},
            "builtin": False,
            "source": "python",
        }


def param(type_: type, default=None, description: str = "",
          choices: list = None, range: tuple = None):
    """Descriptor for declaring skill parameters."""
    return {
        "type": type_.__name__,
        "default": default,
        "description": description,
        "choices": choices,
        "range": list(range) if range else None,
    }


class PythonSkillAdapter:
    """Wrap a Python SkillBase instance to look like a normal loaded skill."""

    def __init__(self, skill_obj: SkillBase, source_path: str = "", builtin: bool = False):
        self.skill_obj = skill_obj
        self.source_path = source_path
        self._builtin = builtin

    @property
    def name(self) -> str:
        return getattr(self.skill_obj, "name", "unnamed")

    @property
    def description(self) -> str:
        return getattr(self.skill_obj, "description", "")

    @property
    def mode(self) -> str:
        return "python"

    @property
    def tags(self) -> list[str]:
        return list(getattr(self.skill_obj, "tags", []) or [])

    @property
    def params(self) -> dict:
        return dict(getattr(self.skill_obj, "params", {}) or {})

    @property
    def is_builtin(self) -> bool:
        return self._builtin

    def get(self, key: str, default=None):
        mapping = {
            "name": self.name,
            "description": self.description,
            "mode": self.mode,
            "tags": self.tags,
            "params": self.params,
            "builtin": self.is_builtin,
        }
        return mapping.get(key, default)

    def validate_params(self, user_params: dict) -> dict:
        return validate_param_spec(self.params, user_params or {})

    async def execute(self, ctx):
        return await self.skill_obj.execute(ctx)

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "description": self.description,
            "mode": self.mode,
            "tags": self.tags,
            "params": self.params,
            "builtin": self.is_builtin,
            "source": self.source_path,
        }
