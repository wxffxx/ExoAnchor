# SI BMC - ESP32-P4 Host

状态：HID + MIPI-CSI 视频输入 bring-up 基线。

本目录存放 ESP32-P4 Host 方向的源码。当前目标开发板是 Waveshare ESP32-P4-NANO，构建系统使用 ESP-IDF。当前版本按现场方向先忽略 GPIO 和 ATX 电源控制，只专注两件事：

- 从 ESP32-S3 方案迁移 TinyUSB HID 键盘/鼠标。
- 从 CM4 Host 迁移网页入口，并把网络切到 RJ45 以太网，把视频输入改成 ESP32-P4 MIPI-CSI + JPEG/MJPEG 输出。

## 当前基线

- 芯片：ESP32-P4
- 开发板：Waveshare ESP32-P4-NANO
- 框架：ESP-IDF v5.4.x
- 网络：P4-NANO 板载 100M RJ45，以 ESP32-P4 EMAC + IP101GR PHY 通过 DHCP 入网
- HID：TinyUSB 原生 USB boot 键盘 + boot 鼠标，网页通过 `/api/ws/hid` WebSocket 下发事件；KVM 页面当前提供虚拟键盘、鼠标摇杆和触控板
- 视频输入：MIPI-CSI 摄像头，默认 Waveshare/Raspberry Pi 兼容 OV5647，2-lane RAW8 `800x640`
- 视频输出：硬件 JPEG 编码，`/api/snapshot` 单帧 JPEG；KVM 页面用快照刷新避免长连接阻塞 HID
- Web：内置 `/` 仪表盘和 `/kvm` KVM/HID 页面
- Flash：实板按 16MB 配置
- PSRAM：按 Waveshare 官方示例启用

已验证 `idf.py build` 通过；当前 app binary 为 `0xa7aa0` 字节，默认 1MB app 分区剩余约 35%。

## 目录

```text
esphost-esp32p4/
├── firmware/
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── main/
│   │   ├── app_main.c
│   │   ├── hid_device.c/.h
│   │   ├── net_manager.c/.h
│   │   ├── video_input.c/.h
│   │   ├── web_server.c/.h
│   │   └── www/
│   │       ├── index.html
│   │       └── kvm.html
│   └── tools/
│       ├── flash-monitor.sh
│       ├── serial-ip-monitor.py
│       └── serve-ui.py
├── README.md
└── README_zh.md
```

## 硬件连接

- 烧录/日志：开发板 USB 烧录口接电脑。
- 网络：RJ45 网口接到有 DHCP 的交换机/路由器。
- HID：ESP32-P4 的 USB-OTG/HID 设备口接被控主机，被控主机才会枚举到键盘/鼠标。
- 视频：OV5647/SC2336 这类 MIPI-CSI 摄像头接到 ESP32-P4-NANO 的 CSI 15pin 座。
- 摄像头 SCCB/I2C：原理图为 `GPIO8=SCL`、`GPIO7=SDA`。

当前默认先跑 OV5647 的 `MIPI_2lane_24Minput_RAW8_800x640_50fps`。更高分辨率可以后续试，但要同时匹配 sensor format、CSI 分辨率、lane bitrate 和 JPEG 编码压力。

## 配置

第一次烧录前建议先进 `menuconfig`：

```bash
cd hosts/esphost-esp32p4/firmware
. "$HOME/esp/esp-idf-v5.4/export.sh"
idf.py menuconfig
```

进入 `SI ESP32-P4 Host Configuration`：

- `Enable RJ45 Ethernet`：默认打开，使用板载 IP101GR PHY。
- `Ethernet SMI MDC GPIO`：默认 `31`。
- `Ethernet SMI MDIO GPIO`：默认 `52`。
- `Ethernet PHY reset GPIO`：默认 `51`。
- `Ethernet PHY address`：默认 `1`。
- `Ethernet DHCP wait timeout`：默认 `15000ms`。
- `Web/API bearer password`：默认 `admin`。为空会关闭写接口鉴权。
- `Video input / Initialize a MIPI camera sensor over SCCB/I2C`：默认打开。
- `Video input / CSI input width`：默认 `800`。
- `Video input / CSI input height`：默认 `640`。
- `Video input / CSI data lanes`：默认 `2`。
- `Video input / CSI lane bitrate in Mbps`：默认 `200`。
- `Video input / MIPI camera sensor format name`：默认 `MIPI_2lane_24Minput_RAW8_800x640_50fps`。
- `Video input / Camera SCCB/I2C SCL GPIO`：默认 `8`。
- `Video input / Camera SCCB/I2C SDA GPIO`：默认 `7`。
- `Video input / Enable CSI byte swap`：只用于外部 YUV422 桥片模式。
- `Video input / MJPEG/JPEG quality`：默认 `75`。
- `Video input / Enable internal LDO for MIPI PHY`：默认打开，LDO 通道 `3`，电压 `2500mV`。

## 构建

