import unittest

from exoanchor.runtime.intent import LLMIntentResolver
from exoanchor.runtime.parsing import (
    heuristic_force_plan,
    is_clarifying_chat_result,
    is_echo_chat_result,
    parse_llm_response,
)


class FakeLLMClient:
    def __init__(self, response_text: str = ""):
        self.response_text = response_text
        self.calls = 0

    async def complete(self, **_kwargs):
        self.calls += 1
        return self.response_text


class FakeKnowledgeStore:
    def get_prompt_injection(self):
        return ""


class FakeSkillStore:
    def list_skills(self):
        return []

    def get_skill(self, _skill_id):
        return None


class FakeFactRecord:
    def __init__(self, key, value):
        self.key = key
        self.value = value


class FakeFailureRecord:
    def __init__(self, source_type, source_id, message):
        self.source_type = source_type
        self.source_id = source_id
        self.message = message


class FakeFactStore:
    def __init__(self, facts=None, failures=None):
        self._facts = {
            key: FakeFactRecord(key, value)
            for key, value in (facts or {}).items()
        }
        self._failures = list(failures or [])

    def get(self, key):
        return self._facts.get(key)

    def list_facts(self, prefix="", limit=100):
        items = [fact for fact in self._facts.values() if not prefix or fact.key.startswith(prefix)]
        return items[:limit]

    def list_failures(self, limit=20):
        return self._failures[:limit]


class FakeAgent:
    def __init__(self, workloads, fact_store=None):
        self._workloads = workloads
        self.skill_store = FakeSkillStore()
        self.knowledge_store = FakeKnowledgeStore()
        self.fact_store = fact_store or FakeFactStore()

    async def list_workloads(self):
        return list(self._workloads)


