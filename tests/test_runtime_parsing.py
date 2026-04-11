import unittest

from exoanchor.runtime.parsing import parse_llm_response


class RuntimeParsingTests(unittest.TestCase):
    def test_parse_plan_normalizes_tools_and_commands(self):
        text = """
        {
          "type": "plan",
          "goal": "检查资源",
          "steps": [
            {"id": 1, "description": "检查磁盘", "tool": "shell.exec", "args": {"command": "df -h"}, "dangerous": false},
            {"id": 2, "description": "检查内存", "command": "free -h", "dangerous": false}
          ]
        }
        """

        result = parse_llm_response(text)

        self.assertEqual(result["type"], "plan")
        self.assertEqual(result["steps"][0]["tool"], "shell.exec")
        self.assertEqual(result["steps"][0]["command"], "df -h")
        self.assertEqual(result["steps"][1]["tool"], "shell.exec")
        self.assertEqual(result["steps"][1]["args"]["command"], "free -h")

    def test_parse_action_result_from_partial_json(self):
        text = '{"action":"modify","replace_step_id":2,"new_command":"echo ok","reason":"repair"}'

        result = parse_llm_response(text)

        self.assertEqual(result["action"], "modify")
        self.assertEqual(result["replace_step_id"], 2)
        self.assertEqual(result["new_command"], "echo ok")


if __name__ == "__main__":
    unittest.main()
