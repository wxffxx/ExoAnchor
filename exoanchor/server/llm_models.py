"""
Provider model listing helpers used by the dashboard settings page.
"""

from __future__ import annotations

from typing import Any

import aiohttp


KNOWN_ANTHROPIC_MODELS = [
    {"id": "claude-sonnet-4-20250514", "name": "Claude Sonnet 4", "desc": "Latest balanced model"},
    {"id": "claude-3-5-haiku-20241022", "name": "Claude 3.5 Haiku", "desc": "Fast and efficient"},
    {"id": "claude-opus-4-20250514", "name": "Claude Opus 4", "desc": "Most capable"},
]


async def fetch_provider_models(provider: str = "gemini", api_key: str = "", endpoint: str = "") -> dict[str, Any]:
    """Return provider model list in the shape expected by the dashboard."""
    try:
        if provider == "gemini":
            url = f"https://generativelanguage.googleapis.com/v1beta/models?key={api_key}"
            async with aiohttp.ClientSession() as session:
                async with session.get(url) as response:
                    data = await response.json()
                    if "error" in data:
                        error_message = data["error"].get("message", "Unknown error")
                        return {"models": [], "error": error_message}
                    models = []
                    for model in data.get("models", []):
                        name = model.get("name", "").replace("models/", "")
                        if "generateContent" in str(model.get("supportedGenerationMethods", [])):
                            models.append(
                                {
                                    "id": name,
                                    "name": model.get("displayName", name),
                                    "desc": model.get("description", "")[:80],
                                }
                            )
                    return {"models": models}

        if provider == "openai":
            url = endpoint or "https://api.openai.com/v1/models"
            headers = {"Authorization": f"Bearer {api_key}"}
            async with aiohttp.ClientSession() as session:
                async with session.get(url, headers=headers) as response:
                    data = await response.json()
                    models = [{"id": model["id"], "name": model["id"], "desc": ""} for model in data.get("data", [])]
                    models.sort(key=lambda item: item["id"])
                    return {"models": models}

        if provider == "ollama":
            url = (endpoint or "http://localhost:11434") + "/api/tags"
            async with aiohttp.ClientSession() as session:
                async with session.get(url) as response:
                    data = await response.json()
                    models = [
                        {
                            "id": model["name"],
                            "name": model["name"],
                            "desc": f"{model.get('size', 0) // 1e9:.1f}GB",
                        }
                        for model in data.get("models", [])
                    ]
                    return {"models": models}

        if provider == "anthropic":
            return {"models": KNOWN_ANTHROPIC_MODELS}

        return {"models": [], "error": f"Unknown provider: {provider}"}
    except Exception as exc:
        return {"models": [], "error": str(exc)}
