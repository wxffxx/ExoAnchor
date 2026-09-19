import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from exoanchor_mcp.jobs import JobManager, OpsJobManager
from exoanchor_mcp.observations import canonical_hash, make_observation


class UnicodeEvidenceTests(unittest.TestCase):
    def test_artifact_hash_describes_the_text_returned_for_nonstring_output(self):
        for output in (None, 0, False, ["value"], {"message": "value"}):
            with self.subTest(output=output), tempfile.TemporaryDirectory() as directory:
                manager = JobManager(directory)
                job, _ = manager.start({"command": "test"}, lambda: {"ok": True, "output": output})
                manager.close()
                result = manager.result(job["job_id"])
                encoded = result["output"].encode("utf-8")
                self.assertEqual(result["artifact"]["bytes"], len(encoded))
                self.assertEqual(result["artifact"]["sha256"], hashlib.sha256(encoded).hexdigest())

    def test_damaged_optional_artifact_cache_is_recomputed_from_saved_output(self):
        for artifact in (None, [], 42):
            for state in ("succeeded", "running"):
                with self.subTest(artifact=artifact, state=state), tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "job_saved.json"
                    original = json.dumps({"job_id": "job_saved", "state": state,
                                           "artifact": artifact, "result": {"ok": True, "output": "saved"}})
                    path.write_text(original)
                    manager = JobManager(directory, persist_output=True)
                    try:
                        result = manager.result("job_saved")
                        self.assertEqual(result["state"], "succeeded" if state == "succeeded" else "interrupted")
                        self.assertEqual(result["output"], "saved")
                        self.assertEqual(result["artifact"]["sha256"], hashlib.sha256(b"saved").hexdigest())
                        if state == "succeeded":
                            self.assertEqual(path.read_text(), original, "terminal journals stay read-only")
                    finally:
                        manager.close()

    def test_legacy_nonstring_output_metadata_is_recomputed_without_rewriting_journal(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "job_saved.json"
            original = json.dumps({"job_id": "job_saved", "state": "succeeded",
                                   "artifact": {"available": True, "bytes": 0,
                                                "sha256": hashlib.sha256(b"").hexdigest()},
                                   "result": {"ok": True, "output": 123}})
            path.write_text(original)
            manager = JobManager(directory)
            try:
                result = manager.result("job_saved")
                self.assertEqual(result["output"], "123")
                self.assertEqual(result["artifact"]["bytes"], 3)
                self.assertEqual(result["artifact"]["sha256"], hashlib.sha256(b"123").hexdigest())
                self.assertEqual(path.read_text(), original)
            finally:
                manager.close()

    def test_observation_hash_preserves_unicode_and_escapes_only_invalid_scalars(self):
        for suffix in ("\ud800", "\udfff"):
            with self.subTest(suffix=ascii(suffix)):
                value = {"text": "中文" + suffix}
                observation = make_observation("logs", "/test", value)
                expected = b'{"text":"' + "中文".encode() + ascii(suffix)[1:-1].encode() + b'"}'
                self.assertEqual(observation["content_sha256"], hashlib.sha256(expected).hexdigest())
                self.assertEqual(observation["data"], value)
                self.assertNotEqual(canonical_hash(value), canonical_hash({"text": "中文" + ascii(suffix)[1:-1]}))
        self.assertEqual(canonical_hash({"text": "中文😀"}),
                         hashlib.sha256('{"text":"中文😀"}'.encode()).hexdigest())

    def test_invalid_unicode_output_preserves_success_and_replay_without_fabricated_hash(self):
        for persist_output in (False, True):
            with self.subTest(persist_output=persist_output), tempfile.TemporaryDirectory() as directory:
                output = "原始结果\ud800/\udfff"
                manager = JobManager(directory, persist_output=persist_output)
                job, _ = manager.start({"command": "test"}, lambda: {"ok": True, "output": output})
                manager.close()
                status = manager.status(job["job_id"])
                self.assertEqual(status["state"], "succeeded")
                self.assertTrue(status["artifact"]["available"])
                self.assertIsNone(status["artifact"]["bytes"])
                self.assertIsNone(status["artifact"]["sha256"])
                self.assertIn("UTF-8", status["artifact"]["encoding_error"])
                self.assertEqual(manager.result(job["job_id"])["output"], output)
                saved = json.loads((Path(directory) / f"{job['job_id']}.json").read_text())
                self.assertEqual(saved["state"], "succeeded")
                if persist_output:
                    self.assertEqual(saved["result"]["output"], output)
                else:
                    self.assertNotIn("result", saved)
                recovered = JobManager(directory, persist_output=persist_output)
                try:
                    replay = recovered.result(job["job_id"])
                    self.assertEqual(replay["state"], "succeeded")
                    self.assertEqual(replay["result_available"], persist_output)
                    if persist_output:
                        self.assertEqual(replay["output"], output)
                finally:
                    recovered.close()

    def test_error_text_with_invalid_unicode_still_saves_the_terminal_state(self):
        error = "模拟失败\udfff"
        def runner():
            raise RuntimeError(error)
        with tempfile.TemporaryDirectory() as directory:
            manager = JobManager(directory)
            job, _ = manager.start({"command": "test"}, runner)
            manager.close()
            saved = json.loads((Path(directory) / f"{job['job_id']}.json").read_text())
            self.assertEqual(saved["state"], "failed")
            self.assertEqual(saved["error"], error)

    def test_ops_finding_with_invalid_unicode_is_saved_without_losing_evidence(self):
        finding = {"message": "原始发现\ud800"}
        with tempfile.TemporaryDirectory() as directory:
            manager = OpsJobManager(directory, lambda _: {"ok": True, "finding": finding}, scheduler=False)
            try:
                job, _ = manager.create(title="test", device_id="offline")
                run, _ = manager.start_run(job["job_id"])
                with manager._lock:
                    future = manager._futures.get(run["job_run_id"])
                if future:
                    future.result(timeout=2)
                path = manager.runs_dir / f"{run['job_run_id']}.json"
                saved = json.loads(path.read_text())
                self.assertEqual(saved["state"], "no_change")
                self.assertEqual(saved["finding"], finding)
            finally:
                manager.close()


if __name__ == "__main__":
    unittest.main()
