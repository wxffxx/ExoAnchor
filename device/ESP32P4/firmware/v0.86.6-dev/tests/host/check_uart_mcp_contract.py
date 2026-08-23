#!/usr/bin/env python3
from pathlib import Path
import re
import sys


PROJECT = Path(__file__).resolve().parents[2]
HTTP_API = PROJECT / "main" / "adapters" / "http_api.c"
DEVICE_HTTP = PROJECT / "main" / "services" / "device_http.c"
UART = PROJECT / "main" / "services" / "web" / "uart_module.inc"
AGENT_UART = (
    PROJECT / "main" / "services" / "web" / "agent_uart_tool_module.inc"
)
DISPATCH = (
    PROJECT / "main" / "services" / "web" / "agent_tool_dispatch_module.inc"
)


def fail(message: str) -> None:
    print(f"UART MCP contract: FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    http = HTTP_API.read_text(encoding="utf-8")
    for path, tool in (
        ("/api/uart/status", "uart_status"),
        ("/api/uart/read", "uart_read"),
        ("/api/uart/write", "uart_write"),
        ("/api/uart/baud", "uart_baud"),
    ):
        pattern = (
            rf'request_uri_starts_with\(req, "{re.escape(path)}"\).*?'
            rf'return "{tool}";'
        )
        if not re.search(pattern, http, re.S):
            fail(f"{path} is not mapped to {tool}")

    device = DEVICE_HTTP.read_text(encoding="utf-8")
    mapping = re.search(
        r"static bool agent_tool_mcp_callable\(.*?static const char \*const mapped\[\] = \{(.*?)\};",
        device,
        re.S,
    )
    if not mapping:
        fail("cannot find agent_tool_mcp_callable mapping")
    for tool in ("uart_status", "uart_read", "uart_write", "uart_baud"):
        if f'"{tool}"' not in mapping.group(1):
            fail(f"{tool} is not mcp_callable")

    uart = UART.read_text(encoding="utf-8")
    read_handler = re.search(
        r"static esp_err_t uart_read_handler\(.*?\n\}",
        uart,
        re.S,
    )
    if not read_handler:
        fail("cannot find uart_read_handler")
    read_body = read_handler.group(0)
    for marker in (
        "si_target_uart_journal_read_from",
        '"next_cursor"',
        '"journal_next_cursor"',
        '"pending_bytes"',
        '"history_lost"',
    ):
        if marker not in read_body:
            fail(f"non-destructive UART read missing {marker}")
    if "si_target_uart_read(" in read_body:
        fail("UART HTTP read still consumes the backlog")

    for handler_name in ("uart_write_handler", "uart_baud_handler"):
        handler = re.search(
            rf"static esp_err_t {handler_name}\(.*?\n\}}",
            uart,
            re.S,
        )
        if not handler:
            fail(f"cannot find {handler_name}")
        for marker in (
            "SI_AUTHZ_RISK_HIGH",
            "uart_mutation_begin()",
            "uart_mutation_end()",
        ):
            if marker not in handler.group(0):
                fail(f"{handler_name} missing {marker}")

    agent_uart = AGENT_UART.read_text(encoding="utf-8")
    for marker in ("uart_mutation_begin()", "uart_mutation_end()"):
        if agent_uart.count(marker) < 2:
            fail(f"Agent write/baud do not both use {marker}")
    dispatch = DISPATCH.read_text(encoding="utf-8")
    for marker in (
        "agent_uart_payload_is_sensitive(data)",
        "sensitive UART payloads are prohibited",
    ):
        if marker not in dispatch:
            fail(f"Agent sensitive payload boundary missing {marker}")

    print("UART MCP contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
