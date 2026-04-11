"""
Agent API Routes — FastAPI Router for the ExoAnchor Agent framework.

Provides REST + WebSocket endpoints that any Host can mount via:
    app.include_router(create_agent_router(...), prefix="/api/agent")
"""

import asyncio
import json
import logging
import os
import secrets
import shlex
import time
from typing import Any, Optional, Union

from fastapi import APIRouter, WebSocket, WebSocketDisconnect, HTTPException, UploadFile, File
from fastapi.responses import StreamingResponse
from pydantic import BaseModel, Field

from ..core.models import AgentMode, AgentStatus, TaskRequest
from ..core.passive import PassiveMonitor
from ..core.executor import SemiActiveExecutor
from ..core.plan_executor import PlanExecutor
from ..core.plan_ir import plan_from_llm, plan_from_scripted_skill
from ..core.run_store import RunStore
from ..channels.ssh import SSHChannelManager
from ..action.driver import ActionDriver
from ..action.adapters import HIDAdapterInterface, VideoAdapterInterface, GPIOAdapterInterface
from ..memory import ArtifactStore, FactStore, RunMemory
from ..tools import ToolExecutor
from ..vision.local_backend import LocalVisionBackend
from ..vision.api_backend import APIVisionBackend
from ..skills.store import SkillStore
from ..skills.recorder import SkillRecorder
from ..safety import AuditLogStore, PolicyAction, PolicyEngine, RiskLevel, SafetyGuard
from ..knowledge.store import KnowledgeStore
from ..runtime import EventHub, SessionRuntime, SessionState, SessionStore, build_snapshot_event
from ..runtime import (
    build_minecraft_console_probe_command,
    build_minecraft_console_setup_command,
    build_minecraft_rcon_exec_command,
    build_workload_logs_command,
    build_workload_start_command,
    build_workload_status_command,
    build_workload_stop_command,
    is_minecraft_workload,
)

logger = logging.getLogger("exoanchor.api")


