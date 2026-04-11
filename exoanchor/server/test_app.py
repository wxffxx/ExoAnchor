"""
App factory for the local ExoAnchor test server.
"""

from __future__ import annotations

import logging
import os
from contextlib import asynccontextmanager
from typing import Any

from fastapi import FastAPI, Request
from fastapi.responses import FileResponse, JSONResponse

from exoanchor.action.adapters import MockGPIOAdapter, MockHIDAdapter, MockVideoAdapter
from exoanchor.api.routes import create_agent_router
from exoanchor.memory.token_store import TokenStore
from exoanchor.runtime import (
    LLMClient,
    LLMIntentResolver,
    PlanStepEvaluator,
    STEP_EVAL_PROMPT,
    SYSTEM_PROMPT,
    heuristic_force_plan,
    is_clarifying_chat_result,
    is_echo_chat_result,
    parse_llm_response,
)

from .config_store import JSONConfigStore
from .conversations import ConversationStore
from .llm_models import fetch_provider_models


def build_default_config(project_root: str) -> dict[str, Any]:
    return {
        "mode": "manual",
        "target": {
            "ip": "192.168.1.67",
            "ssh": {
                "port": 22,
                "username": "wxffxx",
                "key_file": os.path.expanduser("~/.ssh/id_ed25519"),
                "password": "123898",
            },
            "auto_bootstrap": False,
        },
        "passive": {
            "poll_interval": 10,
            "services": {
                "docker": {
                    "type": "systemd",
                    "unit": "docker",
                    "on_down": "restart",
                    "max_restarts": 3,
                },
                "mosquitto": {
                    "type": "systemd",
                    "unit": "mosquitto",
                    "check_port": 1883,
                    "on_down": "restart",
                    "max_restarts": 3,
                },
                "redis": {
                    "type": "systemd",
                    "unit": "redis-server",
                    "check_port": 6379,
                    "on_down": "restart",
                    "max_restarts": 2,
                },
            },
            "local_triggers": {
                "black_screen": {"enabled": False},
                "frozen_screen": {"enabled": False},
            },
            "ssh_triggers": {},
        },
        "vision": {
            "backend": "local",
        },
        "safety": {
            "max_steps": 200,
            "max_duration": 600,
            "require_confirmation": False,
        },
        "skills_dir": os.path.join(project_root, "exoanchor", "skill_library"),
    }


