# ExoAnchor PrototypeV2.4 产品板型配置

这是 PrototypeV2.4 的正式产品构建入口。项目方确认 `V2.4a6` 的 `a6` 只表示
PCB 层数，原理图与 ESP32-P4 GPIO 映射和 V2.4 相同，因此不建立单独的 `a6`
Board ID 或固件 profile。

> 当前状态：配置契约已建立，尚未据此烧录实物，也未完成 V2.4/V2.4a6 HIL。
> 可构建不代表硬件已验证或可以量产。

## 依据与身份

- 硬件映射：`Netlist_SCH_ESP32P4_Prototype_V2.4a6_2026-07-29.net`
- SHA-256：`6cb86589d38065e07911c18dce82e2df5d8e72925160999f526e2a1dc2e73a1d`
- 目标芯片：ESP32-P4 rev3 overlay

| 字段 | 值 |
| --- | --- |
| Kconfig | `SI_BOARD_EXOANCHOR_PROTOTYPE_V24` |
| Board ID | `exoanchor-prototype-v2.4` |
| 显示名称 | `ExoAnchor PrototypeV2.4` |
| Hostname | `exoanchor-v24` |
| 芯片 overlay | `esp32p4-rev3` |
| Ethernet PHY | DP83825I，地址 1 |

## GPIO 与产品能力契约

| 功能 | GPIO / 边界 |
| --- | --- |
| ATX 输出 | PWR 4 / RST 5，高电平有效 |
| ATX 检测 | 12V 0 / 3V3AUX 1，高电平有效 |
| Locator | GPIO17，单线、高电平点亮 |
| MS2109 产品上电 | GPIO13 开启 3V3_MS，等待后由 GPIO18 开启 1V2 |
| Ethernet SMI | MDC 20 / MDIO 21 / RESET 22 / INT-PWDN 23 |
| Ethernet RMII | CRS_DV 28 / RXD0 29 / RXD1 30 / REF_CLK 32 / TX_EN 33 / TXD0 34 / TXD1 35 |
| USB HID | DM 26 / DP 27 |
| TF | D0-D3 39-42 / CLK 43 / CMD 44；无 card-detect，设为 `-1` |
| 目标 UART1 | MCU RX 50 / MCU TX 51；默认 115200，备用 9600 |
| AT24C16 | WP 16 / SCL 47 / SDA 48 只记录物理映射，不作为产品读写能力 |

产品 profile 只执行 MS2109 的正常两级上电，不编译或开放 EEPROM 测试驱动、
UART 写入命令、HTTP 写入接口或 EEPROM 模拟器。物理 AT24C16 的连接不能自动
转化为用户可用 capability。

## Settings 能力显隐

- 使用 0.87.5 产品 Settings 的业务分组和独立“高级设置”入口；不恢复旧的
  “产品功能”总开关或卡片标题状态徽标。
- MS2109 正常电源控制按 `ms2109_power` capability 显示在高级设置；EEPROM
  名称、读取、编程、上传和 API 均不得出现在产品 Settings。
- Locator 亮灭只在 Overview。方向反转仅在设备报告双 GPIO Locator capability
  时显示；本 profile 的 `return_gpio=-1`，因此该设置在 V2.4 上完全隐藏。
- Dev Agent 设置按构建能力显示，外部 MCP 权限保持独立；不支持的板型开关直接
  隐藏，不以置灰控件冒充可用功能。

## 与其他 profile 的边界

- `exoanchor-prototype-v2.4`：普通 Dev 产品构建入口；不含破坏性 EEPROM 功能。
- `exoanchor-prototype-v2.4-ms-test`：独立硬件验证镜像，可以控制 MS2109 电源并
  读写 EEPROM；不得用于普通产品构建、发布或替代本 profile。

任何烧录前仍须由操作人员确认实物型号，并让精确 CH343 serial、eFuse MAC、
silicon revision、目标 profile 和显式空闲端口与板卡身份记录全部匹配。当前
文档不构成烧录授权；首次 V2.4/V2.4a6 烧录和完整能力声明必须等待独立 HIL 证据。
