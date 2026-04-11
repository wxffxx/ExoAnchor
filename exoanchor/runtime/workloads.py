"""
Workload resolution helpers for existing managed services.
"""

from __future__ import annotations

import base64
import re
import shlex
from typing import Optional


DEPLOY_REQUEST_KEYWORDS = (
    "部署", "安装", "搭建", "新建", "创建", "deploy", "install", "setup", "create", "provision"
)
EXISTING_WORKLOAD_KEYWORDS = (
    "修改", "改", "设置", "配置", "调整", "更新", "升级", "重启", "启动", "停止",
    "恢复", "修复", "查看", "检查", "日志", "状态", "端口", "玩家", "人数",
    "motd", "eula", "server.properties", "launch.sh", "manifest", "max-players",
    "restart", "start", "stop", "reload", "status", "log", "logs", "modify",
    "update", "configure", "change", "repair", "recover",
)
MINECRAFT_DOMAIN_HINTS = (
    "minecraft", "mc", "spigot", "paper", "bukkit", "forge", "fabric",
    "玩家", "人数", "max-players", "motd", "eula", "server.properties",
    "世界", "白名单", "插件", "开服",
)
WORKLOAD_PATH_PATTERN = re.compile(r'~/\.(?:exoanchor|cortex)/workloads/[^/\s\'"]+')


def normalize_text(text: str) -> str:
    return " ".join(str(text or "").strip().lower().split())


def workload_remote_dir(workload: dict) -> str:
    if not isinstance(workload, dict):
        return ""
    if workload.get("path"):
        return str(workload["path"])
    base_dir = str(workload.get("base_dir") or "exoanchor").strip() or "exoanchor"
    dir_name = str(workload.get("dir") or workload.get("id") or "").strip()
    if not dir_name:
        return ""
    return f"~/.{base_dir}/workloads/{dir_name}"


def is_minecraft_workload(workload: Optional[dict]) -> bool:
    searchable = normalize_text(" ".join(
        str((workload or {}).get(key) or "")
        for key in ("name", "id", "dir", "command", "path", "type")
    ))
    return any(token in searchable for token in ("minecraft", "spigot", "paper", "bukkit", "forge", "fabric"))


def build_minecraft_console_probe_command(workload: dict) -> str:
    path = str(workload.get("path") or workload_remote_dir(workload)).strip()
    return (
        f"cd {shlex.quote(path)} && "
        "python3 -c "
        + shlex.quote(
            "import json, os\n"
            "props = {}\n"
            "path = 'server.properties'\n"
            "if os.path.isfile(path):\n"
            "    with open(path, 'r', encoding='utf-8', errors='ignore') as fh:\n"
            "        for raw in fh:\n"
            "            line = raw.strip()\n"
            "            if not line or line.startswith('#') or '=' not in line:\n"
            "                continue\n"
            "            key, value = line.split('=', 1)\n"
            "            props[key.strip()] = value.strip()\n"
            "enabled = str(props.get('enable-rcon', 'false')).lower() == 'true'\n"
            "password = str(props.get('rcon.password', ''))\n"
            "port_raw = str(props.get('rcon.port', '25575') or '25575')\n"
            "try:\n"
            "    port = int(port_raw)\n"
            "except ValueError:\n"
            "    port = 25575\n"
            "print(json.dumps({'type': 'minecraft-rcon', 'available': bool(enabled and password), 'enabled': enabled, 'port': port, 'password_present': bool(password)}, ensure_ascii=False))\n"
        )
    )


