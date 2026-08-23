#!/usr/bin/env python3
"""Keep the published Agent tool catalog reachable through the local runtime."""

from pathlib import Path
import re
import sys


PROJECT = Path(__file__).resolve().parents[2]
MAIN = PROJECT / "main"
DEVICE_HTTP = (MAIN / "services" / "device_http.c").read_text(encoding="utf-8")
ROUTER = (MAIN / "core" / "agent_router.c").read_text(encoding="utf-8")
CAPABILITIES = (
    MAIN / "services" / "web" / "agent_request_module.inc"
).read_text(encoding="utf-8")
CLASSIFIER = (
    MAIN / "services" / "web" / "agent_tool_core_module.inc"
).read_text(encoding="utf-8")
DISPATCH = (
    MAIN / "services" / "web" / "agent_tool_dispatch_module.inc"
).read_text(encoding="utf-8")

TOOL_CLASSIFIERS = {
    "observe_status": "observe_status",
    "observe_video_status": "observe_video_status",
    "observe_hid_status": "observe_hid_status",
    "memory_search": "memory_search",
    "history_search": "history_search",
    "memory_write": "memory_write",
    "ask_user": "ask_user",
    "ssh_exec": "ssh_exec",
    "console_login": "console_login",
    "host_display": "host_display",
    "boot_key_sequence": "boot_key",
    "web_search": "web_search",
    "uart_status": "uart_status",
    "uart_read": "uart_read",
    "uart_write": "uart_write",
    "uart_baud": "uart_baud",
    "power_action": "power_action",
    "wait": "wait",
    "ensure_package": "ssh_primitive",
    "ensure_user": "ssh_primitive",
    "ensure_directory": "ssh_primitive",
    "download_file": "ssh_primitive",
    "write_file": "ssh_primitive",
    "ensure_systemd_service": "ssh_primitive",
    "check_service": "ssh_primitive",
    "verify_port": "ssh_primitive",
}


def main() -> int:
    failures: list[str] = []
    for tool, classifier in TOOL_CLASSIFIERS.items():
        if f'ADD_TOOL("{tool}"' not in DEVICE_HTTP:
            failures.append(f"{tool}: missing from device capability catalog")
        if not re.search(rf'"{re.escape(tool)}"', CAPABILITIES):
            failures.append(f"{tool}: missing from model runtime snapshot")
        if not re.search(rf'"{re.escape(tool)}"', ROUTER):
            failures.append(f"{tool}: missing from Router capability set")
        classifier_call = f"agent_tool_is_{classifier}(call)"
        if classifier_call not in CLASSIFIER:
            failures.append(f"{tool}: missing runtime classifier {classifier}")
        if classifier_call not in DISPATCH:
            failures.append(f"{tool}: missing execution dispatch {classifier}")

    if '"verify_port", "wait",' not in DEVICE_HTTP:
        failures.append("wait: runtime surface is not tool_call")
    if failures:
        print("Agent tool contract check failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print(f"Agent tool contract: PASS ({len(TOOL_CLASSIFIERS)} reachable tools)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
