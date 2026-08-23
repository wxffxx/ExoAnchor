# ESP32-P4 板型实现矩阵

更新日期：2026-08-20

本文是物理实现、固件板型配置、芯片 overlay 和验证状态的统一事实入口。构建配置存在只表示固件保留该入口，不代表硬件已打样、验证或可量产。详细 GPIO 以开发固件 `configs/boards/` 下的板型文档和对应 `sdkconfig.defaults` 为准。

## 当前物理实现

| 实现 | Board profile | Silicon overlay | 当前状态 |
| --- | --- | --- | --- |
| Waveshare ESP32-P4-NANO + 外置采集卡 + GPIO26/27 HID | `waveshare-p4-nano` | `esp32p4-rev1` | 冻结兼容参考；无 ATX 和电源检测 |
| Waveshare NANO + 简易扩展板 | 暂时复用 `waveshare-p4-nano` | `esp32p4-rev1` | 有 2026-06-27 设计输出；尚无独立 overlay 或电源能力验收 |
| ExoAnchor PrototypeV0 | `exoanchor-prototype0` | `esp32p4-rev3` | 验证范围较完整的硬件参考 |
| ExoAnchor PrototypeV2.3 | `exoanchor-prototype-v2.3` | `esp32p4-rev3` | 当前 bring-up 主线；实物标识 b4、映射依据 b6，尚非量产定稿 |
| ExoAnchor PrototypeV2.4 / V2.4a6 | `exoanchor-prototype-v2.4` | `esp32p4-rev3` | 正式产品配置入口；V2.4a6 的 `a6` 只表示 PCB 层数，原理图映射与 V2.4 相同；尚未烧录或完成 HIL |
| ExoAnchor Production TypeC | `exoanchor-production-typec` | `esp32p4-rev1` | rev1.3 实物已完成 MS 两级上电、UVC 枚举；此前已完成 DP83825I 100M 全双工和基础 Web 验证 |
| ExoAnchor Production TypeW 候选 | 尚未建立 | 待外部证据确认 | 与 PrototypeV2.4 分离；已确认 TypeW 定位灯为 GPIO21，但正式 profile 和完整板型映射尚未进入本仓库 |

另有三个非现行产品配置：

- `exoanchor-prototype-v2.1 + esp32p4-rev3`：仅编译的中间设计参考。
- `exoanchor-esp32p4x + esp32p4-rev1`：旧版冻结兼容配置。
- `exoanchor-prototype-v2.4-ms-test + esp32p4-rev3`：独立的 MS2109
  电源/EEPROM 硬件验证镜像，包含破坏性维护接口，不得作为 V2.4 产品固件。

## 能力边界

| 实现 | 已确认能力 | 不能声明的能力 |
| --- | --- | --- |
| Waveshare DIY | IP101GRI Ethernet、外置 UVC、GPIO26/27 HID | PWR/RST、12V、3V3AUX |
| Waveshare 扩展板 | NANO 基础 KVM 路径 | 在独立 GPIO/极性配置与真机验收前，不能声明电源能力 |
| Prototype0 | IP101GRI、UVC、HID、PWR/RST、12V、3V3AUX、TF | 被控端 UART |
| PrototypeV2.3 | DP83825I、UVC 模式、HID、目标 UART、基础 Web 与电源 GPIO 的 bring-up 证据 | 整板量产验收；板载 MS2109 的有效原生 1080p |
| PrototypeV2.4 / V2.4a6 | 产品 profile 配置 DP83825I、UVC、HID、目标 UART、PWR/RST、12V/3V3AUX、TF、Locator 和 MS2109 两级上电 | 这些是可构建的配置契约，不是真机证据；不得声明已烧录、已完成 HIL、EEPROM 产品读写或量产就绪 |
| Production TypeC | DP83825I 100M 全双工、基础 Web、PWR/RST、MS 3V3/1V2 上电、UVC 模式 | 插卡状态未知时不能声明 TF 已通过；物理 EEPROM 未接 P4，不提供读写接口 |
| Production TypeW 候选 | 定位灯 GPIO21 的板级映射 | 在外部板型身份和独立 profile 完成前，不能据此声明 Ethernet、EEPROM、电源或其他能力；定位灯有效电平尚未确认 |

Prototype0 的真机结论不能移植到其他板型。PrototypeV2.3 当前板载 MS2109 在输入为 1280×720@60 时稳定；名义 1080p 输出的有效细节仍接近放大的 720p，该结论只适用于当前板载实现。

硬件仓库入口：

