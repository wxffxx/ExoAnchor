import json
import tempfile
import unittest
from copy import deepcopy

from exoanchor_mcp.jobs import OpsJobManager


class OpsRecoveryTests(unittest.TestCase):
    @staticmethod
    def seed(directory):
        manager = OpsJobManager(directory, lambda _: {"ok": True}, scheduler=False)
        try:
            return manager.create(title="Health", device_id="test", interval_seconds=60)[0]
        finally:
            manager.close()

    def test_invalid_schedule_does_not_block_healthy_jobs_or_run_immediately(self):
        for timestamp in ("2000-01-01T00:00:00", "broken", "", "9999-99-99T99:99:99Z"):
            with self.subTest(timestamp=timestamp), tempfile.TemporaryDirectory() as directory:
                job = self.seed(directory)
                manager = OpsJobManager(directory, lambda _: {"ok": True}, scheduler=False)
                healthy = deepcopy(job)
                healthy["job_id"] = "job_healthy"
                healthy["next_run_at"] = "2000-01-01T01:00:00+01:00"
                job["next_run_at"] = timestamp
                manager._persist_job(job)
                manager._persist_job(healthy)
                manager.close()
                manager = OpsJobManager(directory, lambda _: {"ok": True}, scheduler=False)
                try:
                    started = manager.tick("2999-01-01T00:00:00Z")
                    self.assertEqual(len(started), 1)
                    self.assertEqual(manager.run_status(started[0])["job_id"], "job_healthy")
                    self.assertIsNone(manager.status(job["job_id"])["last_run_id"])
                finally:
                    manager.close()

    def test_malformed_definitions_are_ignored_without_overwriting_them(self):
        changes = [
            {"trigger": None}, {"trigger": []}, {"trigger": {"type": "unknown"}},
            {"trigger": {"type": "interval", "interval_seconds": "60"}},
            {"trigger": {"type": "interval", "interval_seconds": -1}},
            {"generation": None}, {"generation": True},
            {"target": []}, {"target": {"device_id": ""}}, {"policy": None},
        ]
        with tempfile.TemporaryDirectory() as directory:
            valid = self.seed(directory)
            manager = OpsJobManager(directory, lambda _: {"ok": True}, scheduler=False)
            originals = {}
            for index, change in enumerate(changes):
                invalid = deepcopy(valid)
                invalid.update(change)
                invalid["job_id"] = f"job_invalid_{index}"
                manager._persist_job(invalid)
                path = manager.jobs_dir / f"{invalid['job_id']}.json"
                originals[path] = path.read_bytes()
            manager.close()
            manager = OpsJobManager(directory, lambda _: {"ok": True}, scheduler=False)
            try:
                self.assertEqual([job["job_id"] for job in manager.list()], [valid["job_id"]])
                self.assertEqual(len(manager.tick("2999-01-01T00:00:00Z")), 1)
                for path, content in originals.items():
                    self.assertEqual(path.read_bytes(), content)
                    self.assertEqual(json.loads(content)["job_id"], path.stem)
            finally:
                manager.close()
