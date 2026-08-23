# ExoAnchor 制造与复刻

本指南覆盖硬件与固件选型、工具链准备、固件构建、烧录和启动验证。

## 1. 选型

| 需求 | 建议选择 | 说明 |
| --- | --- | --- |
| 使用现成开发板复刻 | Waveshare ESP32-P4-NANO DIY | 可使用 Dev 或纯 KVM Stable 固件，采用 ESP32-P4 rev1 配置 |
| 使用验证较完整的自研板 | ExoAnchor PrototypeV0 | 可使用 Dev 或纯 KVM Stable 固件，采用 ESP32-P4 rev3 配置 |
| 验证当前自研板与设备内 Agent | ExoAnchor PrototypeV2.3 | 使用 Dev 固件和 ESP32-P4 rev3 配置；当前仍处于 bring-up 阶段 |
| 只需要人工 KVM | Stable KVM | 不包含设备内 Agent |
| 需要设备内 Agent | Dev | 包含 KVM、设备内 Agent 和相应设备 API |

板型、固件 profile、芯片版本与验证状态以
[ESP32-P4 板型与固件 Profile](../../device/ESP32P4/boards/IMPLEMENTATION_PROFILES_zh.md)
为准。硬件装配、原理图、BOM 和生产资料见
[ExoAnchor-Hardware](https://github.com/wxffxx/ExoAnchor-Hardware)。

不同板型和芯片版本不能共用生成的 `sdkconfig` 或 build 目录。无法确认实物板型时，
不要继续构建或烧录。

## 2. 构建与烧录

| 任务 | 文档 |
| --- | --- |
| 让 AI Agent 获取源码、构建并烧录 | [主 README 中的提示词](../../README_zh.md#2-让-ai-agent-获取源码构建并烧录) |
| 选择固件版本和确认升级要求 | [固件版本入口](../../device/ESP32P4/firmware/README.md) |
| 构建 Dev 固件 | [Dev 构建说明](../../device/ESP32P4/firmware/v0.86.6-dev/README.md) |
| 构建纯 KVM Stable 固件 | [Stable 构建说明](../../device/ESP32P4/firmware/v0.86-stable-kvm/README.md) |
| 装配 Waveshare DIY 硬件 | [硬件装配与烧录指南](https://github.com/wxffxx/ExoAnchor-Hardware/tree/main/ESP32P4/simple-diy/waveshare-nano-diy) |

通用流程：

1. 确认实物板型、固件版本、board profile 和 silicon overlay。
2. 安装该组合要求的 ESP-IDF 版本。
3. 为这一组合创建独立的 `sdkconfig` 和 build 目录。
4. 完整执行 `set-target` 与 build。
5. 确认串口和芯片身份后执行完整有线烧录。
6. 监视启动日志，记录设备获得的地址并完成首次验证。

烧录脚本必须使用与构建时相同的目录：

```bash
./tools/flash-monitor.sh <PORT> \
  --build-dir <build-dir> \
  --wait-ip \
  --exit-on-ip
```

分区变化、首次安装和跨版本升级可能要求完整烧录，具体以
[固件烧录规则](../../device/ESP32P4/firmware/README.md#烧录规则)为准。