def build_minecraft_console_setup_command(workload: dict, password: str, *, port: int = 25575) -> str:
    path = str(workload.get("path") or workload_remote_dir(workload)).strip()
    safe_password = shlex.quote(str(password))
    safe_port = int(port or 25575)
    return (
        f"cd {shlex.quote(path)} && "
        "grep -q '^enable-rcon=' server.properties && "
        "sed -i 's/^enable-rcon=.*/enable-rcon=true/' server.properties || "
        "printf '\\nenable-rcon=true\\n' >> server.properties; "
        "grep -q '^rcon.port=' server.properties && "
        f"sed -i 's/^rcon.port=.*/rcon.port={safe_port}/' server.properties || "
        f"printf 'rcon.port={safe_port}\\n' >> server.properties; "
        "grep -q '^rcon.password=' server.properties && "
        f"sed -i 's/^rcon.password=.*/rcon.password={safe_password}/' server.properties || "
        f"printf 'rcon.password=%s\\n' {safe_password} >> server.properties; "
        "grep -q '^broadcast-rcon-to-ops=' server.properties && "
        "sed -i 's/^broadcast-rcon-to-ops=.*/broadcast-rcon-to-ops=false/' server.properties || "
        "printf 'broadcast-rcon-to-ops=false\\n' >> server.properties"
    )


def build_minecraft_rcon_exec_command(workload: dict, command: str, *, password: str, port: int = 25575) -> str:
    path = str(workload.get("path") or workload_remote_dir(workload)).strip()
    encoded_password = base64.b64encode(str(password).encode("utf-8")).decode("ascii")
    encoded_command = base64.b64encode(str(command).encode("utf-8")).decode("ascii")
    script = (
        "import base64, random, socket, struct, sys\n"
        "host = '127.0.0.1'\n"
        "port = int(sys.argv[1])\n"
        "password = base64.b64decode(sys.argv[2]).decode('utf-8')\n"
        "command = base64.b64decode(sys.argv[3]).decode('utf-8')\n"
        "request_id = random.randint(1, 2147483000)\n"
        "def packet(req_id, kind, body):\n"
        "    payload = struct.pack('<ii', req_id, kind) + body.encode('utf-8') + b'\\x00\\x00'\n"
        "    return struct.pack('<i', len(payload)) + payload\n"
        "def recv_packet(sock):\n"
        "    header = sock.recv(4)\n"
        "    if len(header) != 4:\n"
        "        raise RuntimeError('Incomplete RCON response header')\n"
        "    size = struct.unpack('<i', header)[0]\n"
        "    data = b''\n"
        "    while len(data) < size:\n"
        "        chunk = sock.recv(size - len(data))\n"
        "        if not chunk:\n"
        "            raise RuntimeError('Connection closed while reading RCON response')\n"
        "        data += chunk\n"
        "    req_id, kind = struct.unpack('<ii', data[:8])\n"
        "    body = data[8:-2].decode('utf-8', errors='replace')\n"
        "    return req_id, kind, body\n"
        "sock = socket.create_connection((host, port), timeout=5)\n"
        "sock.sendall(packet(request_id, 3, password))\n"
        "auth_id, _, _ = recv_packet(sock)\n"
        "if auth_id == -1:\n"
        "    raise SystemExit('RCON authentication failed')\n"
        "sock.sendall(packet(request_id, 2, command))\n"
        "resp_id, _, body = recv_packet(sock)\n"
        "print(body, end='')\n"
        "sock.close()\n"
    )
    return (
        f"cd {shlex.quote(path)} && "
        "python3 -c "
        + shlex.quote(script)
        + f" {int(port or 25575)} {shlex.quote(encoded_password)} {shlex.quote(encoded_command)}"
    )


def request_targets_existing_workload(message: str) -> bool:
    norm = normalize_text(message)
    if not norm:
        return False
    if any(keyword in norm for keyword in DEPLOY_REQUEST_KEYWORDS):
        return False

    generic_inspect_keywords = ("查看", "检查", "状态", "日志", "log", "logs", "status")
    strong_existing_keywords = tuple(
        keyword for keyword in EXISTING_WORKLOAD_KEYWORDS if keyword not in generic_inspect_keywords
    )
    if any(keyword in norm for keyword in strong_existing_keywords):
        return True

    if not any(keyword in norm for keyword in generic_inspect_keywords):
        return False

    workload_specific_hints = (
        "workload", "服务", "server", "minecraft", "spigot", "paper", "bukkit",
        "forge", "fabric", "目录", "路径", "manifest", "launch.sh", "server.properties",
        "玩家", "人数", "max-players", "motd", "eula",
    )
    return any(token in norm for token in workload_specific_hints)


