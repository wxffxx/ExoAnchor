"""
Run/task/session watchers for the ExoAnchor CLI.
"""

from __future__ import annotations

import time
from typing import Any, Callable, Iterable, Optional

from .render import normalize_state, print_jsonl, print_step


RequestFn = Callable[[str, str, str, Optional[dict]], Any]
InputFn = Callable[[str], str]
PrintStepFn = Callable[[str, str], None]
EventIterFactory = Callable[..., Iterable[dict[str, Any]]]


def run_still_waiting_confirmation(base_url: str, run_id: str, step_id: str, request_fn: RequestFn) -> bool:
    """Check the current server-side run state before replaying a confirmation prompt."""
    try:
        current = request_fn(base_url, "GET", f"/api/agent/runs/{run_id}")
    except Exception:
        return True
    current_state = normalize_state(current.get("state") or "")
    current_step_id = str(current.get("waiting_step_id") or "")
    return current_state == "waiting_confirmation" and current_step_id == str(step_id or "")


def watch_plan(
    base_url: str,
    run_id: str,
    *,
    auto_approve: bool = False,
    interval: float = 1.0,
    request_fn: RequestFn,
    input_fn: InputFn = input,
    print_step_fn: PrintStepFn = print_step,
) -> int:
    seen_states: dict[str, str] = {}
    shown_outputs: set[tuple[str, str]] = set()

    while True:
        run = request_fn(base_url, "GET", f"/api/agent/runs/{run_id}")
        state = run.get("state", "")
        steps = run.get("steps") or []
        total = run.get("total_steps") or len(steps)
        completed = run.get("completed_steps") or 0

        for step in steps:
            step_id = str(step.get("id"))
            step_state = str(step.get("status") or "pending")
            if seen_states.get(step_id) != step_state:
                seen_states[step_id] = step_state
                print_step_fn(f"[plan {completed}/{total}]", f"{step_id} {step_state.upper()} {step.get('description', '')}")

            output = (step.get("output") or step.get("error") or "").strip()
            output_key = (step_id, output)
            if output and output_key not in shown_outputs and step_state in {"done", "failed", "skipped"}:
                shown_outputs.add(output_key)
                print_step_fn("  output>", output[:1200])

        if state == "waiting_confirmation":
            step_id = run.get("waiting_step_id")
            step = next((item for item in steps if str(item.get("id")) == str(step_id)), {})
            if auto_approve:
                approved = True
            else:
                prompt = f"Approve step {step_id} ({step.get('description', '')})? [y/N]: "
                approved = input_fn(prompt).strip().lower() in {"y", "yes"}
            request_fn(base_url, "POST", f"/api/agent/runs/{run_id}/confirm", {"approved": approved})
            continue

        if state == "completed":
            print_step_fn("[plan]", f"COMPLETED {run.get('goal', '')}")
            return 0
        if state in {"failed", "aborted"}:
            print_step_fn("[plan]", f"{state.upper()} {run.get('error', '')}")
            return 1

        time.sleep(interval)


def watch_plan_stream(
    base_url: str,
    run_id: str,
    *,
    auto_approve: bool = False,
    jsonl: bool = False,
    request_fn: RequestFn,
    iter_events: EventIterFactory,
    input_fn: InputFn = input,
    print_step_fn: PrintStepFn = print_step,
) -> int:
    seen_states: dict[str, str] = {}
    shown_outputs: set[tuple[str, str]] = set()
    handled_confirmations: set[tuple[str, str]] = set()

    for envelope in iter_events(base_url, run_id=run_id):
        if jsonl:
            print_jsonl(envelope)

        payload = envelope.get("payload") or {}
        run = payload.get("run") or {}
        if not run:
            continue

        state = normalize_state(run.get("state") or envelope.get("state") or "")
        steps = run.get("steps") or []
        total = run.get("total_steps") or len(steps)
        completed = run.get("completed_steps") or 0

        for step in steps:
            step_id = str(step.get("id"))
            step_state = str(step.get("status") or "pending")
            if seen_states.get(step_id) != step_state:
                seen_states[step_id] = step_state
                print_step_fn(f"[plan {completed}/{total}]", f"{step_id} {step_state.upper()} {step.get('description', '')}")

            output = (step.get("output") or step.get("error") or "").strip()
            output_key = (step_id, output)
            if output and output_key not in shown_outputs and step_state in {"done", "failed", "skipped"}:
                shown_outputs.add(output_key)
                print_step_fn("  output>", output[:1200])

        if state == "waiting_confirmation":
            step_id = str(run.get("waiting_step_id") or "")
            confirm_key = (run_id, step_id)
            if confirm_key not in handled_confirmations:
                if not run_still_waiting_confirmation(base_url, run_id, step_id, request_fn):
                    handled_confirmations.add(confirm_key)
                    continue
                step = next((item for item in steps if str(item.get("id")) == step_id), {})
                if auto_approve:
                    approved = True
                else:
                    prompt = f"Approve step {step_id} ({step.get('description', '')})? [y/N]: "
                    approved = input_fn(prompt).strip().lower() in {"y", "yes"}
                request_fn(base_url, "POST", f"/api/agent/runs/{run_id}/confirm", {"approved": approved})
                handled_confirmations.add(confirm_key)
            continue

        if state == "completed":
            print_step_fn("[plan]", f"COMPLETED {run.get('goal', '')}")
            return 0
        if state in {"failed", "aborted"}:
            print_step_fn("[plan]", f"{state.upper()} {run.get('error', '')}")
            return 1

    return 1


