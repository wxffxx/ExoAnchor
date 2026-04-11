import os
import tempfile
import unittest

from exoanchor.server.conversations import ConversationStore


class ConversationStoreTests(unittest.TestCase):
    def test_add_message_promotes_title_and_extracts_context(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            store = ConversationStore(os.path.join(tmpdir, "conversations.json"))
            conversation = store.create()

            store.add_message(
                conversation["id"],
                role="user",
                content="部署最新版 Spigot Minecraft 服务器",
            )
            store.add_message(
                conversation["id"],
                role="assistant",
                html="<b>好的</b>，我先检查当前 workload。",
            )

            saved = store.get(conversation["id"])
            self.assertIsNotNone(saved)
            self.assertEqual(saved["title"], "部署最新版 Spigot Minecraft 服务器")

            lines, plain_texts = store.extract_context(conversation["id"])
            self.assertEqual(
                lines,
                [
                    "User: 部署最新版 Spigot Minecraft 服务器",
                    "Assistant: 好的，我先检查当前 workload。",
                ],
            )
            self.assertEqual(
                plain_texts,
                [
                    "部署最新版 Spigot Minecraft 服务器",
                    "好的，我先检查当前 workload。",
                ],
            )


if __name__ == "__main__":
    unittest.main()
