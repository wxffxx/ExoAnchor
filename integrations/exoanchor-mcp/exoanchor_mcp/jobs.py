"""Bounded SSH job lifecycle with a local, sanitized audit journal."""

from __future__ import annotations

import hashlib
import json
import logging
import os
import threading
import time
import uuid
from copy import deepcopy
from concurrent.futures import Future, ThreadPoolExecutor
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Callable

from .observations import utc_now


JSON = dict[str, Any]
LOGGER = logging.getLogger(__name__)
JobRunner = Callable[[], JSON]
TERMINAL_STATES = {"succeeded", "failed", "cancelled", "interrupted"}
OPS_TERMINAL_STATES = {
    "succeeded", "no_change", "degraded", "failed", "blocked",
    "cancelled", "interrupted",
}


class JobNotFoundError(KeyError):
    pass


class JobConflictError(ValueError):
    pass


def _utc_datetime(value: str | None = None) -> datetime:
    if value is None:
        return datetime.now(timezone.utc)
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise ValueError("schedule timestamp must include a timezone")
    return parsed.astimezone(timezone.utc)


def _future_utc(seconds: int, *, base: str | None = None) -> str:
    value = _utc_datetime(base) + timedelta(seconds=seconds)
    return value.isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _read_record(path: Path, identity_key: str) -> JSON | None:
    """Ignore damaged records without rewriting them or guessing identity."""
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, RecursionError):
        return None
    if (not isinstance(record, dict)
            or record.get(identity_key) != path.stem
            or not isinstance(record.get("state"), str)):
        return None
    return record


def _valid_ops_definition(job: JSON) -> bool:
    """Require executable fields without discarding older optional metadata."""
    generation, target, trigger, policy = (
        job.get(key) for key in ("generation", "target", "trigger", "policy")
    )
    if (type(generation) is not int or generation < 1
            or not isinstance(target, dict)
            or not isinstance(trigger, dict)
            or not isinstance(policy, dict)):
        return False
    device_id = target.get("device_id")
    if not isinstance(device_id, str) or not device_id.strip():
        return False
    if trigger.get("type") == "manual":
        return True
    interval = trigger.get("interval_seconds")
    return (trigger.get("type") == "interval" and type(interval) is int
            and 60 <= interval <= 604800)


def _persist_outcome(record: JSON, persist: Callable[[JSON], None]) -> None:
    """Keep the observed outcome distinct from a failure to save its journal."""
    try:
        persist(record)
    except OSError as exc:
        record["journal_error"] = str(exc)


