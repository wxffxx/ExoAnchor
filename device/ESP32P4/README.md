# ESP32-P4 平台

ESP32-P4 是 ExoAnchor 当前唯一正式设备平台。本目录只维护软件侧的板型映射、
固件版本和运行能力；原理图、PCB/CAD、BOM、装配和生产资料由
[ExoAnchor-Hardware](https://github.com/wxffxx/ExoAnchor-Hardware/tree/main/ESP32P4)
维护。

## 目录

```text
device/ESP32P4/
├── boards/
│   └── IMPLEMENTATION_PROFILES_zh.md  # 板型、固件配置、芯片版本和验证状态
└── firmware/
    ├── README.md                       # 固件版本入口
    ├── v0.86.6-dev/                    # 当前 Dev 源码树
    └── v0.86-stable-kvm/               # 独立纯 KVM 源码树
```

软件仓库不再复制各物理板卡的硬件 README。需要接线、制造或检查硬件时，直接使用
Hardware 仓库中的对应目录。

## 物理实现

| # | 实现 | 固件组合 | 当前状态 |
| ---: | --- | --- | --- |
| 1 | ESP32-P4 开发板 + 外接采集卡 | `waveshare-p4-nano + rev1` | 已知良好的冻结参考 |
| 2 | Waveshare NANO + 简易扩展板 | 当前共享 `waveshare-p4-nano + rev1` | 原型，扩展能力未完全建模 |
| 3 | Prototype0 | `exoanchor-prototype0 + rev3` | 较完整真机验证参考 |
| 4 | PrototypeV2.3 | `exoanchor-prototype-v2.3 + rev3` | b4 标识实物使用 b6 映射，bring-up 已验证 |
| 4A | PrototypeV2.4 / V2.4a6 | `exoanchor-prototype-v2.4 + rev3` | 正式产品配置入口；`a6` 仅表示 PCB 层数且原理图映射相同；尚未烧录或完成 HIL |

完整 GPIO、网表来源、能力和证据等级见
[板型与固件 Profile](boards/IMPLEMENTATION_PROFILES_zh.md)。PrototypeV2.1 是仅编译
设计参考，legacy `exoanchor-esp32p4x` 是冻结兼容 profile，均不代表新的现行
物理实现。

`exoanchor-prototype-v2.4-ms-test` 是独立的 MS2109/EEPROM 硬件验证镜像，不是
上述 V2.4 产品 profile；Production TypeC 和 TypeW 也各自使用独立映射。

## 当前能力边界

| 实现 | UVC | HID | Ethernet | 目标机 UART | ATX/电源状态 |
| --- | --- | --- | --- | --- | --- |
| 开发板直连 | 已知良好 | GPIO26/27 | IP101GRI | 无 | 无 |
| NANO 扩展板 | 支持 | 依扩展映射 | IP101GRI | 无 | 依扩展板，未完整建模 |
| Prototype0 | 已验证 | 已验证 | IP101GRI 已验证 | 无 | PWR/RST/12V/3V3AUX 已验证 |
| PrototypeV2.3 | 模式已验证，画质问题开放 | bring-up 已验证 | DP83825I bring-up | GPIO50/51 已验证 | 来自网表，量产验证未完成 |
| PrototypeV2.4 / V2.4a6 | profile 已配置，HIL 待完成 | GPIO26/27，HIL 待完成 | DP83825I，HIL 待完成 | GPIO50/51，HIL 待完成 | PWR/RST/12V/3V3AUX 仅配置，HIL 待完成 |

Dashboard 和 `GET /api/capabilities` 必须反映实际 profile，不能显示其他板型的
能力。

## 构建入口

当前开发版本是 `0.87.6-dev`，源码目录仍名为 `v0.86.6-dev`，因为它从 0.86.6
加固基线演进而来。

- 构建当前开发固件：
  [`firmware/v0.86.6-dev/README.md`](firmware/v0.86.6-dev/README.md)
- 构建独立纯 KVM：
  [`firmware/v0.86-stable-kvm/README.md`](firmware/v0.86-stable-kvm/README.md)
- 选择版本和了解升级边界：
  [`firmware/README.md`](firmware/README.md)

当前 Dev 树的 rev3/ECO7 板型（包括 PrototypeV2.4）使用 ESP-IDF v5.5.5 或更高兼容版本；冻结的 rev1/Waveshare 路线
使用 ESP-IDF v5.4.x。切换板型或芯片版本时必须使用新的 build 目录和生成配置。

## 运行入口

```text
http://<board-ip>/
http://<board-ip>/kvm
http://<board-ip>/agent
http://<board-ip>/terminal
http://<board-ip>/settings
```

首次启动必须替换本地 bootstrap 凭据。共享文档不发布凭据值。
