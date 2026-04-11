"""
LLM response parsing and lightweight fallback heuristics.
"""

from __future__ import annotations

import json
import re


def parse_llm_response(text: str) -> dict:
    """Robustly parse LLM response text into the normalized runtime schema."""
    text = str(text or "").strip()
    if text.startswith("```"):
        text = text.split("\n", 1)[-1].rsplit("```", 1)[0].strip()

    try:
        return normalize_llm_result(json.loads(text), raw_text=text)
    except json.JSONDecodeError:
        pass

    first_brace = text.find("{")
    last_brace = text.rfind("}")
    if first_brace != -1 and last_brace != -1 and last_brace > first_brace:
        json_slice = text[first_brace:last_brace + 1]
        try:
            parsed = json.loads(json_slice)
            return normalize_llm_result(parsed, raw_text=text)
        except json.JSONDecodeError:
            pass

    rtype = extract_json_string_field(text, "type")
    if not rtype:
        action = extract_json_string_field(text, "action")
        if action:
            return extract_action_result(text)
        return {"type": "chat", "message": text}

    if rtype == "ssh":
        return {
            "type": "ssh",
            "command": extract_json_string_field(text, "command") or "",
            "description": extract_json_string_field(text, "description") or "执行命令",
            "dangerous": extract_json_bool_field(text, "dangerous", default=False),
        }

    if rtype == "plan":
        goal = extract_json_string_field(text, "goal") or "执行计划"
        steps = extract_plan_steps(text)
        if steps:
            return {"type": "plan", "goal": goal, "steps": steps}
        return {"type": "chat", "message": goal or text}

    if rtype == "chat":
        return {"type": "chat", "message": extract_json_string_field(text, "message") or text}

    if rtype == "skill_call":
        return {
            "type": "skill_call",
            "skill_id": extract_json_string_field(text, "skill_id"),
            "params": {},
        }

    return {"type": "chat", "message": text}


def normalize_llm_result(result: object, raw_text: str = "") -> dict:
    """Normalize parsed LLM JSON into the small schema used by the runtime."""
    if not isinstance(result, dict):
        return {"type": "chat", "message": raw_text or str(result)}

    if "action" in result and "type" not in result:
        normalized = {"action": str(result.get("action", "")).strip() or "continue"}
        for key in ("reason", "message", "description", "command", "new_command"):
            if result.get(key):
                normalized[key] = result.get(key)
        for key in ("next_step_id", "replace_step_id", "after_step_id"):
            if result.get(key) is not None:
                normalized[key] = result.get(key)
        if result.get("dangerous") is not None:
            normalized["dangerous"] = bool(result.get("dangerous"))
        return normalized

    rtype = str(result.get("type", "")).strip().lower()

    if rtype == "plan":
        steps = []
        for idx, step in enumerate(result.get("steps") or []):
            if not isinstance(step, dict):
                continue
            tool = str(step.get("tool") or "shell.exec").strip()
            args = step.get("args") if isinstance(step.get("args"), dict) else {}
            raw_command = step.get("command")
            command = "" if raw_command in (None, "") else str(raw_command).strip()
            if command and "command" not in args:
                args["command"] = command
            if tool in ("shell", "shell.exec", "ssh.exec", "ssh", "") and not str(args.get("command", "")).strip():
                continue
            steps.append({
                "id": step.get("id", idx + 1),
                "description": str(step.get("description") or f"步骤 {idx + 1}").strip(),
                "tool": tool or "shell.exec",
                "args": args,
                "command": command or str(args.get("command", "")).strip(),
                "dangerous": bool(step.get("dangerous", False)),
            })
        if steps:
            return {
                "type": "plan",
                "goal": str(result.get("goal") or "执行计划").strip(),
                "steps": steps,
            }
        if raw_text:
            recovered_steps = extract_plan_steps(raw_text)
            if recovered_steps:
                return {
                    "type": "plan",
                    "goal": str(result.get("goal") or extract_json_string_field(raw_text, "goal") or "执行计划").strip(),
                    "steps": recovered_steps,
                }
        return {"type": "chat", "message": str(result.get("goal") or raw_text or "计划解析失败")}

    if rtype == "ssh":
        return {
            "type": "ssh",
            "command": str(result.get("command", "")).strip(),
            "description": str(result.get("description") or "执行命令").strip(),
            "dangerous": bool(result.get("dangerous", False)),
        }

    if rtype == "skill_call":
        params = result.get("params")
        return {
            "type": "skill_call",
            "skill_id": str(result.get("skill_id", "")).strip(),
            "params": params if isinstance(params, dict) else {},
        }

    if rtype == "chat":
        return {"type": "chat", "message": str(result.get("message") or raw_text or "").strip()}

    return {"type": "chat", "message": raw_text or json.dumps(result, ensure_ascii=False)}


