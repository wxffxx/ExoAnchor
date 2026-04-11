"""
ExoAnchor Agent — 综合功能测试

覆盖以下模块:
  1. Workload 管理 (list / get / create / control)
  2. Safety / Policy 引擎
  3. 运行时解析 (LLM response parsing)
  4. Session Runtime
  5. Workload Resolution (intent routing)
  6. API 路由层 (FastAPI TestClient)

运行: python -m pytest tests/test_agent_integration.py -v
"""

import asyncio
import json
import os
import re
import shutil
import tempfile
import time
import unittest
from typing import Any, Optional
from unittest.mock import AsyncMock, MagicMock, patch

# ── Workload helpers ───────────────────────────────────
from exoanchor.runtime.workloads import (
    build_workload_context_block,
    build_workload_logs_command,
    build_workload_start_command,
    build_workload_status_command,
    build_workload_stop_command,
    build_existing_workload_plan,
    build_generic_workload_plan,
    detect_generic_workload_action,
    extract_requested_player_count,
    is_minecraft_workload,
    request_targets_existing_workload,
    resolve_missing_task_details,
    resolve_workload_reference,
    score_workload_match,
    workload_remote_dir,
)

# ── Runtime intent / parsing ──────────────────────────
from exoanchor.runtime.parsing import (
    heuristic_force_plan,
    is_clarifying_chat_result,
    is_echo_chat_result,
    parse_llm_response,
)

# ── Safety ────────────────────────────────────────────
from exoanchor.safety import PolicyEngine, SafetyGuard

# ── Session Runtime ───────────────────────────────────
from exoanchor.runtime.sessions import SessionStore, SessionRuntime
from exoanchor.runtime.events import EventHub


# ═══════════════════════════════════════════════════════
#  Fake / Mock helpers
# ═══════════════════════════════════════════════════════

MINECRAFT_WORKLOAD = {
    "id": "spigot-server",
    "dir": "spigot-server",
    "name": "Minecraft (Spigot)",
    "path": "/home/testuser/.exoanchor/workloads/spigot-server",
    "base_dir": "exoanchor",
    "command": "cd /home/testuser/.exoanchor/workloads/spigot-server && ./launch.sh",
    "port": 25565,
    "status": "running",
    "type": "minecraft",
}

CUSTOM_WORKLOAD = {
    "id": "api-backend",
    "dir": "api-backend",
    "name": "Internal API Service",
    "path": "/home/testuser/.exoanchor/workloads/api-backend",
    "base_dir": "exoanchor",
    "command": "./start.sh",
    "port": 8080,
    "status": "running",
}

STOPPED_WORKLOAD = {
    "id": "web-frontend",
    "dir": "web-frontend",
    "name": "Web Frontend",
    "path": "/home/testuser/.exoanchor/workloads/web-frontend",
    "base_dir": "exoanchor",
    "command": "npm start",
    "port": 3000,
    "status": "stopped",
}


# ═══════════════════════════════════════════════════════
#  1. Workload Helper Tests
# ═══════════════════════════════════════════════════════

