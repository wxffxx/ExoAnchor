from __future__ import annotations

import io
import sys
import tempfile
import unittest
import urllib.request
from email.message import Message
from http.client import BadStatusLine, IncompleteRead
from pathlib import Path
from unittest.mock import patch
from urllib.response import addinfourl

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.releases import (
    ReleaseAsset,
    _asset_from_json,
    _download_archive,
    _read_json_response,
    _release_from_json,
    download_firmware_release,
    firmware_releases,
)


class ReleaseTransportTests(unittest.TestCase):
    def test_response_read_failures_use_toolkit_errors(self):
        class BrokenResponse(io.BytesIO):
            def read(self, size=-1):
                raise failure

        for kind in ("metadata", "archive"):
            for failure in (IncompleteRead(b"part", 10), BadStatusLine("invalid status")):
                with self.subTest(kind=kind, failure=type(failure).__name__), \
                        tempfile.TemporaryDirectory() as temporary:
                    response = BrokenResponse()
                    with self.assertRaisesRegex(ToolkitError, "cannot (query|download)"):
                        if kind == "metadata":
                            _read_json_response(urllib.request.Request("https://api.github.com/test"),
                                                opener=lambda *a, **kw: response)
                        else:
                            _download_archive(
                                ReleaseAsset(1, "test.zip", "https://github.com/test.zip", 10, None),
                                Path(temporary) / "test.zip", token="synthetic-test-token",
                                opener=lambda *a, **kw: response,
                            )
                    self.assertTrue(response.closed)

    def test_malformed_metadata_and_parser_depth_use_toolkit_errors(self):
        payloads = [b"{broken", b"\xff", b"[" * 2000 + b"0" + b"]" * 2000]
        for payload in payloads:
            with self.subTest(bytes=len(payload)):
                response = io.BytesIO(payload)
                with self.assertRaisesRegex(ToolkitError, "invalid release metadata"):
                    _read_json_response(urllib.request.Request("https://api.github.com/test"),
                                        opener=lambda *a, **kw: response)
                self.assertTrue(response.closed)

    def test_metadata_integer_limit_uses_toolkit_error(self):
        limit = getattr(sys, "get_int_max_str_digits", lambda: 0)()
        if not limit:
            self.skipTest("interpreter has no integer digit limit")
        with self.assertRaisesRegex(ToolkitError, "invalid release metadata"):
            _read_json_response(urllib.request.Request("https://api.github.com/test"),
                                opener=lambda *a, **kw: io.BytesIO(b"9" * (limit + 1)))

    def test_nonfinite_identifiers_and_asset_sizes_are_ignored(self):
        asset = {
            "id": 1, "size": 10, "state": "uploaded",
            "name": "exoanchor-firmware-test.zip",
            "browser_download_url": "https://github.com/test/package.zip",
        }
        release = {
            "id": 1, "assets": [asset], "tag_name": "vtest", "name": "test",
            "published_at": "2026-01-01T00:00:00Z",
            "html_url": "https://github.com/test/releases/vtest",
        }
        self.assertIsNotNone(_release_from_json(release, include_prerelease=True))
        for value in (float("inf"), float("-inf")):
            for field in ("id", "size"):
                with self.subTest(kind="asset", field=field, value=value):
                    self.assertIsNone(_asset_from_json({**asset, field: value}))
            with self.subTest(kind="release", value=value):
                self.assertIsNone(_release_from_json({**release, "id": value},
                                                     include_prerelease=True))

    def exercise_redirect(self, kind, destination, code, *, retain_token=False):
        requests = []
        streams = []
        origin = (
            "https://api.github.com/repos/example/project/releases"
            if kind == "metadata" else
            "https://github.com/example/project/releases/download/v1/package.zip"
        )
        url = destination or origin + "?renamed=1"
        payload = b"[]" if kind == "metadata" else b"test archive"

        def fake_open(_handler, request):
            requests.append((request.full_url, request.get_header("Authorization")))
            headers = Message()
            if len(requests) == 1:
                headers["Location"] = url
                data, status = b"", code
            else:
                data, status = payload, 200
            stream = io.BytesIO(data)
            streams.append(stream)
            response = addinfourl(stream, headers, request.full_url, status)
            response.msg = "test response"
            return response

        # Exercise the production opener's real redirect processor. Only the
        # lowest HTTP(S) handlers are replaced: no socket or device is used.
        with patch.dict("os.environ", {}, clear=True), \
                patch.object(urllib.request, "_opener", None), \
                patch.object(urllib.request.HTTPSHandler, "https_open", fake_open), \
                patch.object(urllib.request.HTTPHandler, "http_open", fake_open), \
                tempfile.TemporaryDirectory() as temporary:
            def request():
                if kind == "metadata":
                    opener = firmware_releases.__kwdefaults__["opener"]
                    return _read_json_response(urllib.request.Request(origin, headers={
                        "Authorization": "Bearer synthetic-test-token",
                    }), opener=opener)
                opener = download_firmware_release.__kwdefaults__["download_opener"]
                return _download_archive(
                    ReleaseAsset(1, "test.zip", origin, len(payload), None),
                    Path(temporary) / "test.zip",
                    token="synthetic-test-token", opener=opener,
                )

            if url.startswith("http:"):
                with self.assertRaises(ToolkitError):
                    request()
                self.assertEqual(len(requests), 1, "HTTPS must not downgrade")
            else:
                request()
                self.assertEqual(len(requests), 2)
                expected = "Bearer synthetic-test-token" if not destination or retain_token else None
                self.assertEqual(requests[1][1], expected,
                                 "credentials must stay at their original origin")
            self.assertEqual(requests[0][1], "Bearer synthetic-test-token")
            self.assertTrue(all(stream.closed for stream in streams),
                            "redirect and final/error responses must be closed")

    def test_cross_origin_redirect_drops_credentials(self):
        for kind in ("metadata", "archive"):
            for code in (301, 302, 303, 307, 308):
                with self.subTest(kind=kind, code=code):
                    self.exercise_redirect(kind, "https://cdn.example.test/content", code)

    def test_same_origin_redirect_retains_credentials(self):
        for kind in ("metadata", "archive"):
            with self.subTest(kind=kind):
                self.exercise_redirect(kind, None, 302)

    def test_https_redirect_cannot_downgrade_to_http(self):
        for kind in ("metadata", "archive"):
            with self.subTest(kind=kind):
                self.exercise_redirect(kind, "http://cdn.example.test/content", 302)

    def test_origin_comparison_includes_the_effective_port(self):
        for kind, host in (("metadata", "api.github.com"), ("archive", "github.com")):
            with self.subTest(kind=kind, port=444):
                self.exercise_redirect(kind, f"https://{host}:444/content", 302)
            with self.subTest(kind=kind, port=443):
                self.exercise_redirect(kind, f"https://{host}:443/content", 302,
                                       retain_token=True)


if __name__ == "__main__":
    unittest.main()
