# Waveshare ESP32-P4-NANO DIY Build Guide

这是一种面向快速验证的 ExoAnchor DIY 方案：不需要额外 PCB，不需要焊接，只用开发板、视频采集卡和杜邦线就能完成基础 KVM 功能。

## 0. 能实现什么

完成后可以获得：

- 键盘、鼠标控制
- HDMI 视频画面采集
- 有线网络接入
- 通过 USB HID 口为 ESP32-P4-NANO 供电

这个方案不包含：

- ATX 电源控制
- 机箱电源键/重启键控制

如果需要远程开机，请在被控主机 BIOS/UEFI 或操作系统里单独配置 Wake-on-LAN。

## 1. 需要准备什么

硬件：

- Waveshare ESP32-P4-NANO 开发板
- 5pin 杜邦母头结尾的 USB 2.0 线，用于连接被控主机的 USB HID
- 基于 MS2109 的 HDMI USB 视频采集卡
- 网线
- HDMI 线
- USB-C 数据线，用于烧录和串口调试
- 一台已经安装 ESP32-P4 / PlatformIO 工具链的 Mac、Windows 或 Linux 主机

注意：

- 不要使用 MS2109S 视频采集卡，实测存在兼容性问题。
- USB 2.0 线必须能引出 5V、GND、D+、D-。只带充电线芯的 USB 线不能使用。

## 2. 接口分工

ESP32-P4-NANO 上的接口在本方案里的用途：

| 接口 | 用途 |
| --- | --- |
| USB-C | 烧录固件、串口调试、初次供电 |
| USB-A | 接入 MS2109 HDMI 视频采集卡 |
| RJ45 / IP101GRI | 接入局域网 |
| GPIO 排针 | 接入被控主机的 USB HID 线 |

被控主机侧的连接：

| 被控主机接口 | 连接到 |
| --- | --- |
| HDMI 输出 | MS2109 HDMI 采集卡 HDMI 输入 |
| USB-A | ESP32-P4-NANO 排针上的 HID 线 |
| 网口/局域网 | 与 ESP32-P4-NANO 在同一网络 |

## 3. 接线步骤

1. 将 MS2109 视频采集卡插入 ESP32-P4-NANO 的 USB-A 口。
2. 将 HDMI 线从被控主机 HDMI 输出接到 MS2109 采集卡。
3. 将网线接入 ESP32-P4-NANO 的 RJ45 网口，链路会经过板载 IP101GRI 以太网 PHY。
4. 将 5pin USB 2.0 线接到 ESP32-P4-NANO 排针：

| USB 线 | 接到 ESP32-P4-NANO |
| --- | --- |
| D+，通常为绿色 | GPIO27 |
| D-，通常为白色 | GPIO26 |
| 5V，通常为红色 | 5V |
| GND，通常为黑色 | GND |

5. 将这根 USB 2.0 线的 USB-A 端插入被控主机。
6. 用 USB-C 数据线连接 ESP32-P4-NANO 和烧录主机。

安全检查：

- 上电前必须确认 5V 接到了 5V，不要接到 3V3。
- D+ / D- 接反通常不会烧板，但 HID 无法枚举；如果键鼠不可用，优先检查这两根线。
- GND 必须共地，否则 USB 通信可能不稳定。

## 4. 烧录固件

### 4.1 手动烧录

如果你已经在本机准备好 ESP32-P4 工具链，可以在固件目录中执行：

```bash
pio run
pio run -t upload
pio device monitor
```

烧录时如果没有自动进入下载模式，按住 BOOT，再点按 RESET，然后松开 BOOT。

### 4.2 使用 Ai Agent辅助烧录

可以把下面这段话发给 Codex，让它自动完成拉取、构建、烧录和串口检查：

```text
Hi Codex，现在我的电脑接入了一个 ESP32-P4 开发板，并且已经安装 ESP32-P4 / PlatformIO 工具链，这个开发板使用CH343转串口进行烧录。请帮我从 GitHub 的 ExoAnchor 项目拉取适合 Waveshare ESP32-P4-NANO DIY的固件，构建并烧录到开发板，然后通过 UART 串口监视日志，确认启动、网络、USB HID 和视频采集初始化都正常。
```

烧录成功后，USB-C 只用于调试；实际部署时可以拔掉 USB-C，让被控主机的 USB HID 口给 ESP32-P4-NANO 供电。

## 5. 验证

烧录完成后依次检查：

1. 串口日志中没有持续重启、panic 或 USB 初始化失败。
2. ESP32-P4-NANO 能通过 RJ45 / IP101GRI 获取网络地址。
3. Web UI 或上位机能够发现设备。
4. HDMI 画面可以正常显示。
5. 键盘、鼠标输入能作用到被控主机。

## 6. 常见问题

### 串口找不到设备

- 确认CH343的驱动已安装
- 确认 USB-C 线是数据线，不是仅充电线。
- 检查系统是否识别到串口设备。
- 重新进入下载模式后再烧录。

### 键盘鼠标不可用

- 检查 D+ 是否接 GPIO27，D- 是否接 GPIO26。
- 检查 5V 和 GND 是否连接稳定。
- 在被控主机系统里查看是否枚举出 USB HID 设备。

### 设备启动后网络不可用

- 检查网线和交换机端口。
- 确认局域网 DHCP 正常。
- 如果需要远程开机，确认 Wake-on-LAN 是在被控主机上配置的，不是这个 DIY 方案自动提供的 ATX 控制能力。

## 7. 完成

当视频、键鼠、网络都验证正常后，设备就可以离开烧录主机使用。部署时保留：

- MS2109 插在 ESP32-P4-NANO 的 USB-A 口
- HDMI 接被控主机
- HID USB 线接被控主机
- 网线接局域网

USB-C 可以只在升级固件或排查串口日志时再接回烧录主机。