class TestWorkloadHelpers(unittest.TestCase):
    """Tests for workload resolution and command generation helpers."""

    # ── Remote dir resolution ──
    def test_workload_remote_dir_from_path(self):
        self.assertEqual(
            workload_remote_dir(MINECRAFT_WORKLOAD),
            "/home/testuser/.exoanchor/workloads/spigot-server",
        )

    def test_workload_remote_dir_from_components(self):
        wl = {"dir": "my-app", "base_dir": "cortex"}
        self.assertEqual(workload_remote_dir(wl), "~/.cortex/workloads/my-app")

    def test_workload_remote_dir_empty(self):
        self.assertEqual(workload_remote_dir({}), "")
        self.assertEqual(workload_remote_dir(None), "")

    # ── Minecraft detection ──
    def test_is_minecraft_workload_positive(self):
        self.assertTrue(is_minecraft_workload(MINECRAFT_WORKLOAD))

    def test_is_minecraft_workload_negative(self):
        self.assertFalse(is_minecraft_workload(CUSTOM_WORKLOAD))
        self.assertFalse(is_minecraft_workload(None))

    # ── Command builders ──
    def test_build_start_command_uses_manifest_command(self):
        cmd = build_workload_start_command(MINECRAFT_WORKLOAD)
        self.assertIn("launch.sh", cmd)
        self.assertIn("cd", cmd)

    def test_build_stop_command_includes_kill(self):
        cmd = build_workload_stop_command(MINECRAFT_WORKLOAD)
        self.assertIn("kill", cmd)

    def test_build_logs_command_respects_lines(self):
        cmd = build_workload_logs_command(MINECRAFT_WORKLOAD, lines=100)
        self.assertIn("100", cmd)
        self.assertIn("tail", cmd)

    def test_build_status_command_includes_manifest_check(self):
        cmd = build_workload_status_command(MINECRAFT_WORKLOAD)
        self.assertIn("manifest.json", cmd)

    # ── Player count extraction ──
    def test_extract_player_count_chinese(self):
        self.assertEqual(extract_requested_player_count("把人数改为32"), 32)
        self.assertEqual(extract_requested_player_count("100 players"), 100)

    def test_extract_player_count_missing(self):
        self.assertIsNone(extract_requested_player_count("改一下人数"))

    # ── Request targeting ──
    def test_request_targets_existing_workload_restart(self):
        self.assertTrue(request_targets_existing_workload("重启服务"))

    def test_request_targets_existing_workload_deploy(self):
        # Deploy should NOT target existing
        self.assertFalse(request_targets_existing_workload("部署一个新的Minecraft服务器"))

    # ── Generic action detection ──
    def test_detect_generic_workload_action(self):
        self.assertEqual(detect_generic_workload_action("重启一下"), "restart")
        self.assertEqual(detect_generic_workload_action("查看日志"), "logs")
        self.assertEqual(detect_generic_workload_action("停止 server"), "stop")
        self.assertEqual(detect_generic_workload_action("启动"), "start")
        self.assertEqual(detect_generic_workload_action("检查状态"), "status")
        self.assertEqual(detect_generic_workload_action("你好"), "")

    # ── Score matching ──
    def test_score_exact_id_match(self):
        score = score_workload_match("重启 spigot-server", MINECRAFT_WORKLOAD)
        self.assertGreater(score, 0)

    def test_score_domain_hint_match(self):
        score = score_workload_match("查看 minecraft 日志", MINECRAFT_WORKLOAD)
        self.assertGreater(score, 0)

    def test_score_no_match(self):
        score = score_workload_match("做个网站", MINECRAFT_WORKLOAD)
        self.assertEqual(score, 0)

    # ── Context block ──
    def test_build_context_block_includes_all_workloads(self):
        block = build_workload_context_block([MINECRAFT_WORKLOAD, CUSTOM_WORKLOAD])
        self.assertIn("spigot-server", block)
        self.assertIn("api-backend", block)
        self.assertIn("CURRENT TARGET WORKLOADS", block)

    def test_build_context_block_empty(self):
        self.assertEqual(build_workload_context_block([]), "")

    # ── Resolve workload reference ──
    def test_resolve_single_match(self):
        result = resolve_workload_reference(
            "重启 spigot-server",
            [MINECRAFT_WORKLOAD, CUSTOM_WORKLOAD],
            [],
        )
        self.assertEqual(result["action"], "use")
        self.assertEqual(result["workload"]["id"], "spigot-server")

    def test_resolve_ambiguous_asks(self):
        result = resolve_workload_reference(
            "重启服务",  # ambiguous — doesn't name a specific workload
            [MINECRAFT_WORKLOAD, CUSTOM_WORKLOAD],
            [],
        )
        # Should either ask or use — both are valid
        self.assertIn(result["action"], ("ask", "use"))

    def test_resolve_deploy_ignores_existing(self):
        result = resolve_workload_reference(
            "部署一个新的 Redis",
            [MINECRAFT_WORKLOAD],
            [],
        )
        self.assertEqual(result["action"], "ignore")

    # ── Missing detail resolution ──
    def test_resolve_missing_player_count(self):
        msg = resolve_missing_task_details("改一下人数", MINECRAFT_WORKLOAD)
        self.assertIn("最大玩家人数改成多少", msg)

    def test_resolve_missing_port_value(self):
        msg = resolve_missing_task_details("改一下端口", MINECRAFT_WORKLOAD)
        self.assertIn("端口改成多少", msg)

    # ── Plan generation ──
    def test_build_existing_workload_plan_player_update(self):
        plan = build_existing_workload_plan("把人数改为64", MINECRAFT_WORKLOAD)
        self.assertIsNotNone(plan)
        self.assertEqual(plan["type"], "plan")
        self.assertIn("max-players=64", plan["steps"][0]["command"])

    def test_build_generic_workload_plan_restart(self):
        plan = build_generic_workload_plan("重启", CUSTOM_WORKLOAD)
        self.assertIsNotNone(plan)
        self.assertEqual(plan["type"], "plan")
        self.assertEqual(len(plan["steps"]), 3)  # stop + start + verify