def is_echo_chat_result(result: dict, original_message: str) -> bool:
    """Detect useless outputs which just repeat the user's request."""
    if not isinstance(result, dict) or result.get("type") != "chat":
        return False

    original = str(original_message or "").strip()
    reply = str(result.get("message") or "").strip()
    if not original or not reply:
        return False

    normalize = lambda value: "".join(str(value).strip().lower().split())
    original_norm = normalize(original)
    reply_norm = normalize(reply)
    if original_norm == reply_norm:
        return True
    if not is_clarifying_chat_message(reply):
        return original_norm in reply_norm or reply_norm in original_norm
    return False


def is_clarifying_chat_message(message: str) -> bool:
    text = str(message or "").strip()
    if not text:
        return False

    lowered = text.lower()
    question_hints = (
        "?",
        "？",
        "请告诉",
        "请确认",
        "请直接回复",
        "请直接告诉",
        "哪个",
        "哪一个",
        "哪台",
        "目录",
        "路径",
        "workload",
        "多少",
        "几",
        "端口",
        "名称",
        "名字",
        "目标值",
    )
    return any(hint in lowered for hint in question_hints)


def is_clarifying_chat_result(result: dict) -> bool:
    return isinstance(result, dict) and result.get("type") == "chat" and is_clarifying_chat_message(result.get("message", ""))