class AgentInstance:
    """
    Central agent instance that wires everything together.
    Created by create_agent_router() and shared across all endpoints.
    """

    def __init__(
        self,
        hid_adapter: HIDAdapterInterface,
        video_adapter: VideoAdapterInterface,
        gpio_adapter: GPIOAdapterInterface,
        config: dict,
        step_evaluator=None,
    ):
        self.config = config
        self.mode = AgentMode(config.get("mode", "manual"))
        self.start_time = time.time()

        # SSH Channel
        target_cfg = config.get("target", {})
        ssh_cfg = target_cfg.get("ssh", {})
        ssh_cfg["ip"] = target_cfg.get("ip", "")
        self.ssh = SSHChannelManager(ssh_cfg)

        # Action Driver
        self.action_driver = ActionDriver(hid_adapter, gpio_adapter, self.ssh)

        # Vision
        vision_cfg = config.get("vision", {})
        self.local_vision = LocalVisionBackend(video_adapter)
        if vision_cfg.get("api_key"):
            self.vision = APIVisionBackend(video_adapter, vision_cfg)
        else:
            self.vision = self.local_vision

        # Safety
        safety_cfg = config.get("safety", {})
        self.guard = SafetyGuard(safety_cfg)

        # Skill Store
        skills_dir = config.get("skills_dir", "./skill_library")
        self.skill_store = SkillStore(library_dir=skills_dir)
        self.skill_store.load_all()

        # Knowledge Store
        knowledge_dir = config.get("knowledge_dir", "exoanchor/knowledge")
        self.knowledge_store = KnowledgeStore(directory=knowledge_dir)
        self.knowledge_store.load_all()

        memory_root = os.path.abspath(os.path.expanduser(config.get("memory_dir", "~/.exoanchor")))
        tasks_dir = config.get("tasks_dir", os.path.join(memory_root, "tasks"))
        artifacts_dir = config.get("artifacts_dir", os.path.join(memory_root, "artifacts"))
        facts_path = config.get("facts_path", os.path.join(memory_root, "facts.json"))
        self.artifact_store = ArtifactStore(artifacts_dir)
        self.fact_store = FactStore(facts_path)
        self.run_memory = RunMemory(tasks_dir, self.artifact_store, self.fact_store)
        audit_log_path = config.get("audit_log_path", os.path.join(memory_root, "audit.jsonl"))
        self.audit_log = AuditLogStore(audit_log_path)
        self.policy_engine = PolicyEngine(safety_cfg, self.audit_log)
        self.event_hub = EventHub()
        sessions_dir = config.get("sessions_dir", os.path.join(memory_root, "sessions"))
        self.session_store = SessionStore(sessions_dir)
        self.session_runtime = SessionRuntime(self.session_store, self.event_hub)
        self.intent_resolver = None

        # Executor
        self.tool_executor = ToolExecutor(self.action_driver, self.ssh, self.vision)
        self.executor = SemiActiveExecutor(
            action_driver=self.action_driver,
            vision_backend=self.vision,
            safety_guard=self.guard,
            skill_store=self.skill_store,
            tool_executor=self.tool_executor,
            run_memory=self.run_memory,
            policy_engine=self.policy_engine,
        )

        runs_dir = config.get("runs_dir", "~/.exoanchor/runs")
        self.run_store = RunStore(runs_dir)
        self.plan_executor = PlanExecutor(
            ssh_manager=self.ssh,
            tool_executor=self.tool_executor,
            run_memory=self.run_memory,
            policy_engine=self.policy_engine,
            run_store=self.run_store,
            step_evaluator=step_evaluator,
        )
        self.executor.add_ws_callback(self._publish_runtime_event)
        self.plan_executor.add_ws_callback(self._publish_runtime_event)

        # Passive Monitor
        passive_cfg = config.get("passive", {})
        self.passive = PassiveMonitor(
            vision_adapter=video_adapter,
            ssh_manager=self.ssh,
            action_driver=self.action_driver,
            config=passive_cfg,
        )
        self.passive.set_skill_runner(self._run_skill_from_trigger)

        # Recorder
        self.recorder = SkillRecorder(vision_adapter=video_adapter)

    def set_step_evaluator(self, step_evaluator):
        self.plan_executor.set_step_evaluator(step_evaluator)

    def set_intent_resolver(self, resolver):
        self.intent_resolver = resolver

    async def resolve_intent(self, body: dict[str, Any]) -> dict[str, Any]:
        if self.intent_resolver is None:
            raise RuntimeError("Intent resolver is not configured")
        return await self.intent_resolver(body)

    async def _publish_runtime_event(self, payload: dict[str, Any]):
        event = await self.event_hub.publish_raw(payload)
        await self.session_runtime.sync_child_event(event)

    def has_active_run(self) -> bool:
        return self.executor.get_current_task() is not None or self.plan_executor.has_active_run

    async def startup(self):
        """Initialize on server startup."""
        recovery = self.run_memory.recover_stale_state(self.run_store)
        if recovery["recovered_tasks"] or recovery["recovered_runs"]:
            logger.info(
                "Recovered stale state: tasks=%s runs=%s",
                recovery["recovered_tasks"],
                recovery["recovered_runs"],
            )

        # Try SSH connection
        target_ip = self.config.get("target", {}).get("ip", "")
        if target_ip:
            connected = await self.ssh.connect(timeout=5)
            if connected:
                logger.info(f"SSH connected to target: {target_ip}")
            else:
                logger.info(f"SSH not available (target: {target_ip})")

        # Start passive monitor if mode requires it
        if self.mode in (AgentMode.PASSIVE, AgentMode.SEMI_ACTIVE):
            await self.passive.start()

    async def shutdown(self):
        """Cleanup on server shutdown."""
        await self.passive.stop()
        await self.ssh.close()
        if hasattr(self.vision, "close"):
            await self.vision.close()

    async def _run_skill_from_trigger(self, skill_name: str, params: dict):
        """Called by passive monitor to run a recovery skill."""
        try:
            await self.executor.run_skill(
                skill_name,
                params,
                metadata={"policy_context": self.build_policy_context("passive_trigger", request_channel="passive")},
            )
        except Exception as e:
            logger.error(f"Trigger skill execution failed: {e}")

    def get_status(self) -> AgentStatus:
        return AgentStatus(
            mode=self.mode,
            passive_running=self.passive.running,
            current_task=self.executor.get_current_task(),
            ssh_connected=self.ssh.has_shell,
            ssh_target=self.ssh.get_status()["target"],
            services_monitored=len(self.passive.services),
            recent_triggers=self.passive.trigger_history[-5:],
            uptime=time.time() - self.start_time,
        )

    async def list_workloads(self) -> list[dict[str, Any]]:
        """Discover workload manifests on the remote target."""
        if not self.ssh.has_shell:
            return []

        py_script = """
import json
import os

res = []
for label in ('exoanchor', 'cortex'):
    base = os.path.expanduser(f'~/.{label}/workloads')
    if not os.path.exists(base):
        continue
    for d in os.listdir(base):
        p = os.path.join(base, d, 'manifest.json')
        if not os.path.isfile(p):
            continue
        try:
            with open(p, 'r', encoding='utf-8') as f:
                data = json.load(f)
            data['id'] = d
            data['dir'] = d
            data['base_dir'] = label
            data['path'] = os.path.join(base, d)
            searchable = " ".join(str(data.get(key, '') or '') for key in ('name', 'command', 'type')).lower()
            if any(token in searchable or token in d.lower() for token in ('minecraft', 'spigot', 'paper', 'bukkit', 'forge', 'fabric')):
                props_path = os.path.join(base, d, 'server.properties')
                console = {'type': 'minecraft-rcon', 'available': False, 'enabled': False, 'port': 25575, 'password_present': False}
                if os.path.isfile(props_path):
                    props = {}
                    with open(props_path, 'r', encoding='utf-8', errors='ignore') as props_file:
                        for raw in props_file:
                            line = raw.strip()
                            if not line or line.startswith('#') or '=' not in line:
                                continue
                            key, value = line.split('=', 1)
                            props[key.strip()] = value.strip()
                    password = str(props.get('rcon.password', ''))
                    console['enabled'] = str(props.get('enable-rcon', 'false')).lower() == 'true'
                    console['password_present'] = bool(password)
                    console['available'] = bool(console['enabled'] and password)
                    try:
                        console['port'] = int(str(props.get('rcon.port', '25575') or '25575'))
                    except Exception:
                        console['port'] = 25575
                data['console'] = console
            res.append(data)
        except Exception:
            pass

print(json.dumps(res, ensure_ascii=False))
"""
        try:
            output = await self.ssh.run(
                f"python3 -c {shlex.quote(py_script)}",
                timeout=5,
            )
            import json

            start = output.find("[")
            end = output.rfind("]") + 1
            if start == -1 or end == 0:
                return []

            workloads = json.loads(output[start:end])
            for workload in workloads:
                workload["status"] = "unknown"
                port = workload.get("port")
                if port:
                    check_cmd = (
                        f"(ss -tulnp 2>/dev/null || netstat -tuln 2>/dev/null) | "
                        f"grep -E '[:.]({int(port)})\\b' >/dev/null && echo 'running' || echo 'stopped'"
                    )
                    status_out = await self.ssh.run(check_cmd, timeout=2)
                    workload["status"] = status_out.strip()
            self._remember_workloads(workloads)
            return workloads
        except Exception as e:
            logger.error(f"Failed to fetch workloads: {e}")
            return []

    async def get_workload(self, workload_id: str) -> dict[str, Any]:
        target = str(workload_id or "").strip()
        if not target:
            raise ValueError("Workload id is required")

        workloads = await self.list_workloads()
        lowered = target.lower()

        exact = [
            workload for workload in workloads
            if lowered in {
                str(workload.get("id") or "").strip().lower(),
                str(workload.get("dir") or "").strip().lower(),
                str(workload.get("name") or "").strip().lower(),
            }
        ]
        if len(exact) == 1:
            return exact[0]
        if len(exact) > 1:
            names = ", ".join(str(item.get("id") or item.get("dir") or "?") for item in exact[:5])
            raise ValueError(f"Multiple workloads matched `{target}`: {names}")

        partial = [
            workload for workload in workloads
            if lowered in " ".join(
                str(workload.get(key) or "").strip().lower()
                for key in ("id", "dir", "name", "path", "command")
            )
        ]
        if len(partial) == 1:
            return partial[0]
        if len(partial) > 1:
            names = ", ".join(str(item.get("id") or item.get("dir") or "?") for item in partial[:5])
            raise ValueError(f"Multiple workloads matched `{target}`: {names}")
        raise ValueError(f"Workload not found: {target}")

    async def _run_workload_shell(
        self,
        workload: dict[str, Any],
        *,
        action: str,
        command: str,
        timeout: int = 120,
    ) -> dict[str, Any]:
        if not self.ssh.has_shell:
            raise ConnectionError("SSH not connected")

        decision, context = self.evaluate_policy(
            "shell.exec",
            {"command": command, "timeout": timeout},
            source_type="manual_workload_panel",
            metadata={
                "entrypoint": "workload_panel",
                "workload_id": workload.get("id"),
                "workload_name": workload.get("name"),
                "workload_action": action,
                "explicit_user_action": True,
            },
        )
        if decision.action == PolicyAction.DENY:
            raise PermissionError(decision.reason)

        result = await self.ssh.run_with_status(command, timeout=timeout)
        result["policy"] = {
            "action": decision.action.value,
            "risk_level": decision.risk_level.value,
            "reason": decision.reason,
            "matched_rules": list(decision.matched_rules),
            "context": context,
        }
        return result

    async def workload_logs(self, workload_id: str, *, lines: int = 80) -> dict[str, Any]:
        workload = await self.get_workload(workload_id)
        result = await self._run_workload_shell(
            workload,
            action="logs",
            command=build_workload_logs_command(workload, lines=lines),
            timeout=30,
        )
        refreshed = await self.get_workload(workload.get("id") or workload_id)
        return {
            "workload": refreshed,
            "action": "logs",
            "lines": max(10, min(int(lines or 80), 400)),
            "result": result,
            "output": result.get("output") or "",
        }

    async def workload_status(self, workload_id: str) -> dict[str, Any]:
        workload = await self.get_workload(workload_id)
        result = await self._run_workload_shell(
            workload,
            action="status",
            command=build_workload_status_command(workload),
            timeout=20,
        )
        refreshed = await self.get_workload(workload.get("id") or workload_id)
        return {
            "workload": refreshed,
            "action": "status",
            "result": result,
            "output": result.get("output") or "",
        }

    async def get_workload_console_info(self, workload_id: str) -> dict[str, Any]:
        workload = await self.get_workload(workload_id)
        console = dict(workload.get("console") or {})
        if not console and is_minecraft_workload(workload):
            result = await self._run_workload_shell(
                workload,
                action="console:probe",
                command=build_minecraft_console_probe_command(workload),
                timeout=15,
            )
            console = json.loads(str(result.get("output") or "{}") or "{}")
        return {
            "workload": workload,
            "console": console,
        }

    async def setup_workload_console(self, workload_id: str) -> dict[str, Any]:
        workload = await self.get_workload(workload_id)
        if not is_minecraft_workload(workload):
            raise ValueError("This workload does not support automatic console setup")

        password = secrets.token_hex(16)
        setup_result = await self._run_workload_shell(
            workload,
            action="console:setup",
            command=build_minecraft_console_setup_command(workload, password),
            timeout=30,
        )
        control_result = await self.control_workload(workload_id, "restart")
        refreshed = await self.get_workload(workload_id)
        return {
            "workload": refreshed,
            "console": refreshed.get("console") or {},
            "setup": setup_result,
            "restart": control_result,
            "message": "Minecraft RCON console enabled",
        }

    async def exec_workload_console(self, workload_id: str, console_command: str) -> dict[str, Any]:
        workload = await self.get_workload(workload_id)
        command_text = str(console_command or "").strip()
        if not command_text:
            raise ValueError("Console command is required")
        if not is_minecraft_workload(workload):
            raise ValueError("This workload does not expose a supported console transport")

        console = dict(workload.get("console") or {})
        if not console.get("available"):
            raise RuntimeError("RCON console is not enabled for this workload")

        probe_result = await self._run_workload_shell(
            workload,
            action="console:probe",
            command=build_minecraft_console_probe_command(workload),
            timeout=15,
        )
        probe_payload = json.loads(str(probe_result.get("output") or "{}") or "{}")
        if not probe_payload.get("available"):
            raise RuntimeError("RCON console is not enabled for this workload")

        password_result = await self._run_workload_shell(
            workload,
            action="console:read-password",
            command=(
                f"cd {shlex.quote(str(workload.get('path') or ''))} && "
                "grep '^rcon.password=' server.properties | tail -n 1 | cut -d= -f2-"
            ),
            timeout=10,
        )
        password = str(password_result.get("output") or "").strip()
        if not password:
            raise RuntimeError("RCON password is missing")

        exec_result = await self._run_workload_shell(
            workload,
            action="console:exec",
            command=build_minecraft_rcon_exec_command(
                workload,
                command_text,
                password=password,
                port=int(probe_payload.get("port") or 25575),
            ),
            timeout=20,
        )
        refreshed = await self.get_workload(workload_id)
        return {
            "workload": refreshed,
            "console": refreshed.get("console") or probe_payload,
            "command": command_text,
            "result": exec_result,
            "output": exec_result.get("output") or "",
        }

    async def control_workload(self, workload_id: str, action: str) -> dict[str, Any]:
        workload = await self.get_workload(workload_id)
        normalized = str(action or "").strip().lower()
        if normalized not in {"start", "stop", "restart"}:
            raise ValueError(f"Unsupported workload action: {action}")

        steps: list[tuple[str, str, int]] = []
        if normalized == "start":
            steps = [("start", build_workload_start_command(workload), 120)]
        elif normalized == "stop":
            steps = [("stop", build_workload_stop_command(workload), 60)]
        elif normalized == "restart":
            steps = [
                ("stop", build_workload_stop_command(workload), 60),
                ("start", build_workload_start_command(workload), 120),
            ]

        results: list[dict[str, Any]] = []
        for step_action, command, timeout in steps:
            result = await self._run_workload_shell(
                workload,
                action=f"{normalized}:{step_action}",
                command=command,
                timeout=timeout,
            )
            results.append({
                "step": step_action,
                **result,
            })
            if not result.get("success", False):
                break

            if normalized in {"start", "restart"} and step_action == "start":
                await asyncio.sleep(1.5)
            if normalized == "stop" and step_action == "stop":
                await asyncio.sleep(0.5)

        verify = await self._run_workload_shell(
            workload,
            action=f"{normalized}:verify",
            command=build_workload_status_command(workload),
            timeout=20,
        )
        refreshed = await self.get_workload(workload.get("id") or workload_id)
        success = all(item.get("success", False) for item in results) and bool(verify.get("success", False))
        return {
            "workload": refreshed,
            "action": normalized,
            "success": success,
            "results": results,
            "verify": verify,
            "output": verify.get("output") or "",
        }

    def _remember_workloads(self, workloads: list[dict[str, Any]]) -> None:
        if not getattr(self, "fact_store", None):
            return

        remembered: list[dict[str, Any]] = []
        for workload in workloads or []:
            if not isinstance(workload, dict):
                continue
            workload_id = str(workload.get("id") or workload.get("dir") or "").strip()
            if not workload_id:
                continue

            payload = dict(workload)
            payload["remembered_at"] = time.time()
            self.fact_store.upsert(
                f"workload.{workload_id}.manifest",
                payload,
                category="workload",
                source="live_discovery",
                details={
                    "path": payload.get("path"),
                    "status": payload.get("status"),
                },
            )
            remembered.append({
                "id": workload_id,
                "name": payload.get("name") or workload_id,
                "path": payload.get("path") or "",
                "dir": payload.get("dir") or workload_id,
                "base_dir": payload.get("base_dir") or "exoanchor",
                "type": payload.get("type") or "",
                "port": payload.get("port"),
                "status": payload.get("status") or "unknown",
                "command": payload.get("command") or "",
            })

        if remembered:
            self.fact_store.upsert(
                "workloads.latest",
                {
                    "count": len(remembered),
                    "items": remembered,
                    "updated_at": time.time(),
                },
                category="workload",
                source="live_discovery",
            )

    async def create_workload(
        self,
        workload_id: str,
        name: str,
        workload_type: str = "custom",
        command: str = "",
        port: Optional[int] = None,
        base_dir: str = "exoanchor",
    ) -> dict[str, Any]:
        """Create a new workload manifest on the remote target via SSH."""
        if not self.ssh.has_shell:
            raise ConnectionError("SSH not connected — cannot create workload on target")

        # Validate workload_id
        clean_id = str(workload_id or "").strip()
        if not clean_id:
            raise ValueError("Workload id is required")
        import re as _re
        if not _re.match(r'^[a-zA-Z0-9_-]+$', clean_id):
            raise ValueError(
                f"Invalid workload id '{clean_id}': only letters, digits, underscores and hyphens allowed"
            )

        # Check for duplicates
        existing = await self.list_workloads()
        for wl in existing:
            if str(wl.get("id") or wl.get("dir") or "").strip().lower() == clean_id.lower():
                raise ValueError(f"Workload '{clean_id}' already exists")

        # Build manifest
        manifest: dict[str, Any] = {
            "name": str(name or clean_id).strip(),
            "type": str(workload_type or "custom").strip(),
        }
        if command:
            manifest["command"] = str(command).strip()
        if port is not None:
            manifest["port"] = int(port)

        safe_base = str(base_dir or "exoanchor").strip() or "exoanchor"
        remote_dir = f"~/.{safe_base}/workloads/{clean_id}"
        manifest_json = json.dumps(manifest, ensure_ascii=False, indent=2)

        # Create directory and write manifest via SSH
        create_cmd = (
            f"mkdir -p {shlex.quote(remote_dir)} && "
            f"cat > {shlex.quote(remote_dir + '/manifest.json')} << 'EXOANCHOR_MANIFEST_EOF'\n"
            f"{manifest_json}\n"
            f"EXOANCHOR_MANIFEST_EOF"
        )
        result = await self.ssh.run(create_cmd, timeout=10)
        logger.info(f"Created workload '{clean_id}' at {remote_dir}: {result}")

        # Refresh and return the new workload
        refreshed = await self.list_workloads()
        for wl in refreshed:
            if str(wl.get("id") or wl.get("dir") or "").strip().lower() == clean_id.lower():
                return wl

        # Fallback — return the manifest we wrote
        return {
            "id": clean_id,
            "dir": clean_id,
            "base_dir": safe_base,
            "path": os.path.expanduser(remote_dir),
            "status": "stopped",
            **manifest,
        }

    def build_policy_context(self, source_type: str, *, request_channel: str = "api", extra: Optional[dict[str, Any]] = None) -> dict[str, Any]:
        payload = {
            "source_type": source_type,
            "request_channel": request_channel,
            "agent_mode": self.mode.value,
        }
        if extra:
            payload.update(extra)
        return payload

    def evaluate_policy(self, tool_name: str, args: dict[str, Any], *, source_type: str, metadata: Optional[dict[str, Any]] = None):
        context = self.build_policy_context(source_type, extra=metadata or {})
        decision = self.policy_engine.evaluate_tool_call(
            tool_name,
            args,
            source_type=context["source_type"],
            agent_mode=context["agent_mode"],
            metadata=context,
        )
        self.policy_engine.audit(
            decision,
            source_type=context["source_type"],
            agent_mode=context["agent_mode"],
            metadata=context,
        )
        return decision, context

    def preflight_skill(self, skill_name: str, params: dict, *, source_type: str) -> tuple[Any, dict, list[dict]]:
        skill = self.skill_store.get_skill(skill_name)
        if skill is None:
            raise ValueError(f"Skill not found: {skill_name}")

        merged_params = skill.validate_params(params or {}) if hasattr(skill, "validate_params") else (params or {})
        blocked: list[dict] = []

        if skill.get("mode", "guided") == "scripted":
            plan = plan_from_scripted_skill(skill, merged_params)
            for step in plan.steps:
                decision, _ = self.evaluate_policy(
                    step.tool,
                    step.args,
                    source_type=source_type,
                    metadata={"skill_name": skill_name, "step_id": step.id, "preflight": True},
                )
                if decision.action != PolicyAction.ALLOW:
                    blocked.append({
                        "step_id": step.id,
                        "description": step.description,
                        "tool": step.tool,
                        "command": step.command,
                        "action": decision.action.value,
                        "risk_level": decision.risk_level.value,
                        "reason": decision.reason,
                    })

        return skill, merged_params, blocked