def score_workload_match(message: str, workload: dict, context_text: str = "") -> int:
    norm = normalize_text(message)
    context = normalize_text(context_text)
    searchable = normalize_text(" ".join(
        str(workload.get(key) or "")
        for key in ("name", "id", "dir", "command", "path", "status", "type")
    ))

    score = 0
    for field in (workload.get("id"), workload.get("dir"), workload.get("name")):
        field_norm = normalize_text(field)
        if not field_norm:
            continue
        if field_norm in norm:
            score += 12
        elif field_norm in context:
            score += 6

    port = workload.get("port")
    if port and str(port) in norm:
        score += 3

    semantic_text = f"{norm} {context}".strip()
    if any(token in semantic_text for token in MINECRAFT_DOMAIN_HINTS):
        if any(token in searchable for token in ("minecraft", "spigot", "paper", "bukkit", "forge", "fabric", "server.properties", "launch.sh")):
            score += 8

    if any(token in norm for token in ("玩家", "人数", "max-players", "motd", "eula", "server.properties")):
        if any(token in searchable for token in ("minecraft", "spigot", "server.properties")):
            score += 5

    if any(token in norm for token in ("重启", "restart", "启动", "start", "停止", "stop", "日志", "log", "状态", "status")):
        if normalize_text(workload.get("command", "")):
            score += 2

    return score


def format_workload_option(workload: dict) -> str:
    path = workload_remote_dir(workload)
    port = workload.get("port")
    port_text = f", 端口 {port}" if port else ""
    return f"`{workload.get('id', '')}`（{workload.get('name', 'Unnamed')}, 目录 `{path}`{port_text}）"


def build_workload_context_block(workloads: list[dict], resolved_workload: Optional[dict] = None) -> str:
    if not workloads:
        return ""

    lines = [
        "=== CURRENT TARGET WORKLOADS ===",
        "These workloads already exist on the target machine.",
        "When the user wants to modify, restart, inspect, or repair an existing service, you MUST reuse an existing matching workload instead of inventing a new directory.",
    ]
    for workload in workloads[:8]:
        path = workload_remote_dir(workload)
        port = workload.get("port")
        status = workload.get("status") or "unknown"
        command = str(workload.get("command") or "").strip()
        lines.append(
            f"- Workload `{workload.get('id', '')}` | Name: {workload.get('name', 'Unnamed')} | Dir: {path} | "
            f"Port: {port if port is not None else 'none'} | Status: {status} | Command: {command}"
        )

    if resolved_workload:
        path = workload_remote_dir(resolved_workload)
        lines.extend([
            f"Matched workload for THIS request: `{resolved_workload.get('id', '')}`",
            f"Exact directory to reuse: {path}",
            f"Existing startup command: {resolved_workload.get('command', '')}",
            "For this request, you MUST reuse the exact directory above.",
            "Do NOT switch to `minecraft-server` or any other new directory unless the user explicitly asks for a new deployment.",
            "Do NOT propose a fresh deployment when the user is clearly asking to modify an existing running server.",
        ])
    else:
        lines.append("If the request targets an existing service but the target workload is unclear, return type `chat` and ask a clarifying question instead of inventing a path.")

    lines.append("================================")
    return "\n".join(lines)


