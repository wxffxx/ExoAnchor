import tempfile
import threading
import unittest

from exoanchor_mcp.jobs import OpsJobManager
from exoanchor_mcp.observations import ObservationStore, canonical_hash, make_observation


class ObservationOwnershipTests(unittest.TestCase):
    def test_observation_hash_describes_its_owned_data(self):
        data = {"nested": {"values": [1, 2]}}
        derived = {"conditions": {"ready": True}}
        observation = make_observation("status", "/api/status", data, derived=derived)
        data["nested"]["values"].append(3)
        derived["conditions"]["ready"] = False
        self.assertEqual(observation["data"]["nested"]["values"], [1, 2])
        self.assertTrue(observation["derived"]["conditions"]["ready"])
        self.assertEqual(observation["content_sha256"], canonical_hash(observation["data"]))

    def test_store_input_get_and_metadata_cannot_mutate_replay_evidence(self):
        store = ObservationStore()
        original = make_observation("status", "/api/status", {"values": [1]},
                                    derived={"conditions": {"ready": True}})
        observation_id = original["observation_id"]
        returned = store.put(original)
        original["data"]["values"].append(2)
        returned["derived"]["conditions"]["ready"] = False
        read = store.get(observation_id)
        self.assertEqual(read["data"]["values"], [1])
        self.assertTrue(read["derived"]["conditions"]["ready"])
        read["data"]["values"].append(3)
        metadata = store.list_metadata()
        self.assertNotIn("data", metadata[0])
        metadata[0]["derived"]["conditions"]["ready"] = False
        replayed = store.get(observation_id)
        self.assertEqual(replayed["data"]["values"], [1])
        self.assertTrue(replayed["derived"]["conditions"]["ready"])
        self.assertEqual(replayed["content_sha256"], canonical_hash(replayed["data"]))

    def test_owned_copies_preserve_lru_capacity_and_order(self):
        store = ObservationStore(maximum=2)
        observations = [make_observation("status", "/api/status", {"n": n}) for n in range(3)]
        store.put(observations[0])
        store.put(observations[1])
        store.get(observations[0]["observation_id"])
        store.put(observations[2])
        self.assertIsNone(store.get(observations[1]["observation_id"]))
        self.assertEqual([entry["observation_id"] for entry in store.list_metadata()],
                         [observations[2]["observation_id"], observations[0]["observation_id"]])

    def test_operations_result_does_not_alias_runner_owned_evidence(self):
        result = {"ok": True, "finding": {"items": ["healthy"]},
                  "evidence": {"observations": [{"value": 1}]}}
        with tempfile.TemporaryDirectory() as directory:
            manager = OpsJobManager(directory, lambda _: result, scheduler=False)
            job, _ = manager.create(title="Health", device_id="test")
            finished = threading.Event()
            run, _ = manager.start_run(job["job_id"])
            with manager._lock:
                future = manager._futures.get(run["job_run_id"])
                if future:
                    future.add_done_callback(lambda _: finished.set())
                else:
                    finished.set()
            try:
                self.assertTrue(finished.wait(2))
                result["finding"]["items"].clear()
                result["evidence"]["observations"][0]["value"] = 99
                status = manager.run_status(run["job_run_id"])
                self.assertEqual(status["finding"]["items"], ["healthy"])
                self.assertEqual(status["evidence"]["observations"][0]["value"], 1)
            finally:
                manager.close()
