"""Bounded SSH job lifecycle with a local, sanitized audit journal."""

from __future__ import annotations

import hashlib
import json
import os
import threading
import uuid
from concurrent.futures import Future, ThreadPoolExecutor
from pathlib import Path
from typing import Any, Callable

from .observations import utc_now


JSON = dict[str, Any]
JobRunner = Callable[[], JSON]
TERMINAL_STATES = {"succeeded", "failed", "cancelled", "interrupted"}


class JobNotFoundError(KeyError):
    pass


class JobConflictError(ValueError):
    pass


class JobManager:
    """Runs a small number of bounded jobs and journals honest recovery state.

    A running HTTP request cannot be force-cancelled safely. Cancellation is
    therefore cooperative: queued work is cancelled, while in-flight work is
    marked ``cancel_requested`` and its eventual result remains queryable.
    """

    def __init__(self, state_dir: str, *, persist_output: bool = False,
                 max_workers: int = 2):
        self.state_dir = Path(state_dir).expanduser()
        self.persist_output = persist_output
        self._executor = ThreadPoolExecutor(
            max_workers=max_workers,
            thread_name_prefix="exoanchor-ssh-job",
        )
        self._lock = threading.RLock()
        self._jobs: dict[str, JSON] = {}
        self._futures: dict[str, Future[Any]] = {}
        self._idempotency: dict[str, str] = {}
        self._load_journal()

    def _ensure_state_dir(self) -> None:
        self.state_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        try:
            os.chmod(self.state_dir, 0o700)
        except OSError:
            pass

    def _path(self, job_id: str) -> Path:
        return self.state_dir / f"{job_id}.json"

    @staticmethod
    def _public(record: JSON, *, include_result: bool = False) -> JSON:
        public = {key: value for key, value in record.items()
                  if key not in {"result", "error_detail"}}
        result = record.get("result")
        if isinstance(result, dict):
            output = result.get("output", "")
            public["artifact"] = {
                "available": True,
                "bytes": len(output.encode("utf-8")) if isinstance(output, str) else 0,
                "sha256": hashlib.sha256(
                    output.encode("utf-8") if isinstance(output, str) else b""
                ).hexdigest(),
                "truncated_by_device": bool(result.get("truncated")),
            }
            public["exit_status"] = result.get("exit_status")
            public["remote_ok"] = result.get("ok")
        if include_result and isinstance(result, dict):
            public["result"] = result
        if record.get("error_detail"):
            public["error"] = record["error_detail"]
        return public

    def _persist(self, record: JSON) -> None:
        self._ensure_state_dir()
        persisted = self._public(record, include_result=self.persist_output)
        persisted["output_persisted"] = self.persist_output
        path = self._path(record["job_id"])
        temporary = path.with_suffix(".tmp")
        temporary.write_text(
            json.dumps(persisted, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        try:
            os.chmod(temporary, 0o600)
        except OSError:
            pass
        temporary.replace(path)

    def _load_journal(self) -> None:
        if not self.state_dir.is_dir():
            return
        for path in sorted(self.state_dir.glob("job_*.json")):
            try:
                record = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                continue
            job_id = record.get("job_id")
            if not isinstance(job_id, str):
                continue
            if record.get("state") not in TERMINAL_STATES:
                record["state"] = "interrupted"
                record["finished_at"] = utc_now()
                record["error_detail"] = (
                    "MCP bridge restarted while the request was active; remote completion is unknown"
                )
            self._jobs[job_id] = record
            key = record.get("idempotency_key")
            if isinstance(key, str) and key:
                self._idempotency[key] = job_id
            if record.get("state") == "interrupted":
                self._persist(record)

    def start(self, request: JSON, runner: JobRunner, *,
              idempotency_key: str | None = None,
              audit_id: str | None = None) -> tuple[JSON, bool]:
        with self._lock:
            request_sha256 = hashlib.sha256(
                json.dumps(request, ensure_ascii=False, sort_keys=True,
                           separators=(",", ":")).encode("utf-8")
            ).hexdigest()
            if idempotency_key and idempotency_key in self._idempotency:
                existing = self._jobs[self._idempotency[idempotency_key]]
                if existing.get("request_sha256") != request_sha256:
                    raise JobConflictError(
                        "idempotency key was already used for a different SSH request"
                    )
                return self._public(existing), True
            job_id = f"job_{uuid.uuid4().hex[:24]}"
            record: JSON = {
                "job_id": job_id,
                "audit_id": audit_id or f"audit_{uuid.uuid4().hex[:24]}",
                "idempotency_key": idempotency_key,
                "state": "queued",
                "created_at": utc_now(),
                "started_at": None,
                "finished_at": None,
                "cancel_requested": False,
                "cancellation": "cooperative_only",
                "request": request,
                "request_sha256": request_sha256,
                "result": None,
                "error_detail": None,
                "output_persisted": self.persist_output,
            }
            self._jobs[job_id] = record
            if idempotency_key:
                self._idempotency[idempotency_key] = job_id
            self._persist(record)
            future = self._executor.submit(self._run, job_id, runner)
            self._futures[job_id] = future
            return self._public(record), False

    def _run(self, job_id: str, runner: JobRunner) -> None:
        with self._lock:
            record = self._jobs[job_id]
            if record["cancel_requested"]:
                record["state"] = "cancelled"
                record["finished_at"] = utc_now()
                self._persist(record)
                return
            record["state"] = "running"
            record["started_at"] = utc_now()
            self._persist(record)
        try:
            result = runner()
        except Exception as exc:  # Runner errors are captured for later polling.
            with self._lock:
                record = self._jobs[job_id]
                record["state"] = "failed"
                record["error_detail"] = str(exc)
                record["finished_at"] = utc_now()
                self._persist(record)
            return
        with self._lock:
            record = self._jobs[job_id]
            record["result"] = result
            record["state"] = "succeeded" if result.get("ok") is True else "failed"
            if record["state"] == "failed" and not record.get("error_detail"):
                record["error_detail"] = str(result.get("error") or "remote command failed")
            record["finished_at"] = utc_now()
            self._persist(record)

    def status(self, job_id: str) -> JSON:
        with self._lock:
            record = self._jobs.get(job_id)
            if record is None:
                raise JobNotFoundError(job_id)
            return self._public(record)

    def cancel(self, job_id: str) -> JSON:
        with self._lock:
            record = self._jobs.get(job_id)
            if record is None:
                raise JobNotFoundError(job_id)
            if record["state"] in TERMINAL_STATES:
                return self._public(record)
            record["cancel_requested"] = True
            future = self._futures.get(job_id)
            if future is not None and future.cancel():
                record["state"] = "cancelled"
                record["finished_at"] = utc_now()
            else:
                record["state"] = "cancel_requested"
            self._persist(record)
            return self._public(record)

    def result(self, job_id: str, *, offset: int = 0,
               limit: int = 4096) -> JSON:
        with self._lock:
            record = self._jobs.get(job_id)
            if record is None:
                raise JobNotFoundError(job_id)
            public = self._public(record)
            result = record.get("result")
            if not isinstance(result, dict):
                public["result_available"] = False
                return public
            output = result.get("output", "")
            if not isinstance(output, str):
                output = str(output)
            chunk = output[offset:offset + limit]
            next_offset = offset + len(chunk)
            public.update({
                "result_available": True,
                "result": {key: value for key, value in result.items() if key != "output"},
                "output": chunk,
                "output_offset": offset,
                "next_offset": next_offset if next_offset < len(output) else None,
                "output_complete": next_offset >= len(output),
                "output_total_characters": len(output),
            })
            return public