# ═══════════════════════════════════════════════════════════════
# Router Factory
# ═══════════════════════════════════════════════════════════════

def create_agent_router(
    hid_adapter: HIDAdapterInterface,
    video_adapter: VideoAdapterInterface,
    gpio_adapter: GPIOAdapterInterface,
    config: dict,
    step_evaluator=None,
) -> APIRouter:
    """Create a FastAPI Router with all Agent endpoints."""

    router = APIRouter(tags=["Agent (ExoAnchor)"])
    agent = AgentInstance(hid_adapter, video_adapter, gpio_adapter, config, step_evaluator=step_evaluator)

    def _build_event_matcher(*, task_id: str = "", run_id: str = "", session_id: str = ""):
        target_task = str(task_id or "").strip()
        target_run = str(run_id or "").strip()
        target_session = str(session_id or "").strip()
        session = agent.session_runtime.get(target_session) if target_session else None
        bound_run = str(getattr(session, "run_id", "") or "").strip()
        bound_task = str(getattr(session, "task_id", "") or "").strip()
        if not target_task and not target_run and not target_session:
            return None

        def matcher(event):
            if target_session and event.entity_kind == "session" and event.entity_id == target_session:
                return True
            if target_session and bound_run and event.entity_kind == "plan_run" and event.entity_id == bound_run:
                return True
            if target_session and bound_task and event.entity_kind == "task" and event.entity_id == bound_task:
                return True
            if target_run and event.entity_kind == "plan_run" and event.entity_id == target_run:
                return True
            if target_task and event.entity_kind == "task" and event.entity_id == target_task:
                return True
            return False

        return matcher

    def _snapshot_events(*, task_id: str = "", run_id: str = "", session_id: str = ""):
        events = []

        if session_id:
            session_event = agent.session_runtime.snapshot_event(session_id)
            if session_event is not None:
                events.append(session_event)
                bound_session = agent.session_runtime.get(session_id)
                if bound_session is not None:
                    if bound_session.run_id:
                        run = agent.plan_executor.get_run(bound_session.run_id)
                        if run is not None:
                            events.append(build_snapshot_event(
                                stream="plan_run",
                                entity_kind="plan_run",
                                entity_id=run.run_id,
                                state=run.state.value,
                                summary=run.goal,
                                payload={"run": run.model_dump()},
                            ))
                    if bound_session.task_id:
                        snapshot = agent.executor.get_task_snapshot(bound_session.task_id)
                        if snapshot is not None:
                            events.append(build_snapshot_event(
                                stream="task",
                                entity_kind="task",
                                entity_id=snapshot.task_id,
                                state=snapshot.state,
                                summary=snapshot.skill_name,
                                payload={"snapshot": snapshot.model_dump()},
                            ))

        if run_id:
            run = agent.plan_executor.get_run(run_id)
            if run is not None:
                events.append(build_snapshot_event(
                    stream="plan_run",
                    entity_kind="plan_run",
                    entity_id=run.run_id,
                    state=run.state.value if hasattr(run.state, "value") else str(run.state),
                    summary=run.goal,
                    payload={"run": run.model_dump()},
                ))

        if task_id:
            snapshot = agent.executor.get_task_snapshot(task_id)
            if snapshot is not None:
                events.append(build_snapshot_event(
                    stream="task",
                    entity_kind="task",
                    entity_id=task_id,
                    state=str(snapshot.state),
                    summary=snapshot.skill_name,
                    payload={"snapshot": snapshot.model_dump()},
                ))

        return events

    async def _start_background_skill(
        skill_name: str,
        params: dict[str, Any],
        *,
        source_type: str,
        request_channel: str = "api",
        extra_metadata: Optional[dict[str, Any]] = None,
    ):
        _, _, blocked = agent.preflight_skill(skill_name, params, source_type=source_type)
        if blocked:
            raise HTTPException(403, {
                "message": "Skill contains actions which require a supervised plan or are denied by policy",
                "blocked_steps": blocked,
            })
        task_id = await agent.executor.run_skill_async(
            skill_name,
            params,
            metadata={
                "policy_context": agent.build_policy_context(source_type, request_channel=request_channel, extra=extra_metadata),
            },
        )
        return task_id

    async def _run_skill_sync(
        skill_name: str,
        params: dict[str, Any],
        *,
        source_type: str,
        request_channel: str = "api",
        extra_metadata: Optional[dict[str, Any]] = None,
    ):
        _, _, blocked = agent.preflight_skill(skill_name, params, source_type=source_type)
        if blocked:
            raise HTTPException(403, {
                "message": "Skill contains actions which require a supervised plan or are denied by policy",
                "blocked_steps": blocked,
            })
        return await agent.executor.run_skill(
            skill_name,
            params,
            metadata={
                "policy_context": agent.build_policy_context(source_type, request_channel=request_channel, extra=extra_metadata),
            },
        )

    async def _start_backend_plan(
        *,
        goal: str,
        steps: list[dict[str, Any]],
        supervised: bool = False,
        react_mode: str = "on_fail",
        model: str = "",
        source: str = "llm",
        metadata: Optional[dict[str, Any]] = None,
    ):
        if not steps:
            raise HTTPException(400, "Plan must contain at least one step")
        if not agent.ssh.has_shell:
            raise HTTPException(503, "SSH not connected")
        if agent.has_active_run():
            raise HTTPException(409, "Another task or plan is already active")

        payload_metadata = dict(metadata or {})
        default_source_type = "auto_plan" if source in {"passive", "auto", "semi_active"} else "manual_plan"
        policy_context = dict(payload_metadata.get("policy_context") or agent.build_policy_context(default_source_type))
        payload_metadata["policy_context"] = policy_context
        executable_plan = plan_from_llm(goal, steps)

        normalized_steps = []
        blocked_steps = []
        for step in executable_plan.steps:
            decision, _ = agent.evaluate_policy(
                step.tool,
                step.args,
                source_type=policy_context["source_type"],
                metadata={"step_id": step.id, "goal": executable_plan.goal, "preflight": True},
            )
            if decision.action == PolicyAction.DENY:
                blocked_steps.append({
                    "step_id": step.id,
                    "description": step.description,
                    "tool": step.tool,
                    "command": step.command,
                    "risk_level": decision.risk_level.value,
                    "reason": decision.reason,
                })
            normalized_steps.append({
                "id": step.id,
                "description": step.description,
                "tool": step.tool,
                "args": step.args,
                "command": step.command,
                "dangerous": step.dangerous or decision.action == PolicyAction.CONFIRM or decision.risk_level in {RiskLevel.HIGH, RiskLevel.CRITICAL},
            })

        if blocked_steps:
            raise HTTPException(403, {
                "message": "Plan contains steps denied by policy",
                "blocked_steps": blocked_steps,
            })

        return await agent.plan_executor.start_plan(
            goal=executable_plan.goal,
            steps=normalized_steps,
            supervised=supervised,
            react_mode=react_mode,
            model=model,
            source=source,
            metadata=payload_metadata,
        )

    async def _run_direct_ssh(command: str, *, timeout: int = 300, source_type: str = "direct_ssh_exec", metadata: Optional[dict[str, Any]] = None):
        if agent.plan_executor.has_active_run:
            raise HTTPException(409, "A backend plan run is active")
        if not agent.ssh.has_shell:
            raise HTTPException(503, "SSH not connected")

        decision, _ = agent.evaluate_policy(
            "shell.exec",
            {"command": command, "timeout": timeout},
            source_type=source_type,
            metadata=metadata or {"entrypoint": "ssh_exec"},
        )
        if decision.action != PolicyAction.ALLOW:
            raise HTTPException(403, {
                "message": decision.reason,
                "risk_level": decision.risk_level.value,
                "action": decision.action.value,
                "matched_rules": decision.matched_rules,
            })
        try:
            return await agent.ssh.run_with_status(command, timeout=timeout)
        except Exception as e:
            raise HTTPException(500, str(e))

    # ── Lifecycle Events ────────────────────────────────────────

    @router.on_event("startup")
    async def on_startup():
        await agent.startup()

    @router.on_event("shutdown")
    async def on_shutdown():
        await agent.shutdown()

    # ── Status ──────────────────────────────────────────────────

    @router.get("/status")
    async def get_status():
        """Get overall agent status."""
        return agent.get_status().model_dump()

    # ── Mode Control ────────────────────────────────────────────

    class ModeRequest(BaseModel):
        mode: str  # "manual" | "passive" | "semi_active"

    @router.post("/mode")
    async def set_mode(req: ModeRequest):
        """Switch agent operating mode."""
        try:
            new_mode = AgentMode(req.mode)
        except ValueError:
            raise HTTPException(400, f"Invalid mode: {req.mode}")

        old_mode = agent.mode
        agent.mode = new_mode

        # Start/stop passive monitor based on mode
        if new_mode in (AgentMode.PASSIVE, AgentMode.SEMI_ACTIVE):
            if not agent.passive.running:
                await agent.passive.start()
        else:
            if agent.passive.running:
                await agent.passive.stop()

        return {"old_mode": old_mode.value, "new_mode": new_mode.value}

    # ── Passive Monitor ─────────────────────────────────────────

    @router.get("/passive/status")
    async def passive_status():
        """Get passive monitor status and recent triggers."""
        return agent.passive.get_status()

    @router.post("/passive/start")
    async def passive_start():
        """Manually start passive monitoring."""
        await agent.passive.start()
        return {"status": "started"}

    @router.post("/passive/stop")
    async def passive_stop():
        """Stop passive monitoring."""
        await agent.passive.stop()
        return {"status": "stopped"}

    # ── Task Execution ──────────────────────────────────────────

    @router.post("/task")
    async def run_task(req: TaskRequest):
        """Execute a skill. Returns when complete."""
        if agent.plan_executor.has_active_run:
            raise HTTPException(409, "A plan run is already active")
        try:
            result = await _run_skill_sync(
                req.skill_name,
                req.params,
                source_type="manual_task",
                request_channel="api",
            )
            return result.model_dump()
        except ValueError as e:
            raise HTTPException(404, str(e))
        except RuntimeError as e:
            raise HTTPException(409, str(e))

    @router.post("/task/start")
    async def start_task(req: TaskRequest):
        """Start a skill in the background and return its task id."""
        if agent.plan_executor.has_active_run:
            raise HTTPException(409, "A backend plan run is active")
        try:
            task_id = await _start_background_skill(
                req.skill_name,
                req.params,
                source_type="manual_task",
                request_channel="api",
            )
            return {"task_id": task_id, "skill_name": req.skill_name}
        except ValueError as e:
            raise HTTPException(404, str(e))
        except RuntimeError as e:
            raise HTTPException(409, str(e))

    @router.get("/task/current")
    async def current_task():
        """Get currently running task status."""
        task = agent.executor.get_current_task()
        if task:
            return task.model_dump()
        return {"status": "idle"}

    @router.post("/task/abort")
    async def abort_task():
        """Abort the currently running task."""
        agent.executor.abort_current()
        return {"status": "aborting"}

    @router.post("/task/pause")
    async def pause_task():
        """Pause the currently running task."""
        agent.executor.pause_current()
        return {"status": "paused"}

    @router.post("/task/resume")
    async def resume_task():
        """Resume the currently paused task."""
        agent.executor.resume_current()
        return {"status": "resumed"}

    @router.get("/task/history")
    async def task_history():
        """Get recent task execution history."""
        return {
            "tasks": agent.executor.get_task_history(),
            "plan_runs": [run.model_dump() for run in agent.plan_executor.list_runs(limit=20)],
        }

    @router.get("/task/history/{task_id}")
    async def task_history_detail(task_id: str):
        snapshot = agent.executor.get_task_snapshot(task_id)
        if snapshot is None:
            raise HTTPException(404, "Task snapshot not found")
        return snapshot.model_dump()

    @router.get("/events")
    async def runtime_events(limit: int = 50, task_id: str = "", run_id: str = "", session_id: str = ""):
        matcher = _build_event_matcher(task_id=task_id, run_id=run_id, session_id=session_id)
        items = agent.event_hub.recent(limit=limit, matcher=matcher)
        return {"events": [item.model_dump() for item in items]}

    @router.get("/events/stream")
    async def runtime_events_stream(task_id: str = "", run_id: str = "", session_id: str = "", replay: int = 20):
        matcher = _build_event_matcher(task_id=task_id, run_id=run_id, session_id=session_id)
        subscriber_id, queue = agent.event_hub.subscribe(matcher=matcher)
        snapshot_items = _snapshot_events(task_id=task_id, run_id=run_id, session_id=session_id)
        replay_items = agent.event_hub.recent(limit=max(0, replay), matcher=matcher) if session_id or not snapshot_items else []

        async def event_generator():
            try:
                for item in snapshot_items:
                    yield agent.event_hub.encode(item)
                seen_ids = {item.event_id for item in snapshot_items}
                for item in replay_items:
                    if item.event_id in seen_ids:
                        continue
                    yield agent.event_hub.encode(item)

                while True:
                    event = await queue.get()
                    yield agent.event_hub.encode(event)
            finally:
                agent.event_hub.unsubscribe(subscriber_id)

        return StreamingResponse(event_generator(), media_type="application/x-ndjson")

    # ── Plan Runs ──────────────────────────────────────────────

    class PlanStepRequest(BaseModel):
        id: Optional[Union[str, int]] = None
        description: str
        command: Optional[str] = None
        tool: Optional[str] = None
        args: dict[str, Any] = Field(default_factory=dict)
        dangerous: bool = False

    class PlanRunRequest(BaseModel):
        goal: str
        steps: list[PlanStepRequest] = Field(default_factory=list)
        supervised: bool = False
        react_mode: str = "on_fail"
        model: str = ""
        source: str = "llm"
        metadata: dict[str, Any] = Field(default_factory=dict)

    class PlanConfirmationRequest(BaseModel):
        approved: bool

    class SessionRequest(BaseModel):
        message: str
        conversation_id: str = ""
        model: str = ""
        force_plan: bool = False
        dry_run: bool = False
        metadata: dict[str, Any] = Field(default_factory=dict)

    class SessionResumeRequest(BaseModel):
        saved: bool = False

    @router.post("/sessions")
    async def start_session(req: SessionRequest):
        """Create a durable server-owned session from one natural-language request."""
        if agent.intent_resolver is None:
            raise HTTPException(503, "Intent resolver is not configured")

        session = await agent.session_runtime.create(
            request=req.message,
            conversation_id=req.conversation_id,
            model=req.model,
            force_plan=req.force_plan,
            dry_run=req.dry_run,
            metadata=req.metadata,
        )

        try:
            session = await agent.session_runtime.update(
                session,
                event="parsing_started",
                state=SessionState.PARSING,
            )
            result = await agent.resolve_intent({
                "message": req.message,
                "conversation_id": req.conversation_id,
                "model": req.model,
                "force_plan": req.force_plan,
            })

            rtype = str((result or {}).get("type") or "").lower()
            summary = (
                result.get("goal")
                or result.get("description")
                or result.get("skill_name")
                or result.get("skill_id")
                or req.message[:120]
            )
            session = await agent.session_runtime.update(
                session,
                event="parsed",
                result_type=rtype,
                parsed_result=result,
                summary=str(summary or req.message[:120]),
            )

            if req.dry_run:
                session = await agent.session_runtime.update(
                    session,
                    event="dry_run_ready",
                    state=SessionState.COMPLETED,
                    success=True,
                    message="Preview ready",
                )
                return session.model_dump()

            if rtype == "chat":
                message = str(result.get("message") or "").strip()
                question_like = message.endswith(("?", "？")) or ("请" in message and "?" in message) or ("请" in message and "？" in message)
                session = await agent.session_runtime.update(
                    session,
                    event="waiting_input" if question_like else "completed",
                    state=SessionState.WAITING_INPUT if question_like else SessionState.COMPLETED,
                    execution_kind="chat",
                    message=message,
                    success=not question_like,
                )
                return session.model_dump()

            if rtype == "ssh":
                command = str(result.get("command") or "").strip()
                session = await agent.session_runtime.update(
                    session,
                    event="dispatching",
                    state=SessionState.DISPATCHING,
                    execution_kind="ssh",
                    command=command,
                )
                ssh_result = await _run_direct_ssh(
                    command,
                    timeout=int(result.get("timeout") or 300),
                    source_type="session_ssh",
                    metadata={
                        "entrypoint": "session",
                        "session_id": session.session_id,
                        "conversation_id": req.conversation_id,
                    },
                )
                output = str(ssh_result.get("output") or ssh_result.get("stdout") or ssh_result.get("stderr") or "")
                ok = bool(ssh_result.get("success", True))
                session = await agent.session_runtime.update(
                    session,
                    event="completed" if ok else "failed",
                    state=SessionState.COMPLETED if ok else SessionState.FAILED,
                    output=output,
                    message=output[:500],
                    success=ok,
                    error=None if ok else output[:500],
                )
                return session.model_dump()

            if rtype == "skill_call":
                skill_name = result.get("skill_name") or result.get("skill_id")
                params = result.get("params") or {}
                session = await agent.session_runtime.update(
                    session,
                    event="dispatching",
                    state=SessionState.DISPATCHING,
                    execution_kind="task",
                    summary=str(skill_name or session.summary),
                )
                task_id = await _start_background_skill(
                    skill_name,
                    params,
                    source_type="manual_task",
                    request_channel="session",
                    extra_metadata={
                        "entrypoint": "session",
                        "session_id": session.session_id,
                        "conversation_id": req.conversation_id,
                    },
                )
                agent.session_runtime.bind_task(session.session_id, task_id)
                session = await agent.session_runtime.update(
                    session,
                    event="child_attached",
                    state=SessionState.RUNNING,
                    task_id=task_id,
                    execution_kind="task",
                    payload={"child_kind": "task", "child_id": task_id},
                )
                return session.model_dump()

            if rtype == "plan":
                goal = result.get("goal") or req.message
                session = await agent.session_runtime.update(
                    session,
                    event="dispatching",
                    state=SessionState.DISPATCHING,
                    execution_kind="plan_run",
                    summary=str(goal),
                )
                run = await _start_backend_plan(
                    goal=goal,
                    steps=result.get("steps") or [],
                    supervised=False,
                    react_mode="on_fail",
                    model=req.model,
                    source="session",
                    metadata={
                        "conversation_id": req.conversation_id,
                        "session_id": session.session_id,
                        "policy_context": agent.build_policy_context("manual_plan", request_channel="session"),
                    },
                )
                agent.session_runtime.bind_run(session.session_id, run.run_id)
                session = await agent.session_runtime.update(
                    session,
                    event="child_attached",
                    state=SessionState.RUNNING,
                    run_id=run.run_id,
                    execution_kind="plan_run",
                    payload={"child_kind": "plan_run", "child_id": run.run_id},
                )
                return session.model_dump()

            raise HTTPException(400, f"Unsupported result type: {rtype or 'unknown'}")

        except HTTPException as exc:
            detail = exc.detail if isinstance(exc.detail, str) else json.dumps(exc.detail, ensure_ascii=False)
            await agent.session_runtime.update(
                session,
                event="failed",
                state=SessionState.FAILED,
                success=False,
                error=detail,
                message=detail,
            )
            raise
        except Exception as exc:
            await agent.session_runtime.update(
                session,
                event="failed",
                state=SessionState.FAILED,
                success=False,
                error=str(exc),
                message=str(exc),
            )
            raise HTTPException(500, str(exc))

    @router.get("/sessions")
    async def list_sessions(limit: int = 20, conversation_id: str = ""):
        items = agent.session_runtime.list(limit=None)
        if conversation_id:
            items = [item for item in items if str(item.conversation_id or "") == conversation_id]
        items.sort(key=lambda item: item.updated_at, reverse=True)
        if limit is not None:
            items = items[:limit]
        return {"sessions": [item.model_dump() for item in items]}

    @router.get("/sessions/{session_id}")
    async def get_session(session_id: str):
        session = agent.session_runtime.get(session_id)
        if session is None:
            raise HTTPException(404, "Session not found")
        return session.model_dump()

    @router.post("/sessions/{session_id}/resume")
    async def resume_session(session_id: str, req: SessionResumeRequest):
        session = agent.session_runtime.get(session_id)
        if session is None:
            raise HTTPException(404, "Session not found")

        if session.run_id:
            run = agent.plan_executor.get_run(session.run_id)
            if run is None:
                raise HTTPException(404, "Run not found")
            run_state = str(run.state or "").strip().lower()
            if "." in run_state:
                run_state = run_state.split(".")[-1]

            if req.saved or run_state in {"completed", "failed", "aborted"}:
                try:
                    resumed = await agent.plan_executor.resume_saved_run(session.run_id)
                except RuntimeError as e:
                    raise HTTPException(409, str(e))
                agent.session_runtime.bind_run(session.session_id, resumed.run_id)
                await agent.session_runtime.update(
                    session,
                    event="child_reattached",
                    state=SessionState.RUNNING,
                    run_id=resumed.run_id,
                    execution_kind="plan_run",
                    payload={"child_kind": "plan_run", "child_id": resumed.run_id, "resumed_from": session.run_id},
                )
                return {"status": "resumed", "session_id": session.session_id, "run_id": resumed.run_id, "saved": True}

            try:
                resumed = await agent.plan_executor.resume_run(session.run_id)
            except RuntimeError as e:
                raise HTTPException(409, str(e))
            return {"status": "resumed", "session_id": session.session_id, "run_id": resumed.run_id, "saved": False}

        if session.task_id:
            current_task = agent.executor.get_current_task()
            current_task_id = str(getattr(current_task, "task_id", "") or "")
            if current_task is None or current_task_id != str(session.task_id):
                raise HTTPException(409, "Task resume by session currently only works for the active task")
            agent.executor.resume_current()
            return {"status": "resumed", "session_id": session.session_id, "task_id": session.task_id}

        raise HTTPException(409, "Session has no attached run or task to resume")

    @router.post("/sessions/{session_id}/abort")
    async def abort_session(session_id: str):
        session = agent.session_runtime.get(session_id)
        if session is None:
            raise HTTPException(404, "Session not found")

        if session.run_id:
            try:
                run = await agent.plan_executor.abort_run(session.run_id)
                return {"status": "aborting", "session_id": session.session_id, "run_id": run.run_id}
            except RuntimeError as e:
                raise HTTPException(409, str(e))

        if session.task_id:
            current_task = agent.executor.get_current_task()
            current_task_id = str(getattr(current_task, "task_id", "") or "")
            if current_task is None or current_task_id != str(session.task_id):
                raise HTTPException(409, "Task abort by session currently only works for the active task")
            agent.executor.abort_current()
            return {"status": "aborting", "session_id": session.session_id, "task_id": session.task_id}

        raise HTTPException(409, "Session has no attached run or task to abort")

    @router.post("/sessions/{session_id}/approve")
    async def approve_session(session_id: str):
        session = agent.session_runtime.get(session_id)
        if session is None:
            raise HTTPException(404, "Session not found")
        if not session.run_id:
            raise HTTPException(409, "Session is not attached to a plan run")
        try:
            run = await agent.plan_executor.confirm_step(session.run_id, True)
            return {"status": "ok", "session_id": session.session_id, "run_id": run.run_id, "approved": True}
        except RuntimeError as e:
            raise HTTPException(409, str(e))

    @router.post("/sessions/{session_id}/reject")
    async def reject_session(session_id: str):
        session = agent.session_runtime.get(session_id)
        if session is None:
            raise HTTPException(404, "Session not found")
        if not session.run_id:
            raise HTTPException(409, "Session is not attached to a plan run")
        try:
            run = await agent.plan_executor.confirm_step(session.run_id, False)
            return {"status": "ok", "session_id": session.session_id, "run_id": run.run_id, "approved": False}
        except RuntimeError as e:
            raise HTTPException(409, str(e))

    @router.post("/runs/plan")
    async def start_plan_run(req: PlanRunRequest):
        """Start a backend-managed plan run."""
        try:
            run = await _start_backend_plan(
                goal=req.goal,
                steps=[step.model_dump() for step in req.steps],
                supervised=req.supervised,
                react_mode=req.react_mode,
                model=req.model,
                source=req.source,
                metadata=req.metadata,
            )
            return run.model_dump()
        except RuntimeError as e:
            raise HTTPException(409, str(e))

    @router.get("/runs")
    async def list_plan_runs(limit: int = 20):
        return {"runs": [run.model_dump() for run in agent.plan_executor.list_runs(limit=limit)]}

    @router.get("/runs/current")
    async def get_current_plan_run():
        run = agent.plan_executor.get_current_run()
        if run is None:
            return {"run": None}
        return {"run": run.model_dump()}

    @router.get("/runs/{run_id}")
    async def get_plan_run(run_id: str):
        run = agent.plan_executor.get_run(run_id)
        if run is None:
            raise HTTPException(404, "Run not found")
        return run.model_dump()

    @router.post("/runs/{run_id}/pause")
    async def pause_plan_run(run_id: str):
        try:
            run = await agent.plan_executor.pause_run(run_id)
            return run.model_dump()
        except RuntimeError as e:
            raise HTTPException(409, str(e))

    @router.post("/runs/{run_id}/resume")
    async def resume_plan_run(run_id: str):
        try:
            run = await agent.plan_executor.resume_run(run_id)
            return run.model_dump()
        except RuntimeError as e:
            raise HTTPException(409, str(e))

    @router.post("/runs/{run_id}/resume_saved")
    async def resume_saved_plan_run(run_id: str):
        try:
            run = await agent.plan_executor.resume_saved_run(run_id)
            return run.model_dump()
        except RuntimeError as e:
            raise HTTPException(409, str(e))

    @router.post("/runs/{run_id}/abort")
    async def abort_plan_run(run_id: str):
        try:
            run = await agent.plan_executor.abort_run(run_id)
            return {"status": "aborting", "run_id": run.run_id}
        except RuntimeError as e:
            raise HTTPException(409, str(e))

    @router.post("/runs/{run_id}/confirm")
    async def confirm_plan_run(run_id: str, req: PlanConfirmationRequest):
        try:
            run = await agent.plan_executor.confirm_step(run_id, req.approved)
            return {"status": "ok", "run_id": run.run_id, "approved": req.approved}
        except RuntimeError as e:
            raise HTTPException(409, str(e))

    # ── Skills ──────────────────────────────────────────────────

    @router.get("/skills")
    async def list_skills(tag: str = None):
        """List all available skills."""
        tags = [tag] if tag else None
        return {"skills": agent.skill_store.list_skills(tags)}

    # ── Memory ──────────────────────────────────────────────────

    @router.get("/memory/summary")
    async def memory_summary():
        return {
            "tasks": [snapshot.model_dump() for snapshot in agent.run_memory.list_tasks(limit=5)],
            "facts": [fact.model_dump() for fact in agent.fact_store.list_facts(limit=10)],
            "failures": [failure.model_dump() for failure in agent.fact_store.list_failures(limit=10)],
            "artifacts": agent.artifact_store.list_artifacts(limit=10),
        }

    @router.get("/memory/tasks")
    async def memory_tasks(limit: int = 20):
        return {"tasks": [snapshot.model_dump() for snapshot in agent.run_memory.list_tasks(limit=limit)]}

    @router.get("/memory/facts")
    async def memory_facts(prefix: str = "", limit: int = 50):
        return {"facts": [fact.model_dump() for fact in agent.fact_store.list_facts(prefix=prefix, limit=limit)]}

    @router.get("/memory/facts/{key:path}")
    async def memory_fact(key: str):
        fact = agent.fact_store.get(key)
        if fact is None:
            raise HTTPException(404, "Fact not found")
        return fact.model_dump()

    @router.get("/memory/failures")
    async def memory_failures(limit: int = 20):
        return {"failures": [failure.model_dump() for failure in agent.fact_store.list_failures(limit=limit)]}

    @router.get("/policy/summary")
    async def policy_summary():
        return agent.policy_engine.summary()

    @router.get("/audit")
    async def audit_events(limit: int = 50):
        return {"events": [event.model_dump() for event in agent.audit_log.list_events(limit=limit)]}

    @router.get("/skills/{name}")
    async def get_skill(name: str):
        """Get skill details."""
        skill = agent.skill_store.get_skill(name)
        if not skill:
            raise HTTPException(404, f"Skill not found: {name}")
        return skill.to_dict()

    @router.get("/skills/{name}/export")
    async def export_skill(name: str):
        """Export a skill as YAML."""
        content = agent.skill_store.export_skill(name)
        if content is None:
            raise HTTPException(404, f"Skill not found: {name}")
        return {"name": name, "yaml": content}

    @router.post("/skills/import")
    async def import_skill(file: UploadFile = File(...)):
        """Import a skill from a YAML file."""
        if not file.filename.endswith((".yaml", ".yml")):
            raise HTTPException(400, "Only YAML files accepted")

        content = await file.read()
        content_str = content.decode("utf-8")

        # Save to custom directory
        import tempfile, os, yaml as _yaml
        tmp = os.path.join(tempfile.gettempdir(), file.filename)
        with open(tmp, "w") as f:
            f.write(content_str)

        try:
            skill = agent.skill_store.import_skill(tmp, category="custom")
            return {"imported": skill.name, "path": skill.source_path}
        except Exception as e:
            raise HTTPException(400, f"Import failed: {e}")
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)

    class SkillCreateRequest(BaseModel):
        name: str
        content: str
        category: str = "custom"

    @router.post("/skills")
    async def create_skill(req: SkillCreateRequest):
        """Create a new skill from YAML content."""
        path = agent.skill_store.save_skill(req.name, req.content, req.category)
        return {"name": req.name, "path": path}

    @router.delete("/skills/{name}")
    async def delete_skill(name: str):
        """Delete a skill."""
        if agent.skill_store.delete_skill(name):
            return {"deleted": name}
        raise HTTPException(404, f"Skill not found or is builtin: {name}")

    # ── SSH Channel ─────────────────────────────────────────────

    @router.get("/ssh/status")
    async def ssh_status():
        """Get SSH connection status."""
        return agent.ssh.get_status()

    class SSHConnectRequest(BaseModel):
        ip: Optional[str] = None
        port: int = 22
        username: str = "root"
        password: Optional[str] = None

    @router.post("/ssh/connect")
    async def ssh_connect(req: SSHConnectRequest = None):
        """Connect to target machine via SSH."""
        if req and req.ip:
            agent.ssh.target_ip = req.ip
            agent.ssh.port = req.port
            agent.ssh.username = req.username
            if req.password:
                agent.ssh.password = req.password

        success = await agent.ssh.connect()
        if success:
            return {"status": "connected", **agent.ssh.get_status()}
        raise HTTPException(503, "SSH connection failed")

    @router.post("/ssh/disconnect")
    async def ssh_disconnect():
        """Disconnect SSH."""
        await agent.ssh.close()
        return {"status": "disconnected"}

    class SSHCommandRequest(BaseModel):
        command: str
        timeout: int = 300

    class WorkloadConsoleRequest(BaseModel):
        command: str

    class WorkloadCreateRequest(BaseModel):
        id: str = Field(..., description="Directory name (letters, digits, underscores, hyphens)")
        name: str = Field(..., description="Display name")
        type: str = Field("custom", description="Workload type label")
        command: str = Field("", description="Launch command")
        port: Optional[int] = Field(None, description="Listening port for status detection")

    @router.post("/ssh/exec")
    async def ssh_exec(req: SSHCommandRequest):
        """Execute a command on the target machine via SSH."""
        return await _run_direct_ssh(
            req.command,
            timeout=req.timeout,
            source_type="direct_ssh_exec",
            metadata={"entrypoint": "ssh_exec"},
        )

    @router.get("/workloads")
    async def get_workloads():
        """Get list of managed workloads."""
        return await agent.list_workloads()

    @router.post("/workloads")
    async def create_workload(req: WorkloadCreateRequest):
        """Create a new workload manifest on the target machine."""
        try:
            return await agent.create_workload(
                workload_id=req.id,
                name=req.name,
                workload_type=req.type,
                command=req.command,
                port=req.port,
            )
        except ValueError as e:
            raise HTTPException(400, str(e))
        except ConnectionError as e:
            raise HTTPException(503, str(e))

    @router.get("/workloads/{workload_id}")
    async def get_workload_detail(workload_id: str):
        """Get one workload plus a fresh status snapshot."""
        try:
            return await agent.workload_status(workload_id)
        except ValueError as e:
            raise HTTPException(404, str(e))
        except PermissionError as e:
            raise HTTPException(403, str(e))
        except ConnectionError as e:
            raise HTTPException(503, str(e))

    @router.get("/workloads/{workload_id}/logs")
    async def get_workload_logs(workload_id: str, lines: int = 80):
        """Fetch recent workload logs."""
        try:
            return await agent.workload_logs(workload_id, lines=lines)
        except ValueError as e:
            raise HTTPException(404, str(e))
        except PermissionError as e:
            raise HTTPException(403, str(e))
        except ConnectionError as e:
            raise HTTPException(503, str(e))

    @router.get("/workloads/{workload_id}/console")
    async def get_workload_console(workload_id: str):
        """Get console capability info for one workload."""
        try:
            return await agent.get_workload_console_info(workload_id)
        except ValueError as e:
            raise HTTPException(404, str(e))
        except RuntimeError as e:
            raise HTTPException(409, str(e))
        except PermissionError as e:
            raise HTTPException(403, str(e))
        except ConnectionError as e:
            raise HTTPException(503, str(e))

    @router.post("/workloads/{workload_id}/console/setup")
    async def setup_workload_console(workload_id: str):
        """Enable the best supported console transport for one workload."""
        try:
            return await agent.setup_workload_console(workload_id)
        except ValueError as e:
            raise HTTPException(404, str(e))
        except RuntimeError as e:
            raise HTTPException(409, str(e))
        except PermissionError as e:
            raise HTTPException(403, str(e))
        except ConnectionError as e:
            raise HTTPException(503, str(e))

    @router.post("/workloads/{workload_id}/console")
    async def exec_workload_console(workload_id: str, req: WorkloadConsoleRequest):
        """Send one console command to a managed workload."""
        try:
            return await agent.exec_workload_console(workload_id, req.command)
        except ValueError as e:
            raise HTTPException(400, str(e))
        except RuntimeError as e:
            raise HTTPException(409, str(e))
        except PermissionError as e:
            raise HTTPException(403, str(e))
        except ConnectionError as e:
            raise HTTPException(503, str(e))

    @router.post("/workloads/{workload_id}/{action}")
    async def control_workload(workload_id: str, action: str):
        """Directly control one workload from the manual dashboard."""
        if action not in {"start", "stop", "restart"}:
            raise HTTPException(404, "Unsupported workload action")
        try:
            return await agent.control_workload(workload_id, action)
        except ValueError as e:
            raise HTTPException(404, str(e))
        except PermissionError as e:
            raise HTTPException(403, str(e))
        except ConnectionError as e:
            raise HTTPException(503, str(e))

    @router.websocket("/ssh/stream")
    async def ssh_stream(ws: WebSocket):
        """Stream SSH command output via WebSocket."""
        await ws.accept()
        try:
            while True:
                data = await ws.receive_json()
                command = data.get("command", "")
                timeout = data.get("timeout", 300)

                if agent.plan_executor.has_active_run:
                    await ws.send_json({"type": "error", "data": "A backend plan run is active"})
                    continue

                if not agent.ssh.has_shell:
                    await ws.send_json({"type": "error", "data": "SSH not connected"})
                    continue

                decision, _ = agent.evaluate_policy(
                    "shell.exec",
                    {"command": command, "timeout": timeout},
                    source_type="direct_ssh_stream",
                    metadata={"entrypoint": "ssh_stream"},
                )
                if decision.action != PolicyAction.ALLOW:
                    await ws.send_json({
                        "type": "error",
                        "data": decision.reason,
                        "risk_level": decision.risk_level.value,
                        "action": decision.action.value,
                    })
                    continue

                try:
                    full_output = ""
                    success = True
                    exit_status = 0
                    async for stream_type, chunk in agent.ssh.run_stream(command, timeout=timeout):
                        if stream_type == "done":
                            success = chunk.get("success", True)
                            exit_status = chunk.get("exit_status")
                            continue
                        full_output += chunk
                        await ws.send_json({"type": stream_type, "data": chunk})
                    await ws.send_json({
                        "type": "done",
                        "data": full_output,
                        "success": success,
                        "exit_status": exit_status,
                    })
                except Exception as e:
                    await ws.send_json({"type": "error", "data": str(e)})

        except WebSocketDisconnect:
            pass
        except Exception:
            pass

    # ── Recording ───────────────────────────────────────────────

    class RecordRequest(BaseModel):
        name: str

    @router.post("/record/start")
    async def record_start(req: RecordRequest):
        """Start recording operations."""
        agent.recorder.start(req.name)
        return {"status": "recording", "name": req.name}

    @router.post("/record/stop")
    async def record_stop():
        """Stop recording and return the generated skill."""
        skill_data = await agent.recorder.stop()

        # Auto-save to skill store
        import yaml as _yaml
        name = skill_data.get("skill", {}).get("name", "recorded")
        content = _yaml.dump(skill_data, default_flow_style=False, allow_unicode=True)
        agent.skill_store.save_skill(name, content, category="custom")

        return {"skill": skill_data, "saved_as": name}

    @router.get("/record/status")
    async def record_status():
        """Get recorder status."""
        return agent.recorder.get_status()

    # ── WebSocket ───────────────────────────────────────────────

    @router.websocket("/ws")
    async def agent_websocket(websocket: WebSocket):
        """Real-time agent events (task progress, triggers, screenshots)."""
        await websocket.accept()
        logger.info("Agent WebSocket connected")

        async def ws_callback(data: dict):
            try:
                await websocket.send_json(data)
            except Exception:
                pass

        agent.executor.add_ws_callback(ws_callback)
        agent.plan_executor.add_ws_callback(ws_callback)
        subscriber_id, runtime_queue = agent.event_hub.subscribe(
            matcher=lambda event: event.stream == "session"
        )

        async def forward_runtime_events():
            recent = agent.event_hub.recent(limit=20, matcher=lambda event: event.stream == "session")
            seen_ids = set()
            for event in recent:
                seen_ids.add(event.event_id)
                await websocket.send_json({"type": "runtime_event", "event": event.model_dump()})
            while True:
                event = await runtime_queue.get()
                if event.event_id in seen_ids:
                    continue
                seen_ids.add(event.event_id)
                await websocket.send_json({"type": "runtime_event", "event": event.model_dump()})

        runtime_task = asyncio.create_task(forward_runtime_events())

        try:
            while True:
                msg = await websocket.receive_json()
                # Handle client messages (confirm, override, etc.)
                msg_type = msg.get("type")
                if msg_type == "confirm":
                    run_id = msg.get("run_id")
                    approved = bool(msg.get("approved", False))
                    if run_id:
                        try:
                            await agent.plan_executor.confirm_step(run_id, approved)
                        except RuntimeError:
                            await websocket.send_json({
                                "type": "plan_run",
                                "event": "confirmation_error",
                                "run_id": run_id,
                            })
                elif msg_type == "ping":
                    await websocket.send_json({"type": "pong"})
        except WebSocketDisconnect:
            logger.info("Agent WebSocket disconnected")
        finally:
            runtime_task.cancel()
            try:
                await runtime_task
            except asyncio.CancelledError:
                pass
            agent.event_hub.unsubscribe(subscriber_id)
            agent.executor.remove_ws_callback(ws_callback)
            agent.plan_executor.remove_ws_callback(ws_callback)

    return router, agent