- [Waveshare DIY](https://github.com/wxffxx/ExoAnchor-Hardware/tree/main/ESP32P4/simple-diy/waveshare-nano-diy)
- [Waveshare 简易扩展板](https://github.com/wxffxx/ExoAnchor-Hardware/tree/main/ESP32P4/simple-diy/waveshare-nano-expansion)
- [ExoAnchor P4 V2.4a6](https://github.com/wxffxx/ExoAnchor-Hardware/tree/main/ESP32P4/exoanchor-p4-v2.4a6)

## 自研板电气基线

| 设计 | 当前来源与 SHA-256 | 状态 |
| --- | --- | --- |
| Prototype0 | `Netlist_Prototype_v0_2026-07-14.net` · `3ff99c0d8de57df435d843389f619838d44c4707d92ae849c2e8273dad5f7baf` | 已验证参考 |
| PrototypeV2.1b | `Netlist_SCH_ESP32P4_Prototype_V2.1b_2026-07-14.net` · `a474080aee7a09a4d9c43ef9c99ae0bc6784c3ff5f92f2d29c895862406a51e2` | 仅编译参考 |
| PrototypeV2.3b6 | `Netlist_SCH_ESP32P4_Prototype_V2.3b6_1_2026-07-25.tel` · `0ce2340e77fa5e267ef1eb04ab4fb7ea71ba6c3f9917ba7715f6579ec71b85fa` | b4 标识实物的 provisional 映射来源 |
| PrototypeV2.4a6 | `Netlist_SCH_ESP32P4_Prototype_V2.4a6_2026-07-29.net` · `6cb86589d38065e07911c18dce82e2df5d8e72925160999f526e2a1dc2e73a1d` | V2.4 产品 profile 的映射依据；`a6` 仅表示 PCB 层数，原理图映射不变；HIL 待完成 |
| Production TypeC | `Netlist_量产版SCH_2026-08-06.tel` · `30422e276d4e4377111a3ff144cdcef61838ac2fb119a4ceecf93d6c209cf37b` | rev1.3 TypeC 的 MS 上电与板级 profile 依据；已完成首轮 MS/UVC 验证 |

### 关键 GPIO 差异

| 功能 | Prototype0 | PrototypeV2.1b | PrototypeV2.3 | PrototypeV2.4 | Production TypeC |
| --- | ---: | ---: | ---: | ---: | ---: |
| PWR / RST | 1 / 2 | 5 / 4 | 4 / 5 | 4 / 5 | 50 / 51 |
| 12V / 3V3AUX | 4 / 5 | 0 / 1 | 0 / 1 | 0 / 1 | 52 / 53 |
| Locator LED | 3 | 15 | 17 | 17 | 19，高电平点亮 |
| Ethernet PHY | IP101GRI | DP83825I | DP83825I | DP83825I | DP83825I |
| TF detect | 16 | 47 | 无，设为 `-1` | 无，设为 `-1` | 无，设为 `-1` |
| MS2109 3V3 / 1V2 | 无独立产品时序 | 无独立产品时序 | 无独立产品时序 | 13 / 18 | 17 / 18 |
| MS2109 EEPROM SCL/SDA | 13 / 14 | 13 / 14 | 47 / 48 | 47 / 48，仅物理映射 | 仅接 MS2109 与 U7，未接 P4 |
| MS2109 EEPROM WP | MS2109 GPIO5 | MS2109 GPIO5 | ESP32-P4 GPIO16 | ESP32-P4 GPIO16，仅物理映射 | 仅接 MS2109，未接 P4 |
| 目标 UART | 无 | 未分配 | RX 50 / TX 51 | RX 50 / TX 51 | 无；GPIO50/51 用于 PWR/RST |
| 次级 SPI NAND | 无 | 无 | GPIO6–11，当前无驱动 | 不作为当前产品 capability | 无 |

当前自研板产品配置共同使用 GPIO20–23 的 Ethernet SMI、GPIO28–35 的 RMII 主干、GPIO26/27 的 USB HID，以及 GPIO39–44 的四线 TF 总线；具体 PHY、极性与检测脚仍由板型配置决定。Production TypeC 的定位灯由实板确认使用 GPIO19；网表对应连接为 GPIO19 → R19（22 Ω）→ U22.1，U22 其余引脚接地。

PrototypeV2.4a6 不建立单独的 `a6` profile；项目方确认 `a6` 只表示 PCB 层数，
原理图和 ESP32-P4 映射与 V2.4 相同。`exoanchor-prototype-v2.4` 是普通产品入口；
`exoanchor-prototype-v2.4-ms-test` 只用于受控 MS2109/EEPROM 验证。二者也都不能
替代 Production TypeC 或 TypeW 的独立 profile。

Production TypeW 的定位灯 GPIO 单独确认为 GPIO21；受控本地 bring-up 已有逐板证据，但正式构建脚本尚未提供可公开复现的 TypeW profile，有效电平和其他板级映射仍不得从 TypeC 或其他 TypeW 实例推断。

详细配置：

- [Prototype0](../firmware/v0.86.6-dev/configs/boards/exoanchor-prototype0.md)
- [PrototypeV2.1](../firmware/v0.86.6-dev/configs/boards/exoanchor-prototype-v2.1.md)
- [PrototypeV2.3](../firmware/v0.86.6-dev/configs/boards/exoanchor-prototype-v2.3.md)
- [PrototypeV2.4 / V2.4a6](../firmware/v0.86.6-dev/configs/boards/exoanchor-prototype-v2.4.md)

Production TypeC / TypeW 的逐板身份和 bring-up 证据保存在受控开发工作区，不进入正式软件仓库；公开 profile 只能陈述可复现的通用契约。

## 使用规则

1. 构建时必须同时选择一个 board profile 和一个 silicon overlay，并为组合生成独立 `sdkconfig`。
2. 新硬件只有在 GPIO、PHY、存储或 capability 边界不同于现有配置时才增加 board profile。
3. 原理图或网表变化时，先更新本矩阵、板型说明和 overlay，再构建与上板验证。
