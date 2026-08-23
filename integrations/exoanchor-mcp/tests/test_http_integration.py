import json
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from exoanchor_mcp.server import (
    MCP_TOOL_POLICY_MAP,
    ExoAnchorClient,
    ExoAnchorConfig,
    ToolRuntime,
)


def test_jpeg(width=320, height=200):
    return (
        b"\xff\xd8\xff\xc0\x00\x11\x08"
        + height.to_bytes(2, "big")
        + width.to_bytes(2, "big")
        + b"\x03\x01\x11\x00\x02\x11\x00\x03\x11\x00\xff\xd9"
    )


class ReplayState:
    def __init__(self):
        self.posts = []
        self.gets = []
        self.jpeg = test_jpeg()


class ReplayHandler(BaseHTTPRequestHandler):
    state = None

    def log_message(self, format, *args):
        return

    def _json(self, payload, status=200):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        self.state.gets.append((
            self.path,
            self.headers.get("X-ExoAnchor-Client"),
        ))
        if path == "/api/system/logs-invalid":
            body = b'{"logs":[{"message":"bad \x94 byte"}]}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        responses = {
            "/api/settings/mcp": {"enabled": True},
            "/api/capabilities": {
                "schema_version": "replay.v2",
                "video": {"snapshot": True},
                "agent_tools": {
                    "schema_version": "exoanchor.agent_tools.v2",
                    "tools": [
                        {
                            "name": name,
                            "mcp_callable": True,
                            "mcp_enabled": True,
                        }
                        for name in sorted(set(MCP_TOOL_POLICY_MAP.values()))
                    ],
                },
            },
            "/api/status": {"server_uptime": 10},
            "/api/video/status": {
                "connected": True,
                "frames_captured": 4,
                "frames_encoded": 4,
                "frames_dropped": 0,
                "capture_owner": "kvm",
                "last_frame_ms": 33,
            },
            "/api/hid/status": {"initialized": True, "connected": True, "ready": True},
            "/api/power/status": {"detect": {"power": {"state": "on"}}},
            "/api/ssh/status": {
                "supported": True,
                "host_key_check": True,
                "target": {
                    "configured": True,
                    "host": "replay-target",
                    "port": 22,
                    "username": "operator",
                    "auth_method": "key",
                },
            },
            "/api/control/lease": {"active": False, "can_request": True},
            "/api/uart/status": {
                "supported": True,
                "initialized": True,
                "port": 1,
                "baud_rate": 115200,
                "default_baud_rate": 115200,
                "fallback_baud_rate": 9600,
                "boot_id": "0123456789abcdef",
                "uart_generation": 1,
                "journal_next_cursor": "7",
                "websocket_connected": False,
                "last_error": "",
            },
            "/api/system/info": {"hostname": "replay-device"},
            "/api/system/logs": {
                "logs": [{"time": "00:00:01", "level": "INFO", "message": "replayed"}]
            },
        }
        if path == "/api/snapshot":
            body = self.state.jpeg
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/api/uart/read":
            query = parse_qs(parsed.query)
            cursor = query.get("cursor", ["0"])[0]
            self._json({
                "bytes": 3,
                "text": "ok\n",
                "data": "ok\n",
                "base64": "b2sK",
                "empty": False,
                "boot_id": "0123456789abcdef",
                "generation": "1",
                "start_cursor": cursor,
                "next_cursor": str(int(cursor) + 3),
                "journal_next_cursor": str(int(cursor) + 3),
                "pending_bytes": 0,
                "history_lost": False,
            })
            return
        if path in responses:
            self._json(responses[path])
            return
        self._json({"error": "not found"}, 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length) or b"{}")
        path = urlparse(self.path).path
        self.state.posts.append((path, body))
        if path == "/api/ssh/exec":
            self._json({
                "ok": True,
                "exit_status": 0,
                "elapsed_ms": 8,
                "truncated": False,
                "output": "Linux replay\n",
            })
            return
        if path == "/api/uart/write":
            self._json({
                "ok": True,
                "bytes_written": len(body.get("data", "")) + (
                    1 if body.get("append_newline") else 0
                ),
            })
            return
        if path == "/api/uart/baud":
            self._json({"ok": True, "baud_rate": body["baud_rate"]})
            return
        if path in {"/api/control/lease", "/api/hid/actions"}:
            self._json({"ok": True, "request": body})
            return
        self._json({"error": "not found"}, 404)


class HttpIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.state = ReplayState()
        handler = type("BoundReplayHandler", (ReplayHandler,), {"state": self.state})
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.tempdir = tempfile.TemporaryDirectory()
        config = ExoAnchorConfig(
            base_url=f"http://127.0.0.1:{self.server.server_port}",
            username="",
            password=None,
            token=None,
            timeout=5,
            allow_write=True,
            control_owner="mcp",
            device_id="replay-prototype0",
            state_dir=self.tempdir.name,
            persist_job_output=False,
            allow_unverified_ssh_host=False,
            allow_arbitrary_ssh=False,
        )
        self.runtime = ToolRuntime(ExoAnchorClient(config))

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.tempdir.cleanup()

    def test_read_observe_act_observe_over_real_http_transport(self):
        capabilities = self.runtime.call("exoanchor_capabilities", {})["text"]
        self.assertEqual(capabilities["data"]["device_id"], "replay-prototype0")
        status = self.runtime.call("exoanchor_status", {"include_system": True})["text"]
        self.assertTrue(status["derived"]["conditions"]["video_connected"])
        before = self.runtime.call("exoanchor_snapshot", {})["text"]
        receipt = self.runtime.call("exoanchor_click_pixel", {
            "x": 160,
            "y": 100,
            "expected_frame_id": before["data"]["frame_id"],
            "expected_device_id": "replay-prototype0",
            "wait_after_ms": 0,
        })["text"]
        self.assertEqual(receipt["before_frame_id"], before["data"]["frame_id"])
        self.assertEqual(receipt["after_observation"]["data"]["width"], 320)
        self.assertEqual(self.state.posts[-1], (
            "/api/control/lease",
            {"owner": "mcp", "active": False},
        ))

    def test_structured_ssh_job_over_real_http_transport(self):
        started = self.runtime.call("exoanchor_ssh_job_start", {
            "operation": "system_summary",
            "secret_ref": "device-default",
            "idempotency_key": "replay-system-summary",
            "timeout_ms": 5000,
        })["text"]
        job_id = started["job"]["job_id"]
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            status = self.runtime.call("exoanchor_ssh_job_status", {"job_id": job_id})["text"]
            if status["state"] in {"succeeded", "failed"}:
                break
            time.sleep(0.01)
        self.assertEqual(status["state"], "succeeded")
        result = self.runtime.call("exoanchor_ssh_job_result", {"job_id": job_id})["text"]
        self.assertEqual(result["output"], "Linux replay\n")
        self.assertTrue(result["output_complete"])

    def test_invalid_utf8_json_from_older_firmware_remains_diagnosable(self):
        payload = self.runtime.client.get_json("/api/system/logs-invalid")
        self.assertEqual(payload["logs"][0]["message"], "bad \ufffd byte")

    def test_uart_cursor_write_and_baud_over_real_http_transport(self):
        status = self.runtime.call("exoanchor_uart_status", {})["text"]
        self.assertEqual(status["data"]["journal_next_cursor"], "7")
        read = self.runtime.call(
            "exoanchor_uart_read",
            {"cursor": "7", "max_bytes": 128},
        )["text"]
        self.assertEqual(read["data"]["next_cursor"], "10")

        write = self.runtime.call(
            "exoanchor_uart_write",
            {"data": "whoami", "append_enter": True, "wait_ms": 0},
        )["text"]
        self.assertTrue(write["ok"])
        self.assertEqual(self.state.posts[-3:], [
            (
                "/api/control/lease",
                {
                    "owner": "mcp",
                    "active": True,
                    "mode": "supervised",
                    "reason": "mcp uart_write",
                },
            ),
            (
                "/api/uart/write",
                {"data": "whoami", "append_newline": True},
            ),
            (
                "/api/control/lease",
                {"owner": "mcp", "active": False},
            ),
        ])
        self.assertIn(
            ("/api/uart/read?cursor=7&max_bytes=1024", "exoanchor-mcp"),
            self.state.gets,
        )

        baud = self.runtime.call(
            "exoanchor_uart_baud", {"baud_rate": 9600}
        )["text"]
        self.assertTrue(baud["ok"])
        self.assertEqual(self.state.posts[-2][0], "/api/uart/baud")


if __name__ == "__main__":
    unittest.main()