def heuristic_force_plan(original_message: str) -> dict | None:
    """Fallback plan builder for common deployment tasks when the LLM returns a useless echo."""
    msg = str(original_message or "").lower()
    has_minecraft = any(token in msg for token in ("minecraft", "mc", "我的世界"))
    if not has_minecraft:
        return None

    if "spigot" in msg:
        workdir = "~/.exoanchor/workloads/spigot-server"
        buildtools = "https://hub.spigotmc.org/jenkins/job/BuildTools/lastSuccessfulBuild/artifact/target/BuildTools.jar"
        return {
            "type": "plan",
            "goal": "部署最新版 Spigot Minecraft 服务器",
            "steps": [
                {
                    "id": 1,
                    "description": "安装必要依赖 (Git, Java 21 JDK, Curl)",
                    "command": "sudo apt-get update && sudo apt-get install -y git openjdk-21-jdk-headless curl",
                    "dangerous": True,
                },
                {
                    "id": 2,
                    "description": "创建 Spigot workload 目录",
                    "command": f"mkdir -p {workdir}",
                    "dangerous": False,
                },
                {
                    "id": 3,
                    "description": "下载最新 BuildTools",
                    "command": f"cd {workdir} && curl -L -o BuildTools.jar {buildtools}",
                    "dangerous": False,
                },
                {
                    "id": 4,
                    "description": "构建最新版 Spigot 服务端",
                    "command": f"cd {workdir} && java -jar BuildTools.jar --rev latest",
                    "dangerous": False,
                },
                {
                    "id": 5,
                    "description": "接受 EULA 并写入基础配置",
                    "command": f"cd {workdir} && printf 'eula=true\\n' > eula.txt && printf 'server-port=25565\\nmotd=ExoAnchor Spigot Server\\n' > server.properties",
                    "dangerous": False,
                },
                {
                    "id": 6,
                    "description": "写入启动脚本并后台启动 Spigot",
                    "command": (
                        f"cd {workdir} && "
                        f"printf '#!/usr/bin/env bash\\nset -e\\ncd {workdir}\\nexec java -Xms1G -Xmx4G -jar spigot-*.jar nogui\\n' > start.sh && "
                        f"printf '#!/usr/bin/env bash\\nset -e\\ncd {workdir}\\n"
                        "if pgrep -af '\\''spigot-.*jar.*nogui'\\'' > /dev/null; then\\n"
                        "  ps -eo pid=,comm=,args= | awk '\\''$2==\\\"java\\\" && $0 ~ /spigot-.*jar/ && $0 ~ /nogui/ {print $1; exit}'\\'' > server.pid\\n"
                        "  echo ALREADY_RUNNING\\n"
                        "  exit 0\\n"
                        "fi\\n"
                        "setsid -f ./start.sh > server.log 2>&1 < /dev/null\\n"
                        "sleep 2\\n"
                        "ps -eo pid=,comm=,args= | awk '\\''$2==\\\"java\\\" && $0 ~ /spigot-.*jar/ && $0 ~ /nogui/ {print $1; exit}'\\'' > server.pid\\n"
                        "echo STARTED\\n' > launch.sh && "
                        "chmod +x start.sh launch.sh && ./launch.sh"
                    ),
                    "dangerous": True,
                },
                {
                    "id": 7,
                    "description": "验证端口或查看最近日志",
                    "command": f"cd {workdir} && (ss -tulnp | grep :25565 || tail -n 50 server.log)",
                    "dangerous": False,
                },
                {
                    "id": 8,
                    "description": "写入 workload manifest",
                    "command": f"cd {workdir} && printf '{{\"name\":\"Minecraft (Spigot)\",\"type\":\"process\",\"port\":25565,\"command\":\"cd {workdir} && ./launch.sh\"}}\\n' > manifest.json",
                    "dangerous": False,
                },
            ],
        }

    workdir = "~/.exoanchor/workloads/minecraft-vanilla"
    return {
        "type": "plan",
        "goal": "部署最新版本的 Minecraft Java 服务器",
        "steps": [
            {
                "id": 1,
                "description": "检查 Java 是否可用",
                "command": "java -version 2>&1 | head -1 || echo JAVA_NOT_FOUND",
                "dangerous": False,
            },
            {
                "id": 2,
                "description": "安装 Java 运行时和 curl",
                "command": "sudo apt-get update && sudo apt-get install -y openjdk-21-jre-headless curl",
                "dangerous": True,
            },
            {
                "id": 3,
                "description": "创建 Minecraft workload 目录",
                "command": f"mkdir -p {workdir}",
                "dangerous": False,
            },
            {
                "id": 4,
                "description": "下载最新版 Minecraft 服务端",
                "command": (
                    f"cd {workdir} && "
                    "MANIFEST=$(curl -s https://piston-meta.mojang.com/mc/game/version_manifest_v2.json) && "
                    "LATEST=$(echo \"$MANIFEST\" | python3 -c \"import sys,json; print(json.load(sys.stdin)['latest']['release'])\") && "
                    "VER_URL=$(echo \"$MANIFEST\" | python3 -c \"import sys,json; vs=json.load(sys.stdin)['versions']; print(next(v['url'] for v in vs if v['id']=='$LATEST'))\") && "
                    "SERVER_URL=$(curl -s \"$VER_URL\" | python3 -c \"import sys,json; print(json.load(sys.stdin)['downloads']['server']['url'])\") && "
                    "curl -L -o server.jar \"$SERVER_URL\""
                ),
                "dangerous": False,
            },
            {
                "id": 5,
                "description": "接受 EULA 并写入基础配置",
                "command": (
                    f"cd {workdir} && "
                    "printf 'eula=true\\n' > eula.txt && "
                    "printf 'server-port=25565\\nmax-players=20\\nmotd=ExoAnchor Managed Minecraft Server\\n' > server.properties"
                ),
                "dangerous": False,
            },
            {
                "id": 6,
                "description": "写入启动脚本并后台启动 Minecraft",
                "command": (
                    f"cd {workdir} && "
                    f"printf '#!/usr/bin/env bash\\nset -e\\ncd {workdir}\\nexec java -Xms1G -Xmx4G -jar server.jar nogui\\n' > start.sh && "
                    f"printf '#!/usr/bin/env bash\\nset -e\\ncd {workdir}\\n"
                    "if pgrep -af '\\''server.jar.*nogui'\\'' > /dev/null; then\\n"
                    "  ps -eo pid=,comm=,args= | awk '\\''$2==\\\"java\\\" && $0 ~ /server.jar/ && $0 ~ /nogui/ {print $1; exit}'\\'' > server.pid\\n"
                    "  echo ALREADY_RUNNING\\n"
                    "  exit 0\\n"
                    "fi\\n"
                    "setsid -f ./start.sh > server.log 2>&1 < /dev/null\\n"
                    "sleep 2\\n"
                    "ps -eo pid=,comm=,args= | awk '\\''$2==\\\"java\\\" && $0 ~ /server.jar/ && $0 ~ /nogui/ {print $1; exit}'\\'' > server.pid\\n"
                    "echo STARTED\\n' > launch.sh && "
                    "chmod +x start.sh launch.sh && ./launch.sh"
                ),
                "dangerous": True,
            },
            {
                "id": 7,
                "description": "验证端口或查看最近日志",
                "command": f"cd {workdir} && (ss -tulnp | grep :25565 || tail -n 50 server.log)",
                "dangerous": False,
            },
            {
                "id": 8,
                "description": "写入 workload manifest",
                "command": f"cd {workdir} && printf '{{\"name\":\"Minecraft (Vanilla)\",\"type\":\"process\",\"port\":25565,\"command\":\"cd {workdir} && ./launch.sh\"}}\\n' > manifest.json",
                "dangerous": False,
            },
        ],
    }