class JobManager:
    """Runs a small number of bounded jobs and journals honest recovery state.

    A running HTTP request cannot be force-cancelled safely. Cancellation is
    therefore cooperative: queued work is cancelled, while in-flight work is
    marked ``cancel_requested`` and its eventual result remains queryable.
    """

    def __init__(self, state_dir: str, *, persist_output: bool = False,
                 max_workers: int = 2, max_pending: int = 32):
        if not isinstance(max_pending, int) or max_pending < 0:
            raise ValueError("max_pending must be a non-negative integer")
        self.state_dir = Path(state_dir).expanduser()
        self.persist_output = persist_output
        self._executor = ThreadPoolExecutor(
            max_workers=max_workers,
            thread_name_prefix="exoanchor-ssh-job",
        )
        self._lock = threading.RLock()
        self._closed = False
        self._capacity = max_workers + max_pending
        self._jobs: dict[str, JSON] = {}
        self._futures: dict[str, Future[Any]] = {}
        self._idempotency: dict[str, str] = {}
        self._load_journal()

    def close(self) -> None:
        """Drain accepted work before its journal directory is released."""
        with self._lock:
            self._closed = True
        self._executor.shutdown(wait=True)

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
        public = {key: deepcopy(value) for key, value in record.items()
                  if key not in {"result", "error_detail"}}
        result = record.get("result")
        if isinstance(result, dict):
            output = result.get("output", "")
            coerced_output = not isinstance(output, str)
            if coerced_output:
                output = str(output)
            if coerced_output or not isinstance(record.get("artifact"), dict):
                artifact: JSON = {
                    "available": True,
                    "bytes": None,
                    "sha256": None,
                    "truncated_by_device": bool(result.get("truncated")),
                }
                try:
                    encoded = output.encode("utf-8")
                except UnicodeEncodeError:
                    # Retain the exact JSON text and observed remote outcome;
                    # inventing replacement bytes would misrepresent evidence.
                    artifact["encoding_error"] = "output is not valid Unicode; UTF-8 hash unavailable"
                else:
                    artifact["bytes"] = len(encoded)
                    artifact["sha256"] = hashlib.sha256(encoded).hexdigest()
                record["artifact"] = artifact
            public["artifact"] = dict(record["artifact"])
            public["exit_status"] = result.get("exit_status")
            public["remote_ok"] = result.get("ok")
        if include_result and isinstance(result, dict):
            public["result"] = deepcopy(result)
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
            json.dumps(persisted, ensure_ascii=True, indent=2) + "\n",
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
            record = _read_record(path, "job_id")
            if record is None:
                continue
            job_id = record["job_id"]
            if record.get("state") not in TERMINAL_STATES:
                record["state"] = "interrupted"
                record["finished_at"] = utc_now()
                record["error_detail"] = (
                    "MCP bridge restarted while the request was active; remote completion is unknown"
                )
                _persist_outcome(record, self._persist)
            self._jobs[job_id] = record
            key = record.get("idempotency_key")
            if isinstance(key, str) and key:
                self._idempotency[key] = job_id

    def start(self, request: JSON, runner: JobRunner, *,
              idempotency_key: str | None = None,
              audit_id: str | None = None) -> tuple[JSON, bool]:
        with self._lock:
            if self._closed:
                raise JobConflictError("SSH job manager is closed")
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
            if len(self._futures) >= self._capacity:
                raise JobConflictError("SSH job capacity reached; retry after an active job finishes")
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
                "request": deepcopy(request),
                "request_sha256": request_sha256,
                "result": None,
                "error_detail": None,
                "output_persisted": self.persist_output,
            }
            self._persist(record)
            self._jobs[job_id] = record
            if idempotency_key:
                self._idempotency[idempotency_key] = job_id
            future = self._executor.submit(self._run, job_id, runner)
            self._futures[job_id] = future
            future.add_done_callback(lambda _future: self._forget_future(job_id))
            return self._public(record), False

    def _forget_future(self, job_id: str) -> None:
        with self._lock:
            self._futures.pop(job_id, None)

    def _run(self, job_id: str, runner: JobRunner) -> None:
        try:
            with self._lock:
                record = self._jobs[job_id]
                if record["cancel_requested"]:
                    record["state"] = "cancelled"
                    record["finished_at"] = utc_now()
                    _persist_outcome(record, self._persist)
                    return
                record["state"] = "running"
                record["started_at"] = utc_now()
                self._persist(record)
            result = runner()
            if not isinstance(result, dict):
                raise RuntimeError("SSH runner returned a non-object result")
            result = deepcopy(result)
        except Exception as exc:  # Runner errors are captured for later polling.
            with self._lock:
                record = self._jobs[job_id]
                record["state"] = "failed"
                record["error_detail"] = str(exc)
                record["finished_at"] = utc_now()
                _persist_outcome(record, self._persist)
            return
        with self._lock:
            record = self._jobs[job_id]
            record["result"] = result
            record["state"] = "succeeded" if result.get("ok") is True else "failed"
            if record["state"] == "failed" and not record.get("error_detail"):
                record["error_detail"] = str(result.get("error") or "remote command failed")
            record["finished_at"] = utc_now()
            _persist_outcome(record, self._persist)

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
            # Future.cancel() completes callbacks immediately but leaves its
            # work item in ThreadPoolExecutor's queue. Keep it accounted for
            # until a worker dequeues it and observes cancel_requested.
            if future is not None and not future.running():
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
                "result": {key: deepcopy(value) for key, value in result.items() if key != "output"},
                "output": chunk,
                "output_offset": offset,
                "next_offset": next_offset if next_offset < len(output) else None,
                "output_complete": next_offset >= len(output),
                "output_total_characters": len(output),
            })
            return public


OpsRunner = Callable[[JSON], JSON]


