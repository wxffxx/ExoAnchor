# ExoAnchor MCP Controller

本包把 ExoAnchor ESP32-P4 KVM 作为 stdio MCP 服务器提供给 Codex 等客户端，当前包版本为 `0.4.0`。

它与固件内置 Agent 相互独立：

- 不调用 `/api/agent/*`。
- 所有工具名使用 `exoanchor_` 前缀。
- HID 控制使用独立的 `mcp` 控制租约 owner。
- 设备设置中的 MCP Controller 开关可以阻止外部控制。
- 除只读观察外，所有写操作默认关闭，必须显式设置 `EXOANCHOR_ALLOW_WRITE=1`。

通用零样本 BIOS 导航与操作系统安装不在当前验收边界内。

## 工具

| 工具 | 类型 | 用途 |
| --- | --- | --- |
| `exoanchor_capabilities` | 只读 | 读取固件与桥接器能力 |
| `exoanchor_status` | 只读 | 读取设备、视频、HID、电源、SSH 与租约状态 |
| `exoanchor_snapshot` | 只读 | 获取带时间、尺寸、哈希与 frame ID 的 JPEG 观察 |
| `exoanchor_logs` | 只读 | 对一次缓存日志观察做稳定分页 |
| `exoanchor_observation_get` | 只读 | 重放本次服务器生命周期中的近期观察 |
| `exoanchor_wait_for_status` | 只读 | 有界等待一个命名状态条件 |
| `exoanchor_wait_for_frame_change` | 只读 | 有界等待 JPEG 哈希变化 |
| `exoanchor_open_kvm` | 只读 | 返回人工 KVM 页面地址 |
| `exoanchor_uart_status` | 只读 | 读取目标 UART 初始化、波特率、计数器、journal cursor 与人工 Terminal ownership |
| `exoanchor_uart_read` | 只读 | 按十进制 cursor 非破坏读取有界 UART journal |
| `exoanchor_uart_write` | 写入 | 经设备策略与受监督租约向目标 UART 写入精确、非敏感的有界文本 |
| `exoanchor_uart_authenticate` | 写入 | 仅在 UART 尾部精确匹配密码提示时，通过设备本地凭据引用完成登录或 sudo；凭据不进入 MCP 参数或回执 |
| `exoanchor_ssh_bootstrap_from_uart` | 写入 | 从已认证 UART 实测目标地址，在板内复用 KVM 控制台凭据配置 SSH、启用自启动并独立验证；参数与结果不含密码 |
| `exoanchor_uart_baud` | 写入 | 在设备报告的 primary/fallback 波特率之间切换 |
| `exoanchor_console_login` | 写入 | 基于本次 MCP 生命周期中的近期截图，使用设备本地 `console://default` 凭据完成 KVM 用户名/密码输入；工具参数和回执均不含凭据 |
| `exoanchor_click_pixel` | 写入 | 基于指定 frame ID 点击像素坐标 |
| `exoanchor_type_text` | 写入 | 基于指定 frame ID 输入可打印的美式键盘文本 |
| `exoanchor_execute_and_observe` | 写入 | 执行有界 HID 批次、清理按键并重新观察 |
| `exoanchor_control_lease` | 写入 | 获取或释放 MCP HID 控制租约 |
| `exoanchor_hid_actions` | 写入 | 执行严格校验、受监督的 HID 动作批次 |
| `exoanchor_power_action` | 写入 | 执行电源、复位、强制关机或定位灯动作 |
| `exoanchor_video_lease` | 写入 | 切换预览或 KVM 视频所有权 |
| `exoanchor_ssh_job_start/status/cancel/result` | 混合 | 管理结构化、有界、可分页的 SSH 作业 |
| `exoanchor_ssh_exec` | 写入 | 任意 SSH 命令兼容入口，默认额外禁用 |

实际工具与输入 schema 以程序输出为准：

```bash
python3 -m exoanchor_mcp.server --list-tools
```

## 安全契约

