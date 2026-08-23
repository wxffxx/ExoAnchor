#!/usr/bin/env python3
"""Host tests for tools/agent-runtime-http.py."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib import parse


PROJECT = Path(__file__).resolve().parents[2]
CLI = PROJECT / "tools" / "agent-runtime-http.py"
PASSWORD = "test-password-secret"
LOGIN_TOKEN = "login-token-secret"
FILE_TOKEN = "file-token-secret"


class MockAgentHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    calls: list[tuple[str, str, dict]] = []
    redirect_capabilities = False
    submit_mode = "accepted"
    submit_payload_override: dict | None = None
    control_conflict = False
    control_payload_override: dict | None = None
    history_session_id = "s1"

    def log_message(self, _format: str, *_args: object) -> None:
        return

    def _body(self) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        value = json.loads(raw.decode("utf-8"))
        return value if isinstance(value, dict) else {}

    def _send(self, payload: dict, status: int = 200) -> None:
        encoded = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def _authorized(self) -> bool:
        return self.headers.get("Authorization") in {
            f"Bearer {LOGIN_TOKEN}",
            f"Bearer {FILE_TOKEN}",
        }

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler contract
        split = parse.urlsplit(self.path)
        query = parse.parse_qs(split.query)
        if split.path == "/api/auth/login/status":
            self.calls.append(("GET", split.path, query))
            self._send({"ok": True, "token": LOGIN_TOKEN, "username": "admin"})
            return
        if not self._authorized():
            self._send({"error": "unauthorized"}, 401)
            return
        self.calls.append(("GET", split.path, query))
        if split.path == "/api/capabilities":
            if self.redirect_capabilities:
                self.send_response(302)
                self.send_header("Location", "http://127.0.0.1:1/credential-sink")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            # A hostile or buggy peer must not make the CLI echo its credential.
            self._send(
                {
                    "agent_run": True,
                    "embedded_agent": True,
                    "reflected": FILE_TOKEN,
                    "token": FILE_TOKEN,
                }
            )
        elif split.path == "/api/agent/run/status":
            requested = (query.get("run_id") or [""])[0]
            if requested != "r1":
                self._send(
                    {
                        "found": False,
                        "run_id": requested,
                        "reason": "run_id_mismatch",
                    },
                    404,
                )
            else:
                self._send(
                    {
                        "found": True,
                        "run_id": "r1",
                        "job_id": "r1",
                        "state": "done",
                        "ok": True,
                    }
                )
        elif split.path == "/api/agent/run/events":
            after = int((query.get("after_seq") or ["0"])[0])
            events = (
                [{"seq": 1, "run_id": "r1", "phase": "started"}]
                if after < 1
                else []
            )
            self._send(
                {
                    "job_id": "r1",
                    "state": "done",
                    "events": events,
                    "task_events": [],
                    "next_after_seq": 1,
                }
            )
        elif split.path == "/api/agent/history":
            self._send(
                {
                    "session_id": self.history_session_id,
                    "records": [],
                    "supported": True,
                }
            )
        elif split.path == "/api/agent/sessions":
            self._send({"sessions": [{"session_id": "s1"}], "supported": True})
        elif split.path == "/api/agent/requests":
            self._send({"items": [], "count": 0, "owner": "device"})
        else:
            self._send({"error": "not found"}, 404)

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler contract
        split = parse.urlsplit(self.path)
        body = self._body()
        if split.path == "/api/auth/login":
            self.calls.append(("POST", split.path, body))
            if body.get("password") != PASSWORD:
                self._send({"error": "bad password"}, 401)
            else:
                self._send(
                    {"pending": True, "job_id": "login1", "poll_after_ms": 1},
                    202,
                )
            return
        if not self._authorized():
            self._send({"error": "unauthorized"}, 401)
            return
        self.calls.append(("POST", split.path, body))
        if split.path == "/api/agent/run":
            if self.submit_payload_override is not None:
                self._send(self.submit_payload_override)
            elif self.submit_mode == "busy":
                self._send(
                    {
                        "accepted": False,
                        "queued": False,
                        "busy": True,
                        "deduplicated": False,
                        "submitted_run_id": "",
                        "active_run_id": "r-active",
                        "reason": "agent_run_busy",
                    },
                    409,
                )
            elif self.submit_mode == "idempotency_conflict":
                self._send(
                    {
                        "accepted": False,
                        "queued": False,
                        "busy": False,
                        "deduplicated": False,
                        "submitted_run_id": "",
                        "active_run_id": "",
                        "conflicting_run_id": "r-old",
                        "reason": "idempotency_conflict",
                    },
                    409,
                )
            elif self.submit_mode == "deduplicated":
                self._send(
                    {
                        "accepted": True,
                        "queued": False,
                        "busy": True,
                        "receipt_busy": True,
                        "deduplicated": True,
                        "submitted_run_id": "r1",
                        "active_run_id": "r1",
                        "run_id": "r1",
                        "state": "running",
                    }
                )
            elif self.submit_mode == "terminal_deduplicated":
                self._send(
                    {
                        "accepted": True,
                        "queued": False,
                        "busy": False,
                        "receipt_busy": False,
                        "deduplicated": True,
                        "submitted_run_id": "r1",
                        "active_run_id": "",
                        "run_id": "r1",
                        "job_id": "r1",
                        "state": "done",
                    }
                )
            elif self.submit_mode == "active_to_terminal":
                self._send(
                    {
                        "accepted": True,
                        "queued": False,
                        # Live projection observed the run after it finished.
                        "busy": False,
                        # The atomic submit receipt still says it was active.
                        "receipt_busy": True,
                        "deduplicated": True,
                        "submitted_run_id": "r1",
                        "active_run_id": "r1",
                        "run_id": "r1",
                        "job_id": "r1",
                        "state": "done",
                    }
                )
            elif self.submit_mode == "retained_replaced":
                self._send(
                    {
                        "accepted": True,
                        "queued": False,
                        # No exact live projection survived; firmware falls
                        # back to the receipt-time sample for this field.
                        "busy": True,
                        "receipt_busy": True,
                        "deduplicated": True,
                        "submitted_run_id": "r1",
                        "active_run_id": "r1",
                        "status_retained": False,
                    }
                )
            else:
                self._send(
                    {
                        "accepted": True,
                        "queued": False,
                        "busy": True,
                        "receipt_busy": False,
                        "deduplicated": False,
                        "submitted_run_id": "r1",
                        "active_run_id": "",
                        "run_id": "r1",
                        "job_id": "r1",
                        "state": "running",
                    },
                    202,
                )
        elif split.path.startswith("/api/agent/run/"):
            if self.control_conflict:
                self._send(
                    {
                        "accepted": False,
                        "run_id": body.get("run_id"),
                        "reason": "run_id_mismatch",
                    },
                    409,
                )
            elif self.control_payload_override is not None:
                self._send(self.control_payload_override)
            else:
                action = split.path.rsplit("/", 1)[-1]
                control = "abort" if action == "cancel" else action
                self._send(
                    {
                        "accepted": True,
                        "controlled_run_id": "r1",
                        "control": control,
                    }
                )
        elif split.path == "/api/agent/history":
            self._send({"ok": True, "session_id": body.get("session_id")})
        elif split.path == "/api/agent/history/clear":
            self._send({"ok": True, "record_count": 0})
        elif split.path == "/api/agent/sessions":
            self._send({"ok": True, "session_id": "s2", "title": body.get("title")})
        elif split.path == "/api/agent/sessions/delete":
            self._send({"ok": True, "session_id": body.get("session_id")})
        elif split.path in {
            "/api/agent/requests/decision",
            "/api/agent/requests/cancel",
        }:
            self._send({"request_id": body.get("request_id"), "status": "granted"})
        else:
            self._send({"error": "not found"}, 404)


class AgentRuntimeHttpCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        MockAgentHandler.calls = []
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), MockAgentHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base_url = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)

    def setUp(self) -> None:
        MockAgentHandler.calls.clear()
        MockAgentHandler.redirect_capabilities = False
        MockAgentHandler.submit_mode = "accepted"
        MockAgentHandler.submit_payload_override = None
        MockAgentHandler.control_conflict = False
        MockAgentHandler.control_payload_override = None
        MockAgentHandler.history_session_id = "s1"
        self.temp_dir = tempfile.TemporaryDirectory()
        root = Path(self.temp_dir.name)
        self.temp_root = root
        self.token_file = root / "token"
        self.token_file.write_text(FILE_TOKEN, encoding="utf-8")
        os.chmod(self.token_file, 0o600)
        self.password_file = root / "password"
        self.password_file.write_text(PASSWORD + "\n", encoding="utf-8")
        os.chmod(self.password_file, 0o600)

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def run_cli(
        self, *command: str, password: bool = False
    ) -> subprocess.CompletedProcess[str]:
        credential = (
            ["--password-file", str(self.password_file)]
            if password
            else ["--token-file", str(self.token_file)]
        )
        return subprocess.run(
            [
                sys.executable,
                str(CLI),
                "--base-url",
                self.base_url,
                "--allow-http",
                *credential,
                *command,
            ],
            text=True,
            capture_output=True,
            timeout=10,
            check=False,
        )

    def assert_clean_output(self, result: subprocess.CompletedProcess[str]) -> None:
        combined = result.stdout + result.stderr
        self.assertNotIn(PASSWORD, combined)
        self.assertNotIn(LOGIN_TOKEN, combined)
        self.assertNotIn(FILE_TOKEN, combined)

    def test_async_password_login_and_execute_submit(self) -> None:
        result = self.run_cli(
            "submit",
            "--message",
            "inspect target",
            "--session-id",
            "s1",
            "--execute",
            "--screenshot",
            password=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(result.stdout)
        self.assertEqual(receipt["submitted_run_id"], "r1")
        self.assertFalse(receipt["receipt_busy"])
        self.assertEqual(receipt["active_run_id"], "")
        self.assert_clean_output(result)
        login = next(call for call in MockAgentHandler.calls if call[1] == "/api/auth/login")
        self.assertEqual(login[2]["password"], PASSWORD)
        submit = next(call for call in MockAgentHandler.calls if call[1] == "/api/agent/run")
        self.assertEqual(submit[2]["authority_mode"], "policy")
        self.assertFalse(submit[2]["dry_run"])
        self.assertTrue(submit[2]["include_screenshot"])
        self.assertTrue(str(submit[2]["idempotency_key"]).startswith("http-"))

    def test_complete_command_surface_routes(self) -> None:
        commands = (
            ("capabilities",),
            ("status", "--run-id", "r1"),
            ("events", "--run-id", "r1", "--after-seq", "0"),
            ("pause", "--run-id", "r1"),
            ("resume", "--run-id", "r1"),
            ("abort", "--run-id", "r1"),
            ("cancel", "--run-id", "r1"),
            ("steer", "--run-id", "r1", "--message", "continue safely"),
            ("history", "list", "--session-id", "s1"),
            (
                "history",
                "append",
                "--session-id",
                "s1",
                "--role",
                "system",
                "--content",
                "host evidence",
            ),
            ("history", "clear", "--confirm", "CLEAR_AGENT_HISTORY"),
            ("sessions", "list"),
            ("sessions", "create", "--title", "HIL session"),
            ("sessions", "delete", "--session-id", "s1", "--confirm", "s1"),
            ("requests", "list", "--run-id", "r1"),
            ("requests", "approve", "--request-id", "q1", "--response", "yes"),
            ("requests", "cancel", "--request-id", "q1"),
        )
        for command in commands:
            with self.subTest(command=command):
                result = self.run_cli(*command)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.startswith("{"), result.stdout)
                self.assert_clean_output(result)
        status_calls = [
            call for call in MockAgentHandler.calls
            if call[0] == "GET" and call[1] == "/api/agent/run/status"
        ]
        self.assertTrue(status_calls)
        self.assertTrue(
            all(call[2].get("run_id") == ["r1"] for call in status_calls)
        )

    def test_watch_emits_jsonl_and_stops_at_terminal_state(self) -> None:
        result = self.run_cli(
            "watch",
            "--run-id",
            "r1",
            "--interval",
            "0.05",
            "--watch-timeout",
            "1",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([item["type"] for item in lines], ["event", "status"])
        self.assert_clean_output(result)

    def test_plain_http_and_weak_file_permissions_are_rejected(self) -> None:
        without_opt_in = subprocess.run(
            [
                sys.executable,
                str(CLI),
                "--base-url",
                self.base_url,
                "--token-file",
                str(self.token_file),
                "capabilities",
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(without_opt_in.returncode, 2)
        self.assertIn("--allow-http", without_opt_in.stderr)
        self.assert_clean_output(without_opt_in)

        os.chmod(self.token_file, 0o644)
        weak_permissions = self.run_cli("capabilities")
        self.assertNotEqual(weak_permissions.returncode, 0)
        self.assertIn("chmod 600", weak_permissions.stderr)
        self.assert_clean_output(weak_permissions)

    def test_authenticated_requests_never_follow_redirects(self) -> None:
        MockAgentHandler.redirect_capabilities = True
        result = self.run_cli("capabilities")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("HTTP 302", result.stderr)
        self.assertNotIn("credential-sink", result.stderr)
        self.assert_clean_output(result)

    def test_structured_conflicts_remain_json_and_exit_rejected(self) -> None:
        for mode, reason in (
            ("busy", "agent_run_busy"),
            ("idempotency_conflict", "idempotency_conflict"),
        ):
            with self.subTest(mode=mode):
                MockAgentHandler.submit_mode = mode
                result = self.run_cli(
                    "submit",
                    "--message",
                    "inspect target",
                    "--idempotency-key",
                    "stable-hil-key",
                )
                self.assertEqual(result.returncode, 3, result.stderr)
                self.assertEqual(json.loads(result.stdout)["reason"], reason)
                self.assertNotIn("HTTP 409", result.stderr)
                self.assert_clean_output(result)

        MockAgentHandler.submit_mode = "accepted"
        MockAgentHandler.control_conflict = True
        mismatch = self.run_cli("pause", "--run-id", "r1")
        self.assertEqual(mismatch.returncode, 3, mismatch.stderr)
        self.assertEqual(json.loads(mismatch.stdout)["reason"], "run_id_mismatch")
        self.assertNotIn("HTTP 409", mismatch.stderr)

    def test_submit_acceptance_contract_and_active_deduplication(self) -> None:
        MockAgentHandler.submit_mode = "deduplicated"
        deduplicated = self.run_cli(
            "submit",
            "--message",
            "inspect target",
            "--idempotency-key",
            "stable-hil-key",
        )
        self.assertEqual(deduplicated.returncode, 0, deduplicated.stderr)
        receipt = json.loads(deduplicated.stdout)
        self.assertTrue(receipt["deduplicated"])
        self.assertTrue(receipt["receipt_busy"])
        self.assertEqual(receipt["active_run_id"], receipt["submitted_run_id"])

        malformed_payloads = (
            {
                "accepted": True,
                "queued": False,
                "busy": False,
                "receipt_busy": False,
                "deduplicated": False,
                "active_run_id": "",
            },
            {
                "accepted": True,
                "queued": False,
                "busy": True,
                "receipt_busy": False,
                "deduplicated": False,
                "submitted_run_id": "r2",
                "active_run_id": "r-other",
            },
            {
                "accepted": False,
                "queued": False,
                "busy": False,
                "receipt_busy": False,
                "deduplicated": False,
                "submitted_run_id": "r2",
                "active_run_id": "",
            },
            {
                # Otherwise valid new receipt, but HTTP 200 is not an
                # acceptance response for a newly created run.
                "accepted": True,
                "queued": False,
                "busy": True,
                "receipt_busy": False,
                "deduplicated": False,
                "submitted_run_id": "r2",
                "active_run_id": "",
                "run_id": "r2",
            },
            {
                # Receipt state is mandatory even when the live projection
                # already exposes a boolean busy field.
                "accepted": True,
                "queued": False,
                "busy": True,
                "deduplicated": False,
                "submitted_run_id": "r2",
                "active_run_id": "",
                "run_id": "r2",
            },
        )
        for payload in malformed_payloads:
            with self.subTest(payload=payload):
                MockAgentHandler.submit_payload_override = payload
                rejected = self.run_cli("submit", "--message", "inspect target")
                self.assertEqual(rejected.returncode, 3, rejected.stderr)
                output = json.loads(rejected.stdout)
                self.assertEqual(output["client_error"], "invalid_api_response")
                self.assertEqual(output["response"], payload)
        MockAgentHandler.submit_payload_override = None

    def test_terminal_deduplication_receipt(self) -> None:
        MockAgentHandler.submit_mode = "terminal_deduplicated"
        result = self.run_cli(
            "submit",
            "--message",
            "inspect target",
            "--idempotency-key",
            "stable-hil-key",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(result.stdout)
        self.assertTrue(receipt["deduplicated"])
        self.assertFalse(receipt["receipt_busy"])
        self.assertFalse(receipt["busy"])
        self.assertEqual(receipt["active_run_id"], "")
        self.assertEqual(receipt["submitted_run_id"], receipt["run_id"])

    def test_submit_receipt_survives_live_projection_races(self) -> None:
        MockAgentHandler.submit_mode = "active_to_terminal"
        terminal = self.run_cli(
            "submit",
            "--message",
            "inspect target",
            "--idempotency-key",
            "stable-hil-key",
        )
        self.assertEqual(terminal.returncode, 0, terminal.stderr)
        terminal_receipt = json.loads(terminal.stdout)
        self.assertTrue(terminal_receipt["receipt_busy"])
        self.assertFalse(terminal_receipt["busy"])
        self.assertEqual(
            terminal_receipt["active_run_id"],
            terminal_receipt["submitted_run_id"],
        )

        MockAgentHandler.submit_mode = "retained_replaced"
        replaced = self.run_cli(
            "submit",
            "--message",
            "inspect target",
            "--idempotency-key",
            "stable-hil-key",
        )
        self.assertEqual(replaced.returncode, 0, replaced.stderr)
        replaced_receipt = json.loads(replaced.stdout)
        self.assertFalse(replaced_receipt["status_retained"])
        self.assertTrue(replaced_receipt["receipt_busy"])
        self.assertEqual(replaced_receipt["active_run_id"], "r1")
        self.assertNotIn("run_id", replaced_receipt)
        self.assertNotIn("job_id", replaced_receipt)

    def test_exact_run_aliases_and_control_success_contract(self) -> None:
        contradictory_payloads = (
            {
                "accepted": True,
                "control": "pause",
                "run_id": "r1",
                "job_id": "r-other",
            },
            {
                "accepted": True,
                "control": "pause",
                "run_id": "r1",
                "submitted_run_id": "r-other",
            },
            {
                "accepted": True,
                "control": "pause",
                "run_id": "r1",
                "controlled_run_id": "r-other",
            },
        )
        for payload in contradictory_payloads:
            with self.subTest(payload=payload):
                MockAgentHandler.control_payload_override = payload
                rejected = self.run_cli("pause", "--run-id", "r1")
                self.assertEqual(rejected.returncode, 3, rejected.stderr)
                self.assertIn("exact run mismatch", rejected.stderr)

        malformed_payloads = (
            {
                "controlled_run_id": "r1",
                "control": "pause",
            },
            {
                "accepted": True,
                "controlled_run_id": "r1",
                "control": "resume",
            },
        )
        for payload in malformed_payloads:
            with self.subTest(payload=payload):
                MockAgentHandler.control_payload_override = payload
                rejected = self.run_cli("pause", "--run-id", "r1")
                self.assertEqual(rejected.returncode, 3, rejected.stderr)
                output = json.loads(rejected.stdout)
                self.assertEqual(output["client_error"], "invalid_api_response")
                self.assertEqual(output["response"], payload)

        MockAgentHandler.control_payload_override = None
        cancel = self.run_cli("cancel", "--run-id", "r1")
        self.assertEqual(cancel.returncode, 0, cancel.stderr)
        self.assertEqual(json.loads(cancel.stdout)["control"], "abort")

    def test_exact_status_history_binding_and_session_id_rules(self) -> None:
        mismatch = self.run_cli("status", "--run-id", "r-missing")
        self.assertEqual(mismatch.returncode, 3, mismatch.stderr)
        self.assertEqual(json.loads(mismatch.stdout)["reason"], "run_id_mismatch")

        for invalid_session in ("bad.session", "bad:session", "s" * 49):
            with self.subTest(invalid_session=invalid_session):
                invalid = self.run_cli(
                    "history", "list", "--session-id", invalid_session
                )
                self.assertEqual(invalid.returncode, 2, invalid.stderr)
                self.assertIn("invalid session id", invalid.stderr)

        record_file = self.temp_root / "record.json"
        record_file.write_text(
            json.dumps(
                {
                    "kind": "message",
                    "role": "system",
                    "content": "evidence",
                    "session_id": "s-other",
                }
            ),
            encoding="utf-8",
        )
        conflicting_record = self.run_cli(
            "history",
            "append",
            "--session-id",
            "s1",
            "--record-file",
            str(record_file),
        )
        self.assertEqual(conflicting_record.returncode, 2, conflicting_record.stderr)
        self.assertIn("exactly match", conflicting_record.stderr)

        record_file.write_text(
            json.dumps({"kind": "message", "role": "system", "content": "evidence"}),
            encoding="utf-8",
        )
        bound_record = self.run_cli(
            "history",
            "append",
            "--session-id",
            "s1",
            "--record-file",
            str(record_file),
        )
        self.assertEqual(bound_record.returncode, 0, bound_record.stderr)
        history_post = [
            call for call in MockAgentHandler.calls
            if call[0] == "POST" and call[1] == "/api/agent/history"
        ][-1]
        self.assertEqual(history_post[2]["session_id"], "s1")

        MockAgentHandler.history_session_id = "s-other"
        wrong_history = self.run_cli("history", "list", "--session-id", "s1")
        self.assertEqual(wrong_history.returncode, 3, wrong_history.stderr)
        self.assertEqual(
            json.loads(wrong_history.stdout)["client_error"],
            "invalid_api_response",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
