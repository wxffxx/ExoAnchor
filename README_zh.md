# ExoAnchor

**[English](README.md)**

An [RCOS](https://rcos.io) project.

ExoAnchor 是一个开源、低成本、包含完全软硬件设计、面向 AI 的裸机 BMC/KVM 项目。它的目标是在 SSH、系统网络、服务都不可用时，仍然能通过物理控制面接管机器：看屏幕、发键鼠、进 BIOS、处理启动失败，并在需要时配合电源控制完成恢复。

硬件仓库：[wxffxx/ExoAnchor-Hardware](https://github.com/wxffxx/ExoAnchor-Hardware)

项目分成两层：

- **Device 层:** ESP32-P4 等硬件形态提供视频输入、USB HID 输出、以太网接入和板级控制信号。包含完整的软硬件设计，并且有易复刻的DIY方案。
- **Agent 层:** `exoanchor-agent/` 提供服务端 runtime，负责 observation、action、安全确认、日志和恢复 playbook。

## 快速开始：微雪电子 ESP32P4 NANO DIY

1. 先读硬件指南：[BuildGuide.md](device/ESP32P4/waveshare-nano-diy/BuildGuide.md)。
2. 连接目标主机 USB HID 线：

| USB 信号 | ESP32-P4-NANO 引脚 |
| --- | --- |
| D+ | GPIO27 |
| D- | GPIO26 |
| 5V | 5V |
| GND | GND |

3. 将 MS2109 HDMI 采集卡插入 ESP32-P4-NANO 的 USB-A 口。
4. 将目标主机 HDMI 输出接入采集卡。
5. 接入 RJ45 有线网络。
6. 构建并烧录共享固件。

## 当前主线

最新硬件是 [device/ESP32P4](device/ESP32P4/) 下的 ESP32-P4 KVM 平台。

世界上首个使用MCU完成了完整KVM task的设计

| 路径 | 状态 | 用途 |
| --- | --- | --- |
| [device/ESP32P4/waveshare-nano-diy](device/ESP32P4/waveshare-nano-diy/) | 活跃 bring-up | 不做 PCB、不焊接，快速获得视频 + 键鼠 + 有线网络 |
| [device/ESP32P4/waveshare-nano-expansion](device/ESP32P4/waveshare-nano-expansion/) | 原型 | Waveshare NANO + 扩展板 |
| [device/ESP32P4/exoanchor-esp32p4x](device/ESP32P4/exoanchor-esp32p4x/) | 自研板 | 完整视频、HID、电源控制和电源状态检测 |

最快能跑起来的是 Waveshare ESP32-P4-NANO DIY 方案：

- Waveshare ESP32-P4-NANO
- MS2109 HDMI USB 采集卡
- RJ45 有线网络
- GPIO26/GPIO27 上的 USB 2.0 HID 接线
- [device/ESP32P4/firmware](device/ESP32P4/firmware/) 中的共享 ESP-IDF 固件

装配指南见 [device/ESP32P4/waveshare-nano-diy/BuildGuide.md](device/ESP32P4/waveshare-nano-diy/BuildGuide.md)。

## DIY 方案能做什么

| 功能 | 状态 |
| --- | --- |
| HDMI 视频采集 | 支持，通过 ESP32-P4 USB-A host 口接 MS2109 类 UVC 采集卡 |
| 键盘/鼠标 | 支持，通过 GPIO26/GPIO27 输出 USB HID |
| 有线网络 | 支持，走 Waveshare NANO RJ45/IP101GRI |
| Web UI | 支持，由共享 ESP32-P4 固件提供 |
| OTA/更新 UI | 固件里已有入口，仍在验证 |
| ATX 电源控制 | DIY 方案不支持 |
| 12V / 3V3AUX 电源状态检测 | DIY 方案不支持 |

DIY 方案故意保持简单：适合验证视频、键鼠、网络和固件流程。它不能按目标主机电源键，也不能检测 standby 电源轨。需要远程开机时，请使用 Wake-on-LAN、手动操作，或后续扩展板/自研板。

## 架构展示

### ESP32-P4 NANO DIY KVM

```text
                         ┌────────────────────────────────┐
User browser ─ Ethernet →│ RJ45 + IP101GRI PHY            │
                         │ ESP32-P4 + firmware + Web UI   │
                         │                                │
                         │  USB-A host ─────┐             │
                         │  GPIO27 D+  ─┐   │             │
                         │  GPIO26 D-  ─┼───┼──────────┐  │
                         │  5V / GND   ─┘   │          │  │
                         └──────────────────┼──────────┼──┘
                                            │          │
                                      ┌─────▼─────┐    │
                                      │ MS2109    │    │
                                      │ HDMI UVC  │    │
                                      └─────┬─────┘    │
                                            │          │
Target machine HDMI out ────────────────────┘          │
Target machine USB-A  ◀────────────────────────────────┘
```

在 DIY 版本里，ESP32-P4-NANO 就是完整 KVM 设备：

- 视频通过 USB-A host 口上的 MS2109 采集卡进入
- 键盘和鼠标通过 GPIO26/GPIO27 的 USB HID 接线输出
- 用户通过板载 IP101GRI PHY 接入以太网，再访问 dashboard
- bring-up 之后，USB-C 只用于烧录和串口日志

这套接线没有 ATX 电源控制路径。被控主机需要手动开机、通过 Wake-on-LAN 开机，或由另一个外部控制器处理电源。

## 仓库结构

```text
ExoAnchor/
├── device/
│   ├── ESP32P4/
│   │   ├── firmware/                  # 共享 ESP-IDF 固件
│   │   ├── waveshare-nano-diy/        # 无 PCB 的 Waveshare NANO 方案
│   │   ├── waveshare-nano-expansion/  # 扩展板原型
│   │   └── exoanchor-esp32p4x/        # 完整自研板目标
│   ├── ArmLinux/                      # ARM Linux 设备路径
│   ├── ESP32S3/                       # ESP32-S3 控制/参考路径
│   ├── STM32F103/                     # UART/control bridge 路径
│   └── ESP32C6/                       # 轻量远程开关路径
├── exoanchor-agent/                   # Python agent runtime 和 dashboard 集成
└── docs/                              # 项目方向、架构、流程、研究和参考资料
```

## 文档入口

建议从这些文档开始：

- [device/README.md](device/README.md)：设备形态总览
- [device/ESP32P4/README.md](device/ESP32P4/README.md)：ESP32-P4 平台总览
- [device/ESP32P4/waveshare-nano-diy/BuildGuide.md](device/ESP32P4/waveshare-nano-diy/BuildGuide.md)：无 PCB 装配指南
- [device/ESP32P4/firmware/README.md](device/ESP32P4/firmware/README.md)：共享固件构建/烧录说明
- [exoanchor-agent/README.md](exoanchor-agent/README.md)：Agent runtime 总览
- [docs/README_zh.md](docs/README_zh.md)：项目文档地图


## License

[MIT](LICENSE)
