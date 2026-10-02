# ExoAnchor MCP 架构

## 模块

| 模块 | 单一职责 |
| --- | --- |
| `exoanchor_mcp/client.py` | 环境配置、secret file、登录、HTTP(S) 传输与设备错误。 |
| `exoanchor_mcp/contracts.py` | 20 个 MCP tool schema、风险标注与严格参数校验。 |
| `exoanchor_mcp/observations.py` | observation provenance、内容哈希、JPEG 尺寸、条件派生与有界内存重放。 |
| `exoanchor_mcp/jobs.py` | 有界 SSH job 状态机、幂等键、分页结果和私有审计 journal。 |
| `exoanchor_mcp/server.py` | 工具编排、设备动作清理、MCP result 与 JSON-RPC STDIO。 |
| `scripts/stage4_acceptance.py` | 强制只读的真实设备 Stage 4 验收。 |
| `tests/test_http_integration.py` | 可重放本地 HTTP 设备，覆盖真实传输层。 |

## 数据流

```text
Codex
  -> MCP JSON-RPC / STDIO
  -> strict tool schema + write gate
  -> observation / deterministic action / job orchestration
  -> authenticated HTTP API
  -> ESP32-P4 dev firmware
  -> target device
```

原始设备 JSON、日志和屏幕像素始终标记为 `untrusted_external_observation`。bridge 生成的 readiness、frame equality 和任务状态放在 derived/audit 字段，不能冒充固件或目标机直接声明的事实。

## 状态所有权

- ESP32-P4：MCP 开关、视频/HID/电源状态、控制租约、SSH target 与 secret。
- MCP bridge：最近 observation cache、frame-bound action receipt、SSH job journal。
- Codex：计划、工具选择、审批请求和对结果的解释。
- 人：未知 BIOS、破坏性电源行为、未验证 SSH 主机和任意命令的最终授权。

## 已知边界

- observation replay 只保证一个 MCP 进程生命周期；长期证据由显式验收报告承担。
- firmware 同步 SSH 请求无法被 bridge 强杀；cancel 是 cooperative，重启后 active job 标记为 `interrupted/unknown`。
- frame ID 是 JPEG 字节的 SHA-256，只保证精确字节相等，不推断两张图语义相同。
- `server.py` 的 runtime dispatch 仍较长；Stage 5 若新增 profile/playbook 域，应新建独立模块，不再把新域直接堆入该文件。
