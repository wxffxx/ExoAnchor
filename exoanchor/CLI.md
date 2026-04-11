# ExoAnchor CLI

Use ExoAnchor from a pure terminal without opening the dashboard.

## Start the server

```bash
python3 exoanchor_cli.py serve
```

## Check status

```bash
python3 exoanchor_cli.py status
```

## Run a native skill

```bash
python3 exoanchor_cli.py skill python_system_snapshot
```

Pass skill parameters with `--param key=value`:

```bash
python3 exoanchor_cli.py skill restart_service --param service_name=nginx
```

## Ask in natural language

Single request:

```bash
python3 exoanchor_cli.py ask "检查磁盘和内存"
```

`ask` now creates a durable backend session first, then attaches to the shared runtime event stream.
That means:

- the same request model is used by CLI and dashboard
- clarifying questions show up as `waiting_input`
- dangerous plan steps can be approved from the terminal
- the run remains inspectable via `/api/agent/sessions/<id>`

Force plan generation and execute it with live progress:

```bash
python3 exoanchor_cli.py ask "部署最新版 Spigot Minecraft 服务器" --plan
```

Only inspect the generated result without executing:

```bash
python3 exoanchor_cli.py ask "部署最新版 Spigot Minecraft 服务器" --plan --dry-run
```

Auto-approve dangerous confirmation steps:

```bash
python3 exoanchor_cli.py ask "部署最新版 Spigot Minecraft 服务器" --plan --yes
```

Print the raw runtime event stream as JSON lines:

```bash
python3 exoanchor_cli.py ask "检查磁盘和内存" --jsonl
```

## Notes

- The CLI talks to the local ExoAnchor HTTP server at `http://127.0.0.1:8090` by default.
- Long LLM requests use a longer timeout automatically.
- Non-interactive privileged commands should use `sudo -S`.
