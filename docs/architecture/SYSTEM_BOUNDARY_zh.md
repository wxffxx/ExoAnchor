# 系统边界

ExoAnchor 当前分为 Device 层和 Agent 层。文档、固件和 UI 都应该围绕这个边界组织。

## Device 层

Device 层负责靠近硬件的确定性能力：

- 视频输入，例如 MS2109 UVC 采集。
- USB HID 输出，例如 GPIO26/GPIO27 的 USB FS HID。
- 网络接入，例如 ESP32-P4 NANO 的 RJ45/IP101GR。
- 板级状态，例如 Video/USB online 状态。
- 在硬件支持时，提供 ATX PWR/RST、12V detect、3V3AUX detect。

Device 层的文档放在 `device/` 下对应形态旁边。

## Firmware / Dashboard

ESP32-P4 共享固件负责：

- 初始化 UVC、HID、网络。
- 提供 Web dashboard / KVM 页面。
- 暴露状态。
- 提供 OTA/upload 等维护入口。

Dashboard 必须显示真实硬件能力。DIY 方案没有电源控制时，UI 不应暗示它可以远程按电源键。

## Agent 层

Agent 层负责更高层的运行时：

- observation 收集。
- action 编排。
- SSH / KVM 通道选择。
- 安全确认。
- 结构化日志。
- skill / playbook 执行。

Agent 不应该直接把“能不能做电源控制”写死，而应该读取设备能力。

## 当前最重要的接口

近期最重要的接口不是复杂 Agent API，而是设备能力边界：

| 能力 | DIY | Expansion | ESP32-P4x |
|---|---|---|---|
| UVC video | 支持 | 支持 | 支持 |
| USB HID | 支持 | 支持 | 支持 |
| Ethernet | 支持 | 支持 | 支持 |
| ATX power control | 不支持 | 取决于扩展板 | 设计支持 |
| 12V detect | 不支持 | 取决于扩展板 | 设计支持 |
| 3V3AUX detect | 不支持 | 不支持或待定 | 设计支持 |

这些边界应该同步体现在 README、dashboard 和 agent 配置里。
