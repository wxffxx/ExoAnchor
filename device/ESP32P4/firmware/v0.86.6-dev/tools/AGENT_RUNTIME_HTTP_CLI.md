# Agent Runtime HTTP 闭环客户端

`agent-runtime-http.py` 是主机侧、非交互式的 Agent 调试入口。它复用设备现有认证和 capability 检查，不绕过 Runtime 的精确 `run_id`、控制检查点、请求审批和历史存储边界。

## 安全边界

- 密码和 bearer token 只能通过 `--password-file` 或 `--token-file` 输入，不能作为命令行参数输入，也不会写入输出。
- 凭据文件必须是普通文件、不能是符号链接，并且权限不能开放给 group/others；推荐 `chmod 600`。
- `--password-file` 会执行 `/api/auth/login` 及异步 login-status 轮询，得到 browser principal；返回 token 只保留在当前进程内。
- 完整访问必须使用 browser principal。`--token-file` 必须只有一行有效的 browser-session bearer token；MCP token 默认会被 Agent HTTP capability/principal 门以 `403` 拒绝，尤其不能作出请求审批或清空 history。这个客户端不会把 MCP principal 提升成 browser principal。
- HTTPS 默认验证证书，可用 `--ca-file` 指定设备 CA。当前固件只有 HTTP 时，必须显式添加 `--allow-http`，且只能在可信隔离局域网中使用。
- `submit` 默认 dry-run；只有显式 `--execute` 才请求 policy-authorized execution。服务端仍按会话 capability 和 Agent 工具策略做最终裁决。
- `status/pause/resume/abort/cancel/steer/events/watch` 都要求精确 `--run-id`。`status` 与 `watch` 实际调用 `GET /api/agent/run/status?run_id=<exact>`，设备不会返回其他 retained run；客户端仍会二次拒绝 ID 不一致的响应。
- session ID 必须由 1–48 个 ASCII 字母、数字、`_` 或 `-` 组成。`history append --record-file` 中若携带 `session_id`，它必须与命令行精确一致；发送前始终以命令行 session 强绑定。
- `history clear` 和 `sessions delete` 带有客户端侧精确确认门；设备端授权仍然生效。

准备凭据文件：

```sh
install -m 600 /dev/null ~/.config/exoanchor/admin.password
${EDITOR:-vi} ~/.config/exoanchor/admin.password
```

## 最小闭环

下面示例假定当前设备只提供 HTTP。不要把密码、token 或目标机密写进 shell 参数或 Agent prompt。

```sh
CLI=tools/agent-runtime-http.py
DEVICE=http://192.0.2.88
PASSWORD_FILE=$HOME/.config/exoanchor/admin.password

python3 "$CLI" --base-url "$DEVICE" --allow-http \
  --password-file "$PASSWORD_FILE" --pretty capabilities

python3 "$CLI" --base-url "$DEVICE" --allow-http \
  --password-file "$PASSWORD_FILE" --pretty \
  sessions create --title "HIL debug"

python3 "$CLI" --base-url "$DEVICE" --allow-http \
  --password-file "$PASSWORD_FILE" --pretty \
  submit --session-id s01234567 --message-file /tmp/exoanchor-agent-prompt.txt \
  --idempotency-key hil-s01234567-turn-001 --execute --screenshot

python3 "$CLI" --base-url "$DEVICE" --allow-http \
  --password-file "$PASSWORD_FILE" \
  watch --run-id r01234567 --watch-timeout 300

python3 "$CLI" --base-url "$DEVICE" --allow-http \
  --password-file "$PASSWORD_FILE" --pretty \
  history list --session-id s01234567
```

`submit` 输出中的 `submitted_run_id` 是后续命令必须使用的精确 ID。闭环调试和任何可能重试的提交都必须显式使用稳定的 `--idempotency-key`，并在同一逻辑提交的所有重试中复用它。未提供时客户端生成的随机键只适用于确认不会重试的一次性调用，不能提供跨进程去重。

幂等范围还绑定认证 source/session，不只绑定 key。每次以 `--password-file` 启动新进程都会重新登录并创建新的 browser session，因此跨进程复用稳定 key 不保证命中原 source scope；反复登录还可能通过会话 LRU 淘汰原 run 的 source session。需要可靠跨调用去重时，应在授权生命周期内复用同一个有效 browser-session token，通过 mode-600 的 `--token-file` 调用；MCP token 不能替代它。本客户端不会导出或写盘登录 token。

成功 submit 响应同时包含两个时间点的数据，不能混为一谈：`receipt_busy` 是 `agent_run_submit()` 作出接收/去重决定时的原子快照，`active_run_id` 只由它决定；`busy` 是稍后生成响应时的实时状态投影。两次采样之间 run 可能结束或 retained 槽可能被替换，因此二者合法地可以不同。

客户端接受三种成功 receipt：新任务使用 HTTP `202`，必须是 `accepted=true, deduplicated=false, queued=false, receipt_busy=false, active_run_id=""`；稍后的实时投影通常已经看到新任务运行，所以 `busy` 可以为 `true`。相同稳定 key 命中提交时仍活跃的原任务使用 HTTP `200`，必须是 `accepted=true, deduplicated=true, queued=false, receipt_busy=true, active_run_id=submitted_run_id`；即使实时投影已经看到它结束、`busy=false`，这个 receipt 仍然有效。命中提交时已经终态的 retained run 也使用 HTTP `200`，但必须是 `receipt_busy=false, active_run_id=""`。

