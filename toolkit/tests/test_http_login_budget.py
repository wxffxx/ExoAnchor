import json
import unittest
from contextlib import contextmanager
from types import SimpleNamespace
from unittest.mock import patch

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.http_device import DeviceHttpClient


class Clock:
    def __init__(self):
        self.now = 0.0
        self.sleeps = []

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


def response(status, **body):
    return status, json.dumps(body).encode()


class LoginBudgetTests(unittest.TestCase):
    def client(self, clock, **kwargs):
        return DeviceHttpClient("192.0.2.8", password="test", token="previous-token", sleeper=clock.sleep, **kwargs)

    def test_initial_submission_consumes_the_login_budget(self):
        clock = Clock()
        client = self.client(clock)

        def request(method, path, **kwargs):
            if method == "POST":
                clock.now = 29.95
                return response(202, job_id="login", poll_after_ms=100)
            return response(200, token="too-late")

        with patch("exoanchor_toolkit.http_device.time.monotonic", side_effect=lambda: clock.now), \
             patch.object(client, "_request", side_effect=request) as requests:
            with self.assertRaisesRegex(ToolkitError, "login timed out"):
                client.login()
        self.assertEqual(requests.call_count, 1)
        self.assertAlmostEqual(clock.now, 30)
        self.assertEqual(client.token, "previous-token")

    def test_late_direct_success_does_not_replace_the_existing_token(self):
        clock = Clock()
        client = self.client(clock)

        def request(*args, **kwargs):
            clock.now = 31
            return response(200, token="late-token")

        with patch("exoanchor_toolkit.http_device.time.monotonic", side_effect=lambda: clock.now), \
             patch.object(client, "_request", side_effect=request):
            with self.assertRaisesRegex(ToolkitError, "login timed out"):
                client.login()
        self.assertEqual(client.token, "previous-token")

    def test_each_request_uses_the_remaining_budget(self):
        clock = Clock()
        client = self.client(clock, timeout=75)
        timeouts = []

        def request(method, path, **kwargs):
            timeouts.append(kwargs.get("timeout"))
            if method == "POST":
                clock.now = 29
                return response(202, job_id="job /?&", poll_after_ms=100)
            self.assertEqual(path, "/api/auth/login/status?job_id=job%20%2F%3F%26")
            return response(200, token="fresh")

        with patch("exoanchor_toolkit.http_device.time.monotonic", side_effect=lambda: clock.now), \
             patch.object(client, "_request", side_effect=request):
            client.login()
        self.assertEqual(timeouts[0], 30)
        self.assertAlmostEqual(timeouts[1], 0.9)
        self.assertEqual(client.token, "fresh")

    def test_oversleep_does_not_start_another_request(self):
        clock = Clock()
        client = self.client(clock)
        client._sleep = lambda _: setattr(clock, "now", 31)
        with patch("exoanchor_toolkit.http_device.time.monotonic", side_effect=lambda: clock.now), \
             patch.object(client, "_request", side_effect=[
                 response(202, job_id="job"), response(200, token="late"),
             ]) as requests:
            with self.assertRaisesRegex(ToolkitError, "login timed out"):
                client.login()
        self.assertEqual(requests.call_count, 1)

    def test_late_poll_success_is_rejected(self):
        clock = Clock()
        client = self.client(clock)

        def request(method, path, **kwargs):
            if method == "POST":
                return response(202, job_id="job")
            clock.now = 31
            return response(200, token="late")

        with patch("exoanchor_toolkit.http_device.time.monotonic", side_effect=lambda: clock.now), \
             patch.object(client, "_request", side_effect=request):
            with self.assertRaisesRegex(ToolkitError, "login timed out"):
                client.login()
        self.assertEqual(client.token, "previous-token")

    def test_malformed_poll_delay_uses_a_bounded_fallback(self):
        for delay in (None, "later", {}, [], float("nan"), float("inf"), 10 ** 1000):
            with self.subTest(delay_type=type(delay).__name__):
                clock = Clock()
                client = self.client(clock)
                with patch("exoanchor_toolkit.http_device.time.monotonic", side_effect=lambda: clock.now), \
                     patch.object(client, "_request", side_effect=[
                         response(202, job_id="job", poll_after_ms=delay), response(200, token="fresh"),
                     ]):
                    client.login()
                self.assertEqual(clock.sleeps, [0.1])
                self.assertEqual(client.token, "fresh")

    def test_request_timeout_override_reaches_the_opener(self):
        timeouts = []

        @contextmanager
        def opener(request, *, timeout):
            timeouts.append(timeout)
            yield SimpleNamespace(status=200, read=lambda _: b'{"ok":true}')

        client = DeviceHttpClient("192.0.2.8", timeout=10, opener=opener)
        client._request("GET", "/api/example", timeout=0.75)
        client.get_json("/api/example")
        self.assertEqual(timeouts, [0.75, 10])


if __name__ == "__main__":
    unittest.main()