def create_test_app(project_root: str | None = None) -> FastAPI:
    project_root = os.path.abspath(project_root or os.path.join(os.path.dirname(__file__), "..", ".."))
    dashboard_dir = os.path.join(project_root, "exoanchor", "dashboard")
    config_store = JSONConfigStore(primary_path=os.path.join(project_root, "exoanchor_config.json"))
    conversation_store = ConversationStore(primary_path=os.path.join(project_root, "exoanchor_conversations.json"))
    default_config = build_default_config(project_root)

    hid = MockHIDAdapter()
    video = MockVideoAdapter()
    gpio = MockGPIOAdapter()

    router, agent_instance = create_agent_router(
        hid_adapter=hid,
        video_adapter=video,
        gpio_adapter=gpio,
        config=default_config,
    )

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        logging.info("=" * 50)
        logging.info("ExoAnchor Test Server starting...")
        logging.info(f"Target: {default_config['target']['ip']}")
        logging.info("=" * 50)
        await agent_instance.startup()
        yield
        await agent_instance.shutdown()
        logging.info("ExoAnchor Test Server stopped")

    app = FastAPI(
        title="ExoAnchor Test Server",
        description="KVM Agent Framework — Mac 本地测试 (Mock HID/Video + Real SSH)",
        version="0.1.0",
        lifespan=lifespan,
    )
    app.include_router(router, prefix="/api/agent")

    token_store = TokenStore(path=os.path.expanduser("~/.exoanchor/token_usage.json"))
    llm_client = LLMClient(token_store=token_store)
    intent_resolver = LLMIntentResolver(
        load_saved_config=config_store.load,
        base_config=default_config,
        extract_conversation_context=conversation_store.extract_context,
        get_agent=lambda: agent_instance,
        system_prompt=SYSTEM_PROMPT,
        parse_llm_response=parse_llm_response,
        is_clarifying_chat_result=is_clarifying_chat_result,
        is_echo_chat_result=is_echo_chat_result,
        heuristic_force_plan=heuristic_force_plan,
        llm_client=llm_client,
    )
    step_evaluator = PlanStepEvaluator(
        load_saved_config=config_store.load,
        base_config=default_config,
        llm_client=llm_client,
        parse_llm_response=parse_llm_response,
        prompt_template=STEP_EVAL_PROMPT,
    )

    agent_instance.set_intent_resolver(intent_resolver.resolve)
    agent_instance.set_step_evaluator(step_evaluator.evaluate)

    app.state.agent = agent_instance
    app.state.config_store = config_store
    app.state.conversation_store = conversation_store
    app.state.default_config = default_config
    app.state.token_store = token_store

    def dashboard_file_response(filename: str):
        return FileResponse(
            os.path.join(dashboard_dir, filename),
            headers={"Cache-Control": "no-store, max-age=0"},
        )

    @app.get("/")
    async def root():
        return dashboard_file_response("index.html")

    @app.get("/overview")
    async def overview_page():
        return dashboard_file_response("index.html")

    @app.get("/sessions")
    async def sessions_page():
        return dashboard_file_response("index.html")

    @app.get("/workloads")
    async def workloads_page():
        return dashboard_file_response("index.html")

    @app.get("/monitor")
    async def monitor_page():
        return dashboard_file_response("index.html")

    @app.get("/terminal")
    async def terminal_page():
        return dashboard_file_response("index.html")

    @app.get("/settings")
    async def settings_page():
        return dashboard_file_response("settings.html")

    @app.get("/api/agent/config")
    async def get_config():
        return JSONResponse(config_store.load() or default_config)

    @app.post("/api/agent/config")
    async def update_config(request: Request):
        config_store.save(await request.json())
        return JSONResponse({"status": "ok", "message": "Config saved"})

    @app.get("/api/conversations")
    async def list_conversations():
        return JSONResponse(conversation_store.list_summaries())

    @app.post("/api/conversations")
    async def create_conversation(request: Request):
        body = await request.json()
        conversation = conversation_store.create(
            title=body.get("title", "新对话"),
            model=body.get("model", ""),
        )
        return JSONResponse(conversation)

    @app.get("/api/conversations/{conv_id}")
    async def get_conversation(conv_id: str):
        conversation = conversation_store.get(conv_id)
        if conversation is None:
            return JSONResponse({"error": "Not found"}, status_code=404)
        return JSONResponse(conversation)

    @app.post("/api/conversations/{conv_id}/messages")
    async def add_message(conv_id: str, request: Request):
        body = await request.json()
        message = conversation_store.add_message(
            conv_id,
            role=body.get("role", "user"),
            content=body.get("content", ""),
            html=body.get("html", ""),
            cls=body.get("cls", ""),
        )
        if message is None:
            return JSONResponse({"error": "Not found"}, status_code=404)
        return JSONResponse(message)

    @app.patch("/api/conversations/{conv_id}")
    async def update_conversation(conv_id: str, request: Request):
        body = await request.json()
        changes: dict[str, Any] = {}
        if "title" in body:
            changes["title"] = body["title"]
        if "model" in body:
            changes["model"] = body["model"]
        conversation = conversation_store.update(conv_id, **changes)
        if conversation is None:
            return JSONResponse({"error": "Not found"}, status_code=404)
        return JSONResponse(conversation)

    @app.delete("/api/conversations/{conv_id}")
    async def delete_conversation(conv_id: str):
        conversation_store.delete(conv_id)
        return JSONResponse({"status": "ok"})

    @app.delete("/api/conversations")
    async def delete_all_conversations():
        conversation_store.delete_all()
        return JSONResponse({"status": "ok", "deleted": "all"})

    @app.get("/api/llm/models")
    async def proxy_llm_models(provider: str = "gemini", api_key: str = "", endpoint: str = ""):
        result = await fetch_provider_models(provider=provider, api_key=api_key, endpoint=endpoint)
        status_code = 200 if not result.get("error") else 500
        if str(result.get("error", "")).startswith("Unknown provider:"):
            status_code = 400
        return JSONResponse(result, status_code=status_code)

    @app.get("/api/agent/token-usage")
    async def get_token_usage():
        return JSONResponse(token_store.get_summary())

    @app.post("/api/agent/token-usage/reset")
    async def reset_token_usage():
        token_store.reset()
        return JSONResponse({"status": "ok", "message": "Token usage reset"})

    return app
