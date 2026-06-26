"""
Conversation persistence for the local dashboard/CLI.
"""

from __future__ import annotations

import json
import os
import re
import uuid
from datetime import datetime
from typing import Any


class ConversationStore:
    """JSON-backed conversation storage with legacy fallback."""

    def __init__(self, primary_path: str, legacy_paths: list[str] | None = None):
        self.primary_path = os.path.abspath(primary_path)
        self.legacy_paths = [os.path.abspath(path) for path in (legacy_paths or [])]

    def load(self) -> list[dict[str, Any]]:
        for path in [self.primary_path, *self.legacy_paths]:
            if not os.path.exists(path):
                continue
            try:
                with open(path, "r", encoding="utf-8") as handle:
                    return json.load(handle)
            except Exception:
                continue
        return []

    def save(self, conversations: list[dict[str, Any]]) -> None:
        with open(self.primary_path, "w", encoding="utf-8") as handle:
            json.dump(conversations, handle, indent=2, ensure_ascii=False)

    def list_summaries(self) -> list[dict[str, Any]]:
        summary: list[dict[str, Any]] = []
        for conversation in self.load():
            summary.append(
                {
                    "id": conversation["id"],
                    "title": conversation.get("title", "新对话"),
                    "model": conversation.get("model", ""),
                    "created_at": conversation.get("created_at", ""),
                    "updated_at": conversation.get("updated_at", ""),
                    "message_count": len(conversation.get("messages", [])),
                }
            )
        return summary

    def create(self, *, title: str = "新对话", model: str = "") -> dict[str, Any]:
        conversations = self.load()
        now = datetime.now().isoformat()
        conversation = {
            "id": str(uuid.uuid4())[:8],
            "title": title,
            "model": model,
            "created_at": now,
            "updated_at": now,
            "messages": [],
        }
        conversations.insert(0, conversation)
        self.save(conversations)
        return conversation

    def get(self, conversation_id: str) -> dict[str, Any] | None:
        return next((item for item in self.load() if item["id"] == conversation_id), None)

    def add_message(
        self,
        conversation_id: str,
        *,
        role: str = "user",
        content: str = "",
        html: str = "",
        cls: str = "",
    ) -> dict[str, Any] | None:
        conversations = self.load()
        for conversation in conversations:
            if conversation["id"] != conversation_id:
                continue
            message = {
                "role": role,
                "content": content,
                "html": html,
                "timestamp": datetime.now().isoformat(),
                "cls": cls,
            }
            conversation.setdefault("messages", []).append(message)
            conversation["updated_at"] = datetime.now().isoformat()
            if conversation.get("title") == "新对话" and role == "user" and content:
                conversation["title"] = content[:30]
            self.save(conversations)
            return message
        return None

    def update(self, conversation_id: str, **changes: Any) -> dict[str, Any] | None:
        conversations = self.load()
        for conversation in conversations:
            if conversation["id"] != conversation_id:
                continue
            if "title" in changes:
                conversation["title"] = changes["title"]
            if "model" in changes:
                conversation["model"] = changes["model"]
            self.save(conversations)
            return conversation
        return None

    def delete(self, conversation_id: str) -> None:
        conversations = [item for item in self.load() if item["id"] != conversation_id]
        self.save(conversations)

    def delete_all(self) -> None:
        self.save([])

    def extract_context(self, conversation_id: str, limit: int = 10) -> tuple[list[str], list[str]]:
        lines: list[str] = []
        plain_texts: list[str] = []
        if not conversation_id:
            return lines, plain_texts

        conversation = self.get(conversation_id)
        if not conversation or not conversation.get("messages"):
            return lines, plain_texts

        for message in conversation["messages"][-limit:]:
            role = message.get("role", "user")
            content = message.get("content", "") or message.get("html", "")
            clean = re.sub(r"<[^>]+>", "", str(content))[:200]
            if not clean.strip():
                continue
            lines.append(f"{'User' if role == 'user' else 'Assistant'}: {clean}")
            plain_texts.append(clean)
        return lines, plain_texts