# ═══════════════════════════════════════════════════════
#  2. LLM Response Parsing Tests
# ═══════════════════════════════════════════════════════

class TestLLMResponseParsing(unittest.TestCase):
    """Tests for LLM response parsing logic."""

    def test_parse_json_chat_response(self):
        raw = '{"type": "chat", "message": "你好！有什么可以帮你的？"}'
        result = parse_llm_response(raw)
        self.assertEqual(result["type"], "chat")
        self.assertEqual(result["message"], "你好！有什么可以帮你的？")

    def test_parse_json_ssh_response(self):
        raw = '{"type": "ssh", "command": "ls -la /tmp"}'
        result = parse_llm_response(raw)
        self.assertEqual(result["type"], "ssh")
        self.assertEqual(result["command"], "ls -la /tmp")

    def test_parse_json_plan_response(self):
        raw = json.dumps({
            "type": "plan",
            "goal": "Deploy web server",
            "steps": [
                {"id": 1, "description": "Install nginx", "command": "sudo apt install nginx"}
            ]
        })
        result = parse_llm_response(raw)
        self.assertEqual(result["type"], "plan")
        self.assertEqual(len(result["steps"]), 1)

    def test_parse_markdown_wrapped_json(self):
        raw = "```json\n{\"type\": \"chat\", \"message\": \"Hello\"}\n```"
        result = parse_llm_response(raw)
        self.assertEqual(result["type"], "chat")

    def test_parse_invalid_json_fallback(self):
        raw = "I'm not sure what you mean. Can you clarify?"
        result = parse_llm_response(raw)
        self.assertEqual(result["type"], "chat")
        self.assertIn("not sure", result["message"])

    def test_is_echo_chat_result_detects_echo(self):
        result = {"type": "chat", "message": "部署最新版 Spigot Minecraft 服务器"}
        self.assertTrue(is_echo_chat_result(result, "部署最新版 Spigot Minecraft 服务器"))

    def test_is_echo_chat_result_genuine_reply(self):
        result = {"type": "chat", "message": "好的，我来帮你部署"}
        self.assertFalse(is_echo_chat_result(result, "部署 Minecraft"))

    def test_is_clarifying_chat_result_yes(self):
        result = {"type": "chat", "message": "你想把端口改成多少？"}
        self.assertTrue(is_clarifying_chat_result(result))

    def test_heuristic_force_plan_deploy(self):
        self.assertTrue(heuristic_force_plan("部署最新版 Spigot Minecraft 服务器")["force"])

    def test_heuristic_force_plan_normal_chat(self):
        self.assertFalse(heuristic_force_plan("你好")["force"])


# ═══════════════════════════════════════════════════════
#  3. Safety / Policy Tests
# ═══════════════════════════════════════════════════════

class TestSafetyGuard(unittest.TestCase):
    """Tests for the Safety Guard and Policy Engine."""

    def test_safety_guard_default_config(self):
        guard = SafetyGuard({})
        self.assertIsNotNone(guard)

    def test_policy_engine_creation(self):
        engine = PolicyEngine({})
        self.assertIsNotNone(engine)

    def test_policy_engine_evaluate_safe_command(self):
        engine = PolicyEngine({"require_confirmation": False})
        decision = engine.evaluate_tool_call(
            "shell.exec",
            {"command": "ls -la", "timeout": 30},
            source_type="manual_workload_panel",
            agent_mode="manual",
        )
        # Safe commands should be allowed
        self.assertIn(decision.action.value, ("allow", "confirm"))

    def test_policy_engine_evaluate_dangerous_command(self):
        engine = PolicyEngine({"require_confirmation": True})
        decision = engine.evaluate_tool_call(
            "shell.exec",
            {"command": "rm -rf /", "timeout": 30},
            source_type="automated",
            agent_mode="semi_active",
        )
        # Destructive commands should at least be flagged
        self.assertIsNotNone(decision.risk_level)


# ═══════════════════════════════════════════════════════
#  4. Session Runtime Tests
# ═══════════════════════════════════════════════════════

