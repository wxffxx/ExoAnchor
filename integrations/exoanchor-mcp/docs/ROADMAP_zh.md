# ExoAnchor MCP 完善路线图

更新日期：2026-07-14

## 定位

ExoAnchor MCP 是 Codex 与一台已配置 ESP32-P4 KVM 设备之间的薄协议适配层。Codex 负责理解任务和选择工具；MCP 只暴露严格、可审计的观察与动作；ESP32-P4 负责设备能力、租约、权限、状态和真实执行结果。

当前路线不把“自动安装操作系统”作为最终验收，也不把不同厂商、不同版本 BIOS/UEFI 的通用零样本操作视为可靠能力。未经录制、建模和真机验收的 BIOS 页面只能由人工观察或监督式操作，不能沉淀为 autonomous playbook。

## 原则

1. 不调用 `/api/agent/*`，MCP 不成为第二个本地 Agent。
2. 先观察、后动作；每个动作都要能用独立 observation 验证。
3. 工具 schema 是执行契约，不接受模糊对象或未声明字段。
4. MCP annotations 只帮助客户端显示风险，设备端 policy gate 才是最终权限边界。
5. 默认 `supervised`；强制接管、重启、断电和任意 SSH 命令按高风险处理。
6. 不把模型判断、HTTP 200 或命令退出码单独当作任务完成证据。
7. BIOS/UEFI 自动化只允许基于明确的设备 profile、页面证据和可回退 playbook，不提供“通用 BIOS Agent”。

## MCP-0：协议与安全基线

状态：已完成。

交付：

- 固定并正确协商受支持的 MCP protocol version。
- 在 initialize 中提供服务器 instructions。
- 为每个工具声明 title、read-only、destructive、idempotent 和 open-world hints。
- 为 HID 等复合参数提供严格、按动作类型区分的 JSON Schema。
- 区分 JSON-RPC 协议错误和 `isError: true` 工具执行错误。
- 默认控制模式从 `autonomous` 改为 `supervised`。
- 禁止通过普通 MCP 参数传递 SSH 密码，优先使用设备 secret store。
- 增加配置、schema、错误和权限门禁测试。

验收：

- Codex 能稳定完成 initialize、tools/list 和只读工具发现。
- 只读工具与写工具在元数据中可明确区分。
- 未知工具、非法参数、设备拒绝和网络失败具有不同错误语义。
- 未启用写权限时，任何设备状态修改都被 MCP bridge 拒绝。

当前证据：

- MCP package 版本 `0.4.0`。
- 36 项单元、契约和本地 HTTP 集成测试通过。
- STDIO 子进程完成 initialize、initialized notification 和 tools/list 往返。
- `--list-tools` 不再要求设备地址或凭据。

## MCP-1：可信观察面

状态：工程实现完成；dev 固件真机的 capability、status、logs 已通过。当前目标没有可用 JPEG 帧，snapshot 真机项保持 blocked，不伪造通过。

交付：

- 统一设备 capability、状态、租约和目标连接状态的结构化输出。
- 截图返回时间、分辨率、内容哈希、frame ID 和视频状态关联信息。
- 增加分页日志、观察 ID 和来源信息。
- 增加 `wait_for_status` / `wait_for_frame_change`，避免高频盲轮询。
- 将设备、目标机和屏幕内容明确标记为外部 observation，不直接解释为已验证事实。

验收：

- Codex 只使用只读工具即可判断视频、HID、电源和 SSH 是否具备下一步条件。
- 相同 observation 可保存并在测试中重放。
- 黑屏、无信号、旧帧和租约冲突都有结构化状态。

## MCP-2：确定性 KVM 控制

状态：工程实现和可重放 HTTP 模拟验收完成；真实 Prototype0 尚未在固定无破坏页面发送 HID。

交付：

- `click_pixel`：根据当前 frame 尺寸转换到绝对 HID 坐标。
- `type_text`：显式键盘布局、字符范围和最大长度。
- `execute_and_observe`：动作完成后释放按键并返回新 observation。
- 动作可绑定 `expected_frame_id`、目标设备和租约 owner，前置条件不符则拒绝。
- 将 locator、普通按键、PWR、RST、force-off 和强制接管拆成不同风险等级。