```bash
cd hosts/esphost-esp32p4/firmware
. "$HOME/esp/esp-idf-v5.4/export.sh"
idf.py set-target esp32p4
idf.py build
```

## 本地测试 Web UI

UI 修改先在本地 mock server 里验证，再烧进 ESP32-P4：

```bash
cd hosts/esphost-esp32p4/firmware
./tools/serve-ui.py --port 5080
```

然后访问：

```text
http://127.0.0.1:5080/
http://127.0.0.1:5080/kvm
```

`serve-ui.py` 会 mock `/api/status`、`/api/snapshot`、`/api/stream` 和 `/api/ws/hid`，用于先检查 Dashboard/KVM 布局、视频区域、WebSocket 状态、键盘事件和鼠标摇杆/触控板事件。

## 烧录并看日志

推荐这条命令：烧录后会自动从串口日志里抓取以太网 DHCP IP，抓到后写入 `build/last_ip.txt` 并退出。

```bash
cd hosts/esphost-esp32p4/firmware
./tools/flash-monitor.sh /dev/cu.usbmodem5B5E1314701 --wait-ip --exit-on-ip
```

如果想烧录后一直停在串口日志里：

```bash
cd hosts/esphost-esp32p4/firmware
./tools/flash-monitor.sh /dev/cu.usbmodem5B5E1314701
```

也可以直接用 ESP-IDF：

```bash
idf.py -p /dev/cu.usbmodem5B5E1314701 flash monitor
```

串口号以现场 `ls /dev/cu.usbmodem*` 为准。如果没有插网线、交换机没有 DHCP、或 `CONFIG_SI_ETH_ENABLE` 被关闭，脚本会超时拿不到 IP。

## 使用

烧录后看串口日志里的 Ethernet IP，然后访问：

```text
http://<board-ip>/
http://<board-ip>/kvm
```

常用接口：

```bash
curl http://<board-ip>/api/status
curl http://<board-ip>/api/video/status
curl http://<board-ip>/api/snapshot --output frame.jpg
curl http://<board-ip>/api/stream
curl -H 'Authorization: Bearer admin' \
  -H 'Content-Type: application/json' \
  -d '{"quality":75}' \
  http://<board-ip>/api/video/quality
```

HID WebSocket：

```text
ws://<board-ip>/api/ws/hid
```

已支持的 HID 命令包括 `keydown`、`keyup`、`combo`、`releaseall`、`mousemove`、`mousedown`、`mouseup`、`click`、`wheel`。

`mousemove` 兼容两种单位：

```json
{"type":"mousemove","dx":0.2,"dy":-0.1}
{"type":"mousemove","unit":"hid","dx":18,"dy":-6}
```

第一种是归一化位移，`dx/dy` 范围建议为 `-1.0..1.0`；第二种直接发送 USB HID 相对鼠标计数，`dx/dy` 会被限制到 `-127..127`。KVM 页面的摇杆和触控板使用第二种方式；触控板在鼠标按住拖动时会优先使用浏览器 Pointer Lock，把控制端鼠标指针锁在原地，只把相对位移发给被控主机。不支持 Pointer Lock 的浏览器会退回到隐藏控制端光标并固定触控板中心点的模式。

## 当前已迁移功能

- `/`：HID/视频状态仪表盘，含 snapshot 预览和 JPEG quality 调整。
- `/kvm`：快照刷新视频画面 + 键盘 HID + 鼠标摇杆/触控板控制页。
- `/api/status`：系统、网络、HID、视频状态。
- `/api/hid/status`：TinyUSB HID 枚举状态。
- `/api/video/status`：CSI/JPEG 状态、帧计数、FPS、最近 JPEG 大小。
- `/api/snapshot`：最近一帧 JPEG。
- `/api/stream`：兼容 multipart 单帧输出；KVM 默认不用长连接 MJPEG。
- `/api/video/quality`：运行时调整 JPEG quality。
- `/api/ws/hid`：HID WebSocket。

## 当前限制

- GPIO 和 ATX 电源控制这版没有参与构建，也没有注册 Web/API。
- LT6911C HDMI-to-CSI 桥片模式暂时不是默认路径；当前先支持 MIPI 摄像头。
- 还没有 H.264；当前先用 RAW8 -> ISP RGB565 -> JPEG 打通链路。
- ESP-IDF 内置 HTTP server 是单任务处理，长连接 MJPEG 会阻塞后续 HID WebSocket；当前 KVM 页面改用 `/api/snapshot` 快照刷新。
- CM4 的 Linux Web shell / PTY 终端不适合直接移植到 MCU，当前没有实现。
- 当前网络只走 RJ45 以太网；没有启用 Wi-Fi/SoftAP。
- TF 卡 SDMMC 引脚保留为 `CLK=43 CMD=44 D0=39 D1=40 D2=41 D3=42`，但当前 Web/HID/视频链路还没有用 TF 卡存储。
- CSI 捕获目前是 bring-up 实现；实测如果出现花屏、颜色错位或帧率不稳，优先检查 sensor format、CSI 分辨率、lane bitrate、排线方向和摄像头供电。
