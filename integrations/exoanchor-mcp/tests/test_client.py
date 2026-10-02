import unittest
import io
import threading
from concurrent.futures import ThreadPoolExecutor
from urllib.error import HTTPError
from unittest.mock import patch

from exoanchor_mcp.client import ExoAnchorClient, ExoAnchorConfig, ExoAnchorError


def client_config(**overrides):
    fields = dict(
        base_url="http://device.test", username="admin", password="password",
        token=None, timeout=75, allow_write=False, control_owner="mcp",
        device_id="test", state_dir="/unused", persist_job_output=False,
        allow_unverified_ssh_host=False, allow_arbitrary_ssh=False, allow_insecure_http=True,
    )
    fields.update(overrides)
    return ExoAnchorConfig(**fields)


class TransportTests(unittest.TestCase):
    def test_nonfinite_timeout_rejected(self):
        for value in ("nan", "inf", "-inf"):
            with self.subTest(value=value), patch.dict("os.environ", {
                "EXOANCHOR_BASE_URL": "http://device.test",
                "EXOANCHOR_TIMEOUT": value,
            }):
                with self.assertRaises(ExoAnchorError):
                    ExoAnchorConfig.from_env()

    def test_mutating_request_is_not_replayed_on_401(self):
        client = ExoAnchorClient(client_config(token="expired"))
        body = io.BytesIO(b"authentication required")
        error = HTTPError("http://device.test", 401, "Unauthorized", {}, body)
        with patch.object(client._opener, "open", side_effect=error) as request, \
             patch.object(client, "login") as login:
            with self.assertRaisesRegex(ExoAnchorError, "HTTP 401"):
                client.post_json("/api/power", {"action": "reset"})
        self.assertEqual(request.call_count, 1)
        login.assert_not_called()
        self.assertTrue(body.closed)

    def test_concurrent_authentication_uses_one_login(self):
        client = ExoAnchorClient(client_config(token="expired"))
        barrier = threading.Barrier(4)

        def login():
            client.token = "fresh"
            return {"token": "fresh"}

        def authenticate():
            barrier.wait(timeout=2)
            client._ensure_authenticated("expired")

        with patch.object(client, "_login", side_effect=login) as verifier, \
             ThreadPoolExecutor(max_workers=4) as executor:
            list(executor.map(lambda _: authenticate(), range(4)))
        self.assertEqual(verifier.call_count, 1)
        self.assertEqual(client.token, "fresh")


class LoginPollingTests(unittest.TestCase):
    def test_invalid_token_type_does_not_replace_an_existing_session(self):
        for token in (True, 7, ["not-a-token"], {"secret": "do-not-echo"}):
            with self.subTest(token_type=type(token).__name__):
                client = ExoAnchorClient(client_config(token="existing-token"))
                with patch.object(client, "_request", return_value={"token": token}):
                    with self.assertRaisesRegex(ExoAnchorError, "token") as caught:
                        client.login()
                self.assertEqual(client.token, "existing-token")
                self.assertNotIn("do-not-echo", str(caught.exception))

    def test_invalid_login_response_does_not_allow_the_original_post(self):
        client = ExoAnchorClient(client_config())
        with patch.object(client, "_request", return_value={"token": {"bad": True}}) as request:
            with self.assertRaises(ExoAnchorError):
                client.post_json("/api/example", {"message": "test"})
        self.assertEqual(request.call_count, 1)
        self.assertEqual(request.call_args.args[:2], ("POST", "/api/auth/login"))
        self.assertIsNone(client.token)

    def test_polled_login_response_has_the_same_token_validation(self):
        client = ExoAnchorClient(client_config(token="existing-token"))
        with patch.object(client, "_request", side_effect=[
            {"pending": True, "job_id": "login-job"}, {"token": ["invalid"]},
        ]) as request, patch("exoanchor_mcp.client.time.sleep"):
            with self.assertRaises(ExoAnchorError):
                client.login()
        self.assertEqual(request.call_count, 2)
        self.assertEqual(client.token, "existing-token")

    def test_login_polling_uses_configured_transport_timeout(self):
        config = ExoAnchorConfig(
            base_url="http://device.test",
            username="admin",
            password="local-password",
            token=None,
            timeout=75,
            allow_write=False,
            control_owner="mcp",
            device_id="test-device",
            state_dir="/tmp/exoanchor-mcp-test",
            persist_job_output=False,
            allow_unverified_ssh_host=False,
            allow_arbitrary_ssh=False,
            allow_insecure_http=True,
        )
        client = ExoAnchorClient(config)
        responses = [
            {"pending": True, "job_id": "login-job", "poll_after_ms": 100},
            {"token": "test-token"},
        ]

        with patch.object(client, "_request", side_effect=responses), \
             patch("exoanchor_mcp.client.time.monotonic", side_effect=[0.0, 31.0, 31.1, 31.2]), \
             patch("exoanchor_mcp.client.time.sleep"):
            result = client.login()

        self.assertEqual(result["token"], "test-token")
        self.assertEqual(client.token, "test-token")


if __name__ == "__main__":
    unittest.main()
