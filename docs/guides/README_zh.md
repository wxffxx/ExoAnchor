# ExoAnchor 实用指南

烧录完成并从串口日志取得设备地址后，通过浏览器访问：

| 地址 | 功能 |
| --- | --- |
| `http://<设备地址>/` | Dashboard 与设备状态 |
| `http://<设备地址>/kvm` | 视频、键盘和鼠标 |
| `http://<设备地址>/agent` | 设备内 Agent，仅 Dev 固件 |
| `http://<设备地址>/terminal` | 被控端 UART，仅 Dev 固件且取决于板型 |
| `http://<设备地址>/settings` | 设备设置 |

首次使用依次确认：

1. Ethernet 已获得地址，Dashboard 可以打开；
2. KVM 页面能够显示目标机画面；
3. 目标机能够识别 USB HID；
4. 已替换本地 bootstrap 凭据；
5. UART、电源等可选功能与当前板型报告的 capability 一致。

设备内 Agent 或模型不可用时，人工 KVM 仍可独立使用。需要从外部 AI Agent
连接设备时，使用可选的 [ExoAnchor MCP](../../integrations/exoanchor-mcp/README.md)。

修改固件后的主机测试、完整构建和人工验收步骤见
[固件修改验证规程](FIRMWARE_VALIDATION_zh.md)。
