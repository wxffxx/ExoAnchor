"""Configuration, authentication, and HTTP transport for one ExoAnchor device."""

from __future__ import annotations

import json
import math
import os
import secrets
import time
import threading
from dataclasses import dataclass
from http.client import HTTPException
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urljoin, urlparse
from urllib.request import HTTPRedirectHandler, Request, build_opener


JSON = dict[str, Any]


class ExoAnchorError(RuntimeError):
    pass


class _NoRedirect(HTTPRedirectHandler):
    """Keep requests and bearer credentials at the explicitly selected URL."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _env_bool(name: str, default: bool = False) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _read_secret_file(name: str) -> str | None:
    path = os.environ.get(name)
    if not path:
        return None
    try:
        with open(os.path.expanduser(path), "r", encoding="utf-8") as handle:
            value = handle.read().strip()
            return value or None
    except FileNotFoundError:
        return None


@dataclass
class ExoAnchorConfig:
    base_url: str
    username: str
    password: str | None
    token: str | None
    timeout: float
    allow_write: bool
    control_owner: str
    device_id: str
    state_dir: str
    persist_job_output: bool
    allow_unverified_ssh_host: bool
    allow_arbitrary_ssh: bool

    @classmethod
    def from_env(cls) -> "ExoAnchorConfig":
        base_url = os.environ.get("EXOANCHOR_BASE_URL", "").strip().rstrip("/")
        if not base_url:
            raise ExoAnchorError("EXOANCHOR_BASE_URL is required")
        parsed_url = urlparse(base_url)
        if parsed_url.scheme not in {"http", "https"} or not parsed_url.netloc:
            raise ExoAnchorError("EXOANCHOR_BASE_URL must be an absolute http(s) URL")
        try:
            timeout = float(os.environ.get("EXOANCHOR_TIMEOUT", "75"))
        except ValueError as exc:
            raise ExoAnchorError("EXOANCHOR_TIMEOUT must be a number") from exc
        if not math.isfinite(timeout) or timeout <= 0 or timeout > 600:
            raise ExoAnchorError("EXOANCHOR_TIMEOUT must be between 0 and 600 seconds")
        control_owner = os.environ.get("EXOANCHOR_CONTROL_OWNER", "mcp").strip().lower()
        if control_owner not in {"mcp", "agent"}:
            raise ExoAnchorError("EXOANCHOR_CONTROL_OWNER must be mcp or agent")
        return cls(
            base_url=base_url,
            username=os.environ.get("EXOANCHOR_USERNAME", "").strip(),
            password=os.environ.get("EXOANCHOR_PASSWORD") or _read_secret_file("EXOANCHOR_PASSWORD_FILE"),
            token=os.environ.get("EXOANCHOR_TOKEN") or _read_secret_file("EXOANCHOR_TOKEN_FILE"),
            timeout=timeout,
            allow_write=_env_bool("EXOANCHOR_ALLOW_WRITE", False),
            control_owner=control_owner,
            device_id=(
                os.environ.get("EXOANCHOR_DEVICE_ID", "").strip()
                or parsed_url.hostname
                or "configured-device"
            ),
            state_dir=os.environ.get(
                "EXOANCHOR_STATE_DIR",
                os.path.expanduser("~/.local/state/exoanchor-mcp"),
            ),
            persist_job_output=_env_bool("EXOANCHOR_PERSIST_JOB_OUTPUT", False),
            allow_unverified_ssh_host=_env_bool(
                "EXOANCHOR_ALLOW_UNVERIFIED_SSH_HOST", False
            ),
            allow_arbitrary_ssh=_env_bool("EXOANCHOR_ALLOW_ARBITRARY_SSH", False),
        )


class ExoAnchorClient:
    def __init__(self, config: ExoAnchorConfig):
        self.config = config
        self.token = config.token
        self._auth_lock = threading.RLock()
        self._opener = build_opener(_NoRedirect())

    def web_url(self, path: str = "/") -> str:
        if not path.startswith("/"):
            path = "/" + path
        return self.config.base_url + path

    def _request(
        self,
        method: str,
        path: str,
        body: JSON | None = None,
        *,
        raw: bool = False,
        retry_auth: bool = True,
        timeout: float | None = None,
    ) -> Any:
        url = urljoin(self.config.base_url + "/", path.lstrip("/"))
        headers = {"X-ExoAnchor-Client": "exoanchor-mcp"}
        data: bytes | None = None
        if body is not None:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request_token = self.token
        if request_token:
            headers["Authorization"] = f"Bearer {request_token}"
        req = Request(url, data=data, headers=headers, method=method)
        try:
            with self._opener.open(req, timeout=self.config.timeout if timeout is None else timeout) as resp:
                payload = resp.read()
                if raw:
                    return payload, dict(resp.headers)
                content_type = resp.headers.get("Content-Type", "")
                if "json" in content_type:
                    # Diagnostics must remain inspectable when an older device
                    # accidentally places a non-UTF-8 byte in a JSON string.
                    # Current firmware sanitizes at the source; replacement
                    # here keeps the bridge usable during mixed-version repair.
                    try:
                        return json.loads(
                            payload.decode("utf-8", errors="replace") or "{}"
                        )
                    except (ValueError, RecursionError) as exc:
                        raise ExoAnchorError(f"invalid JSON from {path}") from exc
                text = payload.decode("utf-8", errors="replace")
                try:
                    return json.loads(text)
                except (ValueError, RecursionError):
                    return {"ok": True, "text": text}
        except HTTPError as exc:
            try:
                text = exc.read().decode("utf-8", errors="replace")
            except (OSError, HTTPException):
                text = "response body unavailable"
            finally:
                exc.close()
            if exc.code == 401 and retry_auth:
                self._ensure_authenticated(request_token)
                return self._request(method, path, body, raw=raw, retry_auth=False, timeout=timeout)
            raise ExoAnchorError(f"HTTP {exc.code} {path}: {text or exc.reason}") from exc
        except URLError as exc:
            raise ExoAnchorError(f"connect failed {url}: {exc.reason}") from exc
        except (OSError, HTTPException) as exc:
            raise ExoAnchorError(f"response failed {path}: {exc}") from exc

    def _ensure_authenticated(self, rejected_token: str | None = None) -> None:
        with self._auth_lock:
            if not self.token or self.token == rejected_token:
                self.login()

    def login(self) -> JSON:
        with self._auth_lock:
            return self._login()

    def _login(self) -> JSON:
        if not self.config.password:
            raise ExoAnchorError(
                "device requires auth; set EXOANCHOR_PASSWORD or EXOANCHOR_TOKEN"
            )
        if not self.config.username:
            raise ExoAnchorError("password login requires EXOANCHOR_USERNAME")
        deadline = time.monotonic() + self.config.timeout
        resp = self._request(
            "POST",
            "/api/auth/login",
            {
                "username": self.config.username,
                "password": self.config.password,
                "request_id": secrets.token_hex(16),
            },
            retry_auth=False,
            timeout=self.config.timeout,
        )
        # Password verification is intentionally performed by an asynchronous
        # device job.  On a loaded ESP32-P4 the configured PBKDF2 verifier can
        # take longer than 30 seconds, so the polling lifetime must follow the
        # transport timeout instead of imposing a shorter hidden deadline.
        # Submission, backoff and status requests share that budget. urllib's
        # timeout bounds socket operations, so also reject a late response.
        while isinstance(resp, dict) and resp.get("pending"):
            job_id = str(resp.get("job_id") or "")
            if not job_id:
                raise ExoAnchorError("login response did not include job_id")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ExoAnchorError("login timed out")
            try:
                delay = float(resp.get("poll_after_ms", 100)) / 1000
            except (TypeError, ValueError, OverflowError):
                delay = 0.1
            if not math.isfinite(delay):
                delay = 0.1
            time.sleep(min(max(0.05, min(delay, 1.0)), remaining))
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ExoAnchorError("login timed out")
            resp = self._request(
                "GET",
                "/api/auth/login/status?" + urlencode({"job_id": job_id}),
                retry_auth=False,
                timeout=remaining,
            )
        if time.monotonic() >= deadline:
            raise ExoAnchorError("login timed out")
        token = resp.get("token") if isinstance(resp, dict) else None
        if not token:
            raise ExoAnchorError("login response did not include token")
        self.token = token
        return resp

    def get_json(self, path: str) -> JSON:
        resp = self._request("GET", path)
        return resp if isinstance(resp, dict) else {"value": resp}

    def post_json(self, path: str, body: JSON) -> JSON:
        # Never discover authentication by replaying a mutating request.  A
        # device may accept the action and then reset or lose the response;
        # retrying that POST after a 401 would make its outcome ambiguous.
        if not self.token and self.config.password:
            self._ensure_authenticated()
        resp = self._request("POST", path, body, retry_auth=False)
        return resp if isinstance(resp, dict) else {"value": resp}

    def get_raw(self, path: str) -> tuple[bytes, dict[str, Any]]:
        return self._request("GET", path, raw=True)
