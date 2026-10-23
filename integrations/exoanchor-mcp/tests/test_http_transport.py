import threading
import sys
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

from exoanchor_mcp.client import ExoAnchorClient, ExoAnchorError
from test_client import client_config


class HttpTransportTests(unittest.TestCase):
    def setUp(self):
        self.requests = []
        self.status = 200
        self.body = b'{"ok":true}'
        self.headers = {"Content-Type": "application/json"}
        self.declared_length = None
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_GET(self):
                owner.requests.append((self.server.server_port, self.command,
                                       self.path, self.headers.get("Authorization")))
                self.rfile.read(int(self.headers.get("Content-Length", 0)))
                status = owner.status if self.path == "/api/test" else 200
                self.send_response(status)
                for name, value in owner.headers.items():
                    self.send_header(name, value)
                self.send_header("Content-Length", str(
                    len(owner.body) if owner.declared_length is None else owner.declared_length))
                self.end_headers()
                self.wfile.write(owner.body)

            do_POST = do_GET

        self.servers = [ThreadingHTTPServer(("127.0.0.1", 0), Handler) for _ in range(2)]
        self.threads = []
        for server in self.servers:
            thread = threading.Thread(target=lambda srv=server: srv.serve_forever(poll_interval=0.01))
            thread.start()
            self.threads.append(thread)
        self.client = ExoAnchorClient(client_config(
            base_url=f"http://127.0.0.1:{self.servers[0].server_port}", token="test-token"))

    def tearDown(self):
        for server, thread in zip(self.servers, self.threads):
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_bearer_requests_never_follow_redirects(self):
        for method in ("GET", "POST"):
            for status in (301, 302, 303, 307, 308):
                for target in self.servers:
                    with self.subTest(method=method, status=status, port=target.server_port):
                        self.requests.clear()
                        self.status = status
                        self.headers["Location"] = f"http://127.0.0.1:{target.server_port}/redirected"
                        with self.assertRaisesRegex(ExoAnchorError, f"HTTP {status}"):
                            self.client._request(method, "/api/test", retry_auth=False)
                        self.assertEqual(len(self.requests), 1, "redirect destination must not receive a request")
                        self.assertEqual(self.requests[0][3], "Bearer test-token")

    def test_invalid_json_is_a_transport_error(self):
        self.body = b'{"password":"do-not-echo", "broken": '
        with self.assertRaisesRegex(ExoAnchorError, "invalid JSON") as caught:
            self.client.get_json("/api/test")
        self.assertNotIn("do-not-echo", str(caught.exception))

    def test_json_parser_limits_are_transport_errors(self):
        bodies = [b"[" * 2000 + b"0" + b"]" * 2000]
        integer_limit = getattr(sys, "get_int_max_str_digits", lambda: 0)()
        if integer_limit:
            bodies.append(b"9" * (integer_limit + 1))
        for body in bodies:
            with self.subTest(size=len(body)):
                self.body = body
                with self.assertRaisesRegex(ExoAnchorError, "invalid JSON"):
                    self.client.get_json("/api/test")

    def test_truncated_body_is_a_transport_error(self):
        self.declared_length = len(self.body) + 20
        with self.assertRaisesRegex(ExoAnchorError, "response failed"):
            self.client.get_json("/api/test")

    def test_truncated_http_error_body_retains_http_status(self):
        self.status = 503
        self.declared_length = len(self.body) + 20
        with self.assertRaisesRegex(ExoAnchorError, "HTTP 503.*response body unavailable"):
            self.client.get_json("/api/test")

    def test_socket_timeout_is_a_transport_error_and_is_not_replayed(self):
        with patch.object(self.client._opener, "open", side_effect=TimeoutError("timed out")) as request:
            with self.assertRaisesRegex(ExoAnchorError, "response failed"):
                self.client.post_json("/api/test", {"action": "test"})
        self.assertEqual(request.call_count, 1)

    def test_plain_text_and_raw_responses_remain_available(self):
        self.headers["Content-Type"] = "text/plain"
        self.body = b"diagnostic text"
        self.assertEqual(self.client.get_json("/api/test"), {"ok": True, "text": "diagnostic text"})
        self.body = b"\xff\xd8\x00\xff\xd9"
        self.headers["Content-Type"] = "image/jpeg"
        body, headers = self.client.get_raw("/api/test")
        self.assertEqual(body, self.body)
        self.assertEqual(headers["Content-Type"], "image/jpeg")
