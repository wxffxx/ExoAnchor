"""
Shared LLM transport for runtime components.
"""

from __future__ import annotations

from typing import Any, TYPE_CHECKING

import aiohttp
from fastapi import HTTPException

if TYPE_CHECKING:
    from exoanchor.memory.token_store import TokenStore


class LLMClient:
    """Thin async client for provider-specific text completions."""

    def __init__(self, token_store: "TokenStore | None" = None):
        self._token_store = token_store

    def _record_usage(self, provider: str, model: str, usage: dict[str, int], call_type: str):
        if self._token_store is None:
            return
        try:
            self._token_store.record(
                provider=provider,
                model=model,
                input_tokens=usage.get("input_tokens", 0),
                output_tokens=usage.get("output_tokens", 0),
                call_type=call_type,
            )
        except Exception:
            pass

    async def complete(
        self,
        *,
        provider: str,
        api_key: str,
        nlp_cfg: dict[str, Any],
        model: str,
        system_prompt: str,
        user_content: str,
        gemini_user_prefix: str | None = None,
        max_tokens: int = 2048,
        temperature: float = 0.1,
        call_type: str = "intent",
    ) -> str:
        if provider == "gemini":
            model = model or "gemini-2.0-flash"
            url = f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent?key={api_key}"
            payload = {
                "contents": [{"role": "user", "parts": [{"text": gemini_user_prefix or user_content}]}],
                "generationConfig": {"temperature": temperature, "maxOutputTokens": max_tokens},
            }
            async with aiohttp.ClientSession() as session:
                async with session.post(url, json=payload) as response:
                    data = await response.json()
                    if "error" in data:
                        raise HTTPException(500, data["error"].get("message", "API error"))
                    meta = data.get("usageMetadata", {})
                    self._record_usage(provider, model, {
                        "input_tokens": meta.get("promptTokenCount", 0),
                        "output_tokens": meta.get("candidatesTokenCount", 0),
                    }, call_type)
                    return data.get("candidates", [{}])[0].get("content", {}).get("parts", [{}])[0].get("text", "")

        if provider in ("openai", "custom"):
            endpoint = nlp_cfg.get("endpoint", "") or "https://api.openai.com/v1/chat/completions"
            model = model or "gpt-4o"
            headers = {"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"}
            payload = {
                "model": model,
                "messages": [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_content},
                ],
                "temperature": temperature,
                "max_tokens": max_tokens,
            }
            async with aiohttp.ClientSession() as session:
                async with session.post(endpoint, json=payload, headers=headers) as response:
                    data = await response.json()
                    if "error" in data:
                        error_message = data["error"].get("message") if isinstance(data["error"], dict) else str(data["error"])
                        raise HTTPException(500, error_message or "API error")
                    usage = data.get("usage", {})
                    self._record_usage(provider, model, {
                        "input_tokens": usage.get("prompt_tokens", 0),
                        "output_tokens": usage.get("completion_tokens", 0),
                    }, call_type)
                    return data.get("choices", [{}])[0].get("message", {}).get("content", "")

        if provider == "anthropic":
            model = model or "claude-sonnet-4-20250514"
            headers = {
                "x-api-key": api_key,
                "anthropic-version": "2023-06-01",
                "Content-Type": "application/json",
            }
            payload = {
                "model": model,
                "max_tokens": max_tokens,
                "system": system_prompt,
                "messages": [{"role": "user", "content": user_content}],
            }
            async with aiohttp.ClientSession() as session:
                async with session.post("https://api.anthropic.com/v1/messages", json=payload, headers=headers) as response:
                    data = await response.json()
                    if "error" in data:
                        error_message = data["error"].get("message") if isinstance(data["error"], dict) else str(data["error"])
                        raise HTTPException(500, error_message or "API error")
                    usage = data.get("usage", {})
                    self._record_usage(provider, model, {
                        "input_tokens": usage.get("input_tokens", 0),
                        "output_tokens": usage.get("output_tokens", 0),
                    }, call_type)
                    return data.get("content", [{}])[0].get("text", "")

        if provider == "ollama":
            ollama_url = nlp_cfg.get("ollama_url", "http://localhost:11434")
            model = nlp_cfg.get("ollama_model", model or "llama3.1:8b")
            payload = {
                "model": model,
                "messages": [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_content},
                ],
                "stream": False,
            }
            async with aiohttp.ClientSession() as session:
                async with session.post(f"{ollama_url}/api/chat", json=payload) as response:
                    data = await response.json()
                    if "error" in data:
                        raise HTTPException(500, str(data["error"]))
                    self._record_usage(provider, model, {
                        "input_tokens": data.get("prompt_eval_count", 0),
                        "output_tokens": data.get("eval_count", 0),
                    }, call_type)
                    return data.get("message", {}).get("content", "")

        raise HTTPException(400, f"Unsupported provider: {provider}")
