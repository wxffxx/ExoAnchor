# 2026-08-20 — 2026-08-22 板载 Agent 运行时上线记录（0.87.5-dev → 0.87.6-dev）

- 时间：2026-08-20 — 2026-08-22
- 固件：`0.87.5-dev` 的一系列 Agent 运行时候选（runtime-v2、r2、r8、r9c、r10、r11、r19、r23），最后以
  `0.87.6-dev IndigoShore`（r28）收尾；ESP-IDF 5.5.5
- 硬件：ExoAnchor PrototypeV2.4（ESP32-P4 rev3.2，32 MB PSRAM，16 MB Flash），全程同一块板
- 运行时设计见 [Agent Runtime V2](../../device/ESP32P4/firmware/v0.86.6-dev/AGENT_RUNTIME_V2.md)，逐项改动见
  [固件版本历史](../../device/ESP32P4/firmware/docs/FIRMWARE_VERSION_HISTORY_zh.md)；本文记录实机验证的方法、
  数据、根因和走过的弯路
- 这些改动于 2026-08-23 以 `e6d4007d feat: integrate ExoAnchor 0.87.6 agent runtime` 合入主仓

## 1. 结论

| 项目 | 开始时（08-20） | 结束时（0.87.6-dev r28） |
| --- | --- | --- |
| Runtime V2 候选实机可用性 | 首个候选烧录后完全不可用，并发生 panic 重启 | 启动、HTTP、MCP 正常，无 panic、watchdog、OOM |
| Agent 任务栈 | 申请 49152 B 内部栈，现场最大连续内部块 29696 B，任务无法创建 | 24576 B 内部栈，真实 run 后 high-water 12448 B |
| 主执行函数 / SSH 执行函数栈帧 | 7552 B / 10592 B | 2704 B / 704 B |
| 真实云端 run | 无法启动 | UART0 闭环：云端 200，完整 result 分块重组、CRC 校验通过 |
| 对话历史 | — | 立即落盘，软件重启后保留（5208 B / 16 条） |
| Agent 运行后 H.264 | — | 120/120，编码错误 0 |
| MCP 视频观察的归属 | 被标为 Agent，generation 为 0 | `actor=mcp`、`generation=3`，按该代次人工终止成功 |
| Runtime V2 是否为唯一执行者 | 否（shadow） | 仍为否：模型和工具仍由 legacy Agent loop 执行 |

运行时逻辑在主机测试中都已通过，实机上最先挡住的是内部 RAM：Agent 任务要的内部栈比现场最大的连续内部块还大。
把栈减半、把大缓冲移到堆上之后，任务能稳定创建和回收；之后的进展主要受限于设备上没有 Agent API key 和没有
浏览器会话，于是补了 UART0 诊断入口完成真实闭环。`0.87.6-dev` 是操作者要求的补丁检查点，不代表 Runtime V2
已完成切流。

## 2. 测试方法

**身份门**：每次写入前核对物理型号、CH343 序列号、eFuse MAC、硅版本（rev3.2）、16 MB Flash 和板型配置，
与不入库的身份登记表比对；确认串口空闲，不终止其他会话的串口监视。写入后再核对一次 MAC 和硅版本。

**烧录**：使用 ESP-IDF 5.5 环境自带的 esptool 4.12.0，460800 波特率，DIO / 80 MHz / 16 MB，不整片擦除，保留
NVS 和 TF 卡。0.87.5 的候选写四段（bootloader、分区表、OTA data、应用），r28 只写 `0x20000` 的应用区。每次
写完另起一次 `verify_flash` 独立核对摘要。

**候选规则**：每个候选有一张硬件验收矩阵，全部 PASS 才允许改版本号。实机失败的候选隔离，被取代的候选作废，
两者都不再烧录；修复必须用新的候选 ID 重新走身份门。

**主机侧**：完整 `tests/host/run.sh`、Toolkit、全新的 ESP-IDF 5.5.5 构建、镜像校验和、ELF 栈帧审计，以及应用
大小与最小 4 MB 应用分区（`0x400000`）的对比。

**板上观测**：启动日志（panic、Guru、watchdog、栈溢出、OOM），匿名访问受保护 API 是否返回 401，Agent 任务栈
high-water，内部堆最大连续块，电源动作计数（用来确认只读运行没有产生物理副作用）。

