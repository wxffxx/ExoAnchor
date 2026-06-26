# ExoAnchor ESP32-P4x

这是完整自研 ESP32-P4 板卡目标形态，用于承载视频、HID、电源控制和电源状态检测。

## 能力边界

| 功能 | 状态 |
|------|------|
| UVC 视频 | 支持 |
| USB HID | 支持，走板级定义的 USB FS 路径 |
| 以太网 | 支持，走板级定义的 Ethernet 路径 |
| ATX 电源控制 | 设计支持 |
| Power 12V 检测 | 设计支持 |
| Power Standby 3V3AUX 检测 | 设计支持 |

Web dashboard 右上角四个紧凑状态在这块板上都有真实硬件意义：

- `Video`：UVC 采集 online / standby / offline。
- `USB`：HID online / standby / offline。
- `Power`：PCIe/ATX 12V 是否存在。
- `Power Standby`：3V3AUX 是否存在。

固件使用 `../firmware` 这一份共享 ESP-IDF 工程。等原理图锁定后，再在这里补充板级 GPIO 和检测后端定义。