- 服务器协商 MCP `2025-06-18` 协议，并为每个工具声明只读、破坏性、幂等性和开放世界提示。
- 写工具只有在 `EXOANCHOR_ALLOW_WRITE=1` 时可用；提示信息不能替代设备侧策略。
- HID 与控制租约默认使用 `supervised`，不会默认进入自主模式。
- HID 动作按类型严格校验，未声明字段会被拒绝。
- UART 写入和波特率切换同时受 `EXOANCHOR_ALLOW_WRITE`、设备 MCP 工具开关与
  control lease 保护；人工 UART Terminal 在线时会拒绝自动写入。
- UART journal 使用设备返回的十进制 cursor 非破坏读取；有界等待发生在
  MCP bridge，不会占住设备 HTTP task。疑似密码、Token、API Key 或
  Authorization 内容会在写入前拒绝，回执只返回载荷哈希而不回显原文。
- UART 登录或 sudo 只允许 `exoanchor_uart_authenticate` 在 journal 尾部精确匹配
  可打印的 password prompt 后引用板内凭据；实际 secret 不进入参数、回执或
  普通 UART journal，且该动作要求设备为完全访问模式。
- 密码和私钥不能作为工具参数传入；凭据必须保存在设备密钥存储中。
- 控制台登录只接受 `console://default` 引用和本 MCP 进程生成的近期截图 observation ID；用户名、密码或通用 secret 字段不在 schema 中，设备回执必须确认 `secret_redacted=true`。
- `manual` 权限模式会在 HID 执行前拒绝控制台登录；演示自动登录前由浏览器操作员显式选择 `full`，结束后按需要恢复 `manual`。MCP 不得自行修改权限模式。
- 像素点击和文本输入必须携带当前 JPEG 的准确 frame ID；过期画面会在申请租约前被拒绝。
- SSH 结构化作业使用幂等键、本地私有审计记录和有界固件请求。取消是协作式的；桥接器重启时，运行中作业标记为 `interrupted`，不会猜测远端结果。
- 当固件报告 `host_key_check=false` 时，SSH 默认拒绝执行；只有人工监督下显式开启例外才可继续。
- 未知 BIOS/UEFI 页面必须先观察并由人监督，不能直接零样本操作。
- 强制接管租约、强制关机和任意 SSH 都需要用户针对该动作的明确确认。

## 配置

| 环境变量 | 默认值 | 含义 |
| --- | --- | --- |
| `EXOANCHOR_BASE_URL` | 必填 | 设备地址，例如 `http://<设备地址>` |
| `EXOANCHOR_USERNAME` | 未设置 | 本地设备用户名 |
| `EXOANCHOR_PASSWORD` | 未设置 | 本地设备密码 |
| `EXOANCHOR_PASSWORD_FILE` | 未设置 | 保存本地设备密码的文件，优先于命令行内嵌 |
| `EXOANCHOR_TOKEN` | 未设置 | 可选 bearer token |
| `EXOANCHOR_TOKEN_FILE` | 未设置 | 保存 bearer token 的文件 |
| `EXOANCHOR_TIMEOUT` | `75` | HTTP 超时秒数；同步 SSH 最长仍为 60 秒 |
| `EXOANCHOR_ALLOW_WRITE` | `0` | 设为 `1` 后才允许改变 UART、SSH、HID、电源或视频状态 |
| `EXOANCHOR_CONTROL_OWNER` | `mcp` | 控制租约 owner |
| `EXOANCHOR_DEVICE_ID` | URL hostname | 写工具校验的稳定设备身份 |
| `EXOANCHOR_STATE_DIR` | `~/.local/state/exoanchor-mcp` | 私有 SSH 作业审计目录 |
| `EXOANCHOR_PERSIST_JOB_OUTPUT` | `0` | 是否持久化可能敏感的 SSH 输出 |
| `EXOANCHOR_ALLOW_UNVERIFIED_SSH_HOST` | `0` | 允许未验证主机的受监督例外 |
| `EXOANCHOR_ALLOW_ARBITRARY_SSH` | `0` | 启用任意 SSH 命令入口 |

设备侧还需在 `Settings → Agent、Provider 与 Skills → MCP Controller` 中允许外部 MCP 访问。

