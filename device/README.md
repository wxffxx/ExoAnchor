# ExoAnchor Device

`device/` 存放 ExoAnchor 的设备端实现。这里的每个一级目录代表一种硬件或固件路线，用来把被控主机的画面、键盘鼠标、电源状态和控制信号接入 ExoAnchor。

当前主线是 [ESP32P4](ESP32P4/)：它用 ESP32-P4 完成 MCU 形态的 KVM 设备，提供 UVC 视频输入、USB HID 键鼠输出、以太网 Web UI 和 OTA/设置页面。其它目录保留为 ARM Linux 节点、辅助控制板或轻量远程开关路线。

## 目录结构

```text
device/
├── ESP32P4/
│   ├── firmware/                  # 共享 ESP-IDF 固件
│   ├── waveshare-nano-diy/        # Waveshare ESP32-P4-NANO 无 PCB 快速验证方案
│   ├── waveshare-nano-expansion/  # Waveshare NANO + 扩展板原型
│   └── exoanchor-esp32p4x/        # 完整自研 ESP32-P4 板卡目标
├── ArmLinux/                      # ARM Linux 完整节点路线
├── ESP32S3/                       # ESP32-S3 网络键鼠/控制路线
├── STM32F103/                     # STM32 UART 控制桥路线
└── ESP32C6/                       # 轻量远程开关路线
```

## 设备形态

| 目录 | 当前定位 | 主要能力 | 状态 |
| --- | --- | --- | --- |
| [ESP32P4](ESP32P4/) | MCU KVM 主线 | UVC 视频、USB HID、以太网 Web UI、OTA、设置页 | 正在开发和验证 |
| [ArmLinux](ArmLinux/) | ARM Linux 完整节点 | 视频采集、服务进程、本地脚本/Agent、外部 MCU 控制 | 路线预留 |
| [ESP32S3](ESP32S3/) | 网络键鼠/电源控制板 | 以太网入口、USB HID、板级控制 | 路线预留 |
| [STM32F103](STM32F103/) | UART 控制桥 | 接收上位机命令，执行 HID 或电源控制 | 路线预留 |
| [ESP32C6](ESP32C6/) | 远程开关 | 低成本远程开/关控制 | 路线预留 |

## ESP32-P4 主线

[ESP32P4/firmware](ESP32P4/firmware/) 是所有 ESP32-P4 板型共用的 ESP-IDF 固件。当前固件基线包括：

- USB UVC 视频输入，面向 MS2109 类 HDMI USB 采集卡。
- USB HID 键盘鼠标输出，Waveshare NANO 路线使用 GPIO26(D-) / GPIO27(D+)。
- RJ45 以太网接入，Waveshare NANO 路线使用板载 IP101GRI。
- 内置 Web dashboard、KVM 页面和 settings 页面。
- OTA 上传、更新检查配置和默认账号安全提示。
- `Video`、`USB` 是当前真实运行状态；`Power`、`Power Standby` 需要对应板级 GPIO/ADC 后端。

ESP32-P4 下的三个硬件形态能力边界如下：

| 硬件形态 | UVC 视频 | USB HID | 以太网 | ATX 电源控制 | 12V 检测 | 3V3AUX 待机检测 | 适用场景 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| [waveshare-nano-diy](ESP32P4/waveshare-nano-diy/) | 支持 | GPIO26/27 | NANO RJ45/IP101GRI | 不支持 | 不支持 | 不支持 | 最快完成视频 + 键鼠 + 网络验证 |
| [waveshare-nano-expansion](ESP32P4/waveshare-nano-expansion/) | 支持 | GPIO26/27 或扩展板映射 | NANO RJ45/IP101GRI | 扩展板预留 | 可预留 | 不支持 | 扩展板原型验证 |
| [exoanchor-esp32p4x](ESP32P4/exoanchor-esp32p4x/) | 支持 | 自研板定义 | 自研板定义 | 支持设计目标 | 支持设计目标 | 支持设计目标 | 完整自研硬件目标 |

最快的上手路径是 [waveshare-nano-diy](ESP32P4/waveshare-nano-diy/)：

1. 使用 Waveshare ESP32-P4-NANO、MS2109 HDMI USB 采集卡、网线、HDMI 线和一根可引出 D+/D-/5V/GND 的 USB 2.0 线。
2. 将 MS2109 插入 ESP32-P4-NANO 的 USB-A host 口。
3. 将被控主机 HDMI 输出接入 MS2109。
4. 将 HID USB 线接到被控主机 USB-A，并把 D+ 接 GPIO27、D- 接 GPIO26、5V 接 5V、GND 接 GND。
5. 烧录 [ESP32P4/firmware](ESP32P4/firmware/) 后，通过 DHCP 地址访问 Web UI。

详细接线、烧录和排障步骤见 [ESP32P4/waveshare-nano-diy/BuildGuide.md](ESP32P4/waveshare-nano-diy/BuildGuide.md)。

## 构建和烧录

ESP32-P4 固件使用 ESP-IDF：

```bash
cd device/ESP32P4/firmware
. "$HOME/esp/esp-idf-v5.4/export.sh"
idf.py set-target esp32p4
idf.py build
```

烧录并等待 DHCP 地址：

```bash
cd device/ESP32P4/firmware
./tools/flash-monitor.sh /dev/cu.usbmodem5B5E1314701 --wait-ip --exit-on-ip
```

串口名需要替换成本机 `ls /dev/cu.usbmodem*` 或系统设备管理器中看到的实际端口。脚本会把探测到的 IP 写入 `build/last_ip.txt`。

固件启动后常用页面：

```text
http://<board-ip>/
http://<board-ip>/kvm
http://<board-ip>/settings
```

默认登录信息为 `admin` / `admin`。如果仍使用默认密码，Web UI 会提示修改。

## 生成文件

`ESP32P4/firmware/build/`、`ESP32P4/firmware/build-*`、`ESP32P4/firmware/managed_components/` 和 `sdkconfig` 都属于本地构建或依赖产物，不是设备目录的核心源码。整理提交时应优先保留源码、文档、`main/`、`tools/`、`partitions.csv`、`sdkconfig.defaults` 和硬件说明文件。

## 文档入口

- [ESP32P4/README.md](ESP32P4/README.md)：ESP32-P4 平台能力矩阵。
- [ESP32P4/firmware/README.md](ESP32P4/firmware/README.md)：共享固件构建和烧录。
- [ESP32P4/waveshare-nano-diy/README.md](ESP32P4/waveshare-nano-diy/README.md)：无 PCB DIY 方案概览。
- [ESP32P4/waveshare-nano-diy/BuildGuide.md](ESP32P4/waveshare-nano-diy/BuildGuide.md)：DIY 方案详细装配指南。
- [ESP32P4/waveshare-nano-expansion/README.md](ESP32P4/waveshare-nano-expansion/README.md)：扩展板原型能力边界。
- [ESP32P4/exoanchor-esp32p4x/README.md](ESP32P4/exoanchor-esp32p4x/README.md)：完整自研板目标能力边界。