## 3. 问题与根因

按发现顺序排列。

### 3.1 首个 Runtime V2 候选实机完全不可用（08-20）

现象：烧录后实际操作完全不可用；设备在记录到的启动之后又重启了一次，复位原因为 panic。

根因（源码审计）：所有 Runtime V2 写操作都被挡在 legacy 分发接缝处；只读运行调用 Verifier 时不带任何证据，
因此永远无法完成。

处理：该候选隔离。接下来的 r2 把 Task Service submit 的栈占用从约 6592 B 降到 400 B，同步 HTTP submit 调用链
约 3984 / 12288 B；但随后的 P0/P1 审计又发现恢复范围内 SSH、dry-run、截图/视频需求和存储隔离几处缺口，r2
未烧录即作废。

### 3.2 退回只读恢复版 r8

r8 采用仅驻留 RAM 的恢复组合（RAM-only recovery），功能冻结在 legacy 只读范围。实机部分通过：身份、四段写入
与校验、启动、页面资源、boot ID 稳定、匿名访问边界，以及 SYSTEM dry-run 的准入拒绝。烧录后没有活动的浏览器
会话，带认证的 Agent 与单标签页 KVM 正向测试没能进行。

### 3.3 r9c 切流：Agent 任务栈分不到内部 RAM（08-21）

r9c 启用 Runtime V2 的持久 Web 绑定，关闭 RAM-only 恢复组合；legacy 模型/工具循环保留为受限的兼容执行器，
写操作、HID、电源、SSH、UART 写仍一律拒绝。主机测试、构建、镜像校验都通过；实机启动、32 MB PSRAM 自检、TF、
以太网 100 Mbps 全双工、HTTP、匿名 401 也都通过。

现象：第一次真实的 SYSTEM 无截图 dry-run 在调用模型和工具之前就失败，run 以 failed 终态落盘，电源动作计数
保持 0。

根因：执行器用 `xTaskCreateWithCaps` 申请 49152 B 的仅内部 RAM 任务栈，而现场内部空闲 55203 B、最大连续块
只有 29696 B，返回 `ESP_ERR_NO_MEM`。

处理：r9c 隔离，不再烧录。

### 3.4 r10：栈减半，大缓冲移出任务栈

保留 cache-safe 的内部栈能力（`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`），不把任务栈挪到 PSRAM，改为：

- `AGENT_RUN_TASK_STACK` 降到 24576 B；
- 1801 B 的 `latest_message` 缓冲改从优先 PSRAM 的堆分配；
- 删掉未使用的 1801 B 调用方报告缓冲；
- 1801 B 的可操作性检查缓冲只在需要时分配；
- 必需的消息缓冲分配失败时 fail closed。

编译后 `agent_run_execute_to_json` 的栈帧从 7552 B 降到 2704 B。

实机：最大内部连续块同样是 29696 B，24576 B 的任务正常启动；连续 20 次创建、失败、安全自删除全部通过，
high-water 最低 17380 B（门槛 6144 B），最大内部块始终 29696 B，电源动作 0，没有 panic、栈金丝雀或清理任务
失败。完整的模型执行被挡住：设备上没有配置 Agent API key。

### 3.5 r11：SSH 执行函数的 10592 B 栈帧

现象：r10 实机之后做目标文件审计，发现编译后的 SSH 执行辅助函数仍有 10592 B 栈帧。这条写路径在只读候选里
是关闭的，但最终的运行时候选必须先把 SSH 配置改为堆分配。

处理：SSH 执行配置改为优先 PSRAM 的堆分配，用完安全清零凭据。

| 函数 | r9c | r11 |
| --- | ---: | ---: |
| Agent 任务 | 1072 B | 1072 B |
| 主执行函数 | 7552 B | 2704 B |
| SSH 执行辅助函数 | 10592 B | 704 B |

实机：任务正常启动和结束，high-water 17380 B；正向 run 在发出云端请求前 fail closed，原因仍是没有 API key。
页面返回 200，匿名受保护 API 返回 401，但没有认证会话。决定不为配置问题重刷，由操作者在认证后的 Settings
配置模型和 key 后再继续。

### 3.6 r19：对话历史立即落盘

