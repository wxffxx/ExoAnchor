# Waveshare ESP32-P4-NANO DIY

**[English](README.md)** | **装配指南:** [BuildGuide.md](BuildGuide.md)

这是 ExoAnchor ESP32-P4 最快的 bring-up 方案。它直接使用 Waveshare ESP32-P4-NANO、MS2109 类 HDMI USB 采集卡、有线网络和四根 USB 2.0 HID 线，不需要自定义 PCB，也不需要焊接。

## 定位

这个版本适合快速验证核心 KVM 闭环：

- 从被控主机采集 HDMI 画面
- 通过 USB HID 控制键盘和鼠标
- 通过以太网访问 dashboard
- 验证共享 ESP32-P4 固件

如果需求是远程 ATX 电源控制或电源状态检测，不要选择这个 DIY 版本。它没有 PWR/RST/12V/3V3AUX 后端。

## 能力边界

| 功能 | 状态 |
| --- | --- |
| UVC 视频 | 支持，走 ESP32-P4 USB-A 高速 host 口 |
| USB HID | 支持，GPIO26(D-) / GPIO27(D+) |
| 以太网 | 支持，走 NANO RJ45/IP101GRI |
| Web dashboard | 支持，由共享固件提供 |
| OTA 上传 UI | 固件里已有入口，仍在验证 |
| ATX 电源控制 | 不支持 |
| Power 12V 检测 | 不支持 |
| Power standby 3V3AUX 检测 | 不支持 |

如果需要远程开机，请在被控主机上配置 Wake-on-LAN，或在本 DIY 方案之外单独增加电源控制接线。

## 硬件

- Waveshare ESP32-P4-NANO
- MS2109 HDMI USB 采集卡
- HDMI 线
- RJ45 网线
- USB-C 数据线，用于烧录和串口日志
- 能引出 5V、GND、D+、D- 的 USB 2.0 线或转接板
- 已安装 ESP32-P4 ESP-IDF 工具链的 Mac、Windows 或 Linux 烧录主机

这条路径暂时不要使用 MS2109S 采集卡；当前测试中它存在兼容性问题。

## 接线摘要

| 连接 | 接到 |
| --- | --- |
| MS2109 USB | ESP32-P4-NANO USB-A host 口 |
| 被控主机 HDMI 输出 | MS2109 HDMI 输入 |
| RJ45 / IP101GRI | 有 DHCP 的局域网 |
| USB HID D+ | GPIO27 |
| USB HID D- | GPIO26 |
| USB HID 5V | 5V |
| USB HID GND | GND |
| USB-C | 初始化烧录/调试时接烧录主机 |

上电前必须确认 5V 接到了 5V，不要把被控主机 USB 5V 接到 3V3。

## 固件

这个硬件形态使用共享 ESP32-P4 固件：

[../firmware](../firmware/)

典型构建流程：

```bash
cd device/ESP32P4/firmware
. "$HOME/esp/esp-idf-v5.4/export.sh"
idf.py set-target esp32p4
idf.py build
./tools/flash-monitor.sh /dev/cu.usbmodem5B5E1314701 --wait-ip --exit-on-ip
```

串口路径请替换成本机实际设备。Waveshare NANO 常见情况是通过 CH343 USB 转串口桥暴露烧录串口。

## Bring-up 检查

1. 串口日志没有持续重启、panic 或 USB 初始化失败。
2. 以太网能获取 DHCP 地址。
3. 能通过板子的 IP 打开 Web UI。
4. KVM 页面能看到 HDMI 画面。
5. 键盘、鼠标输入能作用到被控主机。
6. 烧录完成后，如果被控主机 USB HID 口能稳定供电，可以拔掉 USB-C。

完整装配步骤、安全检查、烧录说明和故障排查见 [BuildGuide.md](BuildGuide.md)。
