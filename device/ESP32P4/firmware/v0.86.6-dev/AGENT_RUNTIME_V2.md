# ExoAnchor Agent Runtime V2

> 状态：2026-08-22 r23。Runtime V2 安全内核、持久 shadow binding 与兼容投影
> 已启用；UART0 Agent 调试闭环已在实机通过，HTTP 固件/CLI host 合同已通过，
> 但认证 HTTP 与 Agent/KVM HIL 仍未完成。当前 provider/model/tool execution
> owner 仍是 legacy Agent loop；Runtime V2 尚未从 shadow 切为唯一 canonical
> execution owner。本文不是 owner 切流、M0、T1/T2 或完整 HIL 通过声明。

## 架构决定

ExoAnchor 不 fork、裁剪或嵌入 DeepSeek Harness。固件实现一个紧凑、静态、
ESP32 原生的 Runtime V2；DS Harness 与 Codex 以后都只是外部验收客户端。

复用 DeepSeek Harness 的语义，而不是其 Node/TypeScript 运行时：

- append-only canonical event log；
- 明确的 Thread、Turn、RunAttempt、Plan、Step 边界；
- proposal、guard、approval、execute、verify 分层；
- 模型视图、UI 投影和 canonical state 分离；
- 副作用开始前 durable append；
- 崩溃后的 `outcome_unknown`，禁止盲目重放。

设备继续拥有不可委托的安全面：认证、capability、来源 authority ceiling、
工具策略、Request Broker、control lease、人工 KVM 抢占、secret reference、
HID/Power/UART/SSH/Video 等 owning manager，以及最终 Verification。

下图描述目标切流结构，不是 r23 当前调用图。r23 的 Web/UART submit 仍进入 legacy
run manager，Task Service 接收 shadow binding、事件、控制和 status projection。

```text
Web / QQ / MCP / Codex / DS Harness
              │  transport adapter
              ▼
      Agent Task Service
 Thread → Turn → RunAttempt → Plan(version) → Step
              │
              ├── TASKS.LOG (canonical, hash chained, fsync)
              │
              ▼
 Request Broker → Execution Guard → Execution Gateway [尚未开放写切流]
              │
              ▼
 owning manager → typed evidence → independent Verifier
```

## 设备端事实模型

### Task Runtime

`main/core/agent_task_runtime.{h,c}` 是无 I/O 的确定性状态机：

- 一台设备最多一个 active Turn、八个跨 Thread FIFO pending Turn；
- 每个 Turn 有独立 Thread ID、Turn ID、RunAttempt ID；
- Plan 有单调 version；steer 同时提升 intent revision 和 Plan version，旧 Plan
  立即失效；
- Step `completed` 与 Turn `completed` 都需要 Verification PASSED；
- pause、resume、cancel、interrupt、fail、retry 和 outcome unknown 都由合法转移表
  约束；
- 物理动作结果不明时进入 terminal `OUTCOME_UNKNOWN`，不能自动 retry。

`main/application/agent_task_service.{h,c}` 是目标态唯一 transport-neutral
application service，但 r23 仍处于 shadow 阶段：Web/UART 由 legacy run manager
执行，Task Service 接收绑定、事件和控制镜像。未来 QQ adapter 应直接调用 Task
Service；T1 MCP 继续作为外部 Agent 到 KVM Core 的薄 adapter，不调用 Embedded
Agent Runtime，也不得创建第二套任务状态。

### Canonical Event Store

`main/infrastructure/agent_event_store.{h,c}` 将事件追加到
`/EA/AGENT/TASKS.LOG`：

- JSONL v1 固定 envelope，64 位单调 seq；
- payload SHA-256、previous hash 与 event hash 组成 hash chain；
- 单条、payload、日志总量均有硬上限；
- `fflush → fsync → fclose` 全部成功后才推进内存 cursor；
- replay 明确区分 clean、empty、tail truncated、corrupt 和 I/O error；
- 模型可见 Task 状态必须能由该日志重建；旧 `HIST.LOG` 不是 Runtime V2 真相。

事件只保存有界 hash/元数据，不保存 raw goal、page context 或 steer 文本。
主要事件包括：

```text
turn.submitted / turn.phase_changed / turn.steered
plan.started / step.added / step.status_changed
action.started / action.result / action.failed / action.verified
turn.completed / turn.failed / turn.cancelled / turn.interrupted
turn.outcome_unknown
```

所有会把 Turn 变成 `OUTCOME_UNKNOWN` 的 Step/Verification mutation 都必须直接
写 canonical `turn.outcome_unknown`，而不是只留下一个非终态 Step 事件。

### 重启恢复

- `action.started` 没有 authoritative `action.verified` 时，重启后该 Turn 是
  `outcome_unknown`；`action.result` 或工具返回成功不能消除未知状态；
