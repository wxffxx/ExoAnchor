import io
import json
import sys
import unittest
from http.client import IncompleteRead
from unittest.mock import Mock
from urllib.error import HTTPError

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.http_device import DeviceHttpClient


class Response:
    def __init__(self, content=b'{"ok":true}'):
        self.content = content
        self.status = 200
        self.closed = False

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.closed = True

    def read(self, _size):
        if isinstance(self.content, Exception):
            raise self.content
        return self.content


class HttpTransportFailureTests(unittest.TestCase):
    def test_json_and_binary_posts_are_never_replayed_after_401(self):
        for binary in (False, True):
            with self.subTest(binary=binary):
                body = io.BytesIO(b"unauthorized")
                error = HTTPError("http://192.0.2.8/api/example", 401, "Unauthorized", {}, body)
                opener = Mock(side_effect=[error, Response()])
                client = DeviceHttpClient("192.0.2.8", token="expired", password="test", opener=opener)
                client.login = Mock()
                with self.assertRaisesRegex(ToolkitError, "authentication required"):
                    if binary:
                        client.post_binary("/api/example", b"payload")
                    else:
                        client.post_json("/api/example", {"value": 1})
                self.assertEqual(opener.call_count, 1)
                client.login.assert_not_called()
                self.assertTrue(body.closed)

    def test_read_still_authenticates_and_retries_once(self):
        body = io.BytesIO(b"unauthorized")
        error = HTTPError("http://192.0.2.8/api/example", 401, "Unauthorized", {}, body)
        opener = Mock(side_effect=[error, Response()])
        client = DeviceHttpClient("192.0.2.8", password="test", opener=opener)

        def login():
            client.token = "new-token"

        client.login = Mock(side_effect=login)
        self.assertTrue(client.get_json("/api/example")["ok"])
        self.assertEqual(opener.call_count, 2)
        client.login.assert_called_once()
        self.assertEqual(opener.call_args.args[0].get_header("Authorization"), "Bearer new-token")
        self.assertTrue(body.closed)

    def test_http_error_body_is_closed_even_when_its_read_fails(self):
        for code in (401, 503):
            with self.subTest(code=code):
                body = Mock()
                body.read.side_effect = IncompleteRead(b"partial", 10)
                error = HTTPError("http://192.0.2.8/api/example", code, "Unavailable", {}, body)
                client = DeviceHttpClient("192.0.2.8", opener=Mock(side_effect=error))
                with self.assertRaises(ToolkitError) as caught:
                    client.get_json("/api/example")
                if code == 503:
                    self.assertIn("503", str(caught.exception))
                body.close.assert_called_once()

    def test_truncated_success_response_becomes_toolkit_error(self):
        response = Response(IncompleteRead(b'{"ok":', 10))
        opener = Mock(return_value=response)
        client = DeviceHttpClient("192.0.2.8", opener=opener)
        with self.assertRaises(ToolkitError):
            client.get_json("/api/example")
        self.assertTrue(response.closed)
        opener.assert_called_once()

    def test_json_parser_limits_become_toolkit_errors(self):
        payloads = [b"[" * 2000 + b"0" + b"]" * 2000]
        limit = getattr(sys, "get_int_max_str_digits", lambda: 0)()
        if limit:
            payloads.append(b'{"number":' + b"9" * (limit + 1) + b"}")
        payloads += [b'{"bad":}', b'\xff', json.dumps([1, 2]).encode()]
        for payload in payloads:
            with self.subTest(length=len(payload)):
                with self.assertRaises(ToolkitError):
                    DeviceHttpClient._decode_json(payload)

    def test_timeout_must_be_finite_and_positive(self):
        for timeout in (float("nan"), float("inf"), float("-inf"), 0, -1):
            with self.subTest(timeout=timeout):
                with self.assertRaises(ToolkitError):
                    DeviceHttpClient("192.0.2.8", timeout=timeout)


if __name__ == "__main__":
    unittest.main()
