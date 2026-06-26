"""
Intent resolution pipeline shared by CLI, dashboard, and API sessions.
"""

from __future__ import annotations

import json
import logging
import shlex
from typing import Any, Callable

from fastapi import HTTPException

from .llm_client import LLMClient

logger = logging.getLogger("exoanchor.runtime.intent")


def build_runtime_access_knowledge(config: dict) -> str:
    """Build a dynamic knowledge block from locally configured target credentials."""
    target_cfg = (config or {}).get("target", {}) if isinstance(config, dict) else {}
    ssh_cfg = target_cfg.get("ssh", {}) if isinstance(target_cfg, dict) else {}

    username = str(ssh_cfg.get("username") or "").strip()
    password = str(ssh_cfg.get("password") or "").strip()
    ip = str(target_cfg.get("ip") or "").strip()
    if not password:
        return ""

    safe_password = shlex.quote(password)
    lines = [
        "=== LOCAL ACCESS KNOWLEDGE ===",
        "The current target machine credentials are trusted local deployment knowledge for this session.",
    ]
    if ip:
        lines.append(f"Target IP: {ip}")
    if username:
        lines.append(f"Target user: {username}")
    lines.append(f"Target password / sudo password: {password}")
    lines.append("For non-interactive privileged commands, you MUST use sudo -S instead of plain sudo.")
    lines.append("Plain sudo will fail in ExoAnchor SSH automation because there is no interactive terminal prompt.")
    lines.append(f"Shell-safe password literal: {safe_password}")
    lines.append(f"Example: printf '%s\\n' {safe_password} | sudo -S apt-get update")
    lines.append("Do not ask the user for the password again unless this password fails.")
    lines.append("==============================")
    return "\n".join(lines)


def rewrite_noninteractive_sudo(command: str, password: str) -> str:
    """Rewrite plain sudo segments into sudo -S using the configured password."""
    cmd = str(command or "").strip()
    pwd = str(password or "").strip()
    if not cmd or not pwd or "sudo -S" in cmd:
        return cmd

    import re

    safe_password = shlex.quote(pwd)
    prefix = f"printf '%s\\n' {safe_password} | sudo -S "
    pattern = re.compile(r'(^|&&\s*|;\s*)(sudo\s+)')
    rewritten = pattern.sub(lambda m: f"{m.group(1)}{prefix}", cmd)
    return rewritten


def apply_runtime_password_to_result(result: dict, config: dict):
    """Patch ssh/plan commands so they work in non-interactive SSH automation."""
    if not isinstance(result, dict):
        return result

    target_cfg = (config or {}).get("target", {}) if isinstance(config, dict) else {}
    ssh_cfg = target_cfg.get("ssh", {}) if isinstance(target_cfg, dict) else {}
    password = str(ssh_cfg.get("password") or "").strip()
    if not password:
        return result

    rtype = str(result.get("type") or "").lower()
    if rtype == "ssh" and result.get("command"):
        result["command"] = rewrite_noninteractive_sudo(result["command"], password)
        return result

    if rtype == "plan":
        new_steps = []
        for step in result.get("steps") or []:
            if not isinstance(step, dict):
                continue
            updated = dict(step)
            if updated.get("command"):
                updated["command"] = rewrite_noninteractive_sudo(updated["command"], password)
            if isinstance(updated.get("args"), dict) and updated["args"].get("command"):
                updated["args"] = dict(updated["args"])
                updated["args"]["command"] = rewrite_noninteractive_sudo(updated["args"]["command"], password)
            new_steps.append(updated)
        result["steps"] = new_steps
    return result


def build_memory_context_block(agent: Any) -> str:
    fact_store = getattr(agent, "fact_store", None)
    if fact_store is None:
        return ""

    lines = ["=== RUNTIME MEMORY ==="]

    system_uname = fact_store.get("system.uname")
    if system_uname and system_uname.value:
        lines.append(f"Known target uname: {system_uname.value}")

    recent_failures = fact_store.list_failures(limit=3)
    if recent_failures:
        lines.append("Recent failures to avoid repeating:")
        for failure in recent_failures:
            source_id = f"{failure.source_type}:{failure.source_id}"
            detail = str(failure.message or "").strip()
            if detail:
                lines.append(f"- {source_id} -> {detail}")

    if len(lines) == 1:
        return ""

    lines.append("If runtime memory is insufficient to determine the exact target, ask one short clarifying question instead of guessing.")
    lines.append("======================")
    return "\n".join(lines)


