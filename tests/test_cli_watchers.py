import unittest

from exoanchor.cli.watchers import watch_session_stream


class WatchSessionStreamTests(unittest.TestCase):
    def test_waiting_input_returns_success_and_surfaces_message(self):
        events = [
            {
                "event": "updated",
                "state": "waiting_input",
                "payload": {
                    "session": {
                        "state": "waiting_input",
                        "summary": "改一下人数",
                        "message": "你想把最大玩家人数改成多少？",
                    }
                },
            }
        ]
        lines = []

        result = watch_session_stream(
            "http://127.0.0.1:8090",
            "abc12345",
            request_fn=lambda *_args, **_kwargs: {},
            iter_events=lambda *_args, **_kwargs: events,
            print_step_fn=lambda prefix, text: lines.append((prefix, text)),
        )

        self.assertEqual(result, 0)
        self.assertIn(("[session]", "abc12345 WAITING_INPUT 改一下人数"), lines)
        self.assertIn(("  message>", "你想把最大玩家人数改成多少？"), lines)

    def test_waiting_confirmation_auto_approves_once(self):
        events = [
            {
                "event": "child_event",
                "payload": {
                    "session": {"state": "running", "run_id": "run123"},
                    "child_event": {
                        "entity_kind": "plan_run",
                        "entity_id": "run123",
                        "state": "waiting_confirmation",
                        "payload": {
                            "run": {
                                "state": "waiting_confirmation",
                                "waiting_step_id": 2,
                                "steps": [
                                    {"id": 2, "description": "重启服务", "status": "pending", "output": ""},
                                ],
                                "completed_steps": 1,
                                "total_steps": 3,
                            }
                        },
                    },
                },
            },
            {
                "event": "updated",
                "state": "completed",
                "payload": {
                    "session": {
                        "state": "completed",
                        "summary": "重启 workload",
                    }
                },
            },
        ]
        requests = []

        def request_fn(_base_url, method, path, body=None):
            requests.append((method, path, body))
            if method == "GET" and path == "/api/agent/runs/run123":
                return {"state": "waiting_confirmation", "waiting_step_id": "2"}
            return {}

        result = watch_session_stream(
            "http://127.0.0.1:8090",
            "sess1",
            auto_approve=True,
            request_fn=request_fn,
            iter_events=lambda *_args, **_kwargs: events,
            print_step_fn=lambda *_args, **_kwargs: None,
        )

        self.assertEqual(result, 0)
        self.assertEqual(
            requests,
            [
                ("GET", "/api/agent/runs/run123", None),
                ("POST", "/api/agent/runs/run123/confirm", {"approved": True}),
            ],
        )

    def test_replayed_old_confirmation_does_not_reprompt(self):
        events = [
            {
                "event": "child_event",
                "payload": {
                    "session": {"state": "running", "run_id": "run123"},
                    "child_event": {
                        "entity_kind": "plan_run",
                        "entity_id": "run123",
                        "state": "waiting_confirmation",
                        "payload": {
                            "run": {
                                "state": "waiting_confirmation",
                                "waiting_step_id": 2,
                                "steps": [
                                    {"id": 2, "description": "重启服务", "status": "pending", "output": ""},
                                ],
                                "completed_steps": 1,
                                "total_steps": 3,
                            }
                        },
                    },
                },
            },
            {
                "event": "updated",
                "state": "completed",
                "payload": {
                    "session": {
                        "state": "completed",
                        "summary": "重启 workload",
                    }
                },
            },
        ]
        requests = []

        def request_fn(_base_url, method, path, body=None):
            requests.append((method, path, body))
            if method == "GET" and path == "/api/agent/runs/run123":
                return {"state": "running", "waiting_step_id": ""}
            return {}

        result = watch_session_stream(
            "http://127.0.0.1:8090",
            "sess1",
            auto_approve=True,
            request_fn=request_fn,
            iter_events=lambda *_args, **_kwargs: events,
            print_step_fn=lambda *_args, **_kwargs: None,
        )

        self.assertEqual(result, 0)
        self.assertEqual(requests, [("GET", "/api/agent/runs/run123", None)])


if __name__ == "__main__":
    unittest.main()