改为单一、有界的历史写入任务：内容放在 PSRAM，任务使用外部栈；先 fsync 用户消息，再写助手回复；按记录 ID
幂等；能从写了一半的记录恢复；有超时上限。

实机用真实云端模型（DeepSeek）跑 dry-run，返回 200 和预期的 `HISTORY_R19_OK`；`HIST.LOG` 立即从 0 B / 0 条
变为 623 B / 2 条；写入任务外部栈 high-water 3276 B，Agent 内部栈 high-water 12416 B；Agent 之后 H.264
120/120；软件重启后 623 B / 2 条保留，认证后的 Agent 页面显示该会话的 2 条消息。

### 3.7 r23：经 UART0 完成远程 Agent 闭环

背景：r23 重启后没有活动的浏览器会话（`browser_active=0`），无法用真实浏览器身份走 HTTP；MCP 身份默认没有
`AGENT_RUN` 权限，不能拿 MCP token 冒充。

做法：把 UART0（板上 CH343/USB 本地诊断口，不对局域网暴露）作为 SYSTEM 诊断来源，只授予
`OBSERVE | AGENT_RUN`，不授予 SSH、HID、电源或审批权限。pause、cancel、abort 凭精确 run ID 即可停止；resume、
steer 还要求同一诊断来源，不能借浏览器高权限的 run 扩权。

HTTP 调试面同时在固件和主机工具中实现，并通过主机合同测试：submit 区分 202 新受理、200 幂等去重和 409
busy/冲突；稳定的 `receipt_busy` 与之后的实时 `busy` 分开；精确的 `status?run_id=`、事件游标和
pause/resume/abort/cancel/steer；客户端对矛盾的 run 别名、缺失 accepted、错误的 control 和意外 HTTP 状态
fail closed；认证凭据文件必须是权限 0600 的普通文件，使用 HTTP 必须显式加 `--allow-http`。80 端口上的 Bearer
只能用于可信局域网或安全隧道。

实机结果见 5.1。

### 3.8 r28：自动化与人工控制的边界（0.87.6-dev）

设计：电源、HID、UART、SSH、视频配置、视频硬件和系统维护统一用资源状态管理，Agent、MCP 和 SYSTEM 操作都带
actor、operation ID、run ID 和精确 generation。顶部导航显示自动化正在占用的资源；冲突的人工输入临时禁用，只读
共享的视频观察保持可用；操作者按当前 generation 精确终止，不会误停后来启动的任务。UART 和 SSH 的自动化输入
同步到 Terminal 页面并可回放，人工 Terminal 与自动化写入互斥。KVM 与 Agent/MCP 共享只读 JPEG 帧，只有 HID
输入接管才阻断人工输入。

现象：首轮 0.87.5 实机发现两处关联缺陷：MCP 的视频观察被标成 Agent，视频观察的 generation 为 0。

修复：视频观察按认证 principal 区分 Agent 与 MCP，在开始、actor 切换和释放时更新 epoch；释放后旧的长流退出，
旧 keepalive 不能再把已停止的观察复活。

版本：操作者明确要求把补丁检查点递增为 `0.87.6-dev`，取代了此前“全部门通过才能升版”的约定；这次升版不表示
Runtime V2 完成切流。

## 4. 试过但没有采用

| 方案 | 结果 | 原因 |
| --- | --- | --- |
| 把 Agent 任务栈移到 PSRAM | 未采用 | 保持 cache-safe 内部栈，不用不安全的外部栈；改为缩栈并把大缓冲移到堆 |
| 为缺少 API key 重新烧录 | 未采用 | 这是配置问题，应在认证后的 Settings 里配置 |
| 用 MCP token 做认证 HTTP Agent 实测 | 未采用 | MCP 身份默认没有 `AGENT_RUN`，不能冒充浏览器身份 |
| 系统自带的 esptool 4.7.0 | 失败结果不计入判定 | 统一使用 ESP-IDF 5.5 环境的 esptool 4.12.0 |

## 5. 最终实测

### 5.1 r23 UART0 闭环（2026-08-22）

主机侧：固件 host suite 通过，HTTP Agent CLI mock/合同 11/11，Toolkit 77/77。

