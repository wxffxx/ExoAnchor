import unittest
from unittest.mock import MagicMock, patch

from exoanchor_mcp.client import ExoAnchorClient, ExoAnchorError
from test_client import client_config


class LoginDeadlineTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.sleeps = []
        self.client = ExoAnchorClient(client_config(timeout=1))

    def sleep(self, delay):
        self.sleeps.append(delay)
        self.now += delay

    def login(self, request):
        with patch.object(self.client, "_request", side_effect=request), \
             patch("exoanchor_mcp.client.time.monotonic", side_effect=lambda: self.now), \
             patch("exoanchor_mcp.client.time.sleep", side_effect=self.sleep):
            return self.client.login()

    def test_remaining_budget_reaches_http_transport(self):
        response = MagicMock()
        response.__enter__.return_value = response
        response.headers = {"Content-Type": "application/json"}
        response.read.return_value = b'{"token":"fresh"}'
        with patch.object(self.client._opener, "open", return_value=response) as transport:
            self.client._request("GET", "/api/auth/login/status", timeout=0.25)
        self.assertEqual(transport.call_args.kwargs["timeout"], 0.25)

    def test_initial_request_and_polls_share_one_budget(self):
        calls = []

        def request(method, path, *args, **kwargs):
            calls.append((method, path, kwargs))
            if method == "POST":
                self.now += 0.6
                return {"pending": True, "job_id": "job &?#", "poll_after_ms": 100}
            self.assertAlmostEqual(kwargs["timeout"], 0.3)
            self.assertEqual(path, "/api/auth/login/status?job_id=job+%26%3F%23")
            return {"token": "fresh"}

        self.assertEqual(self.login(request)["token"], "fresh")
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0][2]["timeout"], 1)

    def test_slow_initial_request_does_not_start_polling(self):
        calls = []

        def request(*args, **kwargs):
            calls.append(args)
            self.now += 1.1
            return {"pending": True, "job_id": "job"}

        with self.assertRaisesRegex(ExoAnchorError, "login timed out"):
            self.login(request)
        self.assertEqual(len(calls), 1)
        self.assertEqual(self.sleeps, [])
        self.assertIsNone(self.client.token)

    def test_sleep_cannot_launch_a_poll_after_deadline(self):
        calls = []

        def request(*args, **kwargs):
            calls.append(args)
            self.now += 0.95
            return {"pending": True, "job_id": "job", "poll_after_ms": 1000}

        with self.assertRaisesRegex(ExoAnchorError, "login timed out"):
            self.login(request)
        self.assertEqual(len(calls), 1)
        self.assertAlmostEqual(self.now, 1)
        self.assertAlmostEqual(self.sleeps[0], 0.05)

    def test_late_success_is_not_installed_as_a_token(self):
        def request(*args, **kwargs):
            self.now += 1.1
            return {"token": "late"}

        with self.assertRaisesRegex(ExoAnchorError, "login timed out"):
            self.login(request)
        self.assertIsNone(self.client.token)

    def test_invalid_poll_delay_uses_default_delay(self):
        for value in (None, "broken", [], {}, float("nan"), float("inf")):
            with self.subTest(value=value):
                self.now = 0
                self.sleeps.clear()
                responses = iter([
                    {"pending": True, "job_id": "job", "poll_after_ms": value},
                    {"token": "fresh"},
                ])
                self.login(lambda *args, **kwargs: next(responses))
                self.assertEqual(self.sleeps, [0.1])
