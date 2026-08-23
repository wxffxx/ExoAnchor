#!/usr/bin/env python3
"""Keep Agent/MCP terminal ownership symmetric across UART, SSH and UI."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


web = read("main/services/web_server.c")
control = read("main/services/web/terminal_control_module.inc")
uart = read("main/services/web/uart_module.inc")
ssh = read("main/services/web/ssh_module.inc")
agent_uart = read("main/services/web/agent_uart_tool_module.inc")
agent_ssh = read("main/services/web/agent_ssh_tool_module.inc")
terminal = read("main/www/terminal.html")

required = {
    "exact stop route": "/api/terminal/control/stop",
    "SSH upgraded pre-handshake": "cfg.ws_pre_handshake_cb = ssh_ws_pre_handshake",
    "SSH upgraded post-handshake": "cfg.ws_post_handshake_cb = ssh_ws_post_handshake",
}
for label, marker in required.items():
    assert marker in web, f"missing {label}"

assert "generation == 0U" in control
assert "generation_value <= (double)UINT32_MAX" in control
assert "generation_value ==" in control
assert "(double)(uint32_t)generation_value" in control
assert "session.principal != SI_PRINCIPAL_BROWSER" in control
assert "terminal_control_state_request_cancel" in control
assert "terminal_control_manual_input_allowed" in uart
assert "terminal_control_manual_input_allowed" in ssh
assert "manual UART terminal connected; manual control has priority" not in agent_uart
assert "SI_TERMINAL_ACTOR_AGENT" in agent_uart
assert "SI_TERMINAL_ACTOR_AGENT" in agent_ssh
assert "SI_TERMINAL_ACTOR_MCP" in uart
assert "SI_TERMINAL_ACTOR_MCP" in ssh
assert "terminal_control_mirror_ssh_output" in read(
    "main/services/web/agent_memory_tools_module.inc")
assert 'control.type==="terminal.control"' in terminal
assert 'control.type==="terminal.input"' in terminal
assert "term.options.disableStdin=active" in terminal
assert "立即终止" in terminal
assert "UI.lifecycle.interval(loadStatus,1000)" in terminal

print("terminal Agent/MCP ownership contract: PASS")
