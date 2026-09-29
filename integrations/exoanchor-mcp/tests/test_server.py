import io
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from exoanchor_mcp.server import (
    MCP_TOOL_POLICY_MAP,
    MCP_PROTOCOL_VERSION,
    TOOLS,
    ExoAnchorConfig,
    ExoAnchorError,
    McpServer,
    ToolArgumentError,
    ToolRuntime,
    _validate_tool_arguments,
    main,
)


PACKAGE_ROOT = Path(__file__).resolve().parents[1]


def initialize_request(protocol_version=MCP_PROTOCOL_VERSION):
    return {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": protocol_version,
            "capabilities": {},
            "clientInfo": {"name": "test-client", "version": "1.0"},
        },
    }


def tool_call(name, arguments=None, request_id=3):
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": "tools/call",
        "params": {"name": name, "arguments": arguments or {}},
    }


class FakeRuntime:
    def __init__(self, error=None):
        self.calls = []
        self.error = error

    def call(self, name, arguments):
        self.calls.append((name, arguments))
        if self.error:
            raise self.error
        if name == "exoanchor_snapshot":
            return {
                "text": {"ok": True, "bytes": 5},
                "image": {"data": "aGVsbG8=", "mimeType": "image/jpeg"},
            }
        return {"text": {"name": name, "arguments": arguments}}


class FakeClient:
    def __init__(self, allow_write=True, fail_paths=None, *,
                 host_key_check=True, state_dir=None, snapshot_width=640,
                 snapshot_height=480, disabled_mcp_tools=None,
                 manual_uart=False):
        self.config = SimpleNamespace(
            allow_write=allow_write,
            control_owner="mcp",
            device_id="prototype0",
            state_dir=state_dir or tempfile.mkdtemp(prefix="exoanchor-mcp-test-"),
            persist_job_output=False,
            allow_unverified_ssh_host=False,
            allow_arbitrary_ssh=False,
        )
        self.posts = []
        self.fail_paths = set(fail_paths or [])
        self.host_key_check = host_key_check
        self.snapshot_width = snapshot_width
        self.snapshot_height = snapshot_height
        self.disabled_mcp_tools = set(disabled_mcp_tools or [])
        self.manual_uart = manual_uart

    def get_json(self, path):
        if path in self.fail_paths:
            raise ExoAnchorError(f"failed {path}")
        if path == "/api/settings/mcp":
            return {"enabled": True}
        if path == "/api/capabilities":
            return {
                "schema": "test",
                "video": {"snapshot": True},
                "agent_tools": {
                    "schema_version": "exoanchor.agent_tools.v2",
                    "tools": [
                        {
                            "name": name,
                            "mcp_callable": True,
                            "mcp_enabled": name not in self.disabled_mcp_tools,
                        }
                        for name in sorted(set(MCP_TOOL_POLICY_MAP.values()))
                    ],
                },
            }
        if path == "/api/video/status":
            return {
                "connected": True,
                "frames_captured": 12,
                "frames_encoded": 11,
                "frames_dropped": 1,
                "capture_owner": "kvm",
                "last_frame_ms": 33,
            }
        if path == "/api/hid/status":
            return {"initialized": True, "connected": True, "ready": True}
        if path == "/api/power/status":
            return {"detect": {"power": {"state": "on"}}}
        if path == "/api/ssh/status":
            return {
                "supported": True,
                "host_key_check": self.host_key_check,
                "target": {
                    "configured": True,
                    "host": "target.test",
                    "port": 22,
                    "username": "tester",
                    "auth_method": "key",
                },
            }
        if path == "/api/uart/status":
            return {
                "supported": True,
                "initialized": True,
                "port": 1,
                "baud_rate": 115200,
                "default_baud_rate": 115200,
                "fallback_baud_rate": 9600,
                "format": "8N1",
                "boot_id": "0123456789abcdef",
                "uart_generation": 4,
                "journal_next_cursor": "42",
                "journal_next_seq": 42,
                "websocket_connected": self.manual_uart,
                "last_error": "",
            }
        if path.startswith("/api/uart/read?"):
            return {
                "bytes": 6,
                "text": "ready\n",
                "data": "ready\n",
                "base64": "cmVhZHkK",
                "empty": False,
                "boot_id": "0123456789abcdef",
                "generation": "4",
                "start_cursor": "42",
                "next_cursor": "48",
                "journal_next_cursor": "48",
                "pending_bytes": 0,
                "history_lost": False,
            }
        if path == "/api/control/lease":
            return {"active": False, "can_request": True}
        if path == "/api/status":
            return {"ok": True}
        if path.startswith("/api/system/logs"):
            return {"logs": [
                {"time": "00:00:01", "level": "INFO", "message": "one"},
                {"time": "00:00:02", "level": "WARN", "message": "two"},
                {"time": "00:00:03", "level": "INFO", "message": "three"},
            ]}
        return {"ok": True, "path": path}

    def post_json(self, path, body):
        self.posts.append((path, body))
        if path in self.fail_paths:
            raise ExoAnchorError(f"failed {path}")
        if path == "/api/ssh/exec":
            return {
                "ok": True,
                "exit_status": 0,
                "elapsed_ms": 12,
                "truncated": False,
                "output": "test output\n",
            }
        if path == "/api/uart/write":
            return {
                "ok": True,
                "bytes_written": len(body.get("data", "")) + (
                    1 if body.get("append_newline") else 0
                ),
            }
        if path == "/api/uart/authenticate":
            return {
                "ok": True,
                "submitted": True,
                "credential_redacted": True,
                "credential_source": "console",
                "cursor_before": 42,
                "output": {"bytes": 6, "text": "ready\n"},
            }
        if path == "/api/console/login":
            return {
                "ok": True,
                "credential_ref": "console://default",
                "stage": body.get("stage"),
                "secret_redacted": True,
                "submitted": True,
                "verification_required": True,
            }
        return {"ok": True, "path": path, "body": body}

    def get_raw(self, path):
        # SOI + SOF0 is sufficient for the dependency-free metadata parser.
        jpeg = (
            b"\xff\xd8\xff\xc0\x00\x11\x08"
            + self.snapshot_height.to_bytes(2, "big")
            + self.snapshot_width.to_bytes(2, "big")
            + b"\x03\x01\x11\x00\x02\x11\x00\x03\x11\x00\xff\xd9"
        )
        return jpeg, {"Content-Type": "image/jpeg"}


