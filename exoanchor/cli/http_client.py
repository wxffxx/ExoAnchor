"""
HTTP helpers for the ExoAnchor CLI.
"""

from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Iterator


DEFAULT_BASE_URL = "http://127.0.0.1:8090"


def request(base_url: str, method: str, path: str, body: dict | None = None) -> Any:
    url = base_url.rstrip("/") + path
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(url, data=data, method=method.upper(), headers=headers)
    timeout = 120 if path.startswith("/api/agent/sessions") else 30
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            raw = response.read().decode("utf-8")
            return json.loads(raw) if raw else {}
    except urllib.error.HTTPError as exc:
        payload = exc.read().decode("utf-8", errors="replace")
        try:
            parsed = json.loads(payload)
        except Exception:
            parsed = {"error": payload}
        message = parsed.get("detail") or parsed.get("error") or parsed.get("message") or payload or f"HTTP {exc.code}"
        raise RuntimeError(message)
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Cannot reach ExoAnchor server at {base_url}: {exc.reason}")


def stream_url(base_url: str, path: str, params: dict[str, Any] | None = None) -> str:
    query = urllib.parse.urlencode({k: v for k, v in (params or {}).items() if v not in (None, "", False)})
    url = base_url.rstrip("/") + path
    return f"{url}?{query}" if query else url


def iter_runtime_events(
    base_url: str,
    *,
    run_id: str = "",
    task_id: str = "",
    session_id: str = "",
    replay: int = 20,
) -> Iterator[dict[str, Any]]:
    url = stream_url(
        base_url,
        "/api/agent/events/stream",
        {"run_id": run_id, "task_id": task_id, "session_id": session_id, "replay": replay},
    )
    req = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(req, timeout=3600) as response:
        for raw_line in response:
            line = raw_line.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            yield json.loads(line)