def resolve_workload_reference(message: str, workloads: list[dict], context_texts: list[str]) -> dict:
    if not request_targets_existing_workload(message):
        return {"action": "ignore"}

    if not workloads:
        return {
            "action": "ask",
            "message": "我现在还没有发现可操作的已有 workload。请告诉我要操作的服务名称/目录，或者先明确你要新部署哪个服务。",
        }

    context_text = " ".join(context_texts[-6:])
    scored: list[tuple[int, dict]] = []
    for workload in workloads:
        score = score_workload_match(message, workload, context_text)
        if score > 0:
            scored.append((score, workload))

    scored.sort(key=lambda item: item[0], reverse=True)
    if not scored:
        if len(workloads) == 1:
            only = workloads[0]
            return {
                "action": "ask",
                "message": (
                    "我当前只看到一个已有 workload："
                    f"{format_workload_option(only)}。"
                    "如果你要操作它，请直接回复“就改这个 workload”，或者告诉我准确的 workload 名称/目录。"
                ),
            }
        options = "、".join(format_workload_option(workload) for workload in workloads[:4])
        return {
            "action": "ask",
            "message": f"我现在无法从上下文唯一判断你要操作哪个已有 workload。当前可见 workload 有：{options}。请告诉我具体是哪个 workload，或者给我准确目录。",
        }

    if len(scored) == 1 or (len(scored) > 1 and scored[0][0] >= scored[1][0] + 4):
        return {"action": "use", "workload": scored[0][1], "score": scored[0][0]}

    candidates = [workload for score, workload in scored if score >= scored[0][0] - 2][:3]
    options = "、".join(format_workload_option(workload) for workload in candidates)
    return {
        "action": "ask",
        "message": f"我现在不能唯一判断你要操作哪个已有 workload。最接近的候选有：{options}。请直接告诉我你要改哪一个。",
    }


def resolve_missing_task_details(message: str, workload: Optional[dict] = None) -> str:
    norm = normalize_text(message)
    if not norm:
        return ""

    target_name = str((workload or {}).get("name") or "这个服务").strip() or "这个服务"
    is_change_request = any(token in norm for token in ("改", "修改", "设置", "调整", "update", "change", "set"))

    if any(token in norm for token in ("人数", "玩家", "max-players")):
        has_player_count = re.search(r'(\d+)\s*(人|players?)?', message, flags=re.IGNORECASE)
        if not has_player_count:
            return f"你想把 `{target_name}` 的最大玩家人数改成多少？请直接告诉我一个数字。"

    if is_change_request and any(token in norm for token in ("端口", "port")):
        if not re.search(r'\b\d{2,5}\b', message):
            return f"你想把 `{target_name}` 的端口改成多少？请直接告诉我目标端口号。"

    if is_change_request and any(token in norm for token in ("内存", "memory", "xmx", "heap")):
        if not re.search(r'\b\d+\s*(m|mb|g|gb)\b', message, flags=re.IGNORECASE):
            return f"你想把 `{target_name}` 的内存改成多少？例如 `2G`、`4G`。"

    generic_config_request = any(token in norm for token in ("配置", "设置", "参数"))
    if is_change_request and generic_config_request and not any(
        token in norm for token in ("人数", "玩家", "max-players", "端口", "port", "motd", "内存", "memory")
    ):
        return f"你想修改 `{target_name}` 的哪一个配置项？请直接告诉我配置项和目标值。"

    return ""


def extract_requested_player_count(message: str) -> Optional[int]:
    match = re.search(r'(\d+)\s*(?:人|players?)?', str(message or ""), flags=re.IGNORECASE)
    if not match:
        return None
    try:
        value = int(match.group(1))
    except ValueError:
        return None
    return value if value > 0 else None


def workload_verify_command(workload: dict) -> str:
    path = str(workload.get("path") or workload_remote_dir(workload)).strip()
    port = workload.get("port")
    if port:
        return (
            f"cd {shlex.quote(path)} && "
            f"((ss -tulnp 2>/dev/null || netstat -tuln 2>/dev/null) | grep -E '[:.]({int(port)})\\b' || "
            "tail -n 50 server.log 2>/dev/null || true)"
        )
    return (
        f"cd {shlex.quote(path)} && "
        "(if [ -f server.pid ] && kill -0 $(cat server.pid) 2>/dev/null; then "
        "echo \"running pid=$(cat server.pid)\"; "
        "else "
        "tail -n 50 server.log 2>/dev/null || ls -lah; "
        "fi)"
    )


def build_workload_status_command(workload: dict) -> str:
    path = str(workload.get("path") or workload_remote_dir(workload)).strip()
    return (
        f"cd {shlex.quote(path)} && "
        "(cat manifest.json 2>/dev/null || echo 'manifest.json missing') && echo '---' && "
        + workload_verify_command(workload)
    )


