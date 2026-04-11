import unittest
from argparse import Namespace
from unittest.mock import patch

from exoanchor.cli import app as cli_app


class CliCommandTests(unittest.TestCase):
    def test_approve_session_uses_session_endpoint(self):
        with patch.object(cli_app, "resolve_run_target", return_value=("session", "run123")), \
             patch.object(cli_app, "request", return_value={"status": "ok"}) as mock_request, \
             patch.object(cli_app, "print_json") as mock_print:
            code = cli_app.cmd_approve(Namespace(base_url="http://127.0.0.1:8090", target_id="sess1", kind="auto"))

        self.assertEqual(code, 0)
        mock_request.assert_called_once_with("http://127.0.0.1:8090", "POST", "/api/agent/sessions/sess1/approve")
        mock_print.assert_called_once()

    def test_resume_run_saved_uses_resume_saved_endpoint(self):
        with patch.object(cli_app, "resolve_target", return_value=("run", {"run_id": "run123"})), \
             patch.object(cli_app, "request", return_value={"status": "resumed"}) as mock_request, \
             patch.object(cli_app, "print_json"):
            code = cli_app.cmd_resume(Namespace(base_url="http://127.0.0.1:8090", target_id="run123", kind="run", saved=True))

        self.assertEqual(code, 0)
        mock_request.assert_called_once_with("http://127.0.0.1:8090", "POST", "/api/agent/runs/run123/resume_saved")

    def test_abort_task_verifies_current_task_before_abort(self):
        with patch.object(cli_app, "resolve_target", return_value=("task", {"task_id": "task123"})), \
             patch.object(cli_app, "ensure_current_task_matches") as mock_ensure, \
             patch.object(cli_app, "request", return_value={"status": "aborting"}) as mock_request, \
             patch.object(cli_app, "print_json"):
            code = cli_app.cmd_abort(Namespace(base_url="http://127.0.0.1:8090", target_id="task123", kind="task"))

        self.assertEqual(code, 0)
        mock_ensure.assert_called_once_with("http://127.0.0.1:8090", "task123")
        mock_request.assert_called_once_with("http://127.0.0.1:8090", "POST", "/api/agent/task/abort")


if __name__ == "__main__":
    unittest.main()