- action 跟踪容量溢出保留 fail-closed tombstone，不会退化为 interrupted/retry；
- 没有开始副作用的开放 Turn 才能恢复为 `interrupted`；
- legacy `RUN.CHECKPOINT` schema v1 不再续跑。有效旧 checkpoint 一次性迁移为
  durable `turn.outcome_unknown` 后按精确 job identity 清除；
- malformed legacy checkpoint 只清理，不重放 raw goal/page context。

## Request、执行与完成

### Request Broker V2

`main/application/agent_request_broker.{h,c}` 保存八个并行 request slot。每个
request 绑定：

```text
device / thread / turn / run / plan version / step
canonical args hash / idempotency key
requester / reviewer / decider
risk / grant scope / TTL / expected effect / verification requirement
```

grant 必须原子、一次性 consume；过期、取消、错误 actor、参数或 Plan 漂移均
fail closed。重启 snapshot import 会把 EXECUTING/VERIFYING 变成 UNKNOWN，把旧
GRANTED 变成 EXPIRED。Turn 已 durable terminal 后才允许 `retire_run` 回收槽，
terminal tombstone 的事实仍保留在 TASKS.LOG。

### Execution Guard

`main/core/agent_execution_guard.{h,c}` 计算：

```text
effective capability = authenticated principal capability
                     ∩ origin authority ceiling
```

执行前必须同时满足 exact binding、未过期、source auth generation 未变、
policy revision 未变、lease epoch 未变、人工 KVM 未抢占、一次性 grant 已
consume，以及 authorization core 的最终决定。任何后续层都不能把 deny
改回 allow。

当前兼容 dispatcher 明确只开放 low-risk、无 lease、非 manager plan 的只读调用；
HID、Power、UART/SSH 写、memory write、manager plan/execute 等写操作全部返回稳定
拒绝。原因是完整写切流还缺少 tool-specific target fingerprint 与 owning manager
内部的原子 `rebind → guard → durable started → effect` 边界。不能用一次 Browser
批准或本机 device ID 代替真实目标绑定。

### Independent Verifier

`main/core/agent_verifier.{h,c}` 拒绝 tool return、HTTP 200、进程退出码和模型
声明。可接受 evidence 必须具备：

- authoritative issuer：device manager、device observer 或 artifact store；
- exact action ID、target identity、completion-criteria SHA-256；
- manager/observation generation；
- 严格晚于 action started 的 captured time；
- artifact evidence 使用严格 64 字节小写 SHA-256；
- mutation 可强制只接受 fresh readback。

Web 兼容 loop 当前没有可信 evidence issuer，因此不会把模型 JSON 或
`tool_results` 自行包装成 PASSED。mutation 执行结果若缺少 readback，进入
`OUTCOME_UNKNOWN`；其他缺少 typed evidence 的结果不能完成 Turn。

### HID report authority

HID manager 是键盘、相对鼠标和绝对指针 report 的唯一 authority owner。当前
owner token 绑定 owner kind、认证 session/generation、producer resource、lease
epoch 与 manager generation；token 由 manager 签发，调用方不能根据当前状态重建。

- control lease、人工 KVM stream、embedded Agent run 与 boot-key plan 各自持有
  exact token；execute、release 和 cleanup 都必须提交原 token；
- 换主、降级、释放、自然过期、认证撤销和 stream 断开统一执行
  `fence → drain active command → owner-bound neutral → install new owner`；
- neutral report 未全部成功时进入 `neutral_pending`，新 owner 不能发送非零
  report；USB 重新 mount 后也必须先完成 neutral；
- KVM WebSocket context 固定 session ID、auth generation、stream ID 与 owner token，
  每帧在同一 authority gate 内复核 live auth 和 current token；旧 socket close 只能
  compare-and-revoke 自己的 token；
- auth/lease/video manager 的 50 ms maintenance 会物化 expiry 并触发同步 revoke，
  不依赖下一次 getter；lease cleanup 暂时失败会进入 pending retry；
- legacy ambient execute/release 不得读取“当前 owner”。没有 task-bound exact token
  的 producer 一律 fail closed，避免旧 Agent/boot-key cleanup 误释放新 producer。

## Adapter 和权限

- Web submit 的 `thread_id`/legacy `session_id` 只是 conversation ID；认证 principal、
  device-generated auth session ID 与 generation 只能来自 HTTP auth context；
- Task Service 保存 source auth binding 与 origin authority ceiling；TASKS.LOG 只写
  auth ID hash，不写 raw session ID；
- Broker 在 request create、grant consume 和 Guard 前都以 exact session ID +
  generation 重新查询 auth service；Guard 使用实时 capability 与 origin ceiling
  求交，不使用 submit 时快照冒充实时权限；
