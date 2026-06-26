"""
Artifact Store — Persist structured artifacts generated during task execution.
"""

import json
import os
import time
import uuid
from typing import Any, Optional


class ArtifactStore:
    """JSON-backed artifact store for task and run artifacts."""

    def __init__(self, directory: str):
        self.directory = os.path.abspath(os.path.expanduser(directory))
        os.makedirs(self.directory, exist_ok=True)

    def save_json(
        self,
        kind: str,
        data: Any,
        *,
        source_id: str = "",
        metadata: Optional[dict[str, Any]] = None,
    ) -> dict[str, Any]:
        created_at = time.time()
        artifact_id = str(uuid.uuid4())[:12]
        payload = {
            "artifact_id": artifact_id,
            "kind": kind,
            "source_id": source_id,
            "created_at": created_at,
            "metadata": metadata or {},
            "data": data,
        }

        path = os.path.join(self.directory, f"{artifact_id}.json")
        tmp_path = f"{path}.tmp"
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2, ensure_ascii=False)
        os.replace(tmp_path, path)

        return {
            "artifact_id": artifact_id,
            "kind": kind,
            "source_id": source_id,
            "created_at": created_at,
            "path": path,
            "metadata": metadata or {},
        }

    def load(self, artifact_id: str) -> Optional[dict[str, Any]]:
        path = os.path.join(self.directory, f"{artifact_id}.json")
        if not os.path.exists(path):
            return None
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)

    def list_artifacts(self, limit: int = 20) -> list[dict[str, Any]]:
        items = []
        for name in os.listdir(self.directory):
            if not name.endswith(".json"):
                continue
            path = os.path.join(self.directory, name)
            try:
                with open(path, "r", encoding="utf-8") as f:
                    payload = json.load(f)
                items.append({
                    "artifact_id": payload.get("artifact_id", name[:-5]),
                    "kind": payload.get("kind", ""),
                    "source_id": payload.get("source_id", ""),
                    "created_at": payload.get("created_at", 0),
                    "path": path,
                    "metadata": payload.get("metadata", {}),
                })
            except Exception:
                continue

        items.sort(key=lambda item: item.get("created_at", 0), reverse=True)
        return items[:limit]