def extract_json_string_field(text: str, field_name: str) -> str:
    """Extract a string field value from potentially malformed JSON."""
    pattern = f'"{field_name}"\\s*:\\s*"'
    match = re.search(pattern, text)
    if not match:
        return ""

    start = match.end()
    index = start
    result_chars: list[str] = []
    while index < len(text):
        char = text[index]
        if char == "\\" and index + 1 < len(text):
            result_chars.append(text[index:index + 2])
            index += 2
        elif char == '"':
            break
        else:
            result_chars.append(char)
            index += 1

    clean_result = "".join(result_chars)
    if len(clean_result) > 5 or field_name != "command":
        return clean_result

    next_fields = ['"description"', '"dangerous"', '"type"', '"message"', '"command"']
    remaining = text[start:]
    best_end = len(remaining)
    for next_field in next_fields:
        if next_field == f'"{field_name}"':
            continue
        found = remaining.find(next_field)
        if found > 0 and found < best_end:
            best_end = found

    raw = remaining[:best_end].rstrip()
    raw = re.sub(r'[",\s]+$', "", raw)
    return raw.strip('"')


def extract_json_bool_field(text: str, field_name: str, default: bool = False) -> bool:
    match = re.search(rf'"{field_name}"\s*:\s*(true|false)', text, flags=re.IGNORECASE)
    if not match:
        return default
    return match.group(1).lower() == "true"


def extract_json_number_field(text: str, field_name: str) -> float | int | None:
    match = re.search(rf'"{field_name}"\s*:\s*(-?\d+(?:\.\d+)?)', text)
    if not match:
        return None
    raw = match.group(1)
    return float(raw) if "." in raw else int(raw)


def extract_action_result(text: str) -> dict:
    result = {
        "action": extract_json_string_field(text, "action") or "continue",
    }
    for key in ("reason", "message", "description", "command", "new_command"):
        value = extract_json_string_field(text, key)
        if value:
            result[key] = value
    for key in ("next_step_id", "replace_step_id", "after_step_id"):
        value = extract_json_number_field(text, key)
        if value is not None:
            result[key] = value
    if '"dangerous"' in text:
        result["dangerous"] = extract_json_bool_field(text, "dangerous", default=False)
    return result


def extract_plan_steps(text: str) -> list[dict]:
    """Best-effort recovery of plan steps from malformed JSON text."""
    steps_match = re.search(r'"steps"\s*:\s*\[', text)
    if not steps_match:
        return []

    payload = text[steps_match.end():]
    objects: list[str] = []
    depth = 0
    start = None
    in_string = False
    escape = False

    for index, char in enumerate(payload):
        if in_string:
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == '"':
                in_string = False
            continue

        if char == '"':
            in_string = True
            continue

        if char == "{":
            if depth == 0:
                start = index
            depth += 1
            continue

        if char == "}":
            if depth > 0:
                depth -= 1
                if depth == 0 and start is not None:
                    objects.append(payload[start:index + 1])
                    start = None
            continue

        if char == "]" and depth == 0:
            break

    steps: list[dict] = []
    for index, obj_text in enumerate(objects):
        try:
            obj = json.loads(obj_text)
        except json.JSONDecodeError:
            obj = {
                "id": extract_json_number_field(obj_text, "id") or (index + 1),
                "description": extract_json_string_field(obj_text, "description") or f"步骤 {index + 1}",
                "command": extract_json_string_field(obj_text, "command"),
                "dangerous": extract_json_bool_field(obj_text, "dangerous", default=False),
            }

        command = str(obj.get("command", "")).strip()
        if not command:
            continue

        steps.append({
            "id": obj.get("id", index + 1),
            "description": str(obj.get("description") or f"步骤 {index + 1}").strip(),
            "command": command,
            "dangerous": bool(obj.get("dangerous", False)),
        })

    return steps