- QQ 的未来 ceiling 固定为 `OBSERVE + AGENT_RUN`，QQ 不能审批或持有 lease；
- MCP/DS Harness/Codex 不能提供 principal、session、device identity、capability、
  reviewer 或 approval result；
- `/api/agent/run/events` 保留 legacy event projection，同时提供 canonical
  `task_events`、64 位 cursor、recovery status 和截断信息。

## 迁移边界

以下替换已在 V2 状态机与 host 合同层成立，但尚未完成生产 execution-owner cutover：

- singleton task truth、伪 Thread/Plan/Step；
- checkpoint 重跑恢复；
- `ESP_OK`/`ok:true` 即 DONE；
- tool call success 即 verified；
- Request Broker 单槽；
- TASKS.LOG 以外的 Runtime V2 canonical state。

暂时保留的兼容层：

- 旧 Web status/result JSON projection；
- 现有 model provider、prompt builder 和 tool dispatcher；
- `HIST.LOG` 仅用于旧对话展示，不参与 V2 恢复或授权。

r23 的 legacy compatibility modules 仍拥有 live provider/model/tool execution；这是
显式迁移债务，不能写成已删除或已失去 owner。只有在新 registry、model adapter、
typed observation、Execution Gateway 与 cutover HIL 完成后，Runtime V2 才能成为
唯一任务真相，并删除旧 loop。

## 验收矩阵

| 门 | 当前证据 | 状态 |
|---|---|---|
| 纯 Task 状态机、1+8 FIFO、非法转移 | host C tests | 已实现 |
| Step/Turn 独立 Verification gate | host C tests | 已实现 |
| hash-chain TASKS.LOG、replay、尾损坏分类 | host C tests | 已实现 |
| append-before-effect 的 event API | static contract + build | 已实现 |
| media sync 后才提交 cursor | host test + firmware build | 已实现，仍需断电 HIL |
| legacy checkpoint fail-closed 迁移 | host contract tests | 已实现 |
| unknown terminal event 重启不泄漏 open slot | Task Service host tests | 已实现 |
| Broker exact consume、TTL、actor、safe retire | host C tests | 已实现 |
| Web conversation/auth identity 分离 | Task Service host + static contract | 已实现 |
| session revoke/expiry/generation 在 Broker/Guard 实时失效 | static contract + build | 已实现 |
| HID exact owner token、租约/KVM/auth revoke 与 neutral fence | pure C + static contracts + firmware build | 已实现，仍需抢占/掉线 HIL |
| embedded Agent/boot-key stale cleanup 不影响新 owner | pure C owner test + exact-token wiring | 已实现，仍需 HIL |
| 模型/tool return 不能自发 evidence | verifier tests + static negative gate | 已实现 |
| tool-specific target fingerprint | 无 | 未实现，写切流关闭 |
| manager-boundary atomic Execution Gateway | 无 | 未实现，写切流关闭 |
| typed manager observation/readback | 无 | 未实现 |
| Broker snapshot durable storage wiring | snapshot API only | 未实现 |
| UART0 Agent exact-run 闭环 | r23 V2.4 实机 submit/events/status/result/steer/pause/resume/cancel | 调试入口通过，不替代完整 HIL |
| HTTP exact API 与主机 CLI | firmware contract + mock/host CLI tests | host 通过；authenticated device HIL 未验收 |
| execution-owner cutover | V2 shadow binding；legacy loop 执行 | 未完成 |
| 精确板卡身份、掉电/抢占/lease/HID HIL | r23 身份、烧录、verify、UART Agent、历史重启保留与 post-Agent H.264 已过；认证 HTTP/KVM、lease/HID 抢占仍缺证据 | 部分通过 |
| T1 真实 Codex + DS Harness + 同一 MCP/Skill | 未运行 | 未验收 |
| T2 QQ durable inbox/outbox/Gateway | 未实现 | 未验收 |

## 2026-08-21 r9c → r11 实机结果

- r9c 完成精确身份、四段烧录和独立 verify，但第一条 SYSTEM dry-run 因请求
  49152 B 内部栈、现场最大连续块仅 29696 B 而 `ESP_ERR_NO_MEM`；已隔离。
- r10 保持 cache-safe 内部栈，把任务栈降到 24576 B 并堆化大消息缓冲；同一
  实机连续 20 次创建/终止/安全自删均通过，最小 high-water 17380 B，最大
  内部连续块稳定为 29696 B，power actions 始终为 0。
- r11 进一步把 SSH exec 的 10592 B 配置栈帧改为安全清零的堆分配，编译帧降到
  704 B。候选 `0.87.5-v24-dev-agent-runtime-stackfit-r11-20260821`，application
  SHA-256 为
  `5dc17204e97d66e4b625303448d5d1d2e6b7c51ad46d331f3c4316528eeb79bb`。
