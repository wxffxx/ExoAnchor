import io
import json
import os
import subprocess
import sys
import unittest
from contextlib import redirect_stdout
from unittest.mock import Mock, patch

from exoanchor_mcp.server import McpServer


class ProtocolResilienceTests(unittest.TestCase):
    def test_nonstring_methods_are_invalid_requests(self):
        runtime = Mock()
        server = McpServer(runtime)
        for method in ([], {}, None, True, 12, 1.5):
            with self.subTest(method=method):
                response = server.handle({"jsonrpc": "2.0", "id": "bad", "method": method})
                self.assertEqual(response["error"]["code"], -32600)
                self.assertEqual(response["id"], "bad")
        runtime.call.assert_not_called()

    def test_stdio_recovers_after_malformed_methods_and_parser_limits(self):
        bad_lines = [
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": []}),
            "[" * 2000 + "0" + "]" * 2000,
            "9" * 10000,
            "{broken-json",
        ]
        healthy = json.dumps({"jsonrpc": "2.0", "id": "alive", "method": "ping"})
        output = io.StringIO()
        with patch("sys.stdin", io.StringIO("\n".join(bad_lines + [healthy]) + "\n")), \
             redirect_stdout(output):
            McpServer(Mock()).serve()
        responses = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(len(responses), 5)
        self.assertTrue(all("error" in response for response in responses[:-1]))
        self.assertEqual(responses[-1], {"jsonrpc": "2.0", "id": "alive", "result": {}})

    def test_stdio_can_encode_surrogate_and_unicode_ids_without_exiting(self):
        ids = ["\ud800", "请求", "after-surrogate"]
        completed = subprocess.run(
            [sys.executable, "-m", "exoanchor_mcp.server"],
            input="".join(json.dumps({"jsonrpc": "2.0", "id": value, "method": "ping"}) + "\n" for value in ids),
            text=True, encoding="utf-8", capture_output=True,
            env={**os.environ, "EXOANCHOR_BASE_URL": "https://device.test",
                 "PYTHONIOENCODING": "utf-8:strict"},
            timeout=10,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        responses = [json.loads(line) for line in completed.stdout.splitlines()]
        self.assertEqual([response["id"] for response in responses], ids)
        self.assertTrue(all(response["result"] == {} for response in responses))