def build_workload_logs_command(workload: dict, *, lines: int = 80) -> str:
    path = str(workload.get("path") or workload_remote_dir(workload)).strip()
    safe_lines = max(10, min(int(lines or 80), 400))
    return (
        f"cd {shlex.quote(path)} && "
        f"(tail -n {safe_lines} server.log 2>/dev/null || tail -n {safe_lines} *.log 2>/dev/null || ls -lah)"
    )


def build_workload_status_plan(workload: dict) -> dict:
    return {
        "type": "plan",
        "goal": f"检查 {workload.get('name', workload.get('id', 'workload'))} 的状态",
        "steps": [
            {
                "id": 1,
                "description": "检查 workload manifest、进程和监听端口",
                "command": build_workload_status_command(workload),
                "dangerous": False,
            }
        ],
    }


def build_workload_logs_plan(workload: dict) -> dict:
    return {
        "type": "plan",
        "goal": f"查看 {workload.get('name', workload.get('id', 'workload'))} 的最近日志",
        "steps": [
            {
                "id": 1,
                "description": "查看最近的 workload 日志输出",
                "command": build_workload_logs_command(workload),
                "dangerous": False,
            }
        ],
    }


def build_workload_start_command(workload: dict) -> str:
    path = str(workload.get("path") or workload_remote_dir(workload)).strip()
    manifest_command = str(workload.get("command") or "").strip()
    if manifest_command:
        return f"cd {shlex.quote(path)} && ({manifest_command})"
    return (
        f"cd {shlex.quote(path)} && "
        "if [ -f launch.sh ]; then ./launch.sh; "
        "elif [ -f start.sh ]; then nohup ./start.sh > server.log 2>&1 < /dev/null & echo $! > server.pid; "
        "else echo 'No start command found' >&2; exit 1; fi"
    )


def build_workload_stop_command(workload: dict) -> str:
    path = str(workload.get("path") or workload_remote_dir(workload)).strip()
    pattern = shlex.quote(str(workload.get("path") or workload.get("dir") or workload.get("id") or "").strip())
    return (
        f"cd {shlex.quote(path)} && "
        "if [ -f server.pid ] && kill -0 $(cat server.pid) 2>/dev/null; then "
        "kill $(cat server.pid) && echo 'stopped via server.pid'; "
        f"elif [ -n {pattern} ] && pgrep -af {pattern} >/dev/null 2>&1; then "
        f"pkill -f {pattern} && echo 'stopped via pattern'; "
        "else "
        "echo 'workload not running'; "
        "fi"
    )


def detect_generic_workload_action(message: str) -> str:
    norm = normalize_text(message)
    if any(token in norm for token in ("日志", "log", "logs")):
        return "logs"
    if any(token in norm for token in ("重启", "restart", "reload")):
        return "restart"
    if any(token in norm for token in ("停止", "stop", "关闭")):
        return "stop"
    if any(token in norm for token in ("启动", "start", "拉起")):
        return "start"
    if any(token in norm for token in ("状态", "status", "运行", "检查", "查看", "端口", "健康", "health")):
        return "status"
    return ""


def build_generic_workload_plan(message: str, workload: dict) -> Optional[dict]:
    action = detect_generic_workload_action(message)
    if not action:
        return None

    verify_cmd = workload_verify_command(workload)
    start_cmd = build_workload_start_command(workload)
    stop_cmd = build_workload_stop_command(workload)
    workload_name = workload.get("name", workload.get("id", "workload"))

    if action == "logs":
        return build_workload_logs_plan(workload)
    if action == "status":
        return build_workload_status_plan(workload)
    if action == "start":
        return {
            "type": "plan",
            "goal": f"启动 {workload_name}",
            "steps": [
                {
                    "id": 1,
                    "description": "启动现有 workload",
                    "command": start_cmd,
                    "dangerous": True,
                },
                {
                    "id": 2,
                    "description": "验证 workload 已启动",
                    "command": verify_cmd,
                    "dangerous": False,
                },
            ],
        }
    if action == "stop":
        return {
            "type": "plan",
            "goal": f"停止 {workload_name}",
            "steps": [
                {
                    "id": 1,
                    "description": "停止现有 workload",
                    "command": stop_cmd,
                    "dangerous": True,
                },
                {
                    "id": 2,
                    "description": "确认 workload 已停止",
                    "command": verify_cmd,
                    "dangerous": False,
                },
            ],
        }
    if action == "restart":
        return {
            "type": "plan",
            "goal": f"重启 {workload_name}",
            "steps": [
                {
                    "id": 1,
                    "description": "停止现有 workload",
                    "command": stop_cmd,
                    "dangerous": True,
                },
                {
                    "id": 2,
                    "description": "重新启动 workload",
                    "command": start_cmd,
                    "dangerous": True,
                },
                {
                    "id": 3,
                    "description": "验证 workload 已恢复运行",
                    "command": verify_cmd,
                    "dangerous": False,
                },
            ],
        }
    return None


