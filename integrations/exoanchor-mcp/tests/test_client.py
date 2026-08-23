import unittest
from unittest.mock import patch

from exoanchor_mcp.client import ExoAnchorClient, ExoAnchorConfig


class LoginPollingTests(unittest.TestCase):
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
        )
        client = ExoAnchorClient(config)
        responses = [
            {"pending": True, "job_id": "login-job", "poll_after_ms": 100},
            {"token": "test-token"},
        ]

        with patch.object(client, "_request", side_effect=responses), \
             patch("exoanchor_mcp.client.time.monotonic", side_effect=[0.0, 31.0]), \
             patch("exoanchor_mcp.client.time.sleep"):
            result = client.login()

        self.assertEqual(result["token"], "test-token")
        self.assertEqual(client.token, "test-token")


if __name__ == "__main__":
    unittest.main()