class RuntimeIntentTests(unittest.IsolatedAsyncioTestCase):
    def build_resolver(self, *, workloads=None, llm_client=None, fact_store=None):
        config = {
            "nlp": {
                "api_provider": "openai",
                "api_key": "test-key",
                "model": "fake-model",
            },
            "target": {
                "ip": "192.168.1.67",
                "ssh": {
                    "username": "wxffxx",
                    "password": "123898",
                },
            },
        }
        return LLMIntentResolver(
            load_saved_config=lambda: config,
            base_config=config,
            extract_conversation_context=lambda _conv_id: ([], []),
            get_agent=lambda: FakeAgent(workloads or [], fact_store=fact_store),
            system_prompt="SYSTEM",
            parse_llm_response=parse_llm_response,
            is_clarifying_chat_result=is_clarifying_chat_result,
            is_echo_chat_result=is_echo_chat_result,
            heuristic_force_plan=heuristic_force_plan,
            llm_client=llm_client or FakeLLMClient('{"type":"chat","message":"noop"}'),
        )

    async def test_existing_workload_player_update_short_circuits_llm(self):
        llm_client = FakeLLMClient('{"type":"chat","message":"should not be used"}')
        resolver = self.build_resolver(
            workloads=[
                {
                    "id": "spigot-server",
                    "dir": "spigot-server",
                    "name": "Minecraft (Spigot)",
                    "path": "/home/wxffxx/.exoanchor/workloads/spigot-server",
                    "command": "cd /home/wxffxx/.exoanchor/workloads/spigot-server && ./launch.sh",
                    "port": 25565,
                }
            ],
            llm_client=llm_client,
        )

        result = await resolver.resolve({"message": "把玩家人数改为32", "conversation_id": "cli"})

        self.assertEqual(result["type"], "plan")
        self.assertEqual(llm_client.calls, 0)
        self.assertIn("/home/wxffxx/.exoanchor/workloads/spigot-server/server.properties", result["steps"][0]["command"])

    async def test_missing_detail_returns_clarifying_question_before_llm(self):
        llm_client = FakeLLMClient('{"type":"chat","message":"should not be used"}')
        resolver = self.build_resolver(
            workloads=[
                {
                    "id": "spigot-server",
                    "dir": "spigot-server",
                    "name": "Minecraft (Spigot)",
                    "path": "/home/wxffxx/.exoanchor/workloads/spigot-server",
                    "command": "cd /home/wxffxx/.exoanchor/workloads/spigot-server && ./launch.sh",
                    "port": 25565,
                }
            ],
            llm_client=llm_client,
        )

        result = await resolver.resolve({"message": "改一下人数", "conversation_id": "cli"})

        self.assertEqual(result["type"], "chat")
        self.assertIn("最大玩家人数改成多少", result["message"])
        self.assertEqual(llm_client.calls, 0)

    async def test_force_plan_echo_falls_back_to_heuristic_plan(self):
        llm_client = FakeLLMClient('{"type":"chat","message":"部署最新版 Spigot Minecraft 服务器"}')
        resolver = self.build_resolver(workloads=[], llm_client=llm_client)

        result = await resolver.resolve({"message": "部署最新版 Spigot Minecraft 服务器", "force_plan": True})

        self.assertEqual(result["type"], "plan")
        self.assertEqual(result["goal"], "部署最新版 Spigot Minecraft 服务器")
        self.assertGreaterEqual(llm_client.calls, 1)

    async def test_cached_workload_facts_are_used_when_live_discovery_is_empty(self):
        llm_client = FakeLLMClient('{"type":"chat","message":"should not be used"}')
        fact_store = FakeFactStore(
            facts={
                "workloads.latest": {
                    "count": 1,
                    "items": [
                        {
                            "id": "spigot-server",
                            "name": "Minecraft (Spigot)",
                            "path": "/home/wxffxx/.exoanchor/workloads/spigot-server",
                            "dir": "spigot-server",
                            "base_dir": "exoanchor",
                            "port": 25565,
                            "status": "running",
                            "command": "cd /home/wxffxx/.exoanchor/workloads/spigot-server && ./launch.sh",
                        }
                    ],
                },
                "workload.spigot-server.manifest": {
                    "id": "spigot-server",
                    "dir": "spigot-server",
                    "name": "Minecraft (Spigot)",
                    "path": "/home/wxffxx/.exoanchor/workloads/spigot-server",
                    "base_dir": "exoanchor",
                    "port": 25565,
                    "command": "cd /home/wxffxx/.exoanchor/workloads/spigot-server && ./launch.sh",
                },
            },
            failures=[FakeFailureRecord("plan", "run123", "sudo requires a password")],
        )
        resolver = self.build_resolver(workloads=[], llm_client=llm_client, fact_store=fact_store)

        result = await resolver.resolve({"message": "把玩家人数改为32", "conversation_id": "cli"})

        self.assertEqual(result["type"], "plan")
        self.assertEqual(llm_client.calls, 0)
        self.assertIn("/home/wxffxx/.exoanchor/workloads/spigot-server/server.properties", result["steps"][0]["command"])

    async def test_generic_existing_workload_restart_short_circuits_llm(self):
        llm_client = FakeLLMClient('{"type":"chat","message":"should not be used"}')
        resolver = self.build_resolver(
            workloads=[
                {
                    "id": "api-server",
                    "dir": "api-server",
                    "name": "Internal API",
                    "path": "/home/wxffxx/.exoanchor/workloads/api-server",
                    "command": "./launch.sh",
                    "port": 8080,
                }
            ],
            llm_client=llm_client,
        )

        result = await resolver.resolve({"message": "重启 api-server", "conversation_id": "cli"})

        self.assertEqual(result["type"], "plan")
        self.assertEqual(llm_client.calls, 0)
        self.assertEqual(len(result["steps"]), 3)
        self.assertIn("/home/wxffxx/.exoanchor/workloads/api-server", result["steps"][0]["command"])


if __name__ == "__main__":
    unittest.main()