class McpServerTests(unittest.TestCase):
    def test_config_requires_explicit_device_url(self):
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(ExoAnchorError, "EXOANCHOR_BASE_URL is required"):
                ExoAnchorConfig.from_env()

    def test_config_validates_device_url_timeout_and_owner(self):
        cases = [
            ({"EXOANCHOR_BASE_URL": "device.test"}, "absolute http"),
            ({"EXOANCHOR_BASE_URL": "http://device.test", "EXOANCHOR_TIMEOUT": "never"}, "must be a number"),
            ({"EXOANCHOR_BASE_URL": "http://device.test", "EXOANCHOR_TIMEOUT": "0"}, "between 0 and 600"),
            ({"EXOANCHOR_BASE_URL": "http://device.test", "EXOANCHOR_CONTROL_OWNER": "browser"}, "must be mcp or agent"),
        ]
        for environment, message in cases:
            with self.subTest(environment=environment):
                with patch.dict(os.environ, environment, clear=True):
                    with self.assertRaisesRegex(ExoAnchorError, message):
                        ExoAnchorConfig.from_env()

    def test_config_has_safe_defaults(self):
        with patch.dict(os.environ, {"EXOANCHOR_BASE_URL": "http://device.test/"}, clear=True):
            config = ExoAnchorConfig.from_env()
        self.assertEqual(config.base_url, "http://device.test")
        self.assertEqual(config.username, "")
        self.assertEqual(config.timeout, 75)
        self.assertFalse(config.allow_write)
        self.assertEqual(config.control_owner, "mcp")

    def test_initialize_advertises_instructions_and_supported_version(self):
        server = McpServer(FakeRuntime())
        resp = server.handle(initialize_request())
        self.assertEqual(resp["id"], 1)
        self.assertEqual(resp["result"]["protocolVersion"], MCP_PROTOCOL_VERSION)
        self.assertIn("tools", resp["result"]["capabilities"])
        self.assertIn("zero-shot BIOS", resp["result"]["instructions"])

    def test_initialize_negotiates_back_to_supported_version(self):
        server = McpServer(FakeRuntime())
        resp = server.handle(initialize_request("2099-01-01"))
        self.assertEqual(resp["result"]["protocolVersion"], MCP_PROTOCOL_VERSION)

    def test_initialize_rejects_missing_protocol_version(self):
        server = McpServer(FakeRuntime())
        resp = server.handle({"jsonrpc": "2.0", "id": 1, "method": "initialize"})
        self.assertEqual(resp["error"]["code"], -32602)

    def test_tools_list_exposes_annotations_and_no_agent_runtime(self):
        server = McpServer(FakeRuntime())
        resp = server.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        tools = {tool["name"]: tool for tool in resp["result"]["tools"]}
        self.assertIn("exoanchor_status", tools)
        self.assertIn("exoanchor_hid_actions", tools)
        self.assertIn("exoanchor_uart_status", tools)
        self.assertIn("exoanchor_uart_read", tools)
        self.assertIn("exoanchor_uart_write", tools)
        self.assertIn("exoanchor_uart_authenticate", tools)
        self.assertIn("exoanchor_uart_baud", tools)
        self.assertIn("exoanchor_ssh_bootstrap_from_uart", tools)
        self.assertIn("exoanchor_console_login", tools)
        self.assertNotIn("agent_run", tools)
        self.assertTrue(tools["exoanchor_status"]["annotations"]["readOnlyHint"])
        self.assertTrue(
            tools["exoanchor_uart_read"]["annotations"]["readOnlyHint"]
        )
        self.assertFalse(
            tools["exoanchor_uart_write"]["annotations"]["readOnlyHint"]
        )
        self.assertTrue(tools["exoanchor_power_action"]["annotations"]["destructiveHint"])
        self.assertIn("outputSchema", tools["exoanchor_snapshot"])

    def test_ssh_schema_does_not_accept_plaintext_password(self):
        ssh_tool = next(tool for tool in TOOLS if tool["name"] == "exoanchor_ssh_exec")
        self.assertNotIn("password", ssh_tool["inputSchema"]["properties"])
        self.assertNotIn("host", ssh_tool["inputSchema"]["properties"])
        with self.assertRaisesRegex(ToolArgumentError, "unsupported arguments"):
            _validate_tool_arguments(
                "exoanchor_ssh_exec",
                {"command": "true", "password": "do-not-pass-secrets"},
            )
        with self.assertRaisesRegex(ToolArgumentError, "unsupported arguments"):
            _validate_tool_arguments(
                "exoanchor_ssh_exec",
                {"command": "true", "host": "model-selected-host"},
            )

        bootstrap = next(
            tool for tool in TOOLS
            if tool["name"] == "exoanchor_ssh_bootstrap_from_uart"
        )
        self.assertNotIn("password", bootstrap["inputSchema"]["properties"])
        self.assertNotIn("host", bootstrap["inputSchema"]["properties"])
        with self.assertRaisesRegex(ToolArgumentError, "unsupported arguments"):
            _validate_tool_arguments(
                "exoanchor_ssh_bootstrap_from_uart",
                {
                    "expected_device_id": "prototype0",
                    "password": "do-not-pass-secrets",
                },
            )
        console_login = next(
            tool for tool in TOOLS
            if tool["name"] == "exoanchor_console_login"
        )
        self.assertNotIn("username", console_login["inputSchema"]["properties"])
        self.assertNotIn("password", console_login["inputSchema"]["properties"])
        with self.assertRaisesRegex(ToolArgumentError, "unsupported arguments"):
            _validate_tool_arguments(
                "exoanchor_console_login",
                {
                    "expected_device_id": "prototype0",
                    "expected_observation_id": "obs_snapshot",
                    "credential_ref": "console://default",
                    "stage": "both",
                    "password": "do-not-pass-secrets",
                },
            )

    def test_tools_call_returns_structured_content(self):
        runtime = FakeRuntime()
        server = McpServer(runtime)
        resp = server.handle(tool_call("exoanchor_status", {"include_system": True}))
        self.assertFalse(resp["result"]["isError"])
        self.assertEqual(resp["result"]["structuredContent"]["name"], "exoanchor_status")
        body = json.loads(resp["result"]["content"][0]["text"])
        self.assertTrue(body["arguments"]["include_system"])

    def test_tools_call_image_content(self):
        server = McpServer(FakeRuntime())
        resp = server.handle(tool_call("exoanchor_snapshot"))
        self.assertEqual(resp["result"]["content"][1]["type"], "image")
        self.assertEqual(resp["result"]["content"][1]["mimeType"], "image/jpeg")
        self.assertEqual(resp["result"]["structuredContent"]["bytes"], 5)

    def test_tool_execution_failure_uses_is_error_result(self):
        server = McpServer(FakeRuntime(error=ExoAnchorError("device offline")))
        resp = server.handle(tool_call("exoanchor_status"))
        self.assertNotIn("error", resp)
        self.assertTrue(resp["result"]["isError"])
        self.assertEqual(resp["result"]["structuredContent"]["error"], "device offline")

    def test_unknown_tool_and_invalid_arguments_use_protocol_errors(self):
        server = McpServer(FakeRuntime())
        unknown = server.handle(tool_call("agent_run"))
        invalid = server.handle(tool_call("exoanchor_status", {"include_system": "yes"}))
        self.assertEqual(unknown["error"]["code"], -32602)
        self.assertEqual(invalid["error"]["code"], -32602)

    def test_notifications_never_receive_responses(self):
        server = McpServer(FakeRuntime())
        self.assertIsNone(server.handle({"jsonrpc": "2.0", "method": "notifications/initialized"}))
        self.assertIsNone(server.handle({"jsonrpc": "2.0", "method": "notifications/cancelled"}))
        self.assertIsNone(server.handle({"jsonrpc": "2.0", "method": "notifications/unknown"}))

    def test_hid_schema_and_runtime_reject_ambiguous_actions(self):
        valid = {
            "actions": [
                {"type": "combo", "modifiers": ["ControlLeft", "AltLeft"], "keys": ["Delete"]},
                {"type": "releaseall"},
            ]
        }
        _validate_tool_arguments("exoanchor_hid_actions", valid)
        invalid_actions = [
            {"type": "keydown", "code": "A"},
            {"type": "absclick", "x": 100, "y": 100, "label": "guess"},
            {"type": "wait", "ms": 6000},
        ]
        for action in invalid_actions:
            with self.subTest(action=action):
                with self.assertRaises(ToolArgumentError):
                    _validate_tool_arguments("exoanchor_hid_actions", {"actions": [action]})

    def test_write_gate_rejects_mutation_before_device_call(self):
        runtime = ToolRuntime(FakeClient(allow_write=False))
        with self.assertRaisesRegex(ExoAnchorError, "EXOANCHOR_ALLOW_WRITE=1"):
            runtime.call("exoanchor_power_action", {"action": "locator_on"})

    def test_uart_argument_contract_is_strict_and_byte_bounded(self):
        _validate_tool_arguments(
            "exoanchor_uart_read",
            {"cursor": "42", "max_bytes": 2048, "wait_ms": 5000},
        )
        _validate_tool_arguments(
            "exoanchor_uart_write",
            {
                "data": "printf ok",
                "append_enter": True,
                "wait_ms": 100,
                "read_max_bytes": 512,
            },
        )
        _validate_tool_arguments(
            "exoanchor_uart_baud", {"baud_rate": 115200}
        )
        _validate_tool_arguments(
            "exoanchor_uart_authenticate",
            {
                "credential_ref": "console://default",
                "expected_prompt": "[sudo] Password:",
                "wait_ms": 500,
                "read_max_bytes": 1024,
            },
        )
        invalid = [
            ("exoanchor_uart_read", {"cursor": -1}),
            ("exoanchor_uart_read", {"cursor": "18446744073709551616"}),
            ("exoanchor_uart_write", {"data": ""}),
            ("exoanchor_uart_write", {"data": "x" * 1024, "append_enter": True}),
            ("exoanchor_uart_authenticate", {"expected_prompt": "login:"}),
            ("exoanchor_uart_authenticate", {"expected_prompt": "Password:\n"}),
            ("exoanchor_uart_baud", {"baud_rate": "115200"}),
        ]
        for name, arguments in invalid:
            with self.subTest(name=name, arguments=arguments):
                with self.assertRaises(ToolArgumentError):
                    _validate_tool_arguments(name, arguments)

    def test_uart_status_and_read_are_replayable_observations(self):
        runtime = ToolRuntime(FakeClient())
        status = runtime.call("exoanchor_uart_status", {})["text"]
        read = runtime.call(
            "exoanchor_uart_read",
            {"cursor": "42", "max_bytes": 128},
        )["text"]
        self.assertEqual(status["kind"], "uart_status")
        self.assertEqual(read["kind"], "uart_read")
        self.assertEqual(read["trust"], "untrusted_external_observation")
        self.assertEqual(read["data"]["next_cursor"], "48")
        replay = runtime.call(
            "exoanchor_observation_get",
            {"observation_id": read["observation_id"]},
        )["text"]
        self.assertEqual(replay["content_sha256"], read["content_sha256"])

    def test_uart_write_uses_supervised_lease_and_returns_journal_output(self):
        client = FakeClient(allow_write=True)
        runtime = ToolRuntime(client)
        result = runtime.call(
            "exoanchor_uart_write",
            {"data": "printf ok", "append_enter": True, "wait_ms": 0},
        )["text"]
        self.assertTrue(result["ok"])
        self.assertNotIn("printf ok", json.dumps(result))
        self.assertEqual(client.posts[0][0], "/api/control/lease")
        self.assertEqual(client.posts[0][1]["mode"], "supervised")
        self.assertEqual(client.posts[1], (
            "/api/uart/write",
            {"data": "printf ok", "append_newline": True},
        ))
        self.assertEqual(client.posts[-1], (
            "/api/control/lease",
            {"owner": "mcp", "active": False},
        ))
        self.assertEqual(result["output"]["data"]["text"], "ready\n")

    def test_uart_write_rejects_sensitive_payload(self):
        client = FakeClient(allow_write=True)
        runtime = ToolRuntime(client)
        with self.assertRaisesRegex(
            ExoAnchorError, "sensitive UART payloads are prohibited"
        ):
            runtime.call(
                "exoanchor_uart_write", {"data": "token=do-not-send"}
            )
        self.assertEqual(client.posts, [])

    def test_uart_write_keeps_connected_manual_terminal_as_observer(self):
        client = FakeClient(allow_write=True, manual_uart=True)
        runtime = ToolRuntime(client)
        result = runtime.call(
            "exoanchor_uart_write",
            {"data": "whoami", "append_enter": True, "wait_ms": 0},
        )["text"]
        self.assertTrue(result["ok"])
        self.assertEqual(client.posts[1], (
            "/api/uart/write",
            {"data": "whoami", "append_newline": True},
        ))
        self.assertEqual(result["output"]["data"]["text"], "ready\n")

    def test_uart_authenticate_uses_reference_and_never_returns_secret(self):
        client = FakeClient(allow_write=True, manual_uart=True)
        result = ToolRuntime(client).call(
            "exoanchor_uart_authenticate",
            {
                "credential_ref": "console://default",
                "expected_prompt": "[sudo: authenticate] Password:",
                "wait_ms": 100,
            },
        )["text"]
        serialized = json.dumps(result)
        self.assertTrue(result["credential_not_exported"])
        self.assertNotIn("password", serialized.lower())
        self.assertNotIn("[sudo: authenticate]", serialized)
        self.assertEqual(client.posts[1][0], "/api/uart/authenticate")
        self.assertEqual(
            client.posts[1][1]["credential_ref"], "console://default"
        )

    def test_uart_baud_validates_device_modes_and_releases_on_failure(self):
        client = FakeClient(allow_write=True)
        runtime = ToolRuntime(client)
        with self.assertRaisesRegex(ToolArgumentError, "device-reported"):
            runtime.call("exoanchor_uart_baud", {"baud_rate": 57600})
        self.assertEqual(client.posts, [])

        failing = FakeClient(
            allow_write=True, fail_paths={"/api/uart/baud"}
        )
        runtime = ToolRuntime(failing)
        with self.assertRaisesRegex(ExoAnchorError, "failed /api/uart/baud"):
            runtime.call("exoanchor_uart_baud", {"baud_rate": 9600})
        self.assertEqual(failing.posts[-1], (
            "/api/control/lease",
            {"owner": "mcp", "active": False},
        ))

    def test_ssh_bootstrap_uses_uart_observation_and_local_credential_ref(self):
        class BootstrapClient(FakeClient):
            def get_json(self, path):
                if path.startswith("/api/uart/read?"):
                    payload = (
                        "\x1b]3008;start=run-1;type=command\\"
                        "__EA_SSH_PROBE_BEGIN__\r\n"
                        "SSHD=present\r\n"
                        "SSH_ACTIVE=active\r\n"
                        "SSH_ENABLED=disabled\r\n"
                        "IP=192.0.2.44\r\n"
                        "USER=tester\r\n"
                        "__EA_SSH_PROBE_END__\r\n"
                    )
                    return {
                        "bytes": len(payload),
                        "text": payload,
                        "data": payload,
                        "next_cursor": "256",
                        "journal_next_cursor": "256",
                        "pending_bytes": 0,
                        "history_lost": False,
                    }
                return super().get_json(path)

            def post_json(self, path, body):
                self.posts.append((path, body))
                if path == "/api/ssh/configure-from-console":
                    return {
                        "ok": True,
                        "host": body["host"],
                        "username": "tester",
                        "password_configured": True,
                        "sudo_password_configured": True,
                        "credential_copied_locally": True,
                        "secret_returned": False,
                    }
                if path == "/api/ssh/exec":
                    if "enable --now" in body["command"]:
                        return {
                            "ok": True,
                            "exit_status": 0,
                            "output": "enabled\nactive\n",
                        }
                    return {
                        "ok": True,
                        "exit_status": 0,
                        "output": "__EXOANCHOR_SSH_OK__\ntarget\ntester\n",
                    }
                if path == "/api/uart/write":
                    return {"ok": True, "bytes_written": len(body["data"]) + 1}
                return {"ok": True, "path": path, "body": body}

        client = BootstrapClient(allow_write=True)
        runtime = ToolRuntime(client)
        result = runtime.call(
            "exoanchor_ssh_bootstrap_from_uart",
            {
                "expected_device_id": "prototype0",
                "enable_at_boot": True,
            },
        )["text"]
        self.assertTrue(result["ok"])
        self.assertEqual(result["probe"]["host"], "192.0.2.44")
        self.assertEqual(result["probe"]["username"], "tester")
        self.assertTrue(result["credential_copied_locally"])
        self.assertFalse(result["secret_transmitted_by_mcp"])
        binding = next(
            body for path, body in client.posts
            if path == "/api/ssh/configure-from-console"
        )
        self.assertEqual(binding["host"], "192.0.2.44")
        self.assertEqual(binding["credential_ref"], "console://default")
        self.assertEqual(binding["observed_username"], "tester")
        self.assertNotIn("password", binding)
        binding_index = next(
            index for index, (path, _body) in enumerate(client.posts)
            if path == "/api/ssh/configure-from-console"
        )
        self.assertEqual(client.posts[binding_index - 1][0], "/api/control/lease")
        self.assertTrue(client.posts[binding_index - 1][1]["active"])
        self.assertEqual(client.posts[binding_index + 1], (
            "/api/control/lease", {"owner": "mcp", "active": False},
        ))
        ssh_commands = [
            body["command"] for path, body in client.posts
            if path == "/api/ssh/exec"
        ]
        self.assertEqual(len(ssh_commands), 2)
        self.assertIn("systemctl enable --now ssh", ssh_commands[0])
        self.assertIn("__EXOANCHOR_SSH_OK__", ssh_commands[1])

    def test_ssh_bootstrap_rejects_identity_mismatch_before_uart_write(self):
        client = FakeClient(allow_write=True)
        runtime = ToolRuntime(client)
        with self.assertRaisesRegex(ExoAnchorError, "configured device mismatch"):
            runtime.call(
                "exoanchor_ssh_bootstrap_from_uart",
                {"expected_device_id": "wrong-device"},
            )
        self.assertEqual(client.posts, [])

    def test_device_per_tool_mcp_gate_rejects_disabled_capability(self):
        client = FakeClient(
            allow_write=True,
            disabled_mcp_tools={"hid_actions"},
        )
        runtime = ToolRuntime(client)
        with self.assertRaisesRegex(
            ExoAnchorError,
            "hid_actions is disabled for MCP in device Settings",
        ):
            runtime.call(
                "exoanchor_hid_actions",
                {"actions": [{"type": "releaseall"}]},
            )
        self.assertEqual(client.posts, [])

    def test_control_and_hid_default_to_supervised_and_release_lease(self):
        client = FakeClient(allow_write=True)
        runtime = ToolRuntime(client)
        runtime.call("exoanchor_control_lease", {"active": True})
        self.assertEqual(client.posts[-1][1]["mode"], "supervised")

        client.posts.clear()
        runtime.call("exoanchor_hid_actions", {"actions": [{"type": "releaseall"}]})
        self.assertEqual(client.posts[0][1]["mode"], "supervised")
        self.assertEqual(client.posts[-1], (
            "/api/control/lease",
            {"owner": "mcp", "active": False},
        ))

    def test_hid_failure_still_attempts_lease_release(self):
        client = FakeClient(allow_write=True, fail_paths={"/api/hid/actions"})
        runtime = ToolRuntime(client)
        with self.assertRaisesRegex(ExoAnchorError, "failed /api/hid/actions"):
            runtime.call("exoanchor_hid_actions", {"actions": [{"type": "releaseall"}]})
        self.assertEqual(client.posts[-1], (
            "/api/control/lease",
            {"owner": "mcp", "active": False},
        ))

    def test_video_lease_requires_owner_specific_arguments(self):
        _validate_tool_arguments("exoanchor_video_lease", {"owner": "preview", "enabled": True})
        _validate_tool_arguments("exoanchor_video_lease", {"owner": "kvm", "active": False})
        invalid = [
            {"owner": "preview", "active": True},
            {"owner": "preview", "enabled": True, "force": True},
            {"owner": "kvm", "enabled": True},
        ]
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                with self.assertRaises(ToolArgumentError):
                    _validate_tool_arguments("exoanchor_video_lease", arguments)

    def test_stage_one_tools_return_provenance_and_replay(self):
        runtime = ToolRuntime(FakeClient())
        status = runtime.call("exoanchor_status", {})["text"]
        self.assertEqual(status["kind"], "status")
        self.assertEqual(status["trust"], "untrusted_external_observation")
        self.assertEqual(status["data"]["device_id"], "prototype0")
        self.assertTrue(status["derived"]["conditions"]["hid_ready"])
        self.assertTrue(status["derived"]["conditions"]["power_on"])
        replay = runtime.call(
            "exoanchor_observation_get",
            {"observation_id": status["observation_id"]},
        )["text"]
        self.assertEqual(replay["content_sha256"], status["content_sha256"])

    def test_snapshot_has_exact_frame_metadata_and_image(self):
        runtime = ToolRuntime(FakeClient(snapshot_width=800, snapshot_height=600))
        payload = runtime.call("exoanchor_snapshot", {})
        observation = payload["text"]
        self.assertEqual(observation["data"]["width"], 800)
        self.assertEqual(observation["data"]["height"], 600)
        self.assertTrue(observation["data"]["frame_id"].startswith("frame_"))
        self.assertEqual(observation["data"]["frame_id_basis"], "jpeg_sha256")
        self.assertEqual(payload["image"]["mimeType"], "image/jpeg")

    def test_logs_cursor_pages_one_cached_observation(self):
        runtime = ToolRuntime(FakeClient())
        first = runtime.call("exoanchor_logs", {"limit": 2})["text"]
        cursor = first["data"]["pagination"]["next_cursor"]
        self.assertIsNotNone(cursor)
        second = runtime.call("exoanchor_logs", {"limit": 2, "cursor": cursor})["text"]
        self.assertEqual(first["observation_id"], second["observation_id"])
        self.assertEqual(len(second["data"]["logs"]), 1)
        self.assertIsNone(second["data"]["pagination"]["next_cursor"])

    def test_wait_for_status_uses_named_condition(self):
        runtime = ToolRuntime(FakeClient())
        result = runtime.call(
            "exoanchor_wait_for_status",
            {"condition": "hid_ready", "timeout_ms": 250, "poll_interval_ms": 250},
        )["text"]
        self.assertTrue(result["derived"]["wait"]["matched"])
        self.assertEqual(result["derived"]["wait"]["attempts"], 1)

    def test_hid_ready_fails_closed_when_raw_ready_is_false(self):
        class NotReadyClient(FakeClient):
            def get_json(self, path):
                if path == "/api/hid/status":
                    return {
                        "initialized": True,
                        "connected": True,
                        "ready": False,
                    }
                return super().get_json(path)

        runtime = ToolRuntime(NotReadyClient())
        status = runtime.call("exoanchor_status", {})["text"]
        self.assertFalse(status["derived"]["conditions"]["hid_ready"])
        result = runtime.call(
            "exoanchor_wait_for_status",
            {
                "condition": "hid_ready",
                "timeout_ms": 250,
                "poll_interval_ms": 250,
            },
        )["text"]
        self.assertFalse(result["derived"]["wait"]["actual"])
        self.assertFalse(result["derived"]["wait"]["matched"])

    def test_pure_kvm_firmware_is_reported_as_incompatible(self):
        class PureKvmClient(FakeClient):
            def get_json(self, path):
                if path == "/api/settings/mcp":
                    raise ExoAnchorError("HTTP 404 /api/settings/mcp: not found")
                return super().get_json(path)

        runtime = ToolRuntime(PureKvmClient())
        with self.assertRaisesRegex(ExoAnchorError, "pure-KVM"):
            runtime.call("exoanchor_status", {})

    def test_stage_two_rejects_stale_pixel_click(self):
        client = FakeClient(allow_write=True)
        runtime = ToolRuntime(client)
        with self.assertRaisesRegex(ExoAnchorError, "stale frame"):
            runtime.call("exoanchor_click_pixel", {
                "x": 10,
                "y": 10,
                "expected_frame_id": "frame_0000000000000000",
            })
        self.assertEqual(client.posts, [])

    def test_stage_two_pixel_click_converts_coordinates_and_cleans_up(self):
        client = FakeClient(allow_write=True, snapshot_width=101, snapshot_height=51)
        runtime = ToolRuntime(client)
        frame_id = runtime.call("exoanchor_snapshot", {})["text"]["data"]["frame_id"]
        receipt = runtime.call("exoanchor_click_pixel", {
            "x": 50,
            "y": 25,
            "expected_frame_id": frame_id,
            "expected_device_id": "prototype0",
            "wait_after_ms": 0,
        })["text"]
        submitted = next(body for path, body in client.posts if path == "/api/hid/actions")
        self.assertEqual(submitted["actions"][0]["x"], 16384)
        self.assertEqual(submitted["actions"][0]["y"], 16384)
        self.assertEqual(submitted["actions"][-1], {"type": "releaseall"})
        self.assertEqual(client.posts[-1], (
            "/api/control/lease",
            {"owner": "mcp", "active": False},
        ))
        self.assertEqual(receipt["action_kind"], "click_pixel")
        self.assertIn("after_observation", receipt)

    def test_stage_two_text_is_us_layout_and_not_echoed_in_receipt(self):
        client = FakeClient(allow_write=True)
        runtime = ToolRuntime(client)
        frame_id = runtime.call("exoanchor_snapshot", {})["text"]["data"]["frame_id"]
        receipt = runtime.call("exoanchor_type_text", {
            "text": "A!\n",
            "layout": "us",
            "expected_frame_id": frame_id,
            "inter_key_ms": 0,
            "wait_after_ms": 0,
        })["text"]
        action_batches = [body["actions"] for path, body in client.posts
                          if path == "/api/hid/actions" and body["actions"] != [{"type": "releaseall"}]]
        self.assertEqual(action_batches[0][0]["modifiers"], ["ShiftLeft"])
        self.assertEqual(action_batches[0][1]["keys"], ["Digit1"])
        self.assertEqual(action_batches[0][2]["keys"], ["Enter"])
        self.assertNotIn("text", receipt["action"])
        self.assertIn("text_sha256", receipt["action"])

    def test_console_login_uses_recent_observation_and_never_sends_secret(self):
        client = FakeClient(allow_write=True)
        runtime = ToolRuntime(client)
        before = runtime.call("exoanchor_snapshot", {})["text"]
        result = runtime.call("exoanchor_console_login", {
            "expected_device_id": "prototype0",
            "expected_observation_id": before["observation_id"],
            "credential_ref": "console://default",
            "stage": "both",
            "wait_after_ms": 0,
        })["text"]
        path, body = next(
            (path, body) for path, body in client.posts
            if path == "/api/console/login"
        )
        self.assertEqual(path, "/api/console/login")
        self.assertEqual(body["credential_ref"], "console://default")
        self.assertNotIn("username", body)
        self.assertNotIn("password", body)
        self.assertTrue(result["secret_redacted"])
        self.assertFalse(result["secret_transmitted_by_mcp"])
        self.assertEqual(client.posts[-1], (
            "/api/control/lease", {"owner": "mcp", "active": False},
        ))

    def test_console_login_rejects_missing_or_stale_observation_before_lease(self):
        client = FakeClient(allow_write=True)
        runtime = ToolRuntime(client)
        with self.assertRaisesRegex(ExoAnchorError, "unavailable"):
            runtime.call("exoanchor_console_login", {
                "expected_device_id": "prototype0",
                "expected_observation_id": "obs_missing",
                "credential_ref": "console://default",
                "stage": "password",
            })
        before = runtime.call("exoanchor_snapshot", {})["text"]
        runtime._snapshot_observed_at[before["observation_id"]] -= 1.0
        with self.assertRaisesRegex(ExoAnchorError, "stale"):
            runtime.call("exoanchor_console_login", {
                "expected_device_id": "prototype0",
                "expected_observation_id": before["observation_id"],
                "credential_ref": "console://default",
                "stage": "password",
                "max_observation_age_ms": 250,
            })
        self.assertEqual(client.posts, [])

    def test_stage_three_blocks_unverified_ssh_by_default(self):
        runtime = ToolRuntime(FakeClient(allow_write=True, host_key_check=False))
        with self.assertRaisesRegex(ExoAnchorError, "host_key_check=false"):
            runtime.call("exoanchor_ssh_job_start", {
                "operation": "system_summary",
                "secret_ref": "device-default",
                "idempotency_key": "test-unverified-host",
            })

    def test_stage_three_job_lifecycle_and_idempotency(self):
        with tempfile.TemporaryDirectory() as state_dir:
            runtime = ToolRuntime(FakeClient(allow_write=True, state_dir=state_dir))
            arguments = {
                "operation": "system_summary",
                "secret_ref": "device-default",
                "idempotency_key": "test-system-summary",
                "timeout_ms": 5000,
            }
            started = runtime.call("exoanchor_ssh_job_start", arguments)["text"]
            job_id = started["job"]["job_id"]
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                status = runtime.call("exoanchor_ssh_job_status", {"job_id": job_id})["text"]
                if status["state"] in {"succeeded", "failed"}:
                    break
                time.sleep(0.01)
            self.assertEqual(status["state"], "succeeded")
            result = runtime.call("exoanchor_ssh_job_result", {
                "job_id": job_id,
                "limit": 4,
            })["text"]
            self.assertEqual(result["output"], "test")
            self.assertEqual(result["next_offset"], 4)
            reused = runtime.call("exoanchor_ssh_job_start", arguments)["text"]
            self.assertTrue(reused["idempotency_reused"])
            self.assertEqual(reused["job"]["job_id"], job_id)

    def test_persistent_operations_job_run_and_attempt(self):
        with tempfile.TemporaryDirectory() as state_dir:
            runtime = ToolRuntime(FakeClient(allow_write=False, state_dir=state_dir))
            try:
                created = runtime.call("exoanchor_ops_job_create", {
                    "title": "Prototype health",
                    "idempotency_key": "prototype-health-job",
                    "expected_device_id": "prototype0",
                })["text"]
                job_id = created["job"]["job_id"]
                started = runtime.call(
                    "exoanchor_ops_job_run", {"job_id": job_id}
                )["text"]
                run_id = started["run"]["job_run_id"]
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    run = runtime.call("exoanchor_ops_run_status", {
                        "job_run_id": run_id,
                    })["text"]
                    if run["state"] in {"no_change", "degraded", "failed"}:
                        break
                    time.sleep(0.01)
                self.assertEqual(run["state"], "no_change")
                self.assertEqual(run["phase"], "completed")
                self.assertEqual(run["attempts"][0]["state"], "no_change")
                self.assertIn("observation_id", run["evidence"])
                paused = runtime.call("exoanchor_ops_job_set_paused", {
                    "job_id": job_id,
                    "paused": True,
                })["text"]
                self.assertEqual(paused["state"], "paused")
            finally:
                if runtime._ops_jobs is not None:
                    runtime._ops_jobs.close()

    def test_stage_one_to_four_tool_contract_is_exposed(self):
        tools = {tool["name"]: tool for tool in TOOLS}
        expected = {
            "exoanchor_capabilities",
            "exoanchor_wait_for_status",
            "exoanchor_wait_for_frame_change",
            "exoanchor_click_pixel",
            "exoanchor_type_text",
            "exoanchor_execute_and_observe",
            "exoanchor_ssh_job_start",
            "exoanchor_ssh_job_status",
            "exoanchor_ssh_job_cancel",
            "exoanchor_ssh_job_result",
            "exoanchor_ops_job_create",
            "exoanchor_ops_job_list",
            "exoanchor_ops_job_status",
            "exoanchor_ops_job_set_paused",
            "exoanchor_ops_job_run",
            "exoanchor_ops_run_status",
            "exoanchor_ops_run_cancel",
        }
        self.assertTrue(expected.issubset(tools))
        self.assertTrue(tools["exoanchor_capabilities"]["annotations"]["readOnlyHint"])
        self.assertTrue(tools["exoanchor_click_pixel"]["annotations"]["destructiveHint"])
        self.assertTrue(tools["exoanchor_ssh_job_start"]["annotations"]["idempotentHint"])

    def test_list_tools_does_not_require_device_configuration(self):
        output = io.StringIO()
        with patch.dict(os.environ, {}, clear=True), redirect_stdout(output):
            self.assertEqual(main(["--list-tools"]), 0)
        payload = json.loads(output.getvalue())
        self.assertEqual(len(payload["tools"]), len(TOOLS))

    def test_stdio_transport_initialize_and_list_tools(self):
        messages = [
            initialize_request(),
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        ]
        environment = os.environ.copy()
        environment["EXOANCHOR_BASE_URL"] = "http://device.test"
        completed = subprocess.run(
            [sys.executable, "-m", "exoanchor_mcp.server"],
            input="".join(json.dumps(message) + "\n" for message in messages),
            text=True,
            capture_output=True,
            cwd=PACKAGE_ROOT,
            env=environment,
            timeout=10,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        responses = [json.loads(line) for line in completed.stdout.splitlines()]
        self.assertEqual(len(responses), 2)
        self.assertEqual(responses[0]["result"]["protocolVersion"], MCP_PROTOCOL_VERSION)
        self.assertEqual(len(responses[1]["result"]["tools"]), len(TOOLS))


if __name__ == "__main__":
    unittest.main()