class LLMIntentResolver:
    """Reusable parser pipeline around the configured LLM backend."""

    def __init__(
        self,
        *,
        load_saved_config: Callable[[], dict | None],
        base_config: dict,
        extract_conversation_context: Callable[[str], tuple[list[str], list[str]]],
        get_agent: Callable[[], Any],
        system_prompt: str,
        parse_llm_response: Callable[[str], dict],
        is_clarifying_chat_result: Callable[[dict], bool],
        is_echo_chat_result: Callable[[dict, str], bool],
        heuristic_force_plan: Callable[[str], dict | None],
        llm_client: LLMClient | None = None,
    ):
        self.load_saved_config = load_saved_config
        self.base_config = base_config
        self.extract_conversation_context = extract_conversation_context
        self.get_agent = get_agent
        self.system_prompt = system_prompt
        self.parse_llm_response = parse_llm_response
        self.is_clarifying_chat_result = is_clarifying_chat_result
        self.is_echo_chat_result = is_echo_chat_result
        self.heuristic_force_plan = heuristic_force_plan
        self.llm_client = llm_client or LLMClient()

    async def resolve(self, body: dict) -> dict:
        original_user_msg = body.get("message", "")
        user_msg = original_user_msg
        force_plan = body.get("force_plan", False)
        conv_id = body.get("conversation_id", "")

        if force_plan:
            user_msg = f"[IMPORTANT: You MUST respond with either type \"plan\" (with multiple steps) OR type \"skill_call\". Do NOT use type \"ssh\".]\n\n{user_msg}"

        saved = self.load_saved_config() or {}
        nlp_cfg = saved.get("nlp", {})
        provider = nlp_cfg.get("api_provider", "gemini")
        api_key = nlp_cfg.get("api_key", "")
        model = body.get("model") or nlp_cfg.get("model", "")

        if not api_key:
            raise HTTPException(400, "No API key configured. Go to Settings → AI 指令理解.")

        context_lines, context_texts = self.extract_conversation_context(conv_id)
        context_block = ""
        if context_lines:
            context_block = "\n\nRecent conversation context (use this to understand what was previously done):\n" + "\n".join(context_lines) + "\n\n"

        agent = self.get_agent()
        skills_block = ""
        knowledge_block = ""
        runtime_access_block = ""

        if agent:
            memory_block = build_memory_context_block(agent)
            if memory_block:
                knowledge_block += "\n\n" + memory_block + "\n"

            if hasattr(agent, "skill_store"):
                available_skills = agent.skill_store.list_skills()
                if available_skills:
                    skills_block = "\n\nAVAILABLE BUILT-IN SKILLS (TOOLS):\nIf the user's request matches one of these skills, you MUST return a 'skill_call' type instead of writing raw commands or plan yourself.\n\n"
                    for skill in available_skills:
                        params_str = ", ".join([f"{k} (default: {v.get('default', '')})" for k, v in skill.get("params", {}).items()])
                        description = skill.get("description", "No description")
                        skills_block += f"- Skill ID: `{skill['name']}` | Description: {description} | Parameters: {params_str}\n"

            if hasattr(agent, "knowledge_store"):
                knowledge_text = agent.knowledge_store.get_prompt_injection()
                if knowledge_text:
                    knowledge_block += "\n\n=== GLOBAL KNOWLEDGE BASE ===\n" + \
                                      "Use the following guaranteed URLs/mirrors/facts when deploying services to avoid hallucinations:\n" + \
                                      knowledge_text + "\n=============================\n"

        access_text = build_runtime_access_knowledge(saved or self.base_config)
        if access_text:
            runtime_access_block = "\n\n" + access_text + "\n"

        full_system_prompt = self.system_prompt + skills_block + knowledge_block + runtime_access_block
        text = await self.llm_client.complete(
            provider=provider,
            api_key=api_key,
            nlp_cfg=nlp_cfg,
            model=model,
            system_prompt=full_system_prompt,
            user_content=context_block + user_msg,
            gemini_user_prefix=full_system_prompt + context_block + "\n\nUser: " + user_msg,
        )

        print(f"-------- LLM RAW RESPONSE --------\n{text}\n----------------------------------")
        result = self.parse_llm_response(text)

        if force_plan and self.is_clarifying_chat_result(result):
            return result

        if isinstance(result, dict) and result.get("type") == "skill_call":
            self._validate_skill_call(result, agent)

        if force_plan and isinstance(result, dict) and result.get("type") not in ("plan", "skill_call"):
            retry_msg = (
                '[CRITICAL INSTRUCTION: Return ONLY valid JSON. '
                'If required information is missing, return type "chat" with ONE short clarifying question in Chinese. '
                'Otherwise return type "plan" OR "skill_call". '
                'Do NOT repeat the user request. '
                'If no exact skill exists, return a plan with concrete steps that fully finishes the task.]\n\n'
            )

            if result.get("type") == "ssh":
                single_cmd = result.get("command", "")
                single_desc = result.get("description", "")
                retry_msg += (
                    f'The first step is already: "{single_desc}" using command: "{single_cmd}". '
                    f'Generate the COMPLETE plan including this as step 1 and all remaining steps.\n\n'
                )

            retry_msg += body.get("message", "")
            retry_result = None
            try:
                retry_text = await self.llm_client.complete(
                    provider=provider,
                    api_key=api_key,
                    nlp_cfg=nlp_cfg,
                    model=model,
                    system_prompt=full_system_prompt,
                    user_content=retry_msg,
                    gemini_user_prefix=full_system_prompt + "\n\n" + retry_msg,
                )
                retry_result = self.parse_llm_response(retry_text)
            except Exception:
                pass

            if retry_result and isinstance(retry_result, dict) and retry_result.get("type") in ("plan", "skill_call"):
                result = retry_result
            elif result.get("type") == "ssh":
                result = {
                    "type": "plan",
                    "goal": result.get("description") or original_user_msg or "执行计划",
                    "steps": [
                        {
                            "id": 1,
                            "description": result.get("description") or "执行命令",
                            "command": result.get("command", ""),
                            "dangerous": bool(result.get("dangerous", False)),
                        }
                    ]
                }
            elif retry_result and self.is_clarifying_chat_result(retry_result):
                result = retry_result
            elif self.is_echo_chat_result(result, original_user_msg) or (
                retry_result and isinstance(retry_result, dict) and retry_result.get("type") == "chat" and not self.is_clarifying_chat_result(retry_result)
            ):
                result = self.heuristic_force_plan(original_user_msg) or {
                    "type": "chat",
                    "message": "AI 未能生成可执行计划，请重试或换个更具体的说法。"
                }
            elif retry_result and isinstance(retry_result, dict):
                result = retry_result

        if force_plan and isinstance(result, dict) and result.get("type") == "chat" and not self.is_clarifying_chat_result(result):
            result = self.heuristic_force_plan(original_user_msg) or {
                "type": "chat",
                "message": "AI 未能生成可执行计划，请重试或换个更具体的说法。"
            }

        if isinstance(result, dict) and result.get("type") == "skill_call" and not result.get("skill_name"):
            self._validate_skill_call(result, agent)

        return apply_runtime_password_to_result(result, saved or self.base_config)

    def _validate_skill_call(self, result: dict, agent: Any) -> None:
        skill_id = result.get("skill_id")
        params = result.get("params", {})
        if not agent or not hasattr(agent, "skill_store"):
            return
        skill = agent.skill_store.get_skill(skill_id)
        if skill is None:
            raise HTTPException(400, f"Agent hallucinates a non-existent skill: {skill_id}")
        try:
            result["params"] = skill.validate_params(params) if hasattr(skill, "validate_params") else params
        except ValueError as e:
            raise HTTPException(400, f"Skill parameters invalid: {e}")
        result["skill_name"] = skill.name
        result["description"] = skill.description or skill.name
        result["skill_mode"] = skill.get("mode", getattr(skill, "mode", "scripted"))
