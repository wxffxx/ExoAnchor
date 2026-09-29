#!/usr/bin/env python3
"""Authenticated host CLI for the ExoAnchor embedded Agent HTTP surface.

Authentication material is accepted only through a password file or a bearer
token file.  The tool never prints either credential and does not persist the
short-lived token returned by password login.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import ssl
import stat
import sys
import time
import uuid
from pathlib import Path
from typing import Any, NoReturn
from urllib import error, parse, request


EXIT_USAGE = 2
EXIT_REJECTED = 3
EXIT_TIMEOUT = 4
MAX_CREDENTIAL_BYTES = 4096
MAX_ERROR_BODY_BYTES = 16 * 1024
SESSION_ID_PATTERN = re.compile(r"[A-Za-z0-9_-]{1,48}\Z")
STRUCTURED_REJECT_REASONS = frozenset(
    {"agent_run_busy", "idempotency_conflict", "run_id_mismatch"}
)
TERMINAL_STATES = frozenset(
    {"done", "failed", "aborted", "outcome_unknown", "idle"}
)
FAILED_TERMINAL_STATES = frozenset(
    {"failed", "aborted", "outcome_unknown"}
)
ACTIVE_STATES = frozenset({"running", "waiting_request", "paused"})


class CliError(RuntimeError):
    """A user-facing error with an intentional process exit status."""

    def __init__(self, message: str, exit_code: int = 1) -> None:
        super().__init__(message)
        self.exit_code = exit_code


class StructuredReject(CliError):
    """A structured API rejection that must remain machine-readable."""

    def __init__(self, payload: dict[str, Any], http_status: int) -> None:
        super().__init__(f"HTTP {http_status} structured rejection", EXIT_REJECTED)
        self.payload = payload
        self.http_status = http_status


def _fail(message: str, exit_code: int = 1) -> NoReturn:
    raise CliError(message, exit_code)


class _NoRedirect(request.HTTPRedirectHandler):
    """Keep bearer credentials pinned to the explicitly selected origin."""

    def redirect_request(
        self,
        req: request.Request,
        fp: Any,
        code: int,
        msg: str,
        headers: Any,
        newurl: str,
    ) -> None:
        del req, fp, code, msg, headers, newurl
        return None


def _read_private_file(path_text: str, label: str) -> str:
    path = Path(path_text).expanduser()
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        _fail(f"cannot open {label} file {path}: {exc.strerror or exc}")
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            _fail(f"{label} file must be a regular file")
        if metadata.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
            _fail(f"{label} file must not be accessible by group or others (use chmod 600)")
        if metadata.st_size > MAX_CREDENTIAL_BYTES:
            _fail(f"{label} file is too large")
        raw = os.read(descriptor, MAX_CREDENTIAL_BYTES + 1)
    finally:
        os.close(descriptor)
    if len(raw) > MAX_CREDENTIAL_BYTES:
        _fail(f"{label} file is too large")
    try:
        value = raw.decode("utf-8")
    except UnicodeDecodeError:
        _fail(f"{label} file must be UTF-8 text")
    if value.endswith("\r\n"):
        value = value[:-2]
    elif value.endswith(("\r", "\n")):
        value = value[:-1]
    if not value or "\x00" in value or "\n" in value or "\r" in value:
        _fail(f"{label} file must contain exactly one non-empty line")
    return value


def _read_text(path_text: str, label: str) -> str:
    try:
        return Path(path_text).expanduser().read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        _fail(f"cannot read {label} file {path_text}: {exc}")


def _load_json_object(path_text: str, label: str) -> dict[str, Any]:
    text = _read_text(path_text, label)
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        _fail(f"{label} file is not valid JSON: line {exc.lineno} column {exc.colno}")
    if not isinstance(value, dict):
        _fail(f"{label} file must contain one JSON object")
    return value


def _message_value(inline: str | None, path_text: str | None, label: str) -> str:
    value = inline if inline is not None else _read_text(path_text or "", label)
    if not value.strip():
        _fail(f"{label} must not be empty", EXIT_USAGE)
    return value


def _normalize_base_url(raw: str, allow_http: bool) -> str:
    candidate = raw.strip().rstrip("/")
    parts = parse.urlsplit(candidate)
    if parts.scheme not in {"http", "https"} or not parts.hostname:
        _fail("--base-url must be an absolute http:// or https:// URL", EXIT_USAGE)
    if parts.username or parts.password:
        _fail("credentials are forbidden in --base-url", EXIT_USAGE)
    if parts.query or parts.fragment or parts.path not in {"", "/"}:
        _fail("--base-url must not contain a path, query, or fragment", EXIT_USAGE)
    if parts.scheme == "http" and not allow_http:
        _fail(
            "plain HTTP exposes credentials on the network; add --allow-http only on a trusted isolated LAN",
            EXIT_USAGE,
        )
    return candidate


def _redact(text: str, secrets: list[str]) -> str:
    result = text
    for secret in secrets:
        if secret:
            result = result.replace(secret, "[REDACTED]")
    return result


def _redact_value(value: Any, secrets: list[str]) -> Any:
    """Remove authentication material even if a peer reflects it in JSON."""
    if isinstance(value, dict):
        protected = {"authorization", "cookie", "password", "set-cookie", "token"}
        return {
            key: (
                "[REDACTED]"
                if str(key).lower() in protected
                else _redact_value(item, secrets)
            )
            for key, item in value.items()
        }
    if isinstance(value, list):
        return [_redact_value(item, secrets) for item in value]
    if isinstance(value, str):
        return _redact(value, secrets)
    return value


class AgentHttpClient:
    def __init__(
        self,
        base_url: str,
        *,
        timeout: float,
        ca_file: str | None,
        token: str | None = None,
        password: str | None = None,
    ) -> None:
        self.base_url = base_url
        self.timeout = timeout
        self.token = token or ""
        self.last_status = 0
        self._secrets = [value for value in (token, password) if value]
        self._context = (
            ssl.create_default_context(cafile=ca_file)
            if base_url.startswith("https://")
            else None
        )
        handlers: list[Any] = [_NoRedirect()]
        if self._context is not None:
            handlers.append(request.HTTPSHandler(context=self._context))
        self._opener = request.build_opener(*handlers)

    def _request(
        self,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
        *,
        authenticated: bool = True,
    ) -> Any:
        if not path.startswith("/"):
            _fail("internal error: API path must start with /")
        encoded = None
        headers = {"Accept": "application/json"}
        if body is not None:
            encoded = json.dumps(
                body, ensure_ascii=False, separators=(",", ":")
            ).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if authenticated:
            if not self.token:
                _fail("authentication token is unavailable")
            headers["Authorization"] = f"Bearer {self.token}"
        target = self.base_url + path
        call = request.Request(target, data=encoded, headers=headers, method=method)
        try:
            with self._opener.open(call, timeout=self.timeout) as response:
                payload = response.read()
                status_code = response.status
                self.last_status = status_code
                content_type = response.headers.get("Content-Type", "")
        except error.HTTPError as exc:
            payload = exc.read(MAX_ERROR_BODY_BYTES)
            if exc.code in {404, 409}:
                try:
                    structured = json.loads(payload)
                except (json.JSONDecodeError, UnicodeDecodeError):
                    structured = None
                if (
                    isinstance(structured, dict)
                    and structured.get("reason") in STRUCTURED_REJECT_REASONS
                ):
                    raise StructuredReject(structured, exc.code) from None
            detail = payload.decode("utf-8", "replace").strip()
            detail = _redact(detail, self._secrets)
            suffix = f": {detail}" if detail else ""
            _fail(f"HTTP {exc.code} for {method} {path}{suffix}")
        except error.URLError as exc:
            detail = _redact(str(exc.reason), self._secrets)
            _fail(f"cannot reach {method} {path}: {detail}")
        except TimeoutError:
            _fail(f"request timed out for {method} {path}", EXIT_TIMEOUT)
        if not payload:
            return {}
        if "json" not in content_type.lower() and not payload.lstrip().startswith(
            (b"{", b"[")
        ):
            text = _redact(payload.decode("utf-8", "replace"), self._secrets)
            _fail(f"HTTP {status_code} for {method} {path} returned non-JSON: {text}")
        try:
            return json.loads(payload)
        except json.JSONDecodeError:
            _fail(f"HTTP {status_code} for {method} {path} returned invalid JSON")

    def get(self, path: str) -> Any:
        return self._request("GET", path)

    def post(self, path: str, body: dict[str, Any]) -> Any:
        return self._request("POST", path, body)

    def login(self, username: str, password: str, deadline_seconds: float) -> None:
        login_body = {
            "username": username,
            "password": password,
            "request_id": uuid.uuid4().hex[:24],
        }
        result = self._request(
            "POST", "/api/auth/login", login_body, authenticated=False
        )
        deadline = time.monotonic() + deadline_seconds
        while isinstance(result, dict) and result.get("pending"):
            job_id = str(result.get("job_id") or "")
            if not job_id:
                _fail("login response is pending but has no job_id")
            if time.monotonic() >= deadline:
                _fail("login timed out", EXIT_TIMEOUT)
            poll_ms = max(50, min(2000, int(result.get("poll_after_ms") or 100)))
            time.sleep(poll_ms / 1000.0)
            query = parse.urlencode({"job_id": job_id})
            result = self._request(
                "GET", f"/api/auth/login/status?{query}", authenticated=False
            )
        token = result.get("token") if isinstance(result, dict) else None
        if not isinstance(token, str) or not token:
            _fail("login completed without an authentication token")
        self.token = token
        self._secrets.append(token)


def _query(path: str, values: dict[str, Any]) -> str:
    filtered = {key: value for key, value in values.items() if value is not None}
    return path + ("?" + parse.urlencode(filtered) if filtered else "")


def _assert_run(payload: Any, expected: str) -> None:
    if not isinstance(payload, dict):
        _fail("exact run response is not a JSON object", EXIT_REJECTED)
    observed: list[tuple[str, str]] = []
    for key in (
        "run_id",
        "job_id",
        "submitted_run_id",
        "controlled_run_id",
    ):
        if key not in payload:
            continue
        value = payload[key]
        if not isinstance(value, str) or not value:
            _fail(f"exact run response has an invalid {key}", EXIT_REJECTED)
        observed.append((key, value))
    if not observed:
        _fail("exact run response has no run identifier", EXIT_REJECTED)
    conflicts = [f"{key}={value}" for key, value in observed if value != expected]
    if conflicts:
        _fail(
            f"exact run mismatch: requested {expected}, device returned "
            + ", ".join(conflicts),
            EXIT_REJECTED,
        )


def _emit(value: Any, pretty: bool, secrets: list[str]) -> None:
    value = _redact_value(value, secrets)
    print(
        json.dumps(
            value,
            ensure_ascii=False,
            indent=2 if pretty else None,
            separators=None if pretty else (",", ":"),
            sort_keys=pretty,
        ),
        flush=True,
    )


def _semantic_result(payload: Any) -> int:
    if isinstance(payload, dict) and (
        payload.get("accepted") is False or payload.get("ok") is False
    ):
        return EXIT_REJECTED
    return 0


def _submit_contract_error(payload: Any, http_status: int) -> str | None:
    if not isinstance(payload, dict):
        return "response is not a JSON object"
    required_bools = (
        "accepted",
        "queued",
        "busy",
        "receipt_busy",
        "deduplicated",
    )
    for key in required_bools:
        if type(payload.get(key)) is not bool:
            return f"{key} must be present and boolean"
    submitted = payload.get("submitted_run_id")
    active = payload.get("active_run_id")
    if not isinstance(submitted, str) or not submitted:
        return "submitted_run_id must be present and non-empty"
    if len(submitted) > 48:
        return "submitted_run_id exceeds the firmware limit"
    if not isinstance(active, str):
        return "active_run_id must be present and a string"
    if payload.get("accepted") is not True:
        return "accepted must be true for an HTTP-success submission"
    if payload.get("queued") is not False:
        return "queued must be false; this API does not acknowledge a queue"

    receipt_busy = payload["receipt_busy"]
    deduplicated = payload["deduplicated"]
    if deduplicated:
        if http_status != 200:
            return "a deduplicated receipt must use HTTP 200"
        if receipt_busy and active != submitted:
            return "an active deduplicated receipt must identify the same run"
        if not receipt_busy and active:
            return "a terminal deduplicated receipt must not name an active run"
    else:
        if http_status != 202:
            return "a newly accepted submission must use HTTP 202"
        if receipt_busy:
            return "a newly accepted submission cannot be receipt-busy"
        if active:
            return "a newly accepted submission must not name a prior active run"
    for key in ("run_id", "job_id"):
        projected = payload.get(key)
        if projected not in (None, "", submitted):
            return f"{key} contradicts submitted_run_id"
    return None


def _control_contract_error(payload: Any, command: str) -> str | None:
    if not isinstance(payload, dict):
        return "control response is not a JSON object"
    if payload.get("accepted") is not True:
        return "control response must explicitly set accepted=true"
    expected_control = "abort" if command == "cancel" else command
    if payload.get("control") != expected_control:
        return f"control response must identify {expected_control}"
    return None


def _history_contract_error(
    payload: Any, expected_session_id: str, *, required: bool
) -> str | None:
    if not isinstance(payload, dict):
        return "history response is not a JSON object"
    returned = payload.get("session_id")
    if returned is None and not required:
        return None
    if not isinstance(returned, str) or not SESSION_ID_PATTERN.fullmatch(returned):
        return "history response has a missing or invalid session_id"
    if returned != expected_session_id:
        return "history response session_id does not match the requested session"
    return None


def _emit_contract_reject(
    client: AgentHttpClient,
    args: argparse.Namespace,
    payload: Any,
    detail: str,
) -> int:
    _emit(
        {
            "client_error": "invalid_api_response",
            "detail": detail,
            "response": payload,
        },
        args.pretty,
        client._secrets,
    )
    return EXIT_REJECTED


def _handle_watch(client: AgentHttpClient, args: argparse.Namespace) -> int:
    cursor = args.after_seq
    deadline = time.monotonic() + args.watch_timeout if args.watch_timeout else None
    last_status_key: tuple[Any, ...] | None = None
    last_requests: str | None = None
    while True:
        events = client.get(
            _query(
                "/api/agent/run/events",
                {"job_id": args.run_id, "after_seq": cursor},
            )
        )
        _assert_run(events, args.run_id)
        if isinstance(events, dict):
            action_items = events.get("events")
            task_items = events.get("task_events")
            for item in action_items if isinstance(action_items, list) else []:
                _emit({"type": "event", "event": item}, args.pretty, client._secrets)
            for item in task_items if isinstance(task_items, list) else []:
                _emit(
                    {"type": "task_event", "event": item},
                    args.pretty,
                    client._secrets,
                )
            next_cursor = events.get("next_after_seq", cursor)
            try:
                cursor = max(cursor, int(next_cursor))
            except (TypeError, ValueError):
                _fail("event response contained an invalid next_after_seq")

        status = client.get(
            _query("/api/agent/run/status", {"run_id": args.run_id})
        )
        _assert_run(status, args.run_id)
        if not isinstance(status, dict):
            _fail("run status response must be a JSON object")
        status_key = (
            status.get("state"),
            status.get("stage"),
            status.get("detail"),
            status.get("updated_ms"),
        )
        if status_key != last_status_key:
            _emit(
                {"type": "status", "status": status},
                args.pretty,
                client._secrets,
            )
            last_status_key = status_key

        if status.get("state") == "waiting_request":
            queued = client.get(
                _query("/api/agent/requests", {"run_id": args.run_id})
            )
            canonical = json.dumps(queued, ensure_ascii=False, sort_keys=True)
            if canonical != last_requests:
                _emit(
                    {"type": "requests", "requests": queued},
                    args.pretty,
                    client._secrets,
                )
                last_requests = canonical

        state = str(status.get("state") or "")
        if state in TERMINAL_STATES and state not in ACTIVE_STATES:
            return EXIT_REJECTED if state in FAILED_TERMINAL_STATES else 0
        if deadline is not None and time.monotonic() >= deadline:
            _emit(
                {
                    "type": "timeout",
                    "run_id": args.run_id,
                    "after_seq": cursor,
                },
                args.pretty,
                client._secrets,
            )
            return EXIT_TIMEOUT
        time.sleep(args.interval)


def _execute(client: AgentHttpClient, args: argparse.Namespace) -> int:
    command = args.command
    if command == "capabilities":
        result = client.get("/api/capabilities")
    elif command == "submit":
        message = _message_value(args.message, args.message_file, "message")
        body: dict[str, Any] = {
            "message": message,
            "dry_run": not args.execute,
            "authority_mode": "policy" if args.execute else "dry_run",
            "include_screenshot": args.screenshot,
            "include_web_search": args.web_search,
            "idempotency_key": args.idempotency_key or f"http-{uuid.uuid4().hex}",
        }
        for key, value in (
            ("session_id", args.session_id),
            ("profile", args.profile),
            ("model", args.model),
            ("completion_criteria", args.completion_criteria),
        ):
            if value is not None:
                body[key] = value
        if args.page_context_file:
            body["page_context"] = _load_json_object(
                args.page_context_file, "page context"
            )
        result = client.post("/api/agent/run", body)
        contract_error = _submit_contract_error(result, client.last_status)
        if contract_error:
            return _emit_contract_reject(client, args, result, contract_error)
    elif command == "status":
        result = client.get(
            _query("/api/agent/run/status", {"run_id": args.run_id})
        )
        _assert_run(result, args.run_id)
    elif command == "events":
        result = client.get(
            _query(
                "/api/agent/run/events",
                {"job_id": args.run_id, "after_seq": args.after_seq},
            )
        )
        _assert_run(result, args.run_id)
    elif command == "watch":
        return _handle_watch(client, args)
    elif command in {"pause", "resume", "abort", "cancel"}:
        result = client.post(f"/api/agent/run/{command}", {"run_id": args.run_id})
        _assert_run(result, args.run_id)
        contract_error = _control_contract_error(result, command)
        if contract_error:
            return _emit_contract_reject(
                client, args, result, contract_error
            )
    elif command == "steer":
        message = _message_value(args.message, args.message_file, "steer message")
        if len(message.encode("utf-8")) > 512:
            _fail("steer message exceeds the firmware 512-byte limit", EXIT_USAGE)
        result = client.post(
            "/api/agent/run/steer",
            {"run_id": args.run_id, "message": message},
        )
        _assert_run(result, args.run_id)
        contract_error = _control_contract_error(result, "steer")
        if contract_error:
            return _emit_contract_reject(
                client, args, result, contract_error
            )
    elif command == "history":
        if args.history_action == "list":
            result = client.get(
                _query("/api/agent/history", {"session_id": args.session_id})
            )
            contract_error = _history_contract_error(
                result, args.session_id, required=True
            )
            if contract_error:
                return _emit_contract_reject(
                    client, args, result, contract_error
                )
        elif args.history_action == "append":
            if args.record_file:
                body = _load_json_object(args.record_file, "history record")
                if (
                    "session_id" in body
                    and body["session_id"] != args.session_id
                ):
                    _fail(
                        "history record session_id must exactly match --session-id",
                        EXIT_USAGE,
                    )
                body["session_id"] = args.session_id
            else:
                content = _message_value(
                    args.content, args.content_file, "history content"
                )
                body = {
                    "kind": "message",
                    "session_id": args.session_id,
                    "role": args.role,
                    "content": content,
                }
            result = client.post("/api/agent/history", body)
            contract_error = _history_contract_error(
                result, args.session_id, required=False
            )
            if contract_error:
                return _emit_contract_reject(
                    client, args, result, contract_error
                )
        else:
            if args.confirm != "CLEAR_AGENT_HISTORY":
                _fail(
                    "history clear requires --confirm CLEAR_AGENT_HISTORY",
                    EXIT_USAGE,
                )
            result = client.post("/api/agent/history/clear", {})
    elif command == "sessions":
        if args.sessions_action == "list":
            result = client.get("/api/agent/sessions")
        elif args.sessions_action == "create":
            result = client.post("/api/agent/sessions", {"title": args.title})
        else:
            if args.confirm != args.session_id:
                _fail(
                    "session delete requires --confirm to exactly match --session-id",
                    EXIT_USAGE,
                )
            result = client.post(
                "/api/agent/sessions/delete", {"session_id": args.session_id}
            )
    elif command == "requests":
        if args.requests_action == "list":
            result = client.get(
                _query("/api/agent/requests", {"run_id": args.run_id})
            )
        elif args.requests_action == "approve":
            body = {
                "request_id": args.request_id,
                "decision": "allow_once",
                "grant_scope": "once",
            }
            if args.response is not None or args.response_file is not None:
                body["response"] = _message_value(
                    args.response, args.response_file, "request response"
                )
            result = client.post("/api/agent/requests/decision", body)
        else:
            result = client.post(
                "/api/agent/requests/cancel", {"request_id": args.request_id}
            )
    else:
        _fail(f"unsupported command: {command}", EXIT_USAGE)

    _emit(result, args.pretty, client._secrets)
    return _semantic_result(result)


def _add_run_id(parser: argparse.ArgumentParser, *, required: bool = True) -> None:
    parser.add_argument("--run-id", required=required, help="exact run identifier")


def _add_message_input(
    parser: argparse.ArgumentParser, label: str = "message"
) -> None:
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--message", help=f"{label} text")
    group.add_argument("--message-file", help=f"UTF-8 file containing the {label}")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Safely drive the ExoAnchor embedded Agent over HTTP"
    )
    parser.add_argument("--base-url", required=True, help="device origin, without a path")
    credentials = parser.add_mutually_exclusive_group(required=True)
    credentials.add_argument("--password-file", help="mode-0600 file with the account password")
    credentials.add_argument("--token-file", help="mode-0600 file with a bearer token")
    parser.add_argument("--username", default="admin", help="login account (default: admin)")
    parser.add_argument(
        "--allow-http",
        action="store_true",
        help="explicitly allow plaintext HTTP on a trusted isolated LAN",
    )
    parser.add_argument("--ca-file", help="custom CA bundle for HTTPS")
    parser.add_argument("--timeout", type=float, default=8.0, help="per-request timeout seconds")
    parser.add_argument(
        "--login-timeout", type=float, default=30.0, help="async login deadline seconds"
    )
    parser.add_argument("--pretty", action="store_true", help="pretty-print JSON")
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("capabilities", help="read /api/capabilities")

    submit = commands.add_parser("submit", help="submit an Agent run")
    _add_message_input(submit)
    submit.add_argument("--session-id")
    submit.add_argument("--profile")
    submit.add_argument("--model")
    submit.add_argument("--completion-criteria")
    submit.add_argument("--idempotency-key")
    submit.add_argument("--page-context-file", help="JSON object file")
    submit.add_argument("--execute", action="store_true", help="request policy-authorized execution; default is dry-run")
    submit.add_argument("--screenshot", action="store_true")
    submit.add_argument("--web-search", action="store_true")

    status = commands.add_parser("status", help="read one exact run snapshot")
    _add_run_id(status)

    events = commands.add_parser("events", help="read current-run events after a cursor")
    _add_run_id(events)
    events.add_argument("--after-seq", type=int, default=0)

    watch = commands.add_parser("watch", help="stream JSONL events until a terminal state")
    _add_run_id(watch)
    watch.add_argument("--after-seq", type=int, default=0)
    watch.add_argument("--interval", type=float, default=0.5)
    watch.add_argument("--watch-timeout", type=float, default=300.0, help="0 disables the watch deadline")

    for name in ("pause", "resume", "abort", "cancel"):
        control = commands.add_parser(name, help=f"{name} an exact run")
        _add_run_id(control)

    steer = commands.add_parser("steer", help="merge a new instruction at a safe checkpoint")
    _add_run_id(steer)
    _add_message_input(steer, "steer message")

    history = commands.add_parser("history", help="read or manage conversation history")
    history_commands = history.add_subparsers(dest="history_action", required=True)
    history_list = history_commands.add_parser("list")
    history_list.add_argument("--session-id", required=True)
    history_append = history_commands.add_parser("append")
    history_append.add_argument("--session-id", required=True)
    record_source = history_append.add_mutually_exclusive_group(required=True)
    record_source.add_argument("--record-file", help="complete JSON history record")
    record_source.add_argument("--content", help="message content")
    record_source.add_argument("--content-file", help="UTF-8 message content file")
    history_append.add_argument(
        "--role", choices=("user", "assistant", "system"), default="system"
    )
    history_clear = history_commands.add_parser("clear")
    history_clear.add_argument("--confirm", required=True)

    sessions = commands.add_parser("sessions", help="list or manage Agent sessions")
    session_commands = sessions.add_subparsers(dest="sessions_action", required=True)
    session_commands.add_parser("list")
    session_create = session_commands.add_parser("create")
    session_create.add_argument("--title", default="新会话")
    session_delete = session_commands.add_parser("delete")
    session_delete.add_argument("--session-id", required=True)
    session_delete.add_argument("--confirm", required=True)

    requests_command = commands.add_parser("requests", help="inspect or decide device-owned requests")
    request_commands = requests_command.add_subparsers(dest="requests_action", required=True)
    request_list = request_commands.add_parser("list")
    request_list.add_argument("--run-id")
    request_approve = request_commands.add_parser("approve")
    request_approve.add_argument("--request-id", required=True)
    response_source = request_approve.add_mutually_exclusive_group()
    response_source.add_argument("--response")
    response_source.add_argument("--response-file")
    request_cancel = request_commands.add_parser("cancel")
    request_cancel.add_argument("--request-id", required=True)
    return parser


def _validate_args(args: argparse.Namespace) -> None:
    if args.timeout <= 0 or args.login_timeout <= 0:
        _fail("timeouts must be positive", EXIT_USAGE)
    if args.command in {"events", "watch"} and args.after_seq < 0:
        _fail("--after-seq must not be negative", EXIT_USAGE)
    if args.command == "watch":
        if args.interval < 0.05:
            _fail("--interval must be at least 0.05 seconds", EXIT_USAGE)
        if args.watch_timeout < 0:
            _fail("--watch-timeout must not be negative", EXIT_USAGE)
    session_id = getattr(args, "session_id", None)
    if session_id is not None and not SESSION_ID_PATTERN.fullmatch(session_id):
        _fail(
            "invalid session id (use 1..48 ASCII letters, digits, _ or -)",
            EXIT_USAGE,
        )
    for value, label in (
        (getattr(args, "run_id", None), "run id"),
        (getattr(args, "request_id", None), "request id"),
    ):
        if value is not None and (not value or not re.fullmatch(r"[A-Za-z0-9._:-]+", value)):
            _fail(f"invalid {label}", EXIT_USAGE)


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    client: AgentHttpClient | None = None
    try:
        _validate_args(args)
        base_url = _normalize_base_url(args.base_url, args.allow_http)
        password = (
            _read_private_file(args.password_file, "password")
            if args.password_file
            else None
        )
        token = (
            _read_private_file(args.token_file, "token")
            if args.token_file
            else None
        )
        if token is not None and re.search(r"\s", token):
            _fail("token file must contain one bearer token without whitespace")
        client = AgentHttpClient(
            base_url,
            timeout=args.timeout,
            ca_file=args.ca_file,
            token=token,
            password=password,
        )
        if password is not None:
            client.login(args.username, password, args.login_timeout)
        return _execute(client, args)
    except StructuredReject as exc:
        _emit(
            exc.payload,
            args.pretty,
            client._secrets if client is not None else [],
        )
        return EXIT_REJECTED
    except CliError as exc:
        print(f"agent-runtime-http: {exc}", file=sys.stderr)
        return exc.exit_code
    except KeyboardInterrupt:
        print("agent-runtime-http: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
