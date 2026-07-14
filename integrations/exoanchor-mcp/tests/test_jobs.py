import json
import tempfile
import time
import unittest
from pathlib import Path

from exoanchor_mcp.jobs import JobConflictError, JobManager


class JobManagerTests(unittest.TestCase):
    def test_restart_marks_active_job_interrupted_instead_of_guessing(self):
        with tempfile.TemporaryDirectory() as state_dir:
            path = Path(state_dir) / "job_restart.json"
            path.write_text(json.dumps({
                "job_id": "job_restart",
                "audit_id": "audit_restart",
                "idempotency_key": "restart-test",
                "request_sha256": "unused",
                "state": "running",
                "created_at": "2026-07-14T00:00:00Z",
                "started_at": "2026-07-14T00:00:01Z",
                "finished_at": None,
                "cancel_requested": False,
                "request": {"operation": "system_summary"},
            }), encoding="utf-8")
            manager = JobManager(state_dir)
            status = manager.status("job_restart")
            self.assertEqual(status["state"], "interrupted")
            self.assertIn("remote completion is unknown", status["error"])
            persisted = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(persisted["state"], "interrupted")

    def test_idempotency_key_cannot_alias_different_request(self):
        with tempfile.TemporaryDirectory() as state_dir:
            manager = JobManager(state_dir)
            manager.start(
                {"operation": "system_summary"},
                lambda: {"ok": True, "output": "one"},
                idempotency_key="same-key",
            )
            with self.assertRaisesRegex(JobConflictError, "different SSH request"):
                manager.start(
                    {"operation": "disk_usage"},
                    lambda: {"ok": True, "output": "two"},
                    idempotency_key="same-key",
                )

    def test_failed_job_remains_queryable(self):
        with tempfile.TemporaryDirectory() as state_dir:
            manager = JobManager(state_dir)

            def fail():
                raise RuntimeError("simulated disconnect")

            started, _ = manager.start(
                {"operation": "system_summary"},
                fail,
                idempotency_key="failure-test",
            )
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                status = manager.status(started["job_id"])
                if status["state"] == "failed":
                    break
                time.sleep(0.01)
            self.assertEqual(status["state"], "failed")
            self.assertEqual(status["error"], "simulated disconnect")
            result = manager.result(started["job_id"])
            self.assertFalse(result["result_available"])


if __name__ == "__main__":
    unittest.main()
