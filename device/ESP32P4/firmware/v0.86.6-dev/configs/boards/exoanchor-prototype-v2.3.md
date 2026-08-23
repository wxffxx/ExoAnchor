# ExoAnchor PrototypeV2.3 板型配置

这是 ESP32-P4 第 4 种物理实现的 bring-up 配置。操作方将已连接实物标识为 PrototypeV2.3b4，现有电气来源则名为 PrototypeV2.3b6；差异闭合前，固件只使用通用 `PrototypeV2.3` 身份，GPIO 映射不视为量产冻结契约。

## 依据与身份

- 网表：`Netlist_SCH_ESP32P4_Prototype_V2.3b6_1_2026-07-25.tel`
- SHA-256：`0ce2340e77fa5e267ef1eb04ab4fb7ea71ba6c3f9917ba7715f6579ec71b85fa`
- 实测芯片：ESP32-P4 rev3.2、40 MHz 晶振、16 MB Flash、32 MB PSRAM
- 状态：首板 bring-up，整板验收尚未完成

2026-07-25 网表与 2026-07-16 的 V2.3b6 导出连接关系一致；差异只涉及电源电容值与四个 PCIe 网名大小写，不改变 ESP32-P4 GPIO。

| 字段 | 值 |
| --- | --- |
| Kconfig | `SI_BOARD_EXOANCHOR_PROTOTYPE_V23` |
| Board ID | `exoanchor-prototype-v2.3` |
| Hostname | `exoanchor-v23` |
| 芯片 overlay | `esp32p4-rev3` |
| Ethernet PHY | DP83825I U19，地址 1 |

## GPIO 映射

| 功能 | 信号 | GPIO |
| --- | --- | ---: |
| ATX 输出 | 电源 / 复位 | 4 / 5 |
| ATX 检测 | 12V / 3V3AUX | 0 / 1 |
| 状态 | 定位灯 | 17 |
| Ethernet | 软件控制灯 | 15，暂未实现驱动 |
| MS2109 EEPROM | 写保护 | 16，高有效 |
| 次级 SPI NAND | IO3 / CS / IO0 / CLK / IO1 / IO2 | 6 / 7 / 8 / 9 / 10 / 11 |
| Ethernet | MDC / MDIO / RESET / INTR-PWDN | 20 / 21 / 22 / 23 |
| RMII | CRS_DV / RXD0 / RXD1 / RX_ER | 28 / 29 / 30 / 31 |
| RMII | REF_CLK / TX_EN / TXD0 / TXD1 | 32 / 33 / 34 / 35 |
| USB Serial/JTAG | DM / DP | 24 / 25 |
| USB HID FS | DM / DP | 26 / 27 |
| 开发串口 U1 | MCU TX / MCU RX | 37 / 38 |
| TF 卡 | D0–D3 / CLK / CMD | 39–42 / 43 / 44 |
| MS2109 EEPROM | SCL / SDA | 47 / 48 |
| 被控端串口 U17 | MCU RX / MCU TX | 50 / 51 |

TF 卡座没有检测触点，因此 `CONFIG_SI_TF_CARD_DETECT_GPIO=-1`。GPIO16 是 AT24C16C 写保护，不能复用为 TF 卡检测。

次级 GD5F2GQ5UEYIGR SPI NAND 不是启动 Flash，当前固件也没有对应驱动。标准配置保留物理 AT24C16C U5，只控制其写保护；只有确认 U5 已移除的实验板才能追加 `sdkconfig.defaults.exoanchor-prototype-v2.3-ms2109-eeprom-emulator`。

U1 经 DTR/RTS 和 U2 提供自动复位/下载，是开发、烧录和调试串口。U23 暴露 GPIO24/GPIO25 原生 USB Serial/JTAG；板型配置仍以 UART0 为主控制台、USB Serial/JTAG 为次控制台。两者均独立于 GPIO26/GPIO27 的 USB HID。

## 已验证能力与边界

- Ethernet、USB HID、UVC 模式协商、被控端串口和基础 Web 操作已有首板证据。
- U17 是数据串口，没有 DTR/RTS，不能自动复位或进入下载模式。
- 被控端串口默认 `115200 8N1`，可手动切换到 `9600 8N1`；切换不写 NVS，重启恢复 115200。
- 固件为 HTTP、诊断 CLI 和 WebSocket 提供独立接收路径，并拒绝第二个 WebSocket 客户端静默替换第一个客户端。
- 1080p 与 720p 有效画质无差异的问题已经在固件侧修复并通过当前实板确认；
  板载 MS2109、补强电容和板级供电/信号完整性不是该问题的根因。

从旧 `exoanchor-esp32p4x` 配置迁移时，可在首次构建中追加 `sdkconfig.defaults.exoanchor-prototype-v2.3-migrate-legacy-nvs`，清除旧板型保存的 GPIO 设置；普通升级必须移除该 overlay，以保留用户设置。
