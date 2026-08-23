import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

from exoanchor_mcp.contracts import TOOLS


INTEGRATION_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = INTEGRATION_ROOT.parents[1]
PATCH_PATH = INTEGRATION_ROOT / "docs" / "dsh" / "exoanchor.patch.yml"
LAUNCHER_PATH = INTEGRATION_ROOT / "scripts" / "run-dsh.sh"
SKILL_PATH = REPOSITORY_ROOT / "skills" / "exoanchor-mcp-control" / "SKILL.md"
DEMO_PATH = (
    REPOSITORY_ROOT
    / "skills"
    / "exoanchor-mcp-control"
    / "references"
    / "FOUR_DEMOS_zh.md"
)


class DshIntegrationTests(unittest.TestCase):
    def test_overlay_reuses_canonical_stdio_server_without_literal_secrets(self):
        overlay = PATCH_PATH.read_text(encoding="utf-8")
        self.assertIn("@deepseek-ai/dsh-mcp-client", overlay)
        self.assertIn("transport: stdio", overlay)
        self.assertIn("args: ['-m', 'exoanchor_mcp.server']", overlay)
        self.assertIn("EXOANCHOR_PASSWORD_FILE", overlay)
        self.assertNotIn("EXOANCHOR_PASSWORD:", overlay)
        self.assertNotIn("192.168.", overlay)
        self.assertIn("EXOANCHOR_ALLOW_ARBITRARY_SSH: '0'", overlay)
        self.assertIn("failOnStartupError: true", overlay)

    def test_demo_skill_and_canonical_contract_cover_all_four_flows(self):
        tool_names = {tool["name"] for tool in TOOLS}
        required_tools = {
            "exoanchor_capabilities",
            "exoanchor_status",
            "exoanchor_snapshot",
            "exoanchor_console_login",
            "exoanchor_uart_status",
            "exoanchor_uart_read",
            "exoanchor_uart_write",
            "exoanchor_uart_authenticate",
            "exoanchor_ssh_bootstrap_from_uart",
        }
        self.assertTrue(required_tools.issubset(tool_names))

        skill = SKILL_PATH.read_text(encoding="utf-8")
        demos = DEMO_PATH.read_text(encoding="utf-8")
        self.assertIn("integrations/exoanchor-mcp", skill)
        self.assertIn("DSH-specific boundary", skill)
        for number in range(1, 5):
            self.assertIn(f"Demo 0{number}", demos)
        self.assertIn("outcome_unknown", demos)
        self.assertIn("人工输入", demos)

    @unittest.skipUnless(shutil.which("dsh"), "local dsh is not installed")
    def test_launcher_composes_headless_profile_without_starting_device(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            password_file = Path(temp_dir) / "device-password"
            password_file.write_text("not-a-real-password\n", encoding="utf-8")
            password_file.chmod(stat.S_IRUSR | stat.S_IWUSR)
            state_dir = Path(temp_dir) / "state"
            environment = os.environ.copy()
            environment.update(
                {
                    "EXOANCHOR_BASE_URL": "http://device.invalid",
                    "EXOANCHOR_DEVICE_ID": "test-device",
                    "EXOANCHOR_USERNAME": "test-user",
                    "EXOANCHOR_PASSWORD_FILE": str(password_file),
                    "EXOANCHOR_STATE_DIR": str(state_dir),
                    "DSH_PROFILE": "headless",
                }
            )
            result = subprocess.run(
                [str(LAUNCHER_PATH), "--read-only", "--check"],
                cwd=INTEGRATION_ROOT,
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=30,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("mcp-exoanchor", result.stdout)
            self.assertIn("@deepseek-ai/dsh-mcp-client", result.stdout)
            self.assertIn("cwd: !!js process.env.EXOANCHOR_MCP_ROOT", result.stdout)
            self.assertIn(
                "!!js process.env.EXOANCHOR_SKILL_ROOT", result.stdout
            )
            self.assertIn(
                "EXOANCHOR_ALLOW_WRITE: !!js process.env.EXOANCHOR_ALLOW_WRITE",
                result.stdout,
            )
            launcher = LAUNCHER_PATH.read_text(encoding="utf-8")
            self.assertIn("export EXOANCHOR_ALLOW_WRITE=0", launcher)
            self.assertIn("export DSH_PERMISSION_MODE=read-only", launcher)

    def test_supervised_launcher_only_opens_device_write_gate(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            password_file = temp_path / "device-password"
            password_file.write_text("not-a-real-password\n", encoding="utf-8")
            password_file.chmod(stat.S_IRUSR | stat.S_IWUSR)
            fake_dsh = temp_path / "dsh"
            fake_dsh.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$EXOANCHOR_ALLOW_WRITE|$DSH_PERMISSION_MODE|"
                "$DSH_TOOLS_MODE|$EXOANCHOR_CONTROL_OWNER\"\n"
                "printf '%s\\n' \"$@\"\n",
                encoding="utf-8",
            )
            fake_dsh.chmod(
                stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR
            )
            environment = os.environ.copy()
            environment.update(
                {
                    "PATH": f"{temp_dir}:{environment['PATH']}",
                    "EXOANCHOR_BASE_URL": "http://device.invalid",
                    "EXOANCHOR_DEVICE_ID": "test-device",
                    "EXOANCHOR_USERNAME": "test-user",
                    "EXOANCHOR_PASSWORD_FILE": str(password_file),
                    "EXOANCHOR_STATE_DIR": str(temp_path / "state"),
                }
            )
            result = subprocess.run(
                [str(LAUNCHER_PATH), "--supervised", "--", "demo task"],
                cwd=INTEGRATION_ROOT,
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=10,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("1|read-only|native|mcp", result.stdout)
            self.assertIn("--profile", result.stdout)
            self.assertIn("--patch", result.stdout)
            self.assertIn("demo task", result.stdout)
            self.assertNotIn("not-a-real-password", result.stdout)
            state_mode = stat.S_IMODE((temp_path / "state").stat().st_mode)
            self.assertEqual(state_mode, stat.S_IRWXU)


if __name__ == "__main__":
    unittest.main()
