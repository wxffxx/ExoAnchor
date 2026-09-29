import json
import tempfile
import time
import unittest
import threading
from contextlib import ExitStack, closing
from unittest.mock import patch
from pathlib import Path

from exoanchor_mcp.jobs import JobConflictError, JobManager, OpsJobManager


class JobManagerTests(unittest.TestCase):
    def test_completed_jobs_release_futures_and_cache_output_digest(self):
        with tempfile.TemporaryDirectory() as state_dir, ExitStack() as resources:
            manager = resources.enter_context(closing(JobManager(state_dir)))
            request = {"args": {"operation": "system_summary"}}
            result = {"ok": True, "output": "世界" * 10000, "detail": {"code": 0}}
            started, _ = manager.start(request, lambda: result)
            manager.close()
            self.assertEqual(manager._futures, {})
            job_id = started["job_id"]
            request["args"]["operation"] = "changed"
            result["output"] = "changed"
            started["request"]["args"]["operation"] = "also changed"
            with patch("exoanchor_mcp.jobs.hashlib.sha256", side_effect=AssertionError("rehashed output")):
                status = manager.status(job_id)
                page = manager.result(job_id, limit=10)
            self.assertEqual(status["artifact"]["bytes"], 60000)
            self.assertEqual(status["request"]["args"]["operation"], "system_summary")
            self.assertEqual(page["output"], "世界" * 5)
            page["result"]["detail"]["code"] = 99
            self.assertEqual(manager.result(job_id)["result"]["detail"]["code"], 0)

    def test_nonobject_result_is_failed_and_queryable(self):
        with tempfile.TemporaryDirectory() as state_dir, ExitStack() as resources:
            manager = resources.enter_context(closing(JobManager(state_dir)))
            started, _ = manager.start({}, lambda: None)
            manager.close()
            status = manager.status(started["job_id"])
            self.assertEqual(status["state"], "failed")
            self.assertIn("non-object", status["error"])
            self.assertEqual(manager._futures, {})

    def test_restart_marks_active_job_interrupted_instead_of_guessing(self):
        with tempfile.TemporaryDirectory() as state_dir, ExitStack() as resources:
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
            manager = resources.enter_context(closing(JobManager(state_dir)))
            status = manager.status("job_restart")
            self.assertEqual(status["state"], "interrupted")
            self.assertIn("remote completion is unknown", status["error"])
            persisted = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(persisted["state"], "interrupted")

    def test_idempotency_key_cannot_alias_different_request(self):
        with tempfile.TemporaryDirectory() as state_dir, ExitStack() as resources:
            manager = resources.enter_context(closing(JobManager(state_dir)))
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
        with tempfile.TemporaryDirectory() as state_dir, ExitStack() as resources:
            manager = resources.enter_context(closing(JobManager(state_dir)))

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