验收：

- 在固定测试画面上完成可重复的点击、组合键和文本输入。
- 画面在动作前变化时拒绝旧坐标点击。
- HID 失败、连接中断或工具取消后执行 `releaseall` 并释放租约。

## MCP-3：结构化远程运维

状态：桥接层工程实现和模拟验收完成；真机固件当前 `host_key_check=false` 且 SSH target 未配置，因此真实命令按安全策略保持 blocked。

交付：

- 将阻塞式 SSH 调用改为 `job_start/status/cancel/result`。
- 使用 `secret_ref` 引用设备 secret store，不把密码或私钥放入模型参数。
- 增加主机身份/host key 策略、输出分页、artifact 和超时预算。
- 为请求和设备动作增加 idempotency key 与审计关联 ID。

验收：

- Codex断线或工具超时不会导致无法判断的后台命令。
- 重复请求不会重复提交非幂等动作。
- 命令、退出状态、输出、耗时、调用者和验证 observation 可关联查询。

## MCP-4：Codex 真机闭环

状态：配置、脚本、模拟测试和首轮只读真机记录已完成。首轮结果为 partial（4 passed / 0 failed / 1 blocked）；未取得视频帧，所以监督式真机 HID 与 SSH 没有越权执行。

交付：

- 项目级 Codex MCP 配置示例和最小权限工具 allowlist。
- 只读、监督式 HID、租约冲突、网络中断和恢复测试。
- 可重放的模拟设备与真实 Prototype0 验收记录。

验收顺序：

1. 只读状态、日志和截图。
2. 固定测试页面上的 HID 操作。
3. 人工确认后的电源与复位动作。
4. SSH 短任务和异步长任务。
5. Codex任务中断后的状态恢复与证据复查。

当前证据：

- 项目级只读与监督式 Codex 配置示例位于 `docs/codex/`。
- 可重放的本地 HTTP 设备覆盖 observe → frame-bound action → observe，以及 SSH job lifecycle。
- 只读真机验收记录生成在 `docs/acceptance/`，仅本地保留；真机捕获可能包含内网地址、设备指纹、observation ID 和运行时状态，不进入 Git。
- 真机确认 MCP enabled、HID ready、power detection on、lease available；视频为 capture idle。
- 一次非强制 KVM 视频租约试验返回 snapshot `ESP_ERR_NOT_FOUND`，随后只读确认 owner=none、active=false，租约已归还。

## 固件兼容边界

- `v0.86.2-dev`（或具备同等 API 的 dev 固件）：支持 MCP Stage 1-4 契约。
- 纯 KVM 固件：不保证 `/api/settings/mcp`、`/api/capabilities` 或 SSH API；MCP 必须报告 firmware incompatible。
- 不因设备 IP 可达就假定它运行 dev 固件；必须先读取 capability contract。

## MCP-5：基于 profile 的恢复 playbook

交付：

- 记录目标主板、BIOS/UEFI 厂商、版本、分辨率、页面和已知动作序列。
- playbook 声明适用 profile、前置 observation、每一步验证和人工接管点。
- 未匹配 profile 时只允许观察和人工监督，不猜测启动菜单或安全设置位置。

验收：

- 已知 profile 可重复完成一个低风险恢复任务。
- BIOS 版本或页面不匹配时自动停止，不进行零样本探索性点击。

## 当前非目标

- 通用 BIOS/UEFI 零样本导航。
- 以操作系统安装作为 MCP 完成标志。
- 在 MCP bridge 内实现规划、记忆或模型调用。
- 绕过 ESP32-P4 设备端权限、租约或审计。
- 让 Codex通过截图猜测密码、产品密钥或磁盘擦除授权。

## 完成规则

每个阶段必须同时具备接口规格、自动测试、错误路径、权限边界和可复查证据。真机测试必须从只读开始；任何 PWR、RST、force-off、设置写入或目标机破坏性操作仍需用户明确授权。
