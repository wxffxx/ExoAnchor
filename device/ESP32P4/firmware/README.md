# ESP32-P4 固件版本

本目录维护彼此独立的 ESP-IDF 固件源码树。每个版本拥有自己的源码、依赖、
配置、测试、工具和构建输出，不通过软链接共享实现。

## 当前版本

| 目录 | 运行版本 | 用途 | 状态 |
| --- | --- | --- | --- |
| [`v0.86.6-dev/`](v0.86.6-dev/) | `0.87.6-dev IndigoShore` | KVM + 设备内 Agent 开发线 | 当前开发；各板型真机验证独立记录 |
| [`v0.86-stable-kvm/`](v0.86-stable-kvm/) | `0.86.0-stable-kvm` | 独立纯 KVM | 已通过主机测试、两套构建和 Prototype0 启动冒烟 |

目录名 `v0.86.6-dev` 表示源码起点，不是当前运行版本。当前版本的唯一权威值在
该目录的 [`CMakeLists.txt`](v0.86.6-dev/CMakeLists.txt)。

完整演进、编号异常和升级要求见
[固件版本历史](docs/FIRMWARE_VERSION_HISTORY_zh.md)。

## 如何选择

- 需要当前 Agent、UART、SSH、Terminal、存储和外部 MCP API：使用 Dev。
- 只需要独立手动 KVM，并希望缩小功能面：使用 Stable KVM。
- Stable 与 Dev 的修复必须显式回移，不能假设其中一个自动继承另一个。
- 两条源码线都还需要对应板型的完整 HIL 和长期稳定性回归后才能形成发布声明。

## 构建规则

进入所选版本目录后再执行构建：

```bash
cd device/ESP32P4/firmware/v0.86.6-dev
```

每次构建必须同时选择：

1. 一个 `configs/boards/` 板型 profile；
2. 一个 `configs/silicon/` 芯片版本 overlay；
3. 与芯片版本兼容的 ESP-IDF 版本；
4. 独立的 build 目录和 `sdkconfig`。

当前 Dev 树的 Prototype0、PrototypeV2.1、PrototypeV2.3 和 PrototypeV2.4
rev3/ECO7 使用 ESP-IDF v5.5.5 基线。V2.4a6 与 V2.4 共用
`exoanchor-prototype-v2.4` 产品
profile；`a6` 只表示 PCB 层数，原理图映射不变。Waveshare/rev1 冻结路线使用
ESP-IDF v5.4.x。具体命令分别记录在两个版本目录的 README 中。

## 烧录规则

构建成功后，必须把板型对应的 build 目录显式传给烧录助手：

```bash
./tools/flash-monitor.sh <PORT> \
  --build-dir <build-dir> \
  --wait-ip \
  --exit-on-ip
```

该命令需要在所选固件版本目录中执行。`<PORT>` 是实际串口，`<build-dir>` 必须
与构建时 `idf.py -B` 使用的目录相同。也可以直接执行
`idf.py -B <build-dir> -p <PORT> flash monitor`。

## 0.86.6 旧布局升级

0.86.6 将历史 `agentlog` 区域改为第二个 OTA application slot。运行 0.86.5
或更早分区布局的设备第一次安装 0.86.6 及后续 Dev 固件时，必须通过串口完整
写入：

- Bootloader；
- Partition table；
- OTA data；
- Factory application。

不能只把 application BIN 当作普通 OTA 包推送到旧布局。
