import json
import tempfile
import unittest
from unittest.mock import Mock, patch

from exoanchor_mcp.jobs import OpsJobManager


class OpsStorageTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.runner = Mock(return_value={"ok": True})
        self.manager = OpsJobManager(self.directory.name, self.runner, scheduler=False)
        self.job, _ = self.manager.create(title="Health", device_id="test", interval_seconds=60)

    def tearDown(self):
        self.manager.close()
        self.directory.cleanup()

    def test_failed_pause_does_not_change_memory_or_schedule(self):
        with patch.object(self.manager, "_persist_job", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.manager.set_paused(self.job["job_id"], True)
        self.assertEqual(self.manager.status(self.job["job_id"]), self.job)
        path = self.manager.jobs_dir / f"{self.job['job_id']}.json"
        self.assertEqual(json.loads(path.read_text()), self.job)

    def test_failed_run_creation_does_not_advance_job_or_reserve_run(self):
        with patch.object(self.manager, "_persist_run", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.manager.start_run(self.job["job_id"])
        self.assertEqual(self.manager.status(self.job["job_id"]), self.job)
        self.assertEqual(self.manager._runs, {})
        self.assertEqual(self.manager._active_runs, {})
        self.assertEqual(self.manager._futures, {})
        self.runner.assert_not_called()

    def test_failed_job_update_leaves_a_blocked_unexecuted_run(self):
        with patch.object(self.manager, "_persist_job", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.manager.start_run(self.job["job_id"])
        self.assertEqual(self.manager.status(self.job["job_id"]), self.job)
        self.assertEqual(self.manager._active_runs, {})
        self.assertEqual(self.manager._futures, {})
        self.runner.assert_not_called()
        records = list(self.manager.runs_dir.glob("*.json"))
        self.assertEqual(len(records), 1)
        blocked = json.loads(records[0].read_text())
        self.assertEqual(blocked["state"], "blocked")
        self.assertIsNone(blocked["started_at"])
        self.assertEqual(blocked["attempts"][-1]["failure_class"], "storage")
        self.assertEqual(self.manager.run_status(blocked["job_run_id"]), blocked)
        accepted, reused = self.manager.start_run(self.job["job_id"])
        self.assertFalse(reused)
        self.assertNotEqual(accepted["job_run_id"], blocked["job_run_id"])

    def test_scheduler_isolates_storage_failures_and_backs_off(self):
        healthy, _ = self.manager.create(title="Healthy", device_id="test", interval_seconds=60)
        original = self.manager._persist_job
        failures = []

        def persist(job):
            if job["job_id"] == self.job["job_id"]:
                failures.append(job["job_id"])
                raise OSError("disk full")
            original(job)

        with patch.object(self.manager, "_persist_job", side_effect=persist), \
             patch("exoanchor_mcp.jobs.time.monotonic", return_value=0):
            with self.assertLogs("exoanchor_mcp.jobs", level="WARNING"):
                runs = self.manager.tick("2999-01-01T00:00:00Z")
            self.assertEqual(len(runs), 1)
            self.assertEqual(self.manager.run_status(runs[0])["job_id"], healthy["job_id"])
            self.manager.set_paused(healthy["job_id"], True)
            self.assertEqual(self.manager.tick("2999-01-01T00:00:00Z"), [])
            self.assertEqual(len(failures), 1)
        # Once storage recovers, the scheduled retry gets a fresh Run.
        with patch("exoanchor_mcp.jobs.time.monotonic", return_value=61):
            retried = self.manager.tick("2999-01-01T00:00:00Z")
        self.assertEqual(len(retried), 1)
        self.assertEqual(self.manager.run_status(retried[0])["job_id"], self.job["job_id"])