- r11 已在受控身份登记表中精确核验的 Prototype V2.4 / rev3.2 / 16 MiB
  实机上通过四段写入、独立 verify、启动；逐板 CH343 serial 与 eFuse MAC 不进入
  公开仓库，并继续在受控实机证据中核对。
  PSRAM/TF/Ethernet/HTTP、匿名 401 和 Agent task 分配/回收均通过。完整正向 run 当前在
  provider 请求前 fail closed：设备没有配置 Agent API key。
- 版本继续保持 `0.87.5-dev`。只有配置 key 后的只读正向/拒写/cancel/replay 与
  认证 Agent/KVM HIL 全过，才允许推进 `0.87.6-dev`；不得据此跳到 0.88。

## 2026-08-22 r23 远程调试实机结果

- 候选 `0.87.5-v24-dev-agent-remote-r23-20260822`，application SHA-256
  `8c327728d8db373322a8a0a8d29b1ff3afbb6b8ffa2cbfdbbd1ecaac6791d7ad`；
  精确 V2.4 身份、四段写入、独立 verify、启动、TF/Ethernet 均通过。
- UART0 正向 run 返回 cloud 200 与 `UART_REMOTE_R23_OK`；完整 result 分块/CRC、
  durable history、busy receipt、exact steer/pause/resume/cancel、wrong-run mismatch、
  H.264 120/120、network-show 100/100 和软件重启历史保留均通过。
- HTTP exact API、receipt/cursor 与 host CLI 合同已通过代码/主机测试；设备当前没有
  active Browser session，authenticated HTTP HIL 仍 open。UART0 是物理诊断边界，
  不是 LAN 端口，且只授予 SYSTEM `OBSERVE | AGENT_RUN`，不授予审批或高风险工具。
- r23 仍由 legacy Agent loop 执行；Runtime V2 是 shadow/journal/projection。该结果
  不关闭 owner cutover、写工具、M0/T1/T2，也不授权版本递增。

## 2026-08-22 r28 自动化与人工控制边界实机结果

- 开发版本已按操作者明确要求递增为 `0.87.6-dev`；这只是补丁检查点，不表示
  Runtime V2 已完成 execution-owner 切流，也不关闭 M0、T1、T2 或 0.88 门。
- 电源、HID、UART、SSH、视频配置/硬件和维护操作统一投影 actor、operation ID、
  run ID 与 generation；全局 Web 提示、冲突人工输入禁用和精确人工终止的完整
  host suite 通过，V2.4 ESP-IDF 5.5.5 fresh build 通过。
- 首轮 `0.87.5-dev` 实机 MCP snapshot 暴露了视频观察被误报为 Agent 且 generation
  为 0 的问题。r28 将视频观察绑定认证 principal，并在开始、actor 切换、释放时
  更新 epoch；释放会使旧长流退出，不能再由 keepalive 复活。
- 最终应用 SHA-256
  `e7bf95b20f231ed8b8abc0b298ba99c72f1798c0f0a41881469c9d95fb4e1c06`，大小
  3041216 B。精确 V2.4 身份门、应用区写入、独立 verify、写后 MAC、启动、TF、
  Ethernet、HID、UART、HTTP 与 MCP 均通过；NVS、TF、bootloader、分区表和 OTA
  data 未改写。
- 最终 MCP snapshot 取得 1280x720 JPEG；认证 Browser 在操作期间读到
  `actor=mcp/resource=video-observe/generation=3/blocks_manual=false`，随后用相同
  generation 人工终止成功，活动操作回到 0。该证据关闭本次 actor/generation
  缺陷，但不替代同机 KVM 双消费者、输入抢占/neutral、Terminal 自动输入回放、
  H.264 恢复和长期 soak 的完整矩阵。

## 生产切流硬门

以下全部完成前，不允许打开 Runtime V2 写操作：

1. 每类工具冻结真实 target fingerprint 和 resource/config generation；
2. 唯一 Execution Gateway 在 owning manager 边界完成原子复核与副作用；
3. typed observation/readback 能签发 verifier evidence；
4. crash-before-effect、crash-after-effect、丢响应、重复 idempotency、grant 过期、
   steer、lease 丢失、人工 KVM 抢占、取消和 TF 故障注入全部通过；
5. 核对物理型号、CH343 serial、eFuse MAC、silicon revision 后完成 HIL；
6. T1/T2 按 canonical 集成门分别验收，不以 mock 或单个成功样例替代。

操作录制与鼠标宏仍是 HID/control-lease 稳定后的 P3 工作，不属于本次 Runtime
V2 或 0.87 完成门。