def watch_task(
    base_url: str,
    task_id: str,
    *,
    interval: float = 1.0,
    request_fn: RequestFn,
    print_step_fn: PrintStepFn = print_step,
) -> int:
    seen_count = 0
    last_state = ""

    while True:
        snapshot = request_fn(base_url, "GET", f"/api/agent/task/history/{task_id}")
        state = str(snapshot.get("state") or "")
        history = snapshot.get("history") or []

        if state != last_state:
            last_state = state
            print_step_fn("[task]", f"{task_id} {state.upper()} {snapshot.get('skill_name', '')}")

        for item in history[seen_count:]:
            action_type = item.get("action_type") or "step"
            detail = item.get("action_detail") or ""
            result = item.get("action_result") or item.get("observation", {}).get("output") or ""
            line = f"{action_type}"
            if detail:
                line += f" :: {detail}"
            print_step_fn("[task step]", line)
            if result:
                print_step_fn("  output>", str(result)[:1200])
        seen_count = len(history)

        if state == "completed":
            print_step_fn("[task]", f"COMPLETED {snapshot.get('skill_name', '')}")
            return 0
        if state in {"failed", "aborted"}:
            print_step_fn("[task]", f"{state.upper()} {snapshot.get('error', '')}")
            return 1

        time.sleep(interval)


def watch_task_stream(
    base_url: str,
    task_id: str,
    *,
    jsonl: bool = False,
    iter_events: EventIterFactory,
    print_step_fn: PrintStepFn = print_step,
) -> int:
    seen_count = 0
    last_state = ""

    for envelope in iter_events(base_url, task_id=task_id):
        if jsonl:
            print_jsonl(envelope)

        event_type = str(envelope.get("event") or "")
        payload = envelope.get("payload") or {}
        snapshot = payload.get("snapshot") or {}
        status = payload.get("status") or snapshot

        if snapshot:
            state = normalize_state(snapshot.get("state") or "")
            history = snapshot.get("history") or []
            skill_name = snapshot.get("skill_name") or task_id
        else:
            state = normalize_state(status.get("state") or envelope.get("state") or ("running" if event_type in {"task_start", "step"} else ""))
            history = []
            skill_name = status.get("skill_name") or payload.get("skill") or task_id

        if state and state != last_state:
            last_state = state
            print_step_fn("[task]", f"{task_id} {state.upper()} {skill_name}")

        if snapshot:
            for item in history[seen_count:]:
                action_type = item.get("action_type") or "step"
                detail = item.get("action_detail") or ""
                result = item.get("action_result") or item.get("observation", {}).get("output") or ""
                line = f"{action_type}"
                if detail:
                    line += f" :: {detail}"
                print_step_fn("[task step]", line)
                if result:
                    print_step_fn("  output>", str(result)[:1200])
            seen_count = len(history)
        elif event_type == "step":
            line = str(payload.get("step_id") or payload.get("screen_state") or payload.get("action_detail") or "step")
            print_step_fn("[task step]", line)

        if state == "completed":
            print_step_fn("[task]", f"COMPLETED {skill_name}")
            return 0
        if state in {"failed", "aborted"}:
            print_step_fn("[task]", f"{state.upper()} {status.get('error', snapshot.get('error', ''))}")
            return 1

    return 1


