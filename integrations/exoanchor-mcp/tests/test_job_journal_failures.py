import json
import hashlib
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from exoanchor_mcp.jobs import JobManager, OpsJobManager


class JournalFailureTests(unittest.TestCase):
    def test_terminal_recovery_records_are_read_without_rewriting(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "job_interrupted.json"
            original = json.dumps({
                "job_id": "job_interrupted", "state": "interrupted",
                "finished_at": "2026-09-18T00:00:00Z", "error": "completion unknown",
                "output_persisted": True, "result": {"ok": False, "output": "partial output"},
            }).encode()
            path.write_bytes(original)
            with patch.object(JobManager, "_persist", side_effect=OSError("read-only journal")) as persist:
                manager = JobManager(directory)
                try:
                    self.assertEqual(manager.status("job_interrupted")["state"], "interrupted")
                    self.assertEqual(manager.result("job_interrupted")["output"], "partial output")
                    persist.assert_not_called()
                    self.assertEqual(path.read_bytes(), original)
                finally:
                    manager.close()

    def test_recovery_write_failure_keeps_all_history_queryable_without_replay(self):
        for operations in (False, True):
            with self.subTest(operations=operations), tempfile.TemporaryDirectory() as directory:
                kind, identity = ("run", "job_run_id") if operations else ("job", "job_id")
                root = Path(directory) / "operations" / "runs" if operations else Path(directory)
                root.mkdir(parents=True, exist_ok=True)
                originals = {}
                for suffix, state in (("active", "running"), ("done", "succeeded")):
                    record_id = f"{kind}_{suffix}"
                    record = {identity: record_id, "state": state}
                    if operations:
                        record.update(job_id="job_health", attempts=[{"state": state}])
                    elif state == "running":
                        record.update(idempotency_key="recovered", request_sha256=hashlib.sha256(b"{}").hexdigest())
                    path = root / f"{record_id}.json"
                    originals[path] = json.dumps(record).encode()
                    path.write_bytes(originals[path])
                runner = Mock(return_value={"ok": True})
                cls = OpsJobManager if operations else JobManager
                method = "_persist_run" if operations else "_persist"
                with patch.object(cls, method, side_effect=OSError("disk full during recovery")) as persist:
                    manager = (cls(directory, runner, scheduler=False) if operations else cls(directory))
                    try:
                        status = manager.run_status if operations else manager.status
                        interrupted = status(f"{kind}_active")
                        self.assertEqual(interrupted["state"], "interrupted")
                        self.assertIn("disk full", interrupted["journal_error"])
                        self.assertEqual(status(f"{kind}_done")["state"], "succeeded")
                        if operations:
                            self.assertEqual(interrupted["attempts"][-1]["state"], "interrupted")
                        else:
                            existing, reused = manager.start({}, runner, idempotency_key="recovered")
                            self.assertTrue(reused)
                            self.assertEqual(existing["state"], "interrupted")
                        persist.assert_called_once()
                        self.assertEqual(manager._futures, {})
                        for path, original in originals.items():
                            self.assertEqual(path.read_bytes(), original)
                    finally:
                        manager.close()
                runner.assert_not_called()

    def test_ssh_preflight_failure_is_terminal_and_does_not_call_runner(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = JobManager(directory)
            original = manager._persist
            runner = Mock(return_value={"ok": True})

            def persist(record):
                if record["state"] == "running":
                    raise OSError("disk full before execution")
                original(record)

            with patch.object(manager, "_persist", side_effect=persist):
                job, _ = manager.start({}, runner)
                manager.close()
            status = manager.status(job["job_id"])
            self.assertEqual(status["state"], "failed")
            self.assertIn("disk full", status["error"])
            self.assertIsNotNone(status["finished_at"])
            runner.assert_not_called()

    def test_ops_preflight_failure_is_terminal_and_does_not_call_runner(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Mock(return_value={"ok": True})
            manager = OpsJobManager(directory, runner, scheduler=False)
            original = manager._persist_run
            preflight = threading.Event()

            def persist(record):
                if record["state"] == "running":
                    preflight.set()
                    raise OSError("disk full before execution")
                original(record)

            job, _ = manager.create(title="Health", device_id="test")
            with patch.object(manager, "_persist_run", side_effect=persist):
                run, _ = manager.start_run(job["job_id"])
                self.assertTrue(preflight.wait(2))
                manager.close()
            status = manager.run_status(run["job_run_id"])
            self.assertEqual(status["state"], "failed")
            self.assertEqual(status["attempts"][-1]["failure_class"], "storage")
            self.assertIn("disk full", status["finding"]["message"])
            runner.assert_not_called()

    def test_completion_journal_failure_keeps_result_and_reports_storage_error(self):
        for operations in (False, True):
            with self.subTest(operations=operations), tempfile.TemporaryDirectory() as directory:
                entered, release = threading.Event(), threading.Event()

                def runner(*_args):
                    entered.set()
                    self.assertTrue(release.wait(5))
                    return {"ok": True, "outcome": "succeeded", "output": "done"}

                manager = (OpsJobManager(directory, runner, scheduler=False)
                           if operations else JobManager(directory))
                name = "_persist_run" if operations else "_persist"
                original = getattr(manager, name)

                def persist(record):
                    if record["state"] == "succeeded":
                        raise OSError("disk full after execution")
                    original(record)

                try:
                    with patch.object(manager, name, side_effect=persist):
                        if operations:
                            job, _ = manager.create(title="Health", device_id="test")
                            run, _ = manager.start_run(job["job_id"])
                            record_id = run["job_run_id"]
                            status = lambda: manager.run_status(record_id)
                            path = manager.runs_dir / f"{record_id}.json"
                        else:
                            job, _ = manager.start({}, runner)
                            record_id = job["job_id"]
                            status = lambda: manager.status(record_id)
                            path = manager._path(record_id)
                        self.assertTrue(entered.wait(2))
                        release.set()
                        manager.close()
                    record = status()
                    self.assertEqual(record["state"], "succeeded")
                    self.assertIn("disk full", record["journal_error"])
                    self.assertEqual(json.loads(path.read_text())["state"], "running")
                    self.assertEqual(manager._futures, {})
                finally:
                    release.set()
                    manager.close()
