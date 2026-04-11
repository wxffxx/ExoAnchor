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
from .workloads import (
    apply_resolved_workload_to_result,
    build_existing_workload_plan,
    build_workload_context_block,
    resolve_missing_task_details,
    resolve_workload_reference,
    workload_remote_dir,
)

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


def load_cached_workloads(agent: Any) -> list[dict[str, Any]]:
    fact_store = getattr(agent, "fact_store", None)
    if fact_store is None:
        return []

    latest_fact = fact_store.get("workloads.latest")
    preferred_order: list[str] = []
    latest_items: dict[str, dict[str, Any]] = {}
    if latest_fact and isinstance(latest_fact.value, dict):
        for item in latest_fact.value.get("items") or []:
            if not isinstance(item, dict):
                continue
            workload_id = str(item.get("id") or "").strip()
            if not workload_id:
                continue
            preferred_order.append(workload_id)
            latest_items[workload_id] = dict(item)

    cached: dict[str, dict[str, Any]] = {}
    for fact in fact_store.list_facts(prefix="workload.", limit=100):
        if not str(fact.key).endswith(".manifest") or not isinstance(fact.value, dict):
            continue
        workload_id = str(fact.value.get("id") or "").strip()
        if not workload_id:
            workload_id = str(fact.key).removeprefix("workload.").removesuffix(".manifest")
        payload = dict(latest_items.get(workload_id) or {})
        payload.update(dict(fact.value))
        payload.setdefault("id", workload_id)
        payload.setdefault("dir", payload.get("id"))
        payload.setdefault("status", payload.get("status") or "cached")
        payload["memory_source"] = "fact_store"
        cached[workload_id] = payload

    ordered: list[dict[str, Any]] = []
    for workload_id in preferred_order:
        if workload_id in cached:
            ordered.append(cached.pop(workload_id))
    ordered.extend(cached.values())
    return ordered


def merge_workloads(live_workloads: list[dict[str, Any]], cached_workloads: list[dict[str, Any]]) -> list[dict[str, Any]]:
    merged: dict[str, dict[str, Any]] = {}
    order: list[str] = []

    for source in (cached_workloads or [], live_workloads or []):
        for workload in source:
            if not isinstance(workload, dict):
                continue
            workload_id = str(workload.get("id") or workload.get("dir") or "").strip()
            if not workload_id:
                continue
            if workload_id not in merged:
                order.append(workload_id)
                merged[workload_id] = {}
            merged[workload_id].update(workload)
            merged[workload_id].setdefault("id", workload_id)
            merged[workload_id].setdefault("dir", merged[workload_id].get("id"))

    return [merged[workload_id] for workload_id in order]


def build_memory_context_block(agent: Any, workloads: list[dict[str, Any]], cached_workloads: list[dict[str, Any]]) -> str:
    fact_store = getattr(agent, "fact_store", None)
    if fact_store is None:
        return ""

    lines = ["=== RUNTIME MEMORY ==="]

    system_uname = fact_store.get("system.uname")
    if system_uname and system_uname.value:
        lines.append(f"Known target uname: {system_uname.value}")

    if workloads:
        cached_ids = {
            str(workload.get("id") or workload.get("dir") or "").strip()
            for workload in (cached_workloads or [])
        }
        lines.append("Known workloads in runtime memory:")
        for workload in workloads[:6]:
            workload_id = str(workload.get("id") or workload.get("dir") or "").strip()
            source = "cached" if workload_id in cached_ids and workload.get("memory_source") == "fact_store" else "live"
            lines.append(
                f"- `{workload.get('id', '')}` | Name: {workload.get('name', '')} | "
                f"Dir: {workload.get('path', '') or workload_remote_dir(workload)} | "
                f"Status: {workload.get('status', 'unknown')} | Source: {source}"
            )
    elif cached_workloads:
        lines.append("Live workload discovery is unavailable right now. Cached workload facts:")
        for workload in cached_workloads[:6]:
            lines.append(
                f"- `{workload.get('id', '')}` | Name: {workload.get('name', '')} | "
                f"Dir: {workload.get('path', '') or workload_remote_dir(workload)} | "
                f"Status: {workload.get('status', 'cached')}"
            )

    recent_failures = fact_store.list_failures(limit=3)
    if recent_failures:
        lines.append("Recent failures to avoid repeating:")
        for failure in recent_failures:
            source_id = f"{failure.source_type}:{failure.source_id}"
            detail = str(failure.message or "").strip()
            if detail:
                lines.append(f"- {source_id} -> {detail}")

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
        workload_resolution = {"action": "ignore"}
        runtime_workloads: list[dict[str, Any]] = []

        if agent:
            cached_workloads = load_cached_workloads(agent)
            if hasattr(agent, "list_workloads"):
                runtime_workloads = await agent.list_workloads()
            runtime_workloads = merge_workloads(runtime_workloads, cached_workloads)
            workload_resolution = resolve_workload_reference(original_user_msg, runtime_workloads, context_texts)
            if workload_resolution.get("action") == "ask":
                return {"type": "chat", "message": workload_resolution["message"]}

            missing_detail_question = resolve_missing_task_details(
                original_user_msg,
                workload_resolution.get("workload") if workload_resolution.get("action") == "use" else None,
            )
            if missing_detail_question:
                return {"type": "chat", "message": missing_detail_question}

            if workload_resolution.get("action") == "use":
                deterministic_result = build_existing_workload_plan(
                    original_user_msg,
                    workload_resolution.get("workload"),
                )
                if deterministic_result:
                    return apply_runtime_password_to_result(deterministic_result, saved or self.base_config)

            workload_block = build_workload_context_block(
                runtime_workloads,
                workload_resolution.get("workload") if workload_resolution.get("action") == "use" else None,
            )
            if workload_block:
                knowledge_block += "\n\n" + workload_block + "\n"

            memory_block = build_memory_context_block(agent, runtime_workloads, cached_workloads)
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

        if workload_resolution.get("action") == "use":
            workload = workload_resolution.get("workload") or {}
            resolved_dir = workload_remote_dir(workload)
            user_msg = (
                f"[Resolved existing workload for this request: `{workload.get('id', '')}` at `{resolved_dir}`. "
                "Reuse this exact directory and existing manifest command. Do NOT deploy a new service and do NOT invent another workload path.]\n\n"
                + user_msg
            )

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

        if workload_resolution.get("action") == "use":
            result = apply_resolved_workload_to_result(result, workload_resolution.get("workload"))
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
