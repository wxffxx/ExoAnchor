import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.http_device import DeviceHttpClient, parse_device_target


class HttpRedirectTests(unittest.TestCase):
    def test_requests_stay_at_the_explicit_target_and_path(self):
        requests = []
        redirect = {"status": 302, "location": ""}

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_GET(self):
                requests.append((self.server.server_port, self.command, self.path,
                                 self.headers.get("Authorization")))
                self.rfile.read(int(self.headers.get("Content-Length", 0)))
                redirecting = self.path == "/api/example"
                self.send_response(redirect["status"] if redirecting else 200)
                if redirecting:
                    self.send_header("Location", redirect["location"])
                body = b'{"ok":true}'
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            do_POST = do_GET

        servers = [ThreadingHTTPServer(("127.0.0.1", 0), Handler) for _ in range(2)]
        threads = [threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}) for server in servers]
        for thread in threads:
            thread.start()
        try:
            client = DeviceHttpClient(f"127.0.0.1:{servers[0].server_port}", token="test-only-token", timeout=2)
            for method in ("GET", "POST"):
                for status in (301, 302, 303, 307, 308):
                    for target in servers:
                        with self.subTest(method=method, status=status, destination=target.server_port):
                            requests.clear()
                            redirect.update(status=status, location=f"http://127.0.0.1:{target.server_port}/redirected")
                            with self.assertRaisesRegex(ToolkitError, f"HTTP {status}"):
                                if method == "GET":
                                    client.get_json("/api/example")
                                else:
                                    client.post_json("/api/example", {"message": "local test"})
                            self.assertEqual(len(requests), 1, "redirect destination must receive neither credentials nor a replay")
                            self.assertEqual(requests[0][1:], (method, "/api/example", "Bearer test-only-token"))
        finally:
            for server, thread in zip(servers, threads):
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)


class HttpTargetValidationTests(unittest.TestCase):
    def test_invalid_targets_raise_toolkit_error_without_falling_back_to_another_port(self):
        for target in (
            "192.0.2.8:0", "http://192.0.2.8:0/", "http://[broken",
            "http://[192.0.2.8]", "192.0.2.8:not-a-port", "192.0.2.8:65536",
        ):
            with self.subTest(target=target), self.assertRaises(ToolkitError):
                parse_device_target(target)

    def test_valid_default_and_explicit_ports_are_preserved(self):
        for target, port in (("192.0.2.8", 80), ("http://192.0.2.8/", 80), ("192.0.2.8:8080", 8080)):
            with self.subTest(target=target):
                parsed = parse_device_target(target)
                self.assertEqual(parsed.address, "192.0.2.8")
                self.assertEqual(parsed.port, port)


if __name__ == "__main__":
    unittest.main()
