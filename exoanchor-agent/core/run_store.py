"""
Run Store — Persist backend plan runs as JSON snapshots.
"""

import json
import logging
import os
from typing import Optional

from .models import PlanRunStatus

logger = logging.getLogger("exoanchor.run_store")


class RunStore:
    """Simple JSON-backed store for plan run snapshots."""

    def __init__(self, directory: str):
        self.directory = os.path.abspath(os.path.expanduser(directory))
        os.makedirs(self.directory, exist_ok=True)

    def _path_for(self, run_id: str) -> str:
        return os.path.join(self.directory, f"{run_id}.json")

    def save_run(self, run: PlanRunStatus) -> None:
        path = self._path_for(run.run_id)
        tmp_path = f"{path}.tmp"
        payload = run.model_dump()

        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2, ensure_ascii=False)
        os.replace(tmp_path, path)

    def load_run(self, run_id: str) -> Optional[PlanRunStatus]:
        path = self._path_for(run_id)
        if not os.path.exists(path):
            return None

        try:
            with open(path, "r", encoding="utf-8") as f:
                return PlanRunStatus(**json.load(f))
        except Exception as exc:
            logger.warning(f"Failed to load run {run_id}: {exc}")
            return None

    def list_runs(self, limit: Optional[int] = 20) -> list[PlanRunStatus]:
        runs = []
        for name in os.listdir(self.directory):
            if not name.endswith(".json"):
                continue
            path = os.path.join(self.directory, name)
            try:
                with open(path, "r", encoding="utf-8") as f:
                    runs.append(PlanRunStatus(**json.load(f)))
            except Exception as exc:
                logger.warning(f"Failed to parse run snapshot {path}: {exc}")

        runs.sort(key=lambda run: run.updated_at, reverse=True)
        if limit is None:
            return runs
        return runs[:limit]