class OpsJobManagerTests(unittest.TestCase):
    def test_active_run_index_deduplicates_and_releases_completed_runs(self):
        release = threading.Event()
        with tempfile.TemporaryDirectory() as state_dir:
            manager = OpsJobManager(state_dir, lambda _: (
                release.wait(timeout=2) and {"ok": True, "outcome": "no_change"}
            ), scheduler=False)
            try:
                job, _ = manager.create(title="Health", device_id="test")
                first, reused = manager.start_run(job["job_id"])
                second, reused = manager.start_run(job["job_id"])
                self.assertTrue(reused)
                self.assertEqual(first["job_run_id"], second["job_run_id"])
                release.set()
                self._wait_for_run(manager, first["job_run_id"])
                third, reused = manager.start_run(job["job_id"])
                self.assertFalse(reused)
                self.assertNotEqual(first["job_run_id"], third["job_run_id"])
                self._wait_for_run(manager, third["job_run_id"])
            finally:
                release.set()
                manager.close()
            self.assertEqual(manager._futures, {})
            self.assertEqual(manager._active_runs, {})

    @staticmethod
    def _wait_for_run(manager, run_id):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            run = manager.run_status(run_id)
            if run["state"] in {
                "succeeded", "no_change", "degraded", "failed", "blocked",
                "cancelled", "interrupted",
            }:
                return run
            time.sleep(0.01)
        raise AssertionError("operations Run did not finish")

    def test_job_run_attempt_and_idempotency_are_separate(self):
        with tempfile.TemporaryDirectory() as state_dir:
            manager = OpsJobManager(
                state_dir,
                lambda job: {
                    "ok": True,
                    "outcome": "no_change",
                    "evidence": {"observation_id": "obs_test"},
                },
                scheduler=False,
            )
            try:
                job, reused = manager.create(
                    title="Daily health",
                    device_id="ea-p4-test",
                    interval_seconds=60,
                    idempotency_key="health-test-key",
                )
                self.assertFalse(reused)
                same, reused = manager.create(
                    title="Daily health",
                    device_id="ea-p4-test",
                    interval_seconds=60,
                    idempotency_key="health-test-key",
                )
                self.assertTrue(reused)
                self.assertEqual(same["job_id"], job["job_id"])

                started, active_reused = manager.start_run(job["job_id"])
                self.assertFalse(active_reused)
                run = self._wait_for_run(manager, started["job_run_id"])
                self.assertEqual(run["outcome"], "no_change")
                self.assertEqual(run["generation"], job["generation"])
                self.assertEqual(len(run["attempts"]), 1)
                self.assertTrue(run["attempts"][0]["attempt_id"].startswith("attempt_"))
                self.assertEqual(run["evidence"]["observation_id"], "obs_test")
            finally:
                manager.close()

    def test_due_interval_coalesces_and_pause_blocks_future_run(self):
        with tempfile.TemporaryDirectory() as state_dir:
            manager = OpsJobManager(
                state_dir,
                lambda job: {"ok": True, "outcome": "no_change"},
                scheduler=False,
            )
            try:
                job, _ = manager.create(
                    title="Interval health",
                    device_id="ea-p4-test",
                    interval_seconds=60,
                    idempotency_key="interval-health",
                )
                due = manager.tick("2999-01-01T00:00:00.000Z")
                self.assertEqual(len(due), 1)
                manager.set_paused(job["job_id"], True)
                self.assertEqual(
                    manager.tick("2999-01-01T00:10:00.000Z"),
                    [],
                )
            finally:
                manager.close()

    def test_restart_marks_active_run_interrupted(self):
        with tempfile.TemporaryDirectory() as state_dir:
            root = Path(state_dir) / "operations"
            jobs = root / "jobs"
            runs = root / "runs"
            jobs.mkdir(parents=True)
            runs.mkdir(parents=True)
            job = {
                "job_id": "job_restart",
                "generation": 1,
                "state": "active",
                "target": {"device_id": "ea-p4-test"},
                "trigger": {"type": "manual", "interval_seconds": None},
                "policy": {"concurrency": "forbid"},
                "created_at": "2026-07-31T00:00:00.000Z",
            }
            run = {
                "job_run_id": "run_restart",
                "job_id": "job_restart",
                "state": "running",
                "phase": "executing",
                "attempts": [{"attempt_id": "attempt_restart", "state": "running"}],
            }
            (jobs / "job_restart.json").write_text(json.dumps(job), encoding="utf-8")
            (runs / "run_restart.json").write_text(json.dumps(run), encoding="utf-8")
            manager = OpsJobManager(
                state_dir,
                lambda value: {"ok": True},
                scheduler=False,
            )
            try:
                recovered = manager.run_status("run_restart")
                self.assertEqual(recovered["state"], "interrupted")
                self.assertEqual(
                    recovered["attempts"][0]["failure_class"],
                    "uncertain_commit",
                )
            finally:
                manager.close()


if __name__ == "__main__":
    unittest.main()
