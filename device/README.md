# device/

这里存放 ExoAnchor 的不同设备形态。每个一级目录代表一种硬件/固件路线，而不是单纯的实验分支。

当前保留 5 种形态：

| 形态 | 目录 | 定位 | 核心能力 |
|------|------|------|----------|
| ARM Linux | [ArmLinux](ArmLinux/) | 完整 KVM + Agent | 视频采集、键鼠控制、电源/状态控制、Web dashboard、Agent 工作流 |
| ESP32-P4 | [ESP32P4](ESP32P4/) | MCU 完整 KVM | USB UVC 视频、USB HID 键鼠、以太网 dashboard、OTA、基础状态检测预留 |
| ESP32-S3 | [ESP32S3](ESP32S3/) | 以太网键盘 + 电源控制 | 通过以太网提供键盘/控制入口，并承担电源控制类功能 |
| STM32F103 | [STM32F103](STM32F103/) | UART 控制桥 | 通过 UART 接收上位机命令，执行电源控制和键鼠 HID 控制 |
| ESP32-C6 | [ESP32C6](ESP32C6/) | 远程开关 | 只作为轻量远程开关，不承担完整 KVM 或键鼠链路 |

## 形态说明

### ARM Linux

ARM Linux 是能力最完整的方案，适合作为完整 ExoAnchor 节点：

- 完整 KVM。
- Agent 功能。
- Linux 侧视频采集与服务进程。
- 通过外部 MCU 或板级控制器处理 HID、电源和状态控制。
- 适合需要本地计算、脚本、终端、任务执行和复杂自动化的场景。

### ESP32-P4

ESP32-P4 是当前 MCU KVM 主线，目标是在单颗 MCU 上完成完整 KVM：

- USB UVC 视频输入。
- USB HID 键鼠输出。
- RJ45 以太网 Web dashboard / KVM / settings。
- OTA 上传与更新检查预留。
- `Video`、`USB` 为当前真实运行状态。
- `Power`、`Power Standby` 为板级硬件后端预留状态。

ESP32-P4 内部再按硬件形态划分：

```text
ESP32P4/
├── firmware/                  # 共享 ESP-IDF 固件
├── waveshare-nano-diy/        # 无 PCB，不能做 ATX 电源控制
├── waveshare-nano-expansion/  # 扩展板形态，不能检测 standby
└── exoanchor-esp32p4x/        # 完整自研板目标
```

### ESP32-S3

ESP32-S3 保留为以太网键盘和电源控制形态：

- 不作为完整视频 KVM 主线。
- 重点是网络入口、键盘/HID 控制和电源控制。
- 适合低成本控制端、辅助控制板或历史兼容方案。

### STM32F103

STM32F103 是 UART 控制桥：

- 上位机通过 UART 下发命令。
- STM32F103 执行电源控制。
- STM32F103 执行键鼠 HID 控制。
- 适合作为 ARM Linux 或其他主控旁边的稳定控制 MCU。

### ESP32-C6

ESP32-C6 只作为远程开关：

- 不做完整 KVM。
- 不做视频采集。
- 不承担完整键鼠链路。
- 目标是低功耗、低成本、简单可靠的远程开/关控制。
