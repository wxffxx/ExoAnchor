import json
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

from exoanchor_mcp.jobs import JobConflictError, JobManager, OpsJobManager


class JobLifecycleTests(unittest.TestCase):
    def test_default_capacity_bounds_a_burst_of_requests(self):
        with tempfile.TemporaryDirectory() as directory:
            release = threading.Event()
            manager = JobManager(directory)
            accepted, rejected = 0, 0

            def block():
                self.assertTrue(release.wait(5))
                return {"ok": True}

            try:
                for _ in range(1000):
                    try:
                        manager.start({}, block)
                        accepted += 1
                    except JobConflictError:
                        rejected += 1
                self.assertEqual((accepted, rejected), (34, 966))
                self.assertEqual(len(list(Path(directory).glob("*.json"))), 34)
            finally:
                release.set()
                manager.close()

    def test_failed_journal_write_does_not_reserve_an_idempotency_key(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = JobManager(directory)
            try:
                with patch.object(manager, "_persist", side_effect=OSError("disk full")):
                    with self.assertRaises(OSError):
                        manager.start({}, lambda: {"ok": True}, idempotency_key="retry")
                self.assertEqual(manager._jobs, {})
                self.assertEqual(manager._idempotency, {})
                _, reused = manager.start({}, lambda: {"ok": True}, idempotency_key="retry")
                self.assertFalse(reused)
            finally:
                manager.close()

    def test_start_after_close_does_not_create_a_journal_record(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = JobManager(directory)
            manager.close()
            with self.assertRaisesRegex(JobConflictError, "closed"):
                manager.start({}, lambda: {"ok": True})
            self.assertEqual(list(Path(directory).glob("*.json")), [])
            self.assertEqual(manager._jobs, {})

    def test_queue_limit_reuses_idempotent_requests_and_recovers_capacity(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = JobManager(directory, max_workers=1, max_pending=1)
            entered, release = threading.Event(), threading.Event()

            def block():
                entered.set()
                self.assertTrue(release.wait(5))
                return {"ok": True}

            try:
                first, _ = manager.start({"n": 1}, block, idempotency_key="first")
                self.assertTrue(entered.wait(2))
                queued, _ = manager.start({"n": 2}, lambda: {"ok": True})
                reused, duplicate = manager.start({"n": 1}, block, idempotency_key="first")
                self.assertTrue(duplicate)
                self.assertEqual(reused["job_id"], first["job_id"])
                with self.assertRaisesRegex(JobConflictError, "capacity"):
                    manager.start({"n": 3}, lambda: {"ok": True})
                self.assertEqual(len(list(Path(directory).glob("*.json"))), 2)
                self.assertEqual(manager.cancel(queued["job_id"])["state"], "cancelled")
                manager.start({"n": 3}, lambda: {"ok": True})
            finally:
                release.set()
                manager.close()
            self.assertEqual(manager._futures, {})

    def test_nonobject_journals_do_not_prevent_startup(self):
        with tempfile.TemporaryDirectory() as directory:
            for i, value in enumerate([None, [], "damaged", 42]):
                (Path(directory) / f"job_{i}.json").write_text(json.dumps(value))
            manager = JobManager(directory)
            try:
                self.assertEqual(manager._jobs, {})
            finally:
                manager.close()

    def test_mismatched_journal_identity_is_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "job_original.json"
            path.write_text(json.dumps({"job_id": "job_other", "state": "running"}))
            manager = JobManager(directory)
            try:
                self.assertEqual(manager._jobs, {})
                self.assertFalse((Path(directory) / "job_other.json").exists())
            finally:
                manager.close()


class OpsLifecycleTests(unittest.TestCase):
    def test_close_releases_workers_even_when_cancellation_journal_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            entered, release = threading.Event(), threading.Event()

            def block(_job):
                entered.set()
                self.assertTrue(release.wait(5))
                return {"ok": True}

            manager = OpsJobManager(directory, block, scheduler=False)
            jobs = [manager.create(title=str(i), device_id="test")[0] for i in range(3)]
            first, _ = manager.start_run(jobs[0]["job_id"])
            self.assertTrue(entered.wait(2))
            queued = [manager.start_run(job["job_id"])[0] for job in jobs[1:]]
            original_persist = manager._persist_run
            original_shutdown = manager._executor.shutdown

            def persist(run):
                if run["job_run_id"] == queued[0]["job_run_id"]:
                    raise OSError("disk full")
                original_persist(run)

            def shutdown(**kwargs):
                release.set()
                original_shutdown(**kwargs)

            try:
                with patch.object(manager, "_persist_run", side_effect=persist), \
                     patch.object(manager._executor, "shutdown", side_effect=shutdown):
                    with self.assertRaisesRegex(OSError, "disk full"):
                        manager.close()
                self.assertEqual(manager._futures, {})
                self.assertEqual(manager.run_status(first["job_run_id"])["state"], "no_change")
                for run in queued:
                    self.assertEqual(manager.run_status(run["job_run_id"])["state"], "cancelled")
                recovered = json.loads((manager.runs_dir / f"{queued[1]['job_run_id']}.json").read_text())
                self.assertEqual(recovered["state"], "cancelled")
            finally:
                release.set()
                manager.close()

    def test_close_persists_queued_cancellation_and_drains_running_work(self):
        with tempfile.TemporaryDirectory() as directory:
            entered, release = threading.Event(), threading.Event()
            calls = []

            def block(job):
                calls.append(job["job_id"])
                entered.set()
                self.assertTrue(release.wait(5))
                return {"ok": True, "outcome": "no_change"}

            manager = OpsJobManager(directory, block, scheduler=False)
            first, _ = manager.create(title="first", device_id="test")
            second, _ = manager.create(title="queued", device_id="test")
            run, _ = manager.start_run(first["job_id"])
            self.assertTrue(entered.wait(2))
            queued, _ = manager.start_run(second["job_id"])
            original_shutdown = manager._executor.shutdown

            def unblock_shutdown(**kwargs):
                # close must first journal cancellation, then drain workers.
                try:
                    value = manager.run_status(queued["job_run_id"])
                    self.assertEqual(value["state"], "cancelled")
                    persisted = json.loads((manager.runs_dir / f"{queued['job_run_id']}.json").read_text())
                    self.assertEqual(persisted["attempts"][-1]["state"], "cancelled")
                finally:
                    release.set()
                    original_shutdown(**kwargs)

            try:
                with patch.object(manager._executor, "shutdown", side_effect=unblock_shutdown):
                    manager.close()
                self.assertEqual(calls, [first["job_id"]])
                self.assertEqual(manager.run_status(run["job_run_id"])["state"], "no_change")
                self.assertEqual(manager._active_runs, {})
                before = list(manager.runs_dir.glob("*.json"))
                with self.assertRaisesRegex(JobConflictError, "closed"):
                    manager.start_run(first["job_id"])
                self.assertEqual(list(manager.runs_dir.glob("*.json")), before)
                with self.assertRaisesRegex(JobConflictError, "closed"):
                    manager.create(title="late", device_id="test")
            finally:
                release.set()
                manager.close()

    def test_queue_capacity_is_released_by_cancellation(self):
        with tempfile.TemporaryDirectory() as directory:
            release, entered = threading.Event(), threading.Event()

            def block(_job):
                entered.set()
                self.assertTrue(release.wait(5))
                return {"ok": True}

            manager = OpsJobManager(directory, block, scheduler=False, max_pending=1)
            try:
                jobs = [manager.create(title=str(i), device_id="test")[0] for i in range(3)]
                first, _ = manager.start_run(jobs[0]["job_id"])
                self.assertTrue(entered.wait(2))
                second, _ = manager.start_run(jobs[1]["job_id"])
                _, reused = manager.start_run(jobs[0]["job_id"])
                self.assertTrue(reused)
                with self.assertRaisesRegex(JobConflictError, "capacity"):
                    manager.start_run(jobs[2]["job_id"])
                self.assertIsNone(manager.status(jobs[2]["job_id"])["last_run_id"])
                manager.cancel_run(second["job_run_id"])
                manager.start_run(jobs[2]["job_id"])
            finally:
                release.set()
                manager.close()

    def test_nonobject_journals_do_not_prevent_startup(self):
        with tempfile.TemporaryDirectory() as directory:
            for subdir, prefix in [("jobs", "job"), ("runs", "run")]:
                folder = Path(directory) / "operations" / subdir
                folder.mkdir(parents=True)
                (folder / f"{prefix}_invalid.json").write_text("[]")
            manager = OpsJobManager(directory, lambda _: {"ok": True}, scheduler=False)
            try:
                self.assertEqual(manager.list(), [])
            finally:
                manager.close()
