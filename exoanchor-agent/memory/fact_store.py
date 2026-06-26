"""
Fact Store — Persist learned facts and recent failures.
"""

import json
import os
import time
import uuid
from typing import Any, Optional

from pydantic import BaseModel, Field


class FactRecord(BaseModel):
    key: str
    value: Any
    category: str = "general"
    source: str = ""
    details: dict[str, Any] = Field(default_factory=dict)
    confidence: float = 1.0
    first_seen_at: float = Field(default_factory=time.time)
    updated_at: float = Field(default_factory=time.time)


class FailureRecord(BaseModel):
    failure_id: str = Field(default_factory=lambda: str(uuid.uuid4())[:12])
    source_type: str
    source_id: str
    message: str
    state: str = ""
    step_id: Optional[str] = None
    occurred_at: float = Field(default_factory=time.time)
    details: dict[str, Any] = Field(default_factory=dict)


class FactStore:
    """Small JSON-backed store for durable facts and recent failures."""

    def __init__(self, path: str):
        self.path = os.path.abspath(os.path.expanduser(path))
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        self._facts: dict[str, FactRecord] = {}
        self._failures: list[FailureRecord] = []
        self._load()

    def upsert(
        self,
        key: str,
        value: Any,
        *,
        category: str = "general",
        source: str = "",
        details: Optional[dict[str, Any]] = None,
        confidence: float = 1.0,
    ) -> FactRecord:
        now = time.time()
        if key in self._facts:
            fact = self._facts[key]
            fact.value = value
            fact.category = category or fact.category
            fact.source = source or fact.source
            fact.details = details or fact.details
            fact.confidence = confidence
            fact.updated_at = now
        else:
            fact = FactRecord(
                key=key,
                value=value,
                category=category,
                source=source,
                details=details or {},
                confidence=confidence,
                first_seen_at=now,
                updated_at=now,
            )
            self._facts[key] = fact
        self._save()
        return fact

    def get(self, key: str) -> Optional[FactRecord]:
        return self._facts.get(key)

    def list_facts(self, prefix: str = "", limit: int = 100) -> list[FactRecord]:
        items = list(self._facts.values())
        if prefix:
            items = [fact for fact in items if fact.key.startswith(prefix)]
        items.sort(key=lambda fact: fact.updated_at, reverse=True)
        return items[:limit]

    def record_failure(
        self,
        source_type: str,
        source_id: str,
        message: str,
        *,
        state: str = "",
        step_id: Optional[str] = None,
        details: Optional[dict[str, Any]] = None,
    ) -> FailureRecord:
        failure = FailureRecord(
            source_type=source_type,
            source_id=source_id,
            message=message,
            state=state,
            step_id=step_id,
            details=details or {},
        )
        self._failures.insert(0, failure)
        self._failures = self._failures[:100]
        self._save()
        return failure

    def list_failures(self, limit: int = 20) -> list[FailureRecord]:
        return self._failures[:limit]

    def summary(self) -> dict[str, Any]:
        recent_failures = [failure.model_dump() for failure in self.list_failures(limit=5)]
        return {
            "fact_count": len(self._facts),
            "failure_count": len(self._failures),
            "recent_failures": recent_failures,
        }

    def _load(self) -> None:
        if not os.path.exists(self.path):
            return
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                payload = json.load(f)
            self._facts = {
                key: FactRecord(**value)
                for key, value in (payload.get("facts") or {}).items()
            }
            self._failures = [
                FailureRecord(**item)
                for item in (payload.get("failures") or [])
            ]
        except Exception:
            self._facts = {}
            self._failures = []

    def _save(self) -> None:
        tmp_path = f"{self.path}.tmp"
        payload = {
            "facts": {key: fact.model_dump() for key, fact in self._facts.items()},
            "failures": [failure.model_dump() for failure in self._failures],
            "updated_at": time.time(),
        }
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2, ensure_ascii=False)
        os.replace(tmp_path, self.path)
