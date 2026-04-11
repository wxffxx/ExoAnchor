# SimpleIPMI

**[English](README.md)** 

An [RCOS](https://rcos.io) Project

开源、低成本的 KVM-over-IP 解决方案。通过浏览器远程控制物理主机的键盘、鼠标、视频画面和电源开关，功能类似商用 IPMI/BMC 系统。

## 核心功能

- **远程视频** — HDMI 采集 (USB/CSI)，MJPEG 视频流
- **远程键鼠** — USB HID 键盘 + 鼠标模拟
- **电源控制** — 光耦/继电器隔离的开机、关机、重启
- **Web 管理面板** — 浏览器直接操控，零客户端安装
- **灵活接入** — WiFi AP 直连 / 有线局域网 / Tailscale 远程

## 系统架构

项目支持三种不同的主控架构：

### MCU 主控 — ESP32-S3（可用）

轻量级独立方案。ESP32-S3 单芯片完成所有功能：WiFi AP（或通过可选 W5500 接入有线网络）、Web 服务 (SPIFFS)、原生 USB HID。无需 Linux，无需采集卡，BOM 最少，适用于单机管控。

```
                          ┌─────────────────────────────────────────┐
                          │            ESP32-S3                     │
                          │                                         │
用户 ── WiFi AP ─────────→│  AsyncWebServer + WebSocket (SPIFFS)   │
 或                       │         │              │                │
用户 ── W5500 有线* ─────→│    GPIO 控制       USB HID (OTG)       │
       (可选)             │     ┌────┴────┐    ┌───┴────┐          │
                          │     │  光耦隔离 │   │ 键盘   │          │
                          │     │PWR  │ RST │   │ 鼠标   │          │
                          │     └──┬──┴──┬──┘   └───┬────┘          │
                          └────────┼─────┼──────────┼───────────────┘
                                   │     │          │
                              电源键  复位键     USB ──→ 被控主机

* W5500 SPI 以太网为可选配件。无 W5500 时 ESP32-S3 以 WiFi AP 模式
  运行 (192.168.4.1)；接入 W5500 后通过 DHCP 加入局域网。
```

### ARM Linux 主控（可用）

基于 ARM Linux SBC（CM4、OrangePi）的全功能 KVM 方案。SBC 运行 Python (FastAPI) 服务端，包含视频采集、HID 控制、电源管理、终端和固件管理等模块化子系统。

支持两种 HID 模式：通过 UART 串口桥接外部 ESP32-S3（CM4 方案），或使用原生 USB OTG Gadget（OrangePi 方案）。

```
                     ┌──────────────────────────────────────────────────┐
                     │               ARM Linux SBC                     │
                     │          FastAPI + WebSocket 服务端              │
                     │                                                  │
用户 (浏览器) ──────→│  ┌───────────┐ ┌───────────┐ ┌──────────────┐  │
   以太网/WiFi       │  │  视频采集  │ │  HID 管理 │ │  GPIO 控制   │  │
                     │  │  (OpenCV) │ │           │ │              │  │
                     │  │           │ │ 模式 A:   │ │  电源键       │  │
                     │  │           │ │  UART ──────────→ ESP32-S3  │  │
                     │  │           │ │  (串口协议) │ │  复位键       │  │
                     │  │           │ │           │ │  12V 检测     │  │
                     │  │           │ │ 模式 B:   │ │              │  │
                     │  │           │ │  USB OTG  │ └──────┬───────┘  │
                     │  │           │ │ (/dev/hidg)│        │          │
                     │  └─────┬─────┘ └─────┬─────┘        │          │
                     │  ┌─────┴─────┐ ┌─────┴─────┐        │          │
                     │  │  Web 终端  │ │ ESP32 OTA │        │          │
                     │  │ (pty/ssh) │ │  固件刷写  │        │          │
                     │  └───────────┘ └───────────┘        │          │
                     └────────┼───────────┼────────────────┼──────────┘
                              │           │                │
                         USB 采集卡    USB HID         光耦/继电器
                         (MS2109)    (键盘 + 鼠标)   (电源/复位/12V)
                              │           │                │
                         HDMI 输出 ←── 被控主机 ──→ 主板接口
```

### Composite 集中管理（开发中）

运行在 Ubuntu (x86/ARM) 上的中央管理服务器。同时管理多个 KVM Host 和 USB 采集卡，提供统一 Web 面板，实现多目标机的集中控制。

## 仓库结构

```
simpleipmi/
├── hosts/                            # 主控设备 (KVM Host 固件/软件)
│   ├── esphost-esp32s3/              #   ESP32-S3 MCU 方案 (WiFi AP + HID)
│   ├── armhost-cm4/                  #   CM4 ARM Linux 方案 (网络 + HID Bridge)
│   ├── armhost-orangepi4/            #   OrangePi CM4 方案 (USB OTG HID)
│   ├── stmhost-f103/                 #   STM32F103 MCU 方案 (开发中)
│   └── esphost-esp32c3(switch_only)/ #   ESP32-C3 纯开关方案 (开发中)
│
├── hardware/                         # 硬件设计文件 (原理图/PCB/BOM)
│   ├── km/                           #   键鼠模拟模块
│   ├── kvm-carrier/                  #   KVM 载板
│   ├── coreboard/                    #   核心板设计
│   └── accessories/                  #   配件 (视频采集/继电器等)
│
├── exoanchor/                           # KVM Agent 智能框架 (视觉 + 自动修复)
│   ├── core/                         #   被动监控 + 半主动执行器
│   ├── vision/                       #   画面分析 (本地检测 + LLM API)
│   ├── action/                       #   HID/SSH 操作驱动
│   ├── skills/                       #   Skill 系统 (YAML + Python)
│   └── dashboard/                    #   Agent Web 面板
│
├── composite/                        # 多机集中管理系统 (开发中)
│   └── server/
│
├── shared/                           # 跨设备共享资源
│   └── protocol/                     #   通信协议定义 (protocol.h)
│
└── docs/                             # 文档资料
```

## Host 方案对比

| | ESP32-S3 | ARM CM4 | OrangePi CM4 | STM32F103 |
|---|---|---|---|---|
| **架构** | MCU | ARM Linux | ARM Linux | MCU |
| **成本** | 待定 | 待定 | 待定 | 待定 |
| **视频采集** | 无 | USB 采集卡 | USB 采集卡 | 无 |
| **键鼠 HID** | 原生 USB OTG | ESP32-S3 串口桥接 | 原生 USB OTG | 原生 USB |
| **网络** | WiFi AP / W5500 以太网 | 以太网/WiFi | 以太网/WiFi | 需外接 |
| **Web 面板** | 内置 (SPIFFS) | FastAPI 服务端 | FastAPI 服务端 | 无 |
| **状态** | 可用 | 可用 | 可用 | 开发中 |

## 快速开始

### ARM CM4 主控

**准备:** CM4 兼容 SBC + ESP32-S3 (HID Bridge) + USB 采集卡

```bash
# 在 CM4 上部署服务端
cd hosts/armhost-cm4/server
pip install -r requirements.txt
python main.py

# 烧录 ESP32-S3 HID Bridge 固件
cd hosts/armhost-cm4/firmware
pio run -t upload
```

详见 → [hosts/armhost-cm4/docs/DEVELOPMENT.md](hosts/armhost-cm4/docs/DEVELOPMENT.md)

## 硬件设计

所有 PCB/原理图设计文件在 `hardware/` 目录，按功能分类：

| 分类 | 说明 |
|------|------|
| `km/` | 键鼠模拟模块 (ESP32-S2, XIAO-ESP32S3, STM32F103) |
| `kvm-carrier/` | KVM 载板 (PCIe CM4 v1/v2, T113) |
| `coreboard/` | 核心板 (H616, ARM Linux 全集成) |
| `accessories/` | HDMI 采集 (Toshiba TC358743, MS2109)、继电器 |

详见 → [hardware/README.md](hardware/README.md)

## 开发路线

**已完成**
- ESP32-S3 独立 MCU Host（WiFi AP + 可选 W5500 以太网）
- ARM CM4 KVM Host + ESP32-S3 HID Bridge
- OrangePi CM4 KVM Host（原生 USB OTG）
- Web 管理面板（仪表盘 + KVM + 终端）

**待完成**
- STM32F103 低成本 MCU Host
- Composite 多机管理系统
- H616 核心板

## 关于 RCOS

本项目在 [Rensselaer Center for Open Source](https://rcos.io) (RCOS) 下开发。RCOS 是 Rensselaer Polytechnic Institute 的学生开源组织，支持服务于公共利益的开源软件开发。

## License

MIT