## 安装与运行

在本目录直接检查或探测设备：

```bash
python3 -m exoanchor_mcp.server --list-tools
EXOANCHOR_BASE_URL=http://<设备地址> \
EXOANCHOR_USERNAME='<用户名>' \
EXOANCHOR_PASSWORD_FILE=/path/to/password-file \
python3 -m exoanchor_mcp.server --probe
```

可编辑安装：

```bash
python3 -m pip install -e .
exoanchor-mcp
```

最小只读 Codex 配置：

```toml
[mcp_servers.exoanchor]
command = "python3"
args = ["-m", "exoanchor_mcp.server"]
cwd = "/absolute/path/to/ExoAnchor/integrations/exoanchor-mcp"
required = true
tool_timeout_sec = 90
enabled_tools = [
  "exoanchor_capabilities",
  "exoanchor_status",
  "exoanchor_snapshot",
  "exoanchor_logs",
  "exoanchor_observation_get",
  "exoanchor_wait_for_status",
  "exoanchor_wait_for_frame_change",
  "exoanchor_open_kvm",
  "exoanchor_uart_status",
  "exoanchor_uart_read",
]

[mcp_servers.exoanchor.env]
EXOANCHOR_BASE_URL = "http://<设备地址>"
EXOANCHOR_USERNAME = "<用户名>"
EXOANCHOR_PASSWORD_FILE = "/path/to/private/device-password"
```

先完成只读验收，再按需要逐个加入写工具，并设置 `EXOANCHOR_ALLOW_WRITE = "1"`；不要为了方便一次性开放全部写工具。

## 验证

不连接真机的协议与安全测试：

```bash
python3 -m unittest discover -s tests -v
```

测试包含可重放的本地 HTTP 设备，覆盖读取与观察、frame-bound HID、清理流程、结构化 SSH 作业和幂等行为，不会触碰真实硬件。

## DeepSeek Harness

本机 DSH 通过官方 MCP client 插件复用本服务器，不维护第二套设备控制逻辑。项目
提供了环境检查、只读/受监督启动器、DSH patch 与四项演示 skill：

- [`docs/dsh/README_zh.md`](docs/dsh/README_zh.md)
- [`scripts/run-dsh.sh`](scripts/run-dsh.sh)
- [`../../skills/exoanchor-mcp-control/SKILL.md`](../../skills/exoanchor-mcp-control/SKILL.md)

先运行 `./scripts/run-dsh.sh --read-only --check` 验证组合配置，再进行只读冒烟。
当前 DSH DeepSeek chat adapter 不向模型投影 JPEG 内容，KVM 截图步骤需要人工页面
确认；UART 文本步骤不受此限制。

真机只读验收：

```bash
EXOANCHOR_BASE_URL=http://<设备地址> \
EXOANCHOR_USERNAME='<用户名>' \
EXOANCHOR_PASSWORD_FILE=/path/to/password.secret \
python3 scripts/stage4_acceptance.py --output /path/to/report.json
```

脚本检测到 `EXOANCHOR_ALLOW_WRITE` 时会拒绝运行，也不会申请视频/HID 租约、发送 SSH、改变电源或烧录固件。报告可能包含私有地址、设备指纹和运行状态，应保存在本地受控位置，确认不含私有信息后再共享。

真机验证顺序固定为：capabilities → status → logs → snapshot → 固定无破坏测试页面上的受监督 HID → 租约冲突与断线恢复。初始冒烟测试不包含电源、复位、强制关机和任意 SSH。

纯 KVM 固件不提供开发版 MCP API。缺少 `/api/settings/mcp` 或 `/api/capabilities` 时，桥接器会报告固件兼容性错误，而不是笼统的网络错误。

旧固件可能只接受 `/api/control/lease` 和 `/api/hid/actions` 的 `owner=agent`。正常使用应保持 `EXOANCHOR_CONTROL_OWNER=mcp`；只有明确兼容旧固件时才改为 `agent`，并接受它与内置 Agent 共用 owner 身份的冲突风险。