def watch_session_stream(
    base_url: str,
    session_id: str,
    *,
    auto_approve: bool = False,
    jsonl: bool = False,
    request_fn: RequestFn,
    iter_events: EventIterFactory,
    input_fn: InputFn = input,
    print_step_fn: PrintStepFn = print_step,
) -> int:
    session_last_state = ""
    session_message = ""
    plan_seen_states: dict[str, str] = {}
    plan_shown_outputs: set[tuple[str, str]] = set()
    task_seen_count = 0
    handled_confirmations: set[tuple[str, str]] = set()

    for envelope in iter_events(base_url, session_id=session_id):
        if jsonl:
            print_jsonl(envelope)

        payload = envelope.get("payload") or {}
        session = payload.get("session") or {}
        child = payload.get("child_event") or {}
        state = normalize_state(session.get("state") or envelope.get("state") or "")

        if state and state != session_last_state and envelope.get("event") != "child_event":
            session_last_state = state
            print_step_fn("[session]", f"{session_id} {state.upper()} {session.get('summary') or session.get('request') or ''}".strip())

        message = str(session.get("message") or "")
        if message and message != session_message and envelope.get("event") != "child_event":
            session_message = message
            print_step_fn("  message>", message[:1200])

        if child:
            entity_kind = str(child.get("entity_kind") or "")
            child_payload = child.get("payload") or {}
            if entity_kind == "plan_run":
                run = child_payload.get("run") or {}
                child_run_id = str(child.get("entity_id") or session.get("run_id") or "")
                steps = run.get("steps") or []
                total = run.get("total_steps") or len(steps)
                completed = run.get("completed_steps") or 0
                run_state = normalize_state(run.get("state") or child.get("state") or "")

                for step in steps:
                    step_id = str(step.get("id"))
                    step_state = str(step.get("status") or "pending")
                    if plan_seen_states.get(step_id) != step_state:
                        plan_seen_states[step_id] = step_state
                        print_step_fn(f"[plan {completed}/{total}]", f"{step_id} {step_state.upper()} {step.get('description', '')}")

                    output = (step.get("output") or step.get("error") or "").strip()
                    output_key = (step_id, output)
                    if output and output_key not in plan_shown_outputs and step_state in {"done", "failed", "skipped"}:
                        plan_shown_outputs.add(output_key)
                        print_step_fn("  output>", output[:1200])

                if run_state == "waiting_confirmation":
                    step_id = str(run.get("waiting_step_id") or "")
                    confirm_key = (child_run_id, step_id)
                    if child_run_id and confirm_key not in handled_confirmations:
                        if not run_still_waiting_confirmation(base_url, child_run_id, step_id, request_fn):
                            handled_confirmations.add(confirm_key)
                            continue
                        step = next((item for item in steps if str(item.get("id")) == step_id), {})
                        if auto_approve:
                            approved = True
                        else:
                            prompt = f"Approve step {step_id} ({step.get('description', '')})? [y/N]: "
                            approved = input_fn(prompt).strip().lower() in {"y", "yes"}
                        request_fn(base_url, "POST", f"/api/agent/runs/{child_run_id}/confirm", {"approved": approved})
                        handled_confirmations.add(confirm_key)
            elif entity_kind == "task":
                snapshot = child_payload.get("snapshot") or {}
                status = child_payload.get("status") or {}
                history = snapshot.get("history") or []
                task_id = str(child.get("entity_id") or session.get("task_id") or "")
                task_state = normalize_state(snapshot.get("state") or status.get("state") or child.get("state") or "")
                skill_name = snapshot.get("skill_name") or status.get("skill_name") or task_id
                if task_state:
                    print_step_fn("[task]", f"{task_id} {task_state.upper()} {skill_name}")
                for item in history[task_seen_count:]:
                    action_type = item.get("action_type") or "step"
                    detail = item.get("action_detail") or ""
                    result = item.get("action_result") or item.get("observation", {}).get("output") or ""
                    line = f"{action_type}"
                    if detail:
                        line += f" :: {detail}"
                    print_step_fn("[task step]", line)
                    if result:
                        print_step_fn("  output>", str(result)[:1200])
                task_seen_count = len(history)

        if state == "completed":
            return 0
        if state in {"failed", "aborted"}:
            err = session.get("error") or session.get("message") or ""
            print_step_fn("[session]", f"{state.upper()} {err}".strip())
            return 1
        if state == "waiting_input":
            return 0

    return 1
