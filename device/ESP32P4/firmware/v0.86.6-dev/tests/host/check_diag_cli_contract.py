#!/usr/bin/env python3
"""Guard development-console command framing and recovery invariants."""

from pathlib import Path
import sys


PROJECT = Path(__file__).resolve().parents[2]
DIAG_CLI = PROJECT / "main" / "services" / "diag_cli.c"
DIAGNOSTICS_SERVICE = PROJECT / "main" / "application" / "diagnostics_service.c"
DIAGNOSTICS_BACKEND = (
    PROJECT / "main" / "services" / "web" / "diagnostics_backend_module.inc"
)
AGENT_RUN_HTTP = (
    PROJECT / "main" / "services" / "web" / "agent_run_http_module.inc"
)


def main() -> int:
    text = DIAG_CLI.read_text(encoding="utf-8")
    service = DIAGNOSTICS_SERVICE.read_text(encoding="utf-8")
    backend = DIAGNOSTICS_BACKEND.read_text(encoding="utf-8")
    agent_run = AGENT_RUN_HTTP.read_text(encoding="utf-8")
    failures: list[str] = []

    for command in (
        "help",
        "status",
        "target-uart-read",
        "eeprom-wp",
        "eeprom-emu",
        "tf-rm",
        "agent-data-clear",
        "agent-run-b64",
        "agent-events",
        "agent-pause",
        "agent-resume",
        "agent-cancel",
        "agent-abort",
        "agent-steer",
        "agent-result",
        "agent-b64-begin",
        "agent-prompt-cancel",
        "sync",
        "abort",
    ):
        marker = f'strcmp(tmp, "{command}") == 0'
        if marker not in text:
            failures.append(
                f"quarantine recovery does not recognize command: {command}"
            )

    for marker in (
        "flush_stdin_input();\n            discard_input = !has_eol;",
        "if (discard_input) {",
        "if (has_eol) {",
        "line_is_resync_command(line)",
        "agent_prompt_capture_clear();",
    ):
        if marker not in text:
            failures.append(f"input framing/recovery marker missing: {marker}")

    # UART mutations must never capture whichever Run happens to be current.
    # Each lifecycle command parses and forwards a caller-provided exact ID.
    for command, facade in (
        ("agent-pause", "si_diagnostics_agent_run_pause"),
        ("agent-resume", "si_diagnostics_agent_run_resume"),
        ("agent-cancel", "si_diagnostics_agent_run_cancel"),
        ("agent-abort", "si_diagnostics_agent_run_abort"),
    ):
        marker = f'run_agent_control_command("{command}"'
        if marker not in text or facade not in text[text.find(marker) : text.find(marker) + 300]:
            failures.append(f"exact-run UART control missing: {command}")

    for marker in (
        "run_agent_status_command(cmd + 12",
        "run_agent_events_command(cmd + 12",
        "run_agent_steer_command(cmd + 11",
        "run_agent_result_command(cmd + 12",
        "si_diagnostics_agent_run_events(run_id",
        "si_diagnostics_agent_run_steer(run_id, message",
        "si_diagnostics_agent_run_result(run_id, offset, max_bytes",
    ):
        if marker not in text:
            failures.append(f"exact-run UART Agent marker missing: {marker}")

    if "exact_run_id_required(run_id" not in service:
        failures.append("diagnostics facade does not reject naked mutations")
    for member in (
        ".agent_run_pause = web_debug_agent_run_pause",
        ".agent_run_resume = web_debug_agent_run_resume",
        ".agent_run_cancel = web_debug_agent_run_cancel",
        ".agent_run_abort = web_debug_agent_run_abort",
        ".agent_run_steer = web_debug_agent_run_steer",
        ".agent_run_result = web_debug_agent_run_result",
    ):
        if member not in backend:
            failures.append(f"diagnostics backend registration missing: {member}")

    for marker in (
        "submitted_run_id=%s active_run_id=%s",
        "si_diagnostics_agent_submit_receipt_ids(",
        "action, run_id, controlled_job",
        "reason=run_id_mismatch",
        "reason=cross_source_denied",
        "web_debug_agent_run_require_diagnostics_source(run_id)",
        "web_debug_agent_run_has_diagnostics_source_locked()",
        "strcmp(expected_run_id, s_agent_run_job.job_id)",
        "si_diagnostics_format_agent_event_page(",
        "available_latest_seq",
        "oldest_seq",
        "summary_b64=",
        "stage_b64=",
        "detail_b64=",
        "assistant_b64=",
        "encoding=base64",
        "SI_DIAGNOSTICS_RESULT_MAX_CHUNK",
        "ready=0 ok=0",
        "crc32=%08",
    ):
        if marker not in agent_run and marker not in service:
            failures.append(f"exact-run diagnostics backend marker missing: {marker}")

    diagnostics_source_start = agent_run.find("si_agent_task_source_t diagnostics_source")
    diagnostics_source_end = agent_run.find("};", diagnostics_source_start)
    diagnostics_source = agent_run[diagnostics_source_start:diagnostics_source_end]
    if diagnostics_source_start < 0 or not all(
        marker in diagnostics_source
        for marker in ("SI_CAPABILITY_OBSERVE", "SI_CAPABILITY_AGENT_RUN")
    ):
        failures.append("diagnostics source lost bounded OBSERVE|AGENT_RUN authority")
    if "APPROV" in diagnostics_source.upper():
        failures.append("diagnostics source must not gain approval authority")

    if failures:
        print("diagnostic CLI contract failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1

    print("diagnostic CLI contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
