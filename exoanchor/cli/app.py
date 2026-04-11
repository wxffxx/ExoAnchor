"""
Main ExoAnchor CLI command handlers.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from typing import Any

from .http_client import DEFAULT_BASE_URL, iter_runtime_events, request
from .render import print_json, print_step
from .watchers import watch_plan, watch_plan_stream, watch_session_stream, watch_task, watch_task_stream


def parse_params(items: list[str]) -> dict[str, Any]:
    params: dict[str, Any] = {}
    for item in items:
        if "=" not in item:
            raise ValueError(f"Invalid param '{item}', expected key=value")
        key, value = item.split("=", 1)
        params[key] = value
    return params


def maybe_request(base_url: str, method: str, path: str, body: dict | None = None):
    try:
        return request(base_url, method, path, body)
    except RuntimeError as exc:
        lowered = str(exc).lower()
        if "not found" in lowered:
            return None
        raise


def resolve_target(base_url: str, target_id: str, kind: str) -> tuple[str, dict[str, Any]]:
    if kind == "session":
        session = request(base_url, "GET", f"/api/agent/sessions/{target_id}")
        return "session", session
    if kind == "run":
        run = request(base_url, "GET", f"/api/agent/runs/{target_id}")
        return "run", run
    if kind == "task":
        task = request(base_url, "GET", f"/api/agent/task/history/{target_id}")
        return "task", task

    session = maybe_request(base_url, "GET", f"/api/agent/sessions/{target_id}")
    if session is not None:
        return "session", session

    run = maybe_request(base_url, "GET", f"/api/agent/runs/{target_id}")
    if run is not None:
        return "run", run

    task = maybe_request(base_url, "GET", f"/api/agent/task/history/{target_id}")
    if task is not None:
        return "task", task

    raise RuntimeError(f"Target not found: {target_id}")


def resolve_run_target(base_url: str, target_id: str, kind: str) -> tuple[str, str]:
    if kind == "session":
        session = request(base_url, "GET", f"/api/agent/sessions/{target_id}")
        run_id = str(session.get("run_id") or "")
        if not run_id:
            raise RuntimeError("Session is not attached to a plan run")
        return "session", run_id
    if kind == "run":
        return "run", target_id

    session = maybe_request(base_url, "GET", f"/api/agent/sessions/{target_id}")
    if session is not None:
        run_id = str(session.get("run_id") or "")
        if not run_id:
            raise RuntimeError("Session is not attached to a plan run")
        return "session", run_id

    run = maybe_request(base_url, "GET", f"/api/agent/runs/{target_id}")
    if run is not None:
        return "run", str(run.get("run_id") or target_id)

    raise RuntimeError(f"Run target not found: {target_id}")


def ensure_current_task_matches(base_url: str, task_id: str) -> None:
    current = request(base_url, "GET", "/api/agent/task/current")
    current_task_id = str(current.get("task_id") or "")
    if current.get("status") == "idle" or current_task_id != str(task_id):
        raise RuntimeError("Task control by id currently only works for the active task")


def cmd_status(args: argparse.Namespace) -> int:
    status = request(args.base_url, "GET", "/api/agent/status")
    print_json(status)
    return 0


def cmd_serve(args: argparse.Namespace) -> int:
    cmd = [sys.executable, "-m", "uvicorn", "test_server:app", "--host", args.host, "--port", str(args.port)]
    return subprocess.call(cmd)


def cmd_skill(args: argparse.Namespace) -> int:
    params = parse_params(args.param or [])
    result = request(args.base_url, "POST", "/api/agent/task/start", {"skill_name": args.name, "params": params})
    task_id = result.get("task_id")
    if not task_id:
        print_json(result)
        return 1
    print_step("[task]", f"STARTED {args.name} ({task_id})")
    try:
        return watch_task_stream(args.base_url, task_id, jsonl=bool(args.jsonl), iter_events=iter_runtime_events)
    except Exception:
        return watch_task(args.base_url, task_id, request_fn=request)


def cmd_ask(args: argparse.Namespace) -> int:
    session = request(
        args.base_url,
        "POST",
        "/api/agent/sessions",
        {
            "message": args.message,
            "force_plan": bool(args.plan),
            "conversation_id": "cli",
            "dry_run": bool(args.dry_run),
            "metadata": {"client": "cli"},
        },
    )

    if args.dry_run:
        print_json(session.get("parsed_result") or {})
        return 0

    session_id = session.get("session_id")
    if not session_id:
        print_json(session)
        return 1

    try:
        return watch_session_stream(
            args.base_url,
            session_id,
            auto_approve=args.yes,
            jsonl=bool(args.jsonl),
            request_fn=request,
            iter_events=iter_runtime_events,
        )
    except Exception:
        current = request(args.base_url, "GET", f"/api/agent/sessions/{session_id}")
        if current.get("run_id"):
            return watch_plan(args.base_url, current["run_id"], auto_approve=args.yes, request_fn=request)
        if current.get("task_id"):
            return watch_task(args.base_url, current["task_id"], request_fn=request)
        if current.get("message"):
            print(current["message"])
            return 0 if current.get("state") != "failed" else 1
        print_json(current)
        return 1


def cmd_attach(args: argparse.Namespace) -> int:
    target_kind, target = resolve_target(args.base_url, args.target_id, args.kind)

    if target_kind == "session":
        session_id = str(target.get("session_id") or args.target_id)
        try:
            return watch_session_stream(
                args.base_url,
                session_id,
                auto_approve=args.yes,
                jsonl=bool(args.jsonl),
                request_fn=request,
                iter_events=iter_runtime_events,
            )
        except Exception:
            current = request(args.base_url, "GET", f"/api/agent/sessions/{session_id}")
            if current.get("run_id"):
                return watch_plan(args.base_url, current["run_id"], auto_approve=args.yes, request_fn=request)
            if current.get("task_id"):
                return watch_task(args.base_url, current["task_id"], request_fn=request)
            if current.get("message"):
                print(current["message"])
                return 0 if current.get("state") != "failed" else 1
            print_json(current)
            return 1

    if target_kind == "run":
        run_id = str(target.get("run_id") or args.target_id)
        try:
            return watch_plan_stream(
                args.base_url,
                run_id,
                auto_approve=args.yes,
                jsonl=bool(args.jsonl),
                request_fn=request,
                iter_events=iter_runtime_events,
            )
        except Exception:
            return watch_plan(args.base_url, run_id, auto_approve=args.yes, request_fn=request)

    task_id = str(target.get("task_id") or args.target_id)
    try:
        return watch_task_stream(args.base_url, task_id, jsonl=bool(args.jsonl), iter_events=iter_runtime_events)
    except Exception:
        return watch_task(args.base_url, task_id, request_fn=request)


def cmd_resume(args: argparse.Namespace) -> int:
    target_kind, target = resolve_target(args.base_url, args.target_id, args.kind)
    if target_kind == "session":
        result = request(args.base_url, "POST", f"/api/agent/sessions/{args.target_id}/resume", {"saved": bool(args.saved)})
    elif target_kind == "run":
        path = f"/api/agent/runs/{args.target_id}/resume_saved" if args.saved else f"/api/agent/runs/{args.target_id}/resume"
        result = request(args.base_url, "POST", path)
    else:
        task_id = str(target.get("task_id") or args.target_id)
        ensure_current_task_matches(args.base_url, task_id)
        result = request(args.base_url, "POST", "/api/agent/task/resume")
    print_json(result)
    return 0


def cmd_abort(args: argparse.Namespace) -> int:
    target_kind, target = resolve_target(args.base_url, args.target_id, args.kind)
    if target_kind == "session":
        result = request(args.base_url, "POST", f"/api/agent/sessions/{args.target_id}/abort")
    elif target_kind == "run":
        result = request(args.base_url, "POST", f"/api/agent/runs/{args.target_id}/abort")
    else:
        task_id = str(target.get("task_id") or args.target_id)
        ensure_current_task_matches(args.base_url, task_id)
        result = request(args.base_url, "POST", "/api/agent/task/abort")
    print_json(result)
    return 0


def cmd_approve(args: argparse.Namespace) -> int:
    source_kind, run_id = resolve_run_target(args.base_url, args.target_id, args.kind)
    if source_kind == "session":
        result = request(args.base_url, "POST", f"/api/agent/sessions/{args.target_id}/approve")
    else:
        result = request(args.base_url, "POST", f"/api/agent/runs/{run_id}/confirm", {"approved": True})
    print_json(result)
    return 0


def cmd_reject(args: argparse.Namespace) -> int:
    source_kind, run_id = resolve_run_target(args.base_url, args.target_id, args.kind)
    if source_kind == "session":
        result = request(args.base_url, "POST", f"/api/agent/sessions/{args.target_id}/reject")
    else:
        result = request(args.base_url, "POST", f"/api/agent/runs/{run_id}/confirm", {"approved": False})
    print_json(result)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ExoAnchor pure CLI")
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL, help=f"ExoAnchor server URL (default: {DEFAULT_BASE_URL})")

    sub = parser.add_subparsers(dest="command", required=True)

    p_status = sub.add_parser("status", help="Show ExoAnchor status")
    p_status.set_defaults(func=cmd_status)

    p_serve = sub.add_parser("serve", help="Run the ExoAnchor test server")
    p_serve.add_argument("--host", default="0.0.0.0")
    p_serve.add_argument("--port", type=int, default=8090)
    p_serve.set_defaults(func=cmd_serve)

    p_skill = sub.add_parser("skill", help="Run a native skill")
    p_skill.add_argument("name", help="Skill name")
    p_skill.add_argument("--param", action="append", default=[], help="Skill param in key=value form")
    p_skill.add_argument("--jsonl", action="store_true", help="Also print raw runtime events as JSON lines")
    p_skill.set_defaults(func=cmd_skill)

    p_ask = sub.add_parser("ask", help="Send a natural-language request and execute the result")
    p_ask.add_argument("message", help="Natural-language request")
    p_ask.add_argument("--plan", action="store_true", help="Force plan/skill output instead of a single ssh command")
    p_ask.add_argument("--dry-run", action="store_true", help="Only print the parsed LLM result without executing")
    p_ask.add_argument("--yes", action="store_true", help="Auto-approve dangerous plan confirmations")
    p_ask.add_argument("--jsonl", action="store_true", help="Also print raw runtime events as JSON lines")
    p_ask.set_defaults(func=cmd_ask)

    p_attach = sub.add_parser("attach", help="Attach to an existing session/run/task and stream its events")
    p_attach.add_argument("target_id", help="Session ID, run ID, or task ID")
    p_attach.add_argument("--kind", choices=["auto", "session", "run", "task"], default="auto")
    p_attach.add_argument("--yes", action="store_true", help="Auto-approve dangerous plan confirmations")
    p_attach.add_argument("--jsonl", action="store_true", help="Also print raw runtime events as JSON lines")
    p_attach.set_defaults(func=cmd_attach)

    p_resume = sub.add_parser("resume", help="Resume a paused session/run/task")
    p_resume.add_argument("target_id", help="Session ID, run ID, or task ID")
    p_resume.add_argument("--kind", choices=["auto", "session", "run", "task"], default="auto")
    p_resume.add_argument("--saved", action="store_true", help="Resume a saved completed/failed run as a new run")
    p_resume.set_defaults(func=cmd_resume)

    p_abort = sub.add_parser("abort", help="Abort a session/run/task")
    p_abort.add_argument("target_id", help="Session ID, run ID, or task ID")
    p_abort.add_argument("--kind", choices=["auto", "session", "run", "task"], default="auto")
    p_abort.set_defaults(func=cmd_abort)

    p_approve = sub.add_parser("approve", help="Approve a waiting plan confirmation")
    p_approve.add_argument("target_id", help="Session ID or run ID")
    p_approve.add_argument("--kind", choices=["auto", "session", "run"], default="auto")
    p_approve.set_defaults(func=cmd_approve)

    p_reject = sub.add_parser("reject", help="Reject a waiting plan confirmation")
    p_reject.add_argument("target_id", help="Session ID or run ID")
    p_reject.add_argument("--kind", choices=["auto", "session", "run"], default="auto")
    p_reject.set_defaults(func=cmd_reject)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