| 项目 | 结果 |
| --- | --- |
| 正向 run | 云端 200，回复精确为 `UART_REMOTE_R23_OK` |
| 状态与事件 | run ID 匹配，事件游标与 `done` 终态可读 |
| 完整 result | 2206 B，base64 分块重组成功，CRC32 `d908ae85` |
| 对话历史 | `history_saved=true`、`history_pending=false`；3930 B / 12 条 → 4566 B / 14 条 |
| 资源 | Agent 内部栈 high-water 12448 B；历史写入任务外部栈 high-water 3276 B |
| 并发提交 | 第二个 submit 返回 `accepted=0,busy=1`，不生成新 run，active ID 指向当前 run |
| 精确控制 | 第二个 run 的 steer、pause、resume、cancel 均受理，终态 `aborted` |
| 错误的 run ID | 返回 `run_id_mismatch`，不泄漏其他 run 的结果 |
| Agent 之后 H.264 | 120/120，编码错误 0，SPS/PPS/IDR 齐全 |
| 网络读取 | `network show` 连续 100/100，无 cache 断言或复位 |
| 外部栈上的网络修改 | 返回 `ESP_ERR_INVALID_STATE`，当前网络未改变 |
| 软件重启 | 历史保持 5208 B / 16 条 |
| 匿名 HTTP | Agent status/events/history/sessions/requests 均为 401 |

### 5.2 r28（0.87.6-dev IndigoShore，2026-08-22）

- 应用 3041216 B（`0x2e67c0`），最小 4 MB 应用分区剩余 27%；SHA-256
  `e7bf95b20f231ed8b8abc0b298ba99c72f1798c0f0a41881469c9d95fb4e1c06`。
- 只写应用区；写入哈希与独立 `verify_flash` 一致，写后 MAC 与硅版本一致，固件自报
  `exoanchor-prototype-v2.4 / 0.87.6-dev`；设备上的 UI shell SHA-256 与本地源码一致。
- 以太网 100 Mbps 全双工，TF 已挂载，HTTP 在线，HID 就绪，UART1 115200 8N1 已初始化；启动日志无 panic、Guru、
  watchdog、栈溢出、abort、OOM。
- 认证 MCP 的能力与状态查询、1280×720 JPEG 快照通过。
- MCP 快照期间，认证浏览器读到 `actor=mcp`、`resource=video-observe`、`generation=3`、`blocks_manual=false`、
  `manual_takeover_available=true`；浏览器按该 generation 终止返回 `accepted=true`，随后活动操作为 0。

### 5.3 应用大小

| 候选 | 应用大小 |
| --- | ---: |
| 0.87.5 Settings 基线（08-20） | 2880464 B |
| r8 | 2934016 B |
| r9c | 2955104 B（`0x2d1760`） |
| r10 | 2955360 B |
| r11 | 2955408 B |
| r23 | 3008464 B（剩余约 28%） |
| r28 | 3041216 B（剩余 27%） |

## 6. 遗留问题

- Runtime V2 还不是唯一执行者：模型和工具仍由 legacy Agent loop 执行，V2 只负责 shadow 绑定、事件日志和状态
  投影。打开写操作前还缺：每类工具的目标指纹、owning manager 边界上的原子 Execution Gateway、能签发 Verifier
  证据的类型化回读，以及断电、丢响应、租约丢失、人工 KVM 抢占等故障注入。
- 认证 HTTP 下的完整 Agent 闭环（submit → events/control → result → history）还没有在设备上跑过，需要真实的
  浏览器会话。
- r23 未关闭：`include_screenshot=true` 被拒且 KVM 所有者不变；SSH TOFU 内部 broker 实机；网络应用后的 120 s
  内部回滚；静态 IP 的 ACD 冲突/超时回滚；功能关闭后的持久化、重启和 UART 早期拒绝。
- r28 未关闭：浏览器 KVM 与自动化同时输入及 neutral 的完整矩阵，Terminal 自动输入回放与故障注入，H.264 恢复，
  长时间 soak。
- 写工具、HID/租约抢占以及 M0、T1、T2 发布门均未完成；`0.87.6-dev` 不是 RC 或 Stable。
- 2026-10-23 起，内置 Agent 的写操作改由操作者设定的自动化等级（L0–L4，默认 L1 只读）加固件判定的动作类别
  控制，不再一刀切拒绝；Runtime V2 仍是 shadow，切流硬门不变。
