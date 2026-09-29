import io
import unittest
from urllib.error import HTTPError
from unittest.mock import patch

from exoanchor_mcp.client import (
    ExoAnchorClient, ExoAnchorError, JSON_RESPONSE_LIMIT, IMAGE_RESPONSE_LIMIT,
    ERROR_RESPONSE_LIMIT,
)
from test_client import client_config


class Response(io.BytesIO):
    def __init__(self, body, headers=None):
        super().__init__(body)
        self.headers = headers or {"Content-Type": "application/json"}
        self.read_sizes = []

    def read(self, size=-1):
        self.read_sizes.append(size)
        return super().read(size)


class ResponseBoundsTests(unittest.TestCase):
    def setUp(self):
        self.client = ExoAnchorClient(client_config(token="synthetic-token"))

    def test_oversized_content_length_is_rejected_without_reading(self):
        response = Response(b"", {"Content-Length": str(JSON_RESPONSE_LIMIT + 1)})
        with patch.object(self.client._opener, "open", return_value=response):
            with self.assertRaisesRegex(ExoAnchorError, "size limit"):
                self.client.get_json("/api/test")
        self.assertEqual(response.read_sizes, [])
        self.assertTrue(response.closed)

    def test_json_and_image_without_length_are_bounded_and_closed(self):
        for limit, raw in ((JSON_RESPONSE_LIMIT, False), (IMAGE_RESPONSE_LIMIT, True)):
            response = Response(b"x" * (limit + 1))
            with self.subTest(raw=raw), patch.object(self.client._opener, "open", return_value=response):
                with self.assertRaisesRegex(ExoAnchorError, "size limit"):
                    self.client._request("GET", "/api/test", raw=raw)
            self.assertTrue(response.closed)
            self.assertTrue(all(0 < size <= 65536 for size in response.read_sizes))

    def test_oversized_error_keeps_status_and_does_not_echo_body(self):
        response = Response(b"secret-marker" + b"x" * ERROR_RESPONSE_LIMIT)
        error = HTTPError("http://test.invalid", 503, "Unavailable", {}, response)
        with patch.object(self.client._opener, "open", side_effect=error):
            with self.assertRaisesRegex(ExoAnchorError, "HTTP 503") as caught:
                self.client.get_json("/api/test")
        self.assertNotIn("secret-marker", str(caught.exception))
        self.assertTrue(response.closed)

    def test_trickle_response_cannot_extend_total_deadline(self):
        response = Response(b'{"ok":true}')
        with patch.object(self.client._opener, "open", return_value=response), \
             patch("exoanchor_mcp.client.time.monotonic", side_effect=[0, 0, 6]):
            with self.assertRaisesRegex(ExoAnchorError, "deadline"):
                self.client._request("GET", "/api/test", timeout=5)
        self.assertTrue(response.closed)

    def test_cleartext_requires_explicit_legacy_opt_in(self):
        with self.assertRaisesRegex(ExoAnchorError, "HTTPS"):
            ExoAnchorClient(client_config(allow_insecure_http=False))


if __name__ == "__main__":
    unittest.main()