若二次状态投影无法再精确关联原 run，固件返回 `status_retained=false`，并可能不带 `run_id/job_id`；这不会推翻稳定的 `submitted_run_id/receipt_busy/active_run_id` 回执。任何出现的 `run_id/job_id` 都必须与 `submitted_run_id` 一致。对 exact-run 命令，响应中出现的 `run_id/job_id/submitted_run_id/controlled_run_id` 必须全部非空且彼此等于请求 ID，不能用“第一个匹配的别名”掩盖另一个矛盾别名。控制成功还必须明确返回 `accepted=true` 和正确的 `control`；`cancel` 是 `abort` 的 CLI 别名，因此设备回执为 `control=abort`。缺字段、HTTP 状态错误或相互矛盾的成功响应会以退出码 `3` 拒绝，不能被误当成已受理。

## 命令与 HTTP 面

| CLI | HTTP API | 说明 |
|---|---|---|
| `capabilities` | `GET /api/capabilities` | 读取设备和产品能力 |
| `submit` | `POST /api/agent/run` | 提交任务，默认 dry-run |
| `status` | `GET /api/agent/run/status?run_id=...` | 读取一个精确 run；`--run-id` 必填 |
| `events` | `GET /api/agent/run/events` | 从 `--after-seq` 读取当前 RAM event ring |
| `watch` | events + status + requests | 输出 JSONL，终态退出，等待请求时同时显示队列 |
| `pause/resume/abort/cancel` | `POST /api/agent/run/<action>` | 精确控制当前 run |
| `steer` | `POST /api/agent/run/steer` | 在安全检查点合并不超过 512 字节的新要求 |
| `history list/append/clear` | `/api/agent/history*` | 读取、追加或清空历史 |
| `sessions list/create/delete` | `/api/agent/sessions*` | 管理 Thread/session |
| `requests list/approve/cancel` | `/api/agent/requests*` | 查看和处理设备拥有的一次性请求 |

查看每个子命令参数：

```sh
python3 tools/agent-runtime-http.py --help
python3 tools/agent-runtime-http.py --base-url https://device.example \
  --password-file ~/.config/exoanchor/admin.password submit --help
```

## 闭环判定与退出码

`watch` 每行输出一个 JSON 对象：`event`、`task_event`、`status`、`requests` 或 `timeout`。这使主机自动化可以持续收集证据，同时在 `waiting_request` 时通过另一条显式 `requests approve/cancel` 命令决定是否继续。

- `0`：HTTP 操作成功，或 watch 到达 `done`。
- `1`：认证、网络或非结构化 HTTP 错误。
- `2`：命令参数或本地安全门错误。
- `3`：设备结构化拒绝、submit/history 响应契约缺失或矛盾、精确 run 不匹配，或 watch 到达 `failed/aborted/outcome_unknown`。固件返回的结构化 `409`（busy、run mismatch、idempotency conflict）会原样作为 JSON 输出，不会丢成通用文本错误。
- `4`：请求或 watch 超时。

典型控制：

```sh
python3 "$CLI" --base-url "$DEVICE" --allow-http \
  --password-file "$PASSWORD_FILE" \
  steer --run-id r01234567 --message-file /tmp/exoanchor-agent-steer.txt

python3 "$CLI" --base-url "$DEVICE" --allow-http \
  --password-file "$PASSWORD_FILE" --pretty \
  requests list --run-id r01234567

python3 "$CLI" --base-url "$DEVICE" --allow-http \
  --password-file "$PASSWORD_FILE" --pretty \
  requests approve --request-id q01234567 --response-file /tmp/exoanchor-answer.txt

python3 "$CLI" --base-url "$DEVICE" --allow-http \
  --password-file "$PASSWORD_FILE" \
  abort --run-id r01234567
```

注意：当前 events 接口只保证当前 run 的 RAM ring 回放，返回 `history_lost:true` 时不能把缺失事件解释为“动作没有发生”。`latest_seq` 表示读取时 ring 中可见的最新序号；`next_after_seq` 只前进到本次响应中最后一个完整序列化的事件。若响应返回 `truncated:true`，调用方应从该 `next_after_seq` 继续读取，不能直接跳到 `latest_seq`，否则可能永久跳过因内存不足未写入本次 JSON 的事件。最终闭环证据应组合 `watch/status`、对应 session 的 history、设备日志以及目标主机的实际观测。

## 主机测试

测试使用本地 HTTP mock，不需要实机，也不会接触真实凭据：

```sh
python3 tests/host/test_agent_runtime_http_cli.py
```

它覆盖异步密码登录、token 认证、全部命令路由、exact-status 查询、watch JSONL、结构化 404/409、严格 submit receipt、活跃到终态及 retained 替换竞态、矛盾 run ID aliases、严格控制回执、session/history 绑定、凭据不出现在 stdout/stderr、HTTP 显式 opt-in 和凭据文件权限门。
