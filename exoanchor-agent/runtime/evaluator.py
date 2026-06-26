"""
Backend step evaluator shared by API and plan runtime.
"""

from __future__ import annotations

import json
from typing import Any, Callable

from .llm_client import LLMClient
from .prompts import STEP_EVAL_PROMPT


class PlanStepEvaluator:
    """Evaluate one executed plan step and suggest the next action."""

    def __init__(
        self,
        *,
        load_saved_config: Callable[[], dict | None],
        base_config: dict,
        llm_client: LLMClient | None = None,
        parse_llm_response: Callable[[str], dict],
        prompt_template: str = STEP_EVAL_PROMPT,
    ):
        self.load_saved_config = load_saved_config
        self.base_config = base_config
        self.llm_client = llm_client or LLMClient()
        self.parse_llm_response = parse_llm_response
        self.prompt_template = prompt_template

    async def evaluate(self, body: dict[str, Any]) -> dict:
        goal = body.get("goal", "")
        step_id = body.get("step_id", 0)
        total = body.get("total", 0)
        description = body.get("description", "")
        tool = body.get("tool", "shell.exec")
        args = body.get("args", {})
        command = body.get("command", "")
        observation = body.get("observation", {})
        output = body.get("output", "")
        success = body.get("success", True)
        remaining = body.get("remaining", [])

        eval_prompt = self.prompt_template.format(
            goal=goal,
            step_id=step_id,
            total=total,
            description=description,
            tool=tool,
            args=json.dumps(args, ensure_ascii=False)[:500],
            command=command,
            observation=json.dumps(observation, ensure_ascii=False)[:1200],
            output=str(output or "")[:2000],
            success=success,
            remaining=json.dumps(remaining, ensure_ascii=False)[:500],
        )

        saved = self.load_saved_config() or {}
        effective_config = saved or self.base_config
        nlp_cfg = effective_config.get("nlp", {})
        provider = nlp_cfg.get("api_provider", "gemini")
        api_key = nlp_cfg.get("api_key", "")
        model = body.get("model") or nlp_cfg.get("model", "")

        if not api_key:
            return {"action": "continue"}

        try:
            text = await self.llm_client.complete(
                provider=provider,
                api_key=api_key,
                nlp_cfg=nlp_cfg,
                model=model,
                system_prompt="",
                user_content=eval_prompt,
                gemini_user_prefix=eval_prompt,
                max_tokens=512,
                temperature=0.1,
            )
            result = self.parse_llm_response(text)
            if "action" not in result:
                return {"action": "continue"}
            return result
        except Exception as exc:
            return {"action": "continue", "_error": str(exc)}
