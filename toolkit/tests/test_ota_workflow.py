"""Release-to-OTA acceptance with synthetic images and real loopback HTTP.

Only release URL routing is injected; OTA uses the production HTTP transport.
No GitHub connection, physical device, Flash operation or boot is involved.
"""
from __future__ import annotations

import hashlib
import io
import json
import socket
import tempfile
import threading
import unittest
import urllib.parse
import urllib.request
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.firmware import MANIFEST_NAME
from exoanchor_toolkit.http_device import DeviceHttpClient, NetworkOtaUpdater
from exoanchor_toolkit.releases import download_firmware_release

BOARD = "exoanchor-prototype-v2.3"
TOKEN = "synthetic-offline-token"
IMAGE = bytes(range(256)) * 641


def make_archive(*, corrupt_segment=False):
    files = {"boot.bin": b"boot", "partitions.bin": b"partitions", "ota.bin": b"ota", "app.bin": IMAGE}
    manifest = {
        "schema_version": 1, "product": "exoanchor-esp32p4",
        "firmware": {"version": "test", "board": BOARD, "chip": "esp32p4", "flash_size": "16MB"},
        "flash": {"mode": "dio", "frequency": "80m"},
        "segments": [{"address": hex(address), "file": name, "size": len(content),
                      "sha256": hashlib.sha256(content).hexdigest()}
                     for address, (name, content) in zip((0x2000, 0x8000, 0xF000, 0x20000), files.items())],
    }
    if corrupt_segment:
        files["app.bin"] = b"X" + IMAGE[1:]
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(MANIFEST_NAME, json.dumps(manifest))
        for name, content in files.items():
            archive.writestr(name, content)
    return output.getvalue()


class DeviceState:
    def __init__(self):
        self.board = BOARD
        self.active = False
        self.session = "synthetic-session"
        self.size = 0
        self.sha = ""
        self.uploaded = bytearray()
        self.events = []
        self.chunks = []
        self.reboots = 0
        self.start_count = 0
        self.drop_after_bytes = None
        self.dropped = False
        self.status_overrides = {}
        self.recovery_overrides = {}
        self.start_overrides = {}
        self.chunk_overrides = {}
        self.finish_overrides = {}
        self.reboot_overrides = {}

    def status(self):
        result = {"ok": True, "active": self.active, "session_id": self.session,
                  "size": self.size, "sha256": self.sha, "offset": len(self.uploaded), "chunk_size": 65536}
        result.update(self.status_overrides)
        if self.dropped:
            result.update(self.recovery_overrides)
        return result


class WorkflowHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def send_payload(self, value, status=200):
        payload = value if isinstance(value, bytes) else json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        state = self.server.state
        state.events.append(("GET", self.path))
        if self.path == "/releases":
            self.send_payload([{
                "id": 1, "tag_name": "firmware-vtest", "name": "Offline test",
                "published_at": "2026-01-01T00:00:00Z",
                "html_url": "https://github.com/example/project/releases/tag/firmware-vtest",
                "assets": [{"id": 1, "name": "exoanchor-firmware-test.zip", "state": "uploaded",
                            "browser_download_url": "https://github.com/example/project/releases/download/firmware-vtest/test.zip",
                            "size": len(self.server.archive), "digest": "sha256:" + self.server.archive_digest}],
            }])
        elif self.path == "/archive":
            self.send_payload(self.server.archive)
        elif self.headers.get("Authorization") != "Bearer " + TOKEN:
            self.send_payload({"error": "unauthorized"}, 401)
        elif self.path == "/api/capabilities":
            self.send_payload({"device": {"board": state.board}})
        elif self.path == "/api/ota/upload/status":
            self.send_payload(state.status())
        else:
            self.send_payload({"error": "unknown test route"}, 404)

    def do_POST(self):
        state = self.server.state
        route = urllib.parse.urlsplit(self.path)
        state.events.append(("POST", route.path))
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            self.send_payload({"error": "unauthorized"}, 401)
            return
        if route.path == "/api/ota/upload/start":
            request = json.loads(body)
            state.start_count += 1
            state.active = True
            state.size, state.sha = request["size"], request["sha256"]
            state.uploaded.clear()
            self.send_payload({**state.status(), **state.start_overrides})
        elif route.path == "/api/ota/upload/chunk":
            query = urllib.parse.parse_qs(route.query)
            offset = int(query["offset"][0])
            if (not state.active or query["session"] != [state.session] or offset != len(state.uploaded)):
                self.send_payload({"error": "invalid session or replayed chunk"}, 409)
                return
            state.chunks.append((offset, len(body)))
            if state.drop_after_bytes is not None and not state.dropped:
                state.uploaded.extend(body[:state.drop_after_bytes])
                state.dropped = True
                self.close_connection = True
                self.connection.shutdown(socket.SHUT_RDWR)
                return
            state.uploaded.extend(body)
            self.send_payload({**state.status(), **state.chunk_overrides})
        elif route.path == "/api/ota/upload/finish":
            if (json.loads(body).get("session_id") != state.session or len(state.uploaded) != state.size
                    or hashlib.sha256(state.uploaded).hexdigest() != state.sha):
                self.send_payload({"error": "incomplete or mismatched image"}, 400)
                return
            state.active = False
            self.send_payload({"ok": True, "sha256": state.sha, **state.finish_overrides})
        elif route.path == "/api/ota/reboot":
            state.reboots += 1
            self.send_payload({"ok": True, **state.reboot_overrides})
        else:
            self.send_payload({"error": "unknown test route"}, 404)


class OtaWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), WorkflowHandler)
        self.server.state = DeviceState()
        self.server.archive = make_archive()
        self.server.archive_digest = hashlib.sha256(self.server.archive).hexdigest()
        self.handler_errors = []
        self.server.handle_error = lambda *_: self.handler_errors.append("handler failed")
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:" + str(self.server.server_port)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.assertFalse(self.thread.is_alive())
        self.assertEqual(self.handler_errors, [])

    @property
    def state(self):
        return self.server.state

    def download(self):
        def local_open(request, *, timeout):
            # Retain the production downloader, hash checks and extractor, but
            # route both synthetic release URLs to this loopback fixture.
            route = "/releases" if request.full_url.startswith("https://api.github.com/") else "/archive"
            return urllib.request.build_opener(urllib.request.ProxyHandler({})).open(
                self.base + route, timeout=timeout)
        return download_firmware_release("example/project", output_dir=self.temp.name,
                                         api_opener=local_open, download_opener=local_open)

    def updater(self):
        return NetworkOtaUpdater(DeviceHttpClient(self.base, token=TOKEN, timeout=2))

    def assert_no_finish(self):
        self.assertNotIn(("POST", "/api/ota/upload/finish"), self.state.events)
        self.assertEqual(self.state.reboots, 0)

    def test_download_verify_cached_package_and_repeat_uploads(self):
        downloaded = self.download()
        self.assertFalse(downloaded.cached)
        for _ in range(3):
            lines = self.updater().update(downloaded.package)
            self.assertEqual(bytes(self.state.uploaded), IMAGE)
            self.assertIn("Device reboot requested", lines)
        self.assertEqual(self.state.start_count, 3)
        self.assertEqual(self.state.reboots, 3)
        before = self.state.events.count(("GET", "/archive"))
        self.assertTrue(self.download().cached)
        self.assertEqual(self.state.events.count(("GET", "/archive")), before)

    def test_lost_chunk_response_resumes_only_from_confirmed_offset(self):
        package = self.download().package
        for accepted in (1200, 65536):
            with self.subTest(accepted=accepted):
                self.server.state = DeviceState()
                self.state.drop_after_bytes = accepted
                lines = self.updater().update(package, reboot=False)
                self.assertEqual(self.state.chunks[1][0], accepted)
                self.assertEqual(bytes(self.state.uploaded), IMAGE)
                self.assertEqual(self.state.start_count, 1)
                self.assertEqual(self.state.reboots, 0)
                self.assertIn("Reboot required to activate the update", lines)

    def test_no_progress_stops_and_a_later_invocation_resumes(self):
        package = self.download().package
        self.state.drop_after_bytes = 0
        with self.assertRaises(ToolkitError):
            self.updater().update(package)
        self.assertEqual(len(self.state.chunks), 1)
        self.assert_no_finish()
        self.updater().update(package, reboot=False)
        self.assertEqual(self.state.start_count, 1)
        self.assertEqual(bytes(self.state.uploaded), IMAGE)

    def test_image_changes_after_manifest_verification_never_contact_device(self):
        package = self.download().package
        app = package.segments[-1].path
        for replacement in (b"X" + IMAGE[1:], IMAGE[:-1], IMAGE + b"X"):
            with self.subTest(size=len(replacement)):
                app.write_bytes(replacement)
                self.state.events.clear()
                with self.assertRaisesRegex(ToolkitError, "changed after verification"):
                    self.updater().update(package)
                self.assertEqual(self.state.events, [])

    def test_mismatched_board_never_starts_upload(self):
        package = self.download().package
        self.state.board = "different-board"
        with self.assertRaisesRegex(ToolkitError, "does not match"):
            self.updater().update(package)
        self.assertEqual(self.state.start_count, 0)
        self.assert_no_finish()

    def test_invalid_initial_status_never_starts_upload(self):
        package = self.download().package
        for overrides in ({"ok": False}, {"active": None}, {"active": 0}):
            with self.subTest(overrides=overrides):
                self.state.status_overrides = overrides
                with self.assertRaises(ToolkitError):
                    self.updater().update(package)
                self.assertEqual(self.state.start_count, 0)

    def test_mismatched_or_malformed_active_session_never_sends_bytes(self):
        package = self.download().package
        for overrides in ({"sha256": "0" * 64}, {"size": len(IMAGE) + 1}, {"session_id": ""},
                          {"offset": True}, {"offset": -1}, {"offset": len(IMAGE) + 1}):
            with self.subTest(overrides=overrides):
                self.server.state = DeviceState()
                self.state.active = True
                self.state.size = len(IMAGE)
                self.state.sha = hashlib.sha256(IMAGE).hexdigest()
                self.state.status_overrides = overrides
                with self.assertRaises(ToolkitError):
                    self.updater().update(package)
                self.assertEqual(self.state.chunks, [])
                self.assertEqual(self.state.start_count, 0)
                self.assert_no_finish()

    def test_recovery_revalidates_entire_session(self):
        package = self.download().package
        for overrides in ({"sha256": "0" * 64}, {"size": len(IMAGE) + 1}, {"session_id": "replacement"},
                          {"active": False}, {"ok": False}, {"offset": True}, {"offset": 65537}):
            with self.subTest(overrides=overrides):
                self.server.state = DeviceState()
                self.state.drop_after_bytes = 1200
                self.state.recovery_overrides = overrides
                with self.assertRaises(ToolkitError):
                    self.updater().update(package)
                self.assertEqual(len(self.state.chunks), 1)
                self.assert_no_finish()

    def test_start_response_types_are_checked_before_sending_bytes(self):
        package = self.download().package
        for overrides in ({"session_id": 1}, {"session_id": ""}, {"offset": False},
                          {"chunk_size": True}, {"chunk_size": 0}, {"ok": False}):
            with self.subTest(overrides=overrides):
                self.server.state = DeviceState()
                self.state.start_overrides = overrides
                with self.assertRaises(ToolkitError):
                    self.updater().update(package)
                self.assertEqual(self.state.chunks, [])
                self.assert_no_finish()

    def test_new_session_chunk_limit_is_respected(self):
        package = self.download().package
        self.state.start_overrides = {"chunk_size": 1024}
        self.updater().update(package, reboot=False)
        self.assertTrue(all(size <= 1024 for _, size in self.state.chunks))
        self.assertEqual(bytes(self.state.uploaded), IMAGE)

    def test_invalid_chunk_acknowledgements_do_not_finish(self):
        package = self.download().package
        for overrides in ({"offset": True}, {"offset": 0}, {"offset": 65537}, {"ok": False}):
            with self.subTest(overrides=overrides):
                self.server.state = DeviceState()
                self.state.chunk_overrides = overrides
                with self.assertRaises(ToolkitError):
                    self.updater().update(package)
                self.assertEqual(len(self.state.chunks), 1)
                self.assert_no_finish()

    def test_failed_finish_or_wrong_digest_never_reboots(self):
        package = self.download().package
        for overrides in ({"ok": False}, {"sha256": "0" * 64}):
            with self.subTest(overrides=overrides):
                self.server.state = DeviceState()
                self.state.finish_overrides = overrides
                with self.assertRaises(ToolkitError):
                    self.updater().update(package)
                self.assertEqual(self.state.reboots, 0)

    def test_rejected_reboot_is_not_reported_as_success(self):
        package = self.download().package
        self.state.reboot_overrides = {"ok": False}
        with self.assertRaisesRegex(ToolkitError, "did not confirm reboot"):
            self.updater().update(package)
        self.assertEqual(self.state.reboots, 1)

    def test_corrupt_download_or_manifest_does_not_publish_cache(self):
        for corrupt_manifest in (False, True):
            with self.subTest(corrupt_manifest=corrupt_manifest):
                self.server.archive = make_archive(corrupt_segment=corrupt_manifest)
                self.server.archive_digest = (hashlib.sha256(self.server.archive).hexdigest()
                                              if corrupt_manifest else "0" * 64)
                with self.assertRaisesRegex(ToolkitError, "SHA-256 mismatch"):
                    self.download()
                self.assertEqual(list(Path(self.temp.name).iterdir()), [])
                self.assertEqual(self.state.start_count, 0)


if __name__ == "__main__":
    unittest.main()
