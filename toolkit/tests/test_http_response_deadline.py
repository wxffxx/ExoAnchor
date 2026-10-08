import threading
import time
import unittest
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.http_device import DeviceHttpClient, MAX_JSON_RESPONSE_BYTES


@contextmanager
def response_server(*, status=200, declared=None, trickle=False, body=b'{"ok":true}'):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            if declared is not None:
                self.send_header("Content-Length", str(declared))
            self.end_headers()
            try:
                if trickle:
                    for value in body:
                        self.wfile.write(bytes([value]))
                        self.wfile.flush()
                        time.sleep(0.04)
                else:
                    self.wfile.write(body)
            except OSError:
                pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01})
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


class HttpResponseDeadlineTests(unittest.TestCase):
    def test_trickle_success_cannot_extend_response_budget(self):
        with response_server(trickle=True, declared=11) as target:
            client = DeviceHttpClient(target, timeout=0.15)
            with self.assertRaises(ToolkitError):
                client.get_json("/api/status")

    def test_trickle_error_is_bounded_and_keeps_http_status(self):
        with response_server(status=503, trickle=True, declared=11) as target:
            client = DeviceHttpClient(target, timeout=0.15)
            started = time.monotonic()
            with self.assertRaisesRegex(ToolkitError, "HTTP 503") as caught:
                client.get_json("/api/status")
            self.assertIn("body unavailable", str(caught.exception))
            self.assertLess(time.monotonic() - started, 0.4)

    def test_valid_json_with_truncated_content_length_is_rejected(self):
        with response_server(declared=20) as target:
            with self.assertRaises(ToolkitError):
                DeviceHttpClient(target).get_json("/api/status")

    def test_oversized_declared_body_is_rejected(self):
        with response_server(declared=MAX_JSON_RESPONSE_BYTES + 1) as target:
            with self.assertRaisesRegex(ToolkitError, "large"):
                DeviceHttpClient(target).get_json("/api/status")

    def test_normal_and_close_delimited_bodies_work(self):
        for length in (None, 11):
            with self.subTest(length=length), response_server(declared=length) as target:
                self.assertEqual(DeviceHttpClient(target).get_json("/api/status"), {"ok": True})

    def test_timeout_override_must_be_finite_and_positive(self):
        client = DeviceHttpClient("127.0.0.1", opener=lambda *_a, **_k: self.fail("unexpected request"))
        for timeout in (0, -1, float("nan"), float("inf")):
            with self.subTest(timeout=timeout), self.assertRaises(ToolkitError):
                client._request("GET", "/api/status", timeout=timeout)


if __name__ == "__main__":
    unittest.main()
