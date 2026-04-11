import tempfile
import unittest

from exoanchor.runtime.events import EventHub, RuntimeEvent
from exoanchor.runtime.sessions import SessionRuntime, SessionState, SessionStore


class SessionRuntimeTests(unittest.IsolatedAsyncioTestCase):
    async def test_sync_child_event_moves_session_to_waiting_confirmation(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            hub = EventHub()
            runtime = SessionRuntime(SessionStore(tmpdir), hub)
            session = await runtime.create(request="重启现有 Minecraft workload")
            runtime.bind_run(session.session_id, "run123")

            event = RuntimeEvent(
                stream="plan_run",
                event="confirmation_requested",
                entity_kind="plan_run",
                entity_id="run123",
                state="waiting_confirmation",
                summary="Waiting for confirmation",
                payload={
                    "run": {
                        "run_id": "run123",
                        "state": "waiting_confirmation",
                        "waiting_step_id": 2,
                        "goal": "重启现有 Minecraft workload",
                    }
                },
            )

            await runtime.sync_child_event(event)

            updated = runtime.get(session.session_id)
            self.assertIsNotNone(updated)
            self.assertEqual(updated.state, SessionState.WAITING_CONFIRMATION)
            self.assertEqual(updated.run_id, "run123")

            recent = hub.recent(limit=5)
            self.assertTrue(
                any(item.stream == "session" and item.entity_id == session.session_id and item.state == "waiting_confirmation" for item in recent)
            )


if __name__ == "__main__":
    unittest.main()
