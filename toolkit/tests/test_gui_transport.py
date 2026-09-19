import http.client
import json
import socket
import sys
import threading
import unittest

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.gui import MAX_REQUEST_BYTES, ToolkitController, create_gui_server


class OfflineController(ToolkitController):
    def __init__(self):
        super().__init__()
        self.calls = 0
        self.payload = {"ports": []}
        self.error = None

    def ports(self):
        self.calls += 1
        if self.error:
            raise self.error
        return self.payload


class GuiTransportTests(unittest.TestCase):
    def setUp(self):
        self.controller = OfflineController()
        self.server = create_gui_server(controller=self.controller)
        self.handler_errors = []
        self.server.handle_error = lambda *_: self.handler_errors.append(sys.exc_info()[1])
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=1)

    def post(self, body=b"{}", headers=None, *, half_close=False):
        connection = http.client.HTTPConnection(*self.server.server_address, timeout=2)
        request_headers = {"X-ExoAnchor-Token": self.server.token, "Content-Type": "application/json"}
        request_headers.update(headers or {})
        try:
            connection.request("POST", "/api/ports", body, request_headers)
            if half_close:
                connection.sock.shutdown(socket.SHUT_WR)
            response = connection.getresponse()
            return response.status, json.loads(response.read()), response.getheader("Connection")
        finally:
            connection.close()

    def test_non_ascii_authentication_is_denied_without_handler_failure(self):
        status, payload, _ = self.post(headers={"X-ExoAnchor-Token": "invalid-é"})
        self.assertEqual(status, 403)
        self.assertEqual(payload["error"], "forbidden")
        self.assertEqual(self.controller.calls, 0)
        self.assertEqual(self.handler_errors, [])

    def test_json_parse_failures_never_dispatch_or_crash_the_handler(self):
        payloads = [b"{broken", b"\xff", b'{"nested":' + b"[" * 2000 + b"0" + b"]" * 2000 + b"}"]
        digit_limit = getattr(sys, "get_int_max_str_digits", lambda: 0)()
        if digit_limit:
            payloads.append(b'{"number":' + b"9" * (digit_limit + 1) + b"}")
        for body in payloads:
            with self.subTest(bytes=len(body)):
                status, payload, _ = self.post(body)
                self.assertEqual(status, 400)
                self.assertEqual(payload["error"], "request body must be JSON")
        self.assertEqual(self.controller.calls, 0)
        self.assertEqual(self.handler_errors, [])

    def test_truncated_content_length_never_dispatches_a_partial_request(self):
        status, payload, _ = self.post(b'{"value":1}', {"Content-Length": "20"}, half_close=True)
        self.assertEqual(status, 400)
        self.assertIn("incomplete", payload["error"])
        self.assertEqual(self.controller.calls, 0)

    def test_unsupported_transfer_encoding_is_rejected_before_dispatch(self):
        status, _, _ = self.post(b"0\r\n\r\n", {"Transfer-Encoding": "chunked", "Content-Length": "0"})
        self.assertEqual(status, 400)
        self.assertEqual(self.controller.calls, 0)

    def test_error_responses_close_connections_with_unread_bodies(self):
        cases = [({"X-ExoAnchor-Token": "wrong"}, 403),
                 ({"Origin": "https://example.invalid"}, 403),
                 ({"Content-Length": str(MAX_REQUEST_BYTES + 1)}, 413)]
        for headers, expected in cases:
            with self.subTest(headers=headers):
                status, _, connection = self.post(headers=headers)
                self.assertEqual(status, expected)
                self.assertEqual(connection, "close")
        self.assertEqual(self.controller.calls, 0)

    def test_unpaired_unicode_in_results_and_errors_remains_json(self):
        self.controller.payload = {"diagnostic": "text\ud800"}
        status, payload, _ = self.post()
        self.assertEqual(status, 200)
        self.assertEqual(payload["diagnostic"], "text\ud800")
        self.controller.error = ToolkitError("failure\udfff")
        status, payload, _ = self.post()
        self.assertEqual(status, 400)
        self.assertEqual(payload["error"], "failure\udfff")
        self.assertEqual(self.handler_errors, [])

    def test_successful_requests_keep_http_connection_reuse(self):
        connection = http.client.HTTPConnection(*self.server.server_address, timeout=2)
        try:
            for _ in range(2):
                connection.request("POST", "/api/ports", b"{}", {"X-ExoAnchor-Token": self.server.token})
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                self.assertIsNone(response.getheader("Connection"))
                self.assertTrue(json.loads(response.read())["ok"])
            self.assertEqual(self.controller.calls, 2)
        finally:
            connection.close()


if __name__ == "__main__":
    unittest.main()