def build_existing_workload_plan(message: str, workload: Optional[dict]) -> Optional[dict]:
    if not workload:
        return None

    norm = normalize_text(message)
    searchable = normalize_text(" ".join(
        str(workload.get(key) or "")
        for key in ("name", "id", "dir", "command", "path", "type")
    ))
    path = str(workload.get("path") or workload_remote_dir(workload)).strip()
    if not path:
        return None

    is_minecraft_workload = any(token in searchable for token in ("minecraft", "spigot", "paper", "bukkit", "forge", "fabric"))
    wants_player_update = any(token in norm for token in ("玩家", "人数", "max-players"))
    if is_minecraft_workload and wants_player_update:
        player_count = extract_requested_player_count(message)
        if player_count is None:
            return None

        port = workload.get("port")
        verify_suffix = f"ss -tulnp | grep :{port}" if port else "pgrep -af 'java.*jar.*nogui'"
        return {
            "type": "plan",
            "goal": f"将 {workload.get('name', 'Minecraft 服务器')} 的最大玩家数修改为 {player_count} 并重启服务",
            "steps": [
                {
                    "id": 1,
                    "description": f"将 server.properties 中的 max-players 更新为 {player_count}",
                    "command": (
                        f"grep -q '^max-players=' {path}/server.properties && "
                        f"sed -i 's/^max-players=.*/max-players={player_count}/' {path}/server.properties || "
                        f"echo 'max-players={player_count}' >> {path}/server.properties"
                    ),
                    "dangerous": False,
                },
                {
                    "id": 2,
                    "description": "重启现有 Minecraft workload",
                    "command": (
                        f"cd {path} && "
                        "if [ -f server.pid ] && kill -0 $(cat server.pid) 2>/dev/null; then "
                        "kill $(cat server.pid) && sleep 5; "
                        "fi; "
                        "./launch.sh"
                    ),
                    "dangerous": True,
                },
                {
                    "id": 3,
                    "description": "验证配置已写入且服务重新运行",
                    "command": f"grep '^max-players=' {path}/server.properties && (cd {path} && ({verify_suffix} || tail -n 50 server.log))",
                    "dangerous": False,
                },
            ],
        }

    return build_generic_workload_plan(message, workload)


def rewrite_workload_path(command: str, workload: Optional[dict]) -> str:
    cmd = str(command or "")
    expected_dir = workload_remote_dir(workload or {})
    if not cmd or not expected_dir:
        return cmd
    return WORKLOAD_PATH_PATTERN.sub(expected_dir, cmd)


def apply_resolved_workload_to_result(result: dict, workload: Optional[dict]):
    if not isinstance(result, dict) or not workload:
        return result

    rtype = str(result.get("type") or "").lower()
    if rtype == "ssh" and result.get("command"):
        result["command"] = rewrite_workload_path(result["command"], workload)
        return result

    if rtype == "plan":
        updated_steps = []
        for step in result.get("steps") or []:
            if not isinstance(step, dict):
                continue
            updated = dict(step)
            if updated.get("command"):
                updated["command"] = rewrite_workload_path(updated["command"], workload)
            if isinstance(updated.get("args"), dict) and updated["args"].get("command"):
                updated["args"] = dict(updated["args"])
                updated["args"]["command"] = rewrite_workload_path(updated["args"]["command"], workload)
            updated_steps.append(updated)
        result["steps"] = updated_steps
    return result
