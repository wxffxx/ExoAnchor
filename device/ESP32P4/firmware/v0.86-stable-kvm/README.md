# ExoAnchor 0.86 Stable KVM

`v0.86-stable-kvm` 是从 `v0.86.2-dev` 快照直接复制并裁剪得到的独立纯 KVM 固件，固件版本为 `0.86.0-stable-kvm`。本目录不通过软链接或共享源码依赖 Dev；后续修复需要明确回移。

当前状态：主机测试、ExoAnchor PrototypeV0 和 Waveshare ESP32-P4-NANO 两套完整编译已通过。Prototype0 已完成烧录和启动冒烟测试：ESP32-P4 rev3.2、32 MiB PSRAM、USB HID、ATX GPIO、IP101GRI Ethernet、HTTP 页面和 UART CLI 均已实际启动；UVC 已在 KVM 页面实测输出 `1280x720 @ 30fps` MJPEG 画面。测试地址为 DHCP 动态分配，不应写入固件配置。

Stable KVM 的默认板型和当前真机烧录目标是 **ExoAnchor PrototypeV0**。Waveshare、ESP32-P4x、PrototypeV2.1 和 PrototypeV2.3 必须通过各自的显式 `SDKCONFIG_DEFAULTS` overlay 构建，禁止将未核对板型的默认构建烧入当前 Prototype0。

公开配置不包含默认口令。每个构建必须在生成的本地 `sdkconfig` 中设置唯一的 6..64 位 `CONFIG_SI_AUTH_PASSWORD`；源码会拒绝以空口令编译，生成的配置与构建目录不得提交。

本次没有对目标主机实际发送 HID、执行电源/复位动作，也没有完成长时间运行和完整 KVM 交互回归，因此仍不能视为最终硬件发布验收。

## 功能边界

| 保留 | 删除 |
| --- | --- |
| UVC HDMI 视频、截图与 MJPEG 流 | 嵌入式 Agent 及运行引擎 |
| USB HID 键盘和鼠标 | Provider、Prompt、Skills、History、Memory、Sessions |
| Ethernet、认证、Dashboard 和手动 KVM | SSH 客户端和 Web Terminal / xterm |
| 状态、capabilities、日志与必要诊断 | TF 卡文件管理与存储 API |
| 控制租约与批量 HID API | MCP 设置和 Agent 专用 API |
| 已支持板型的电源与复位控制 | Agent 日志分区与相关依赖 |

控制租约仍保留 `human`、`agent`、`mcp` 等既有 owner 字符串，目的是兼容已授权的外部控制客户端；它们不表示稳定固件内含 Agent 或 MCP 运行时。`GET /api/capabilities` 会明确报告 `embedded_agent=false`、`ssh_client=false`、`tf_file_manager=false`。

稳定版不提供在线固件更新、远程清单或固件上传入口。更新固件必须通过受控的有线烧录流程完成。

## UVC 兼容覆盖

稳定版固定使用 `usb_host_uvc 2.5.1`，并在构建时以 [`patches/usb_host_uvc-2.5.1/uvc_isoc.c`](patches/usb_host_uvc-2.5.1/uvc_isoc.c) 替换其 ISOC 源文件。原因是部分 HDMI 采集卡不发送 UVC EOF 标志，原始驱动会持续报告 `missed EoF`；兼容实现改用 JPEG SOI/EOI 识别 MJPEG 帧边界。

KVM 页面将视频元素约束在独立的绝对定位显示框内，再以 `object-fit: contain` 保持输入宽高比。浏览器可用区域与视频比例不一致时会留出黑边，不允许拉伸视频元素后由容器裁掉下边界。

顶层 CMake 对上游原始文件执行 SHA-256 防漂移检查。升级 `usb_host_uvc` 时必须重新审阅该覆盖；来源、维护规则和第三方 `Apache-2.0` 许可边界见 [`patches/usb_host_uvc-2.5.1/README.md`](patches/usb_host_uvc-2.5.1/README.md)。

## 测试

```bash
cd device/ESP32P4/firmware/v0.86-stable-kvm
./tests/host/run.sh
```

该命令覆盖核心主机测试、设置存储测试，以及稳定版 HTTP/UI 路由边界检查。

## 构建 Prototype0

```bash
cd device/ESP32P4/firmware/v0.86-stable-kvm
. "$HOME/esp/esp-idf-v5.5.4/export.sh"
idf.py \
  -B build-prototype0 \
  -D SDKCONFIG=build-prototype0/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/boards/sdkconfig.defaults.exoanchor-prototype0;configs/silicon/sdkconfig.defaults.esp32p4-rev3" \
  set-target esp32p4
idf.py -B build-prototype0 menuconfig # SI ESP32-P4 Host Configuration -> bootstrap password
idf.py -B build-prototype0 build
```

## 构建 Waveshare ESP32-P4-NANO

```bash
cd device/ESP32P4/firmware/v0.86-stable-kvm
. "$HOME/esp/esp-idf-v5.4/export.sh"
idf.py \
  -B build-waveshare \
  -D SDKCONFIG=build-waveshare/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/boards/sdkconfig.defaults.waveshare-p4-nano;configs/silicon/sdkconfig.defaults.esp32p4-rev1" \
  set-target esp32p4
idf.py -B build-waveshare menuconfig # SI ESP32-P4 Host Configuration -> bootstrap password
idf.py -B build-waveshare build
```

PrototypeV2.3 已进入开发固件 bring-up，但不属于当前 Stable KVM 的发布和真机验收目标。板型和能力依据见 [`../../boards/IMPLEMENTATION_PROFILES_zh.md`](../../boards/IMPLEMENTATION_PROFILES_zh.md)。

## 烧录

使用与构建命令相同的 build 目录。以 Prototype0 为例：

```bash
./tools/flash-monitor.sh <PORT> \
  --build-dir build-prototype0 \
  --wait-ip \
  --exit-on-ip
```

`<PORT>` 必须替换为系统识别到的实际串口。