class OpsJobManager:
    """Durable, read-only operations jobs owned by the MCP control host.

    The first product profile does not embed an Agent runtime in Stable
    firmware.  This manager therefore owns the minimum persistent operations
    slice on the control host while the device remains the source of truth for
    observations.  A definition (Job), one trigger (Job Run), and every
    execution try (Attempt) are separate journal records.
    """

    def __init__(self, state_dir: str, runner: OpsRunner, *,
                 scheduler: bool = True, max_workers: int = 1,
                 max_pending: int = 32):
        if not isinstance(max_pending, int) or max_pending < 0:
            raise ValueError("max_pending must be a non-negative integer")
        self.root = Path(state_dir).expanduser() / "operations"
        self.jobs_dir = self.root / "jobs"
        self.runs_dir = self.root / "runs"
        self.runner = runner
        self._executor = ThreadPoolExecutor(
            max_workers=max_workers,
            thread_name_prefix="exoanchor-ops-run",
        )
        self._lock = threading.RLock()
        self._closed = False
        self._capacity = max_workers + max_pending
        self._jobs: dict[str, JSON] = {}
        self._runs: dict[str, JSON] = {}
        self._active_runs: dict[str, str] = {}
        self._schedule_retry_at: dict[str, float] = {}
        self._futures: dict[str, Future[Any]] = {}
        self._idempotency: dict[str, str] = {}
        self._stop = threading.Event()
        self._load_journal()
        self._scheduler: threading.Thread | None = None
        if scheduler:
            self._scheduler = threading.Thread(
                target=self._scheduler_loop,
                name="exoanchor-ops-scheduler",
                daemon=True,
            )
            self._scheduler.start()

    def close(self) -> None:
        self._stop.set()
        failure = None
        try:
            with self._lock:
                self._closed = True
                # Journal cancellation before shutdown. Even if storage fails,
                # continue cancelling the remaining queued observations.
                for run_id, future in list(self._futures.items()):
                    if not future.running():
                        try:
                            self.cancel_run(run_id)
                        except Exception as exc:
                            if failure is None:
                                failure = exc
        finally:
            if self._scheduler and self._scheduler.is_alive():
                self._scheduler.join(timeout=2)
            self._executor.shutdown(wait=True)
        if failure is not None:
            raise failure

    def _ensure_dirs(self) -> None:
        for path in (self.root, self.jobs_dir, self.runs_dir):
            path.mkdir(mode=0o700, parents=True, exist_ok=True)
            try:
                os.chmod(path, 0o700)
            except OSError:
                pass

    @staticmethod
    def _atomic_json(path: Path, record: JSON) -> None:
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(
            json.dumps(record, ensure_ascii=True, indent=2) + "\n",
            encoding="utf-8",
        )
        try:
            os.chmod(temporary, 0o600)
        except OSError:
            pass
        temporary.replace(path)

    def _persist_job(self, job: JSON) -> None:
        self._ensure_dirs()
        self._atomic_json(self.jobs_dir / f"{job['job_id']}.json", job)

    def _persist_run(self, run: JSON) -> None:
        self._ensure_dirs()
        self._atomic_json(self.runs_dir / f"{run['job_run_id']}.json", run)

    def _load_journal(self) -> None:
        if self.jobs_dir.is_dir():
            for path in sorted(self.jobs_dir.glob("job_*.json")):
                job = _read_record(path, "job_id")
                if job is None or not _valid_ops_definition(job):
                    continue
                job_id = job["job_id"]
                self._jobs[job_id] = job
                key = job.get("idempotency_key")
                if isinstance(key, str) and key:
                    self._idempotency[key] = job_id
        if self.runs_dir.is_dir():
            for path in sorted(self.runs_dir.glob("run_*.json")):
                run = _read_record(path, "job_run_id")
                if run is None:
                    continue
                run_id = run["job_run_id"]
                if run.get("state") not in OPS_TERMINAL_STATES:
                    finished = utc_now()
                    run["state"] = "interrupted"
                    run["phase"] = "needs_attention"
                    run["outcome"] = "interrupted"
                    run["finished_at"] = finished
                    run["finding"] = {
                        "severity": "warning",
                        "classification": "uncertain_commit",
                        "message": (
                            "MCP bridge restarted during this Run; no action is "
                            "replayed until the device is observed again"
                        ),
                    }
                    attempts = run.get("attempts")
                    if isinstance(attempts, list) and attempts:
                        attempt = attempts[-1]
                        if isinstance(attempt, dict) and attempt.get("state") not in OPS_TERMINAL_STATES:
                            attempt["state"] = "interrupted"
                            attempt["finished_at"] = finished
                            attempt["failure_class"] = "uncertain_commit"
                    _persist_outcome(run, self._persist_run)
                self._runs[run_id] = run

    @staticmethod
    def _job_public(job: JSON) -> JSON:
        return deepcopy(job)

    @staticmethod
    def _run_public(run: JSON) -> JSON:
        return deepcopy(run)

    def create(self, *, title: str, device_id: str,
               interval_seconds: int | None = None,
               idempotency_key: str | None = None) -> tuple[JSON, bool]:
        title = title.strip()
        device_id = device_id.strip()
        if not title or len(title) > 120:
            raise JobConflictError("operations job title must be 1 to 120 characters")
        if not device_id or len(device_id) > 128:
            raise JobConflictError("operations job requires an exact device_id")
        if interval_seconds is not None and (
                type(interval_seconds) is not int or not 60 <= interval_seconds <= 604800):
            raise JobConflictError("interval_seconds must be between 60 and 604800")
        fingerprint = hashlib.sha256(json.dumps({
            "title": title,
            "device_id": device_id,
            "interval_seconds": interval_seconds,
        }, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()
        with self._lock:
            if self._closed:
                raise JobConflictError("operations job manager is closed")
            if idempotency_key and idempotency_key in self._idempotency:
                job = self._jobs[self._idempotency[idempotency_key]]
                if job.get("definition_sha256") != fingerprint:
                    raise JobConflictError(
                        "idempotency key was already used for another operations job"
                    )
                return self._job_public(job), True
            now = utc_now()
            job_id = f"job_{uuid.uuid4().hex[:24]}"
            job: JSON = {
                "schema": "exoanchor.ops.job.v1",
                "job_id": job_id,
                "generation": 1,
                "title": title,
                "kind": "device_health",
                "state": "active",
                "target": {"device_id": device_id},
                "trigger": {
                    "type": "interval" if interval_seconds else "manual",
                    "interval_seconds": interval_seconds,
                },
                "policy": {
                    "concurrency": "forbid",
                    "read_only": True,
                    "max_attempts": 1,
                    "missed_run": "coalesce",
                },
                "created_at": now,
                "updated_at": now,
                "last_run_id": None,
                "next_run_at": (
                    _future_utc(interval_seconds, base=now)
                    if interval_seconds else None
                ),
                "idempotency_key": idempotency_key,
                "definition_sha256": fingerprint,
            }
            self._persist_job(job)
            self._jobs[job_id] = job
            if idempotency_key:
                self._idempotency[idempotency_key] = job_id
            return self._job_public(job), False

    def list(self, *, state: str | None = None) -> list[JSON]:
        with self._lock:
            jobs = [job for job in self._jobs.values()
                    if state is None or job.get("state") == state]
            jobs.sort(key=lambda item: str(item.get("created_at", "")))
            return [self._job_public(job) for job in jobs]

    def status(self, job_id: str) -> JSON:
        with self._lock:
            job = self._jobs.get(job_id)
            if job is None:
                raise JobNotFoundError(job_id)
            value = self._job_public(job)
            run_id = job.get("last_run_id")
            if isinstance(run_id, str) and run_id in self._runs:
                value["last_run"] = self._run_public(self._runs[run_id])
            return value

    def set_paused(self, job_id: str, paused: bool) -> JSON:
        with self._lock:
            if self._closed:
                raise JobConflictError("operations job manager is closed")
            job = self._jobs.get(job_id)
            if job is None:
                raise JobNotFoundError(job_id)
            if job.get("state") == "retired":
                raise JobConflictError("retired operations job cannot be resumed")
            updated = self._job_public(job)
            updated["state"] = "paused" if paused else "active"
            updated["updated_at"] = utc_now()
            interval = updated.get("trigger", {}).get("interval_seconds")
            if not paused and isinstance(interval, int):
                updated["next_run_at"] = _future_utc(interval)
            self._persist_job(updated)
            self._jobs[job_id] = updated
            self._schedule_retry_at.pop(job_id, None)
            return self._job_public(updated)

    def _active_run_locked(self, job_id: str) -> JSON | None:
        run_id = self._active_runs.get(job_id)
        if run_id is not None:
            run = self._runs[run_id]
            if run.get("state") not in OPS_TERMINAL_STATES:
                return run
        return None

    def start_run(self, job_id: str, *, trigger: str = "manual") -> tuple[JSON, bool]:
        with self._lock:
            if self._closed:
                raise JobConflictError("operations job manager is closed")
            job = self._jobs.get(job_id)
            if job is None:
                raise JobNotFoundError(job_id)
            if job.get("state") != "active":
                raise JobConflictError("operations job is not active")
            active = self._active_run_locked(job_id)
            if active is not None:
                return self._run_public(active), True
            if len(self._futures) >= self._capacity:
                raise JobConflictError("operations run capacity reached; retry after an active run finishes")
            now = utc_now()
            run_id = f"run_{uuid.uuid4().hex[:24]}"
            attempt_id = f"attempt_{uuid.uuid4().hex[:20]}"
            run: JSON = {
                "schema": "exoanchor.ops.run.v1",
                "job_run_id": run_id,
                "job_id": job_id,
                "generation": job["generation"],
                "trigger": trigger,
                "state": "queued",
                "phase": "queued",
                "outcome": None,
                "created_at": now,
                "started_at": None,
                "finished_at": None,
                "target": self._job_public(job["target"]),
                "policy": self._job_public(job["policy"]),
                "attempts": [{
                    "attempt_id": attempt_id,
                    "number": 1,
                    "state": "queued",
                    "started_at": None,
                    "finished_at": None,
                    "failure_class": None,
                }],
                "finding": None,
                "evidence": None,
                "cancel_requested": False,
            }
            updated = self._job_public(job)
            updated["last_run_id"] = run_id
            updated["updated_at"] = now
            interval = job.get("trigger", {}).get("interval_seconds")
            if isinstance(interval, int):
                updated["next_run_at"] = _future_utc(interval, base=now)
            self._persist_run(run)
            self._runs[run_id] = run
            try:
                self._persist_job(updated)
            except OSError as exc:
                # No runner has been submitted. Preserve that fact even when
                # only the first half of the journal update could be written.
                run["state"] = run["outcome"] = "blocked"
                run["phase"] = "needs_attention"
                run["finished_at"] = utc_now()
                run["finding"] = {
                    "severity": "warning",
                    "classification": "storage",
                    "message": "Run was not submitted because its job journal could not be saved",
                }
                run["attempts"][-1].update({
                    "state": "blocked", "finished_at": run["finished_at"],
                    "failure_class": "storage",
                })
                # If this write also fails, a restart conservatively marks the
                # older queued journal interrupted; no work is replayed.
                _persist_outcome(run, self._persist_run)
                raise OSError(f"could not queue {run_id}: {exc}") from exc
            self._jobs[job_id] = updated
            self._schedule_retry_at.pop(job_id, None)
            self._active_runs[job_id] = run_id
            future = self._executor.submit(self._run, run_id)
            self._futures[run_id] = future
            future.add_done_callback(lambda _future: self._forget_run(job_id, run_id))
            return self._run_public(run), False

    def _forget_run(self, job_id: str, run_id: str) -> None:
        with self._lock:
            self._futures.pop(run_id, None)
            if self._active_runs.get(job_id) == run_id:
                self._active_runs.pop(job_id, None)

    def _run(self, run_id: str) -> None:
        preflight_complete = False
        try:
            with self._lock:
                run = self._runs[run_id]
                attempt = run["attempts"][-1]
                if run.get("cancel_requested"):
                    run["state"] = "cancelled"
                    run["phase"] = "completed"
                    run["outcome"] = "cancelled"
                    run["finished_at"] = utc_now()
                    attempt["state"] = "cancelled"
                    attempt["finished_at"] = run["finished_at"]
                    _persist_outcome(run, self._persist_run)
                    return
                now = utc_now()
                run["state"] = "running"
                run["phase"] = "preflight"
                run["started_at"] = now
                attempt["state"] = "running"
                attempt["started_at"] = now
                job = self._job_public(self._jobs[run["job_id"]])
                self._persist_run(run)
                preflight_complete = True
            result = self.runner(job)
            if not isinstance(result, dict):
                raise RuntimeError("operations runner returned a non-object result")
            result = deepcopy(result)
        except Exception as exc:
            with self._lock:
                failure_class = (
                    "storage" if isinstance(exc, OSError) and not preflight_complete else "transient"
                )
                run = self._runs[run_id]
                attempt = run["attempts"][-1]
                finished = utc_now()
                run["state"] = "failed"
                run["phase"] = "needs_attention"
                run["outcome"] = "failed"
                run["finished_at"] = finished
                run["finding"] = {
                    "severity": "warning",
                    "classification": failure_class,
                    "message": str(exc),
                }
                attempt["state"] = "failed"
                attempt["finished_at"] = finished
                attempt["failure_class"] = failure_class
                _persist_outcome(run, self._persist_run)
            return
        with self._lock:
            run = self._runs[run_id]
            attempt = run["attempts"][-1]
            finished = utc_now()
            outcome = str(result.get("outcome") or (
                "no_change" if result.get("ok") is True else "degraded"
            ))
            if outcome not in OPS_TERMINAL_STATES:
                outcome = "failed"
            run["phase"] = "completed" if outcome in {"succeeded", "no_change"} else "needs_attention"
            run["state"] = outcome
            run["outcome"] = outcome
            run["finished_at"] = finished
            run["finding"] = result.get("finding")
            run["evidence"] = result.get("evidence")
            attempt["state"] = outcome
            attempt["finished_at"] = finished
            attempt["failure_class"] = result.get("failure_class")
            _persist_outcome(run, self._persist_run)

    def run_status(self, run_id: str) -> JSON:
        with self._lock:
            run = self._runs.get(run_id)
            if run is None:
                raise JobNotFoundError(run_id)
            return self._run_public(run)

    def cancel_run(self, run_id: str) -> JSON:
        with self._lock:
            run = self._runs.get(run_id)
            if run is None:
                raise JobNotFoundError(run_id)
            if run.get("state") in OPS_TERMINAL_STATES:
                return self._run_public(run)
            run["cancel_requested"] = True
            future = self._futures.get(run_id)
            # Retain the queue slot until the cooperative worker drains this
            # cancelled entry; repeated cancel/submit must not grow the queue.
            if future is not None and not future.running():
                run["state"] = "cancelled"
                run["phase"] = "completed"
                run["outcome"] = "cancelled"
                run["finished_at"] = utc_now()
                attempt = run["attempts"][-1]
                attempt["state"] = "cancelled"
                attempt["finished_at"] = run["finished_at"]
            else:
                run["state"] = "cancel_requested"
            self._persist_run(run)
            return self._run_public(run)

    def tick(self, now: str | None = None) -> list[str]:
        current = _utc_datetime(now)
        retry_clock = time.monotonic()
        due: list[str] = []
        with self._lock:
            if self._closed:
                return []
            for job in self._jobs.values():
                if self._schedule_retry_at.get(job["job_id"], 0) > retry_clock:
                    continue
                next_run = job.get("next_run_at")
                if (
                    job.get("state") == "active"
                    and job.get("trigger", {}).get("type") == "interval"
                    and isinstance(next_run, str)
                ):
                    try:
                        scheduled = _utc_datetime(next_run)
                    except (ValueError, OverflowError):
                        # An unreadable schedule must not run immediately or
                        # prevent healthy jobs from being considered.
                        continue
                    if scheduled <= current:
                        due.append(job["job_id"])
        started: list[str] = []
        for job_id in due:
            try:
                run, reused = self.start_run(job_id, trigger="interval")
            except (JobNotFoundError, JobConflictError):
                continue
            except OSError as exc:
                with self._lock:
                    self._schedule_retry_at[job_id] = time.monotonic() + 60
                LOGGER.warning("Operations job %s could not be queued; retry in 60s: %s", job_id, exc)
                continue
            if not reused:
                started.append(run["job_run_id"])
        return started

    def _scheduler_loop(self) -> None:
        while not self._stop.wait(1.0):
            self.tick()
