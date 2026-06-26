"""
Policy Engine — Backend enforcement for risky operations.
"""

import re
from enum import Enum
from typing import Any, Optional

from pydantic import BaseModel, Field

from .audit import AuditEvent, AuditLogStore


class RiskLevel(str, Enum):
    LOW = "low"
    MEDIUM = "medium"
    HIGH = "high"
    CRITICAL = "critical"


class PolicyAction(str, Enum):
    ALLOW = "allow"
    CONFIRM = "confirm"
    DENY = "deny"


class PolicyDecision(BaseModel):
    tool_name: str
    risk_level: RiskLevel = RiskLevel.LOW
    action: PolicyAction = PolicyAction.ALLOW
    reason: str = ""
    matched_rules: list[str] = Field(default_factory=list)
    command: str = ""
    args: dict[str, Any] = Field(default_factory=dict)

    @property
    def allowed(self) -> bool:
        return self.action == PolicyAction.ALLOW

    @property
    def requires_confirmation(self) -> bool:
        return self.action == PolicyAction.CONFIRM


class PolicyEngine:
    """Classify tool calls and decide whether they may execute."""

    DEFAULT_CRITICAL_PATTERNS = [
        (r"\brm\s+-rf\s+/(?:\s|$)", "rm-root"),
        (r"\bmkfs(?:\.\w+)?\b", "mkfs"),
        (r"\bdd\s+if=", "dd-write"),
        (r"\b(?:reboot|shutdown|poweroff|halt)\b", "power-cycle"),
        (r"\bsystemctl\s+(?:reboot|poweroff|halt)\b", "systemd-power"),
    ]

    DEFAULT_HIGH_PATTERNS = [
        (r"\brm\s+-rf\b", "rm-rf"),
        (r"\buserdel\b", "userdel"),
        (r"\bgroupdel\b", "groupdel"),
        (r"\bpasswd\b", "passwd"),
        (r"\bmount\b", "mount"),
        (r"\bumount\b", "umount"),
        (r"\biptables\b", "iptables"),
        (r"\bufw\b", "ufw"),
        (r"\bchmod\b", "chmod"),
        (r"\bchown\b", "chown"),
        (r"\bsed\s+-i\b", "sed-inplace"),
        (r">\s*/(?:etc|boot|usr|bin|sbin|lib|var/lib|root)/", "protected-write"),
        (r"\btee\s+/+(?:etc|boot|usr|bin|sbin|lib|var/lib|root)/", "protected-tee"),
        (r"\bcp\b.*\s/(?:etc|boot|usr|bin|sbin|lib|var/lib|root)/", "protected-copy"),
        (r"\bmv\b.*\s/(?:etc|boot|usr|bin|sbin|lib|var/lib|root)/", "protected-move"),
    ]

    DEFAULT_MEDIUM_PATTERNS = [
        (r"\bapt(?:-get)?\b", "package-manager"),
        (r"\byum\b", "package-manager"),
        (r"\bdnf\b", "package-manager"),
        (r"\bpip(?:3)?\s+install\b", "pip-install"),
        (r"\bnpm\s+(?:install|update)\b", "npm-install"),
        (r"\bsystemctl\s+(?:start|stop|restart|enable|disable)\b", "systemd-mutate"),
        (r"\bdocker\s+(?:restart|stop|start|rm)\b", "docker-mutate"),
        (r"\bmkdir\b", "mkdir"),
        (r"\btouch\b", "touch"),
        (r">\s*[^\s]+", "shell-redirection"),
    ]

    def __init__(self, config: dict, audit_log: Optional[AuditLogStore] = None):
        self.config = config or {}
        self.audit_log = audit_log
        self.audit_all = bool(self.config.get("audit_all", False))
        self.auto_sources = set(self.config.get("automated_sources", ["passive_trigger", "semi_active_task", "scheduled_task", "auto_plan"]))
        self.block_ssh_high_risk = bool(self.config.get("block_direct_ssh_high_risk", True))
        self.medium_tools = set(self.config.get("medium_tools", ["systemd.restart", "power.exec", "hid.type_text"]))
        self.high_tools = set(self.config.get("high_tools", ["ssh.upload", "action.power"]))

        self._critical_patterns = [(re.compile(p), name) for p, name in self.DEFAULT_CRITICAL_PATTERNS]
        self._high_patterns = [(re.compile(p), name) for p, name in self.DEFAULT_HIGH_PATTERNS]
        self._medium_patterns = [(re.compile(p), name) for p, name in self.DEFAULT_MEDIUM_PATTERNS]

    def evaluate_tool_call(
        self,
        tool_name: str,
        args: Optional[dict[str, Any]] = None,
        *,
        source_type: str = "manual_task",
        agent_mode: str = "manual",
        metadata: Optional[dict[str, Any]] = None,
    ) -> PolicyDecision:
        tool = str(tool_name or "").strip()
        params = dict(args or {})
        command = self._extract_command(tool, params)

        risk_level = RiskLevel.LOW
        matched_rules: list[str] = []

        if tool in self.medium_tools:
            risk_level, matched_rules = self._merge_risk(risk_level, RiskLevel.MEDIUM, matched_rules, [f"tool:{tool}"])
        if tool in self.high_tools:
            risk_level, matched_rules = self._merge_risk(risk_level, RiskLevel.HIGH, matched_rules, [f"tool:{tool}"])
        if tool == "power.exec":
            risk_level, matched_rules = self._merge_risk(risk_level, RiskLevel.CRITICAL, matched_rules, ["tool:power.exec"])
        if tool == "systemd.status" or tool == "docker.ps":
            risk_level, matched_rules = self._merge_risk(risk_level, RiskLevel.LOW, matched_rules, [f"tool:{tool}"])

        if tool == "shell.exec" and command:
            risk_level, matched_rules = self._classify_command(command, risk_level, matched_rules)

        action = PolicyAction.ALLOW
        reason = "Allowed by policy"

        if source_type in self.auto_sources:
            if risk_level in {RiskLevel.HIGH, RiskLevel.CRITICAL}:
                action = PolicyAction.DENY
                reason = "High-risk actions are blocked for automated tasks"
            elif risk_level == RiskLevel.MEDIUM and agent_mode == "passive" and tool == "power.exec":
                action = PolicyAction.DENY
                reason = "Power actions are blocked in passive automation"
        elif source_type in {"direct_ssh_exec", "direct_ssh_stream"}:
            if risk_level in {RiskLevel.HIGH, RiskLevel.CRITICAL} and self.block_ssh_high_risk:
                action = PolicyAction.DENY
                reason = "High-risk commands must run through a supervised plan"
        else:
            if risk_level in {RiskLevel.HIGH, RiskLevel.CRITICAL}:
                action = PolicyAction.CONFIRM
                reason = "High-risk action requires explicit confirmation"

        return PolicyDecision(
            tool_name=tool,
            risk_level=risk_level,
            action=action,
            reason=reason,
            matched_rules=matched_rules,
            command=command,
            args=params,
        )

    def audit(
        self,
        decision: PolicyDecision,
        *,
        source_type: str = "",
        agent_mode: str = "",
        metadata: Optional[dict[str, Any]] = None,
    ) -> None:
        if self.audit_log is None:
            return
        if not self.audit_all and decision.action == PolicyAction.ALLOW and decision.risk_level == RiskLevel.LOW:
            return

        self.audit_log.record(AuditEvent(
            source_type=source_type,
            agent_mode=agent_mode,
            tool_name=decision.tool_name,
            risk_level=decision.risk_level.value,
            action=decision.action.value,
            allowed=decision.allowed,
            requires_confirmation=decision.requires_confirmation,
            reason=decision.reason,
            matched_rules=decision.matched_rules,
            command=decision.command,
            args=decision.args,
            metadata=metadata or {},
        ))

    def summary(self) -> dict[str, Any]:
        return {
            "automated_sources": sorted(self.auto_sources),
            "direct_ssh_high_risk_blocked": self.block_ssh_high_risk,
            "audit_all": self.audit_all,
            "medium_tools": sorted(self.medium_tools),
            "high_tools": sorted(self.high_tools),
        }

    def _classify_command(
        self,
        command: str,
        risk_level: RiskLevel,
        matched_rules: list[str],
    ) -> tuple[RiskLevel, list[str]]:
        lowered = command.lower()
        for pattern, name in self._critical_patterns:
            if pattern.search(lowered):
                risk_level, matched_rules = self._merge_risk(risk_level, RiskLevel.CRITICAL, matched_rules, [name])

        for pattern, name in self._high_patterns:
            if pattern.search(lowered):
                risk_level, matched_rules = self._merge_risk(risk_level, RiskLevel.HIGH, matched_rules, [name])

        for pattern, name in self._medium_patterns:
            if pattern.search(lowered):
                risk_level, matched_rules = self._merge_risk(risk_level, RiskLevel.MEDIUM, matched_rules, [name])

        return risk_level, matched_rules

    def _extract_command(self, tool_name: str, args: dict[str, Any]) -> str:
        if tool_name == "shell.exec":
            return str(args.get("command") or "").strip()
        if tool_name == "systemd.restart":
            unit = args.get("unit") or args.get("service") or ""
            prefix = "sudo " if args.get("sudo", True) else ""
            return f"{prefix}systemctl restart {unit}".strip()
        return ""

    def _merge_risk(
        self,
        current: RiskLevel,
        candidate: RiskLevel,
        matched_rules: list[str],
        new_rules: list[str],
    ) -> tuple[RiskLevel, list[str]]:
        order = {
            RiskLevel.LOW: 0,
            RiskLevel.MEDIUM: 1,
            RiskLevel.HIGH: 2,
            RiskLevel.CRITICAL: 3,
        }
        level = candidate if order[candidate] > order[current] else current
        for rule in new_rules:
            if rule not in matched_rules:
                matched_rules.append(rule)
        return level, matched_rules
