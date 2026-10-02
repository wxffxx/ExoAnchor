#!/usr/bin/env python3
"""Both clients must authenticate the peer before transmitting a bearer."""
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ssl
import importlib.util
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[4]
sys.path[:0] = [str(ROOT / "toolkit"), str(ROOT / "integrations/exoanchor-mcp"),
               str(ROOT / "integrations/exoanchor-mcp/tests")]
from exoanchor_toolkit.http_device import DeviceHttpClient
from exoanchor_toolkit.errors import ToolkitError
from exoanchor_mcp.client import ExoAnchorClient, ExoAnchorError
from test_client import client_config


def main():
    seen = []
    spec = importlib.util.spec_from_file_location("security_h264_accept", ROOT / "device/ESP32P4/firmware/v0.86.6-dev/tools/h264-accept.py")
    diagnostic = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = diagnostic
    spec.loader.exec_module(diagnostic)
    diagnostic.LEGACY_HTTP = False

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            seen.append(self.headers.get("Authorization"))
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "11")
            self.end_headers()
            self.wfile.write(b'{"ok":true}')

    with tempfile.TemporaryDirectory(prefix="exoanchor-tls-test-") as tmp:
        tmp = Path(tmp)
        for name in ("paired", "other"):
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                            "-days", "1", "-subj", "/CN=synthetic-device", "-keyout", str(tmp / (name + ".key")),
                            "-out", str(tmp / (name + ".pem"))], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(tmp / "paired.pem", tmp / "paired.key")
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01})
        thread.start()
        url = f"https://127.0.0.1:{server.server_port}"
        try:
            for certificate, accepted in ((None, False), (tmp / "other.pem", False), (tmp / "paired.pem", True)):
                for kind in ("toolkit", "mcp"):
                    seen.clear()
                    if kind == "toolkit":
                        client = DeviceHttpClient(url, token="test-only-token", tls_certificate_file=certificate, timeout=2)
                    else:
                        client = ExoAnchorClient(client_config(base_url=url, token="test-only-token", timeout=2,
                                                               tls_certificate_file=certificate, allow_insecure_http=False))
                    try:
                        result = client.get_json("/api/test")
                        assert accepted and result == {"ok": True}
                    except (ToolkitError, ExoAnchorError):
                        assert not accepted
                    assert seen == (["Bearer test-only-token"] if accepted else [])
                diagnostic.TLS_CERTIFICATE = str(certificate) if certificate else None
                seen.clear()
                with diagnostic.VerifiedDeviceSession() as session:
                    try:
                        response = session.get(url + "/api/test", headers={"Authorization": "Bearer test-only-token"}, timeout=2)
                        assert accepted and response.json() == {"ok": True}
                    except diagnostic.requests.RequestException:
                        assert not accepted
                assert seen == (["Bearer test-only-token"] if accepted else [])
                # A rejected WSS identity must fail before the upgrade cookie is sent.
                if not accepted:
                    seen.clear()
                    try:
                        diagnostic.websocket_upgrade("127.0.0.1", server.server_port, "/api/ws/test", "test-only-cookie")
                        raise AssertionError("untrusted WSS accepted")
                    except ssl.SSLError:
                        pass
                    assert not seen
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
    print("Toolkit/MCP/diagnostic HTTPS and WSS certificate pairing: PASS")


if __name__ == "__main__":
    main()