class TestSessionRuntime(unittest.IsolatedAsyncioTestCase):
    """Tests for the Session Runtime system."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.session_store = SessionStore(self.tmpdir)
        self.event_hub = EventHub()
        self.runtime = SessionRuntime(self.session_store, self.event_hub)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    async def test_create_session(self):
        session = self.runtime.create("Test Session")
        self.assertIsNotNone(session)
        self.assertEqual(session.title, "Test Session")
        self.assertIsNotNone(session.session_id)

    async def test_list_sessions(self):
        self.runtime.create("Session A")
        self.runtime.create("Session B")
        sessions = self.runtime.list_all()
        self.assertGreaterEqual(len(sessions), 2)

    async def test_get_session(self):
        session = self.runtime.create("Lookup Test")
        fetched = self.runtime.get(session.session_id)
        self.assertIsNotNone(fetched)
        self.assertEqual(fetched.title, "Lookup Test")


# ═══════════════════════════════════════════════════════
#  5. Event Hub Tests
# ═══════════════════════════════════════════════════════

class TestEventHub(unittest.IsolatedAsyncioTestCase):
    """Tests for the EventHub pub/sub system."""

    async def test_publish_and_receive(self):
        hub = EventHub()
        events = []

        async def listener(event):
            events.append(event)

        hub.subscribe(listener)
        await hub.publish_raw({
            "stream": "test",
            "entity_kind": "test",
            "entity_id": "t1",
            "event_type": "test_event",
            "payload": {"hello": "world"},
        })

        # Give the event loop a moment
        await asyncio.sleep(0.05)
        self.assertEqual(len(events), 1)


# ═══════════════════════════════════════════════════════
#  6. API Integration Tests (with TestClient)
# ═══════════════════════════════════════════════════════

class TestAPIIntegration(unittest.TestCase):
    """Integration tests for the ExoAnchor API using FastAPI TestClient.
    
    These tests verify:
    - Status endpoint
    - Mode switching
    - Skill listing
    - Workload endpoints (list, create)
    """

    @classmethod
    def setUpClass(cls):
        try:
            from fastapi.testclient import TestClient
            from exoanchor.action.adapters import MockGPIOAdapter, MockHIDAdapter, MockVideoAdapter
            from exoanchor.api.routes import create_agent_router

            cls._skip = False
        except ImportError as e:
            cls._skip = True
            cls._skip_reason = str(e)
            return

        config = {
            "mode": "manual",
            "target": {
                "ip": "",
                "ssh": {"port": 22, "username": "test"},
            },
            "passive": {"poll_interval": 60, "services": {}},
            "vision": {"backend": "local"},
            "safety": {"max_steps": 10, "max_duration": 60, "require_confirmation": False},
            "skills_dir": tempfile.mkdtemp(),
        }

        hid = MockHIDAdapter()
        video = MockVideoAdapter()
        gpio = MockGPIOAdapter()
        router, cls.agent = create_agent_router(hid, video, gpio, config)

        from fastapi import FastAPI
        app = FastAPI()
        app.include_router(router, prefix="/api/agent")
        cls.client = TestClient(app)

    def setUp(self):
        if getattr(self.__class__, '_skip', False):
            self.skipTest(self.__class__._skip_reason)

    def test_status_endpoint(self):
        resp = self.client.get("/api/agent/status")
        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        self.assertIn("mode", data)
        self.assertIn("ssh_connected", data)
        self.assertIn("uptime", data)

    def test_mode_endpoint(self):
        resp = self.client.post("/api/agent/mode", json={"mode": "manual"})
        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        self.assertEqual(data["new_mode"], "manual")

    def test_workloads_list_empty_without_ssh(self):
        resp = self.client.get("/api/agent/workloads")
        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        # No SSH = empty list
        self.assertEqual(data, [])

    def test_workloads_create_fails_without_ssh(self):
        resp = self.client.post("/api/agent/workloads", json={
            "id": "test-wl",
            "name": "Test Workload",
            "type": "custom",
        })
        self.assertEqual(resp.status_code, 503)  # SSH not connected

    def test_workloads_create_validation_bad_id(self):
        # Patch SSH to appear connected
        original_has_shell = self.agent.ssh.has_shell
        self.agent.ssh.has_shell = True
        try:
            resp = self.client.post("/api/agent/workloads", json={
                "id": "bad id with spaces!!",
                "name": "Bad Workload",
            })
            self.assertEqual(resp.status_code, 400)
            self.assertIn("Invalid workload id", resp.json().get("detail", ""))
        finally:
            self.agent.ssh.has_shell = original_has_shell

    def test_skills_list_endpoint(self):
        resp = self.client.get("/api/agent/skills")
        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        self.assertIsInstance(data, list)

    def test_triggers_endpoint(self):
        resp = self.client.get("/api/agent/triggers")
        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        self.assertIsInstance(data, list)


# ═══════════════════════════════════════════════════════
#  7. Summary report
# ═══════════════════════════════════════════════════════

if __name__ == "__main__":
    unittest.main(verbosity=2)
