# ExoAnchor PrototypeV2.1b 板型配置

本配置记录 2026-07-14 的 PrototypeV2.1b 网表，仅作编译和中间设计参考。它不是已连接的 Prototype0，也没有继承 Prototype0 的真机结论。

## 依据与身份

- 网表：`Netlist_SCH_ESP32P4_Prototype_V2.1b_2026-07-14.net`
- SHA-256：`a474080aee7a09a4d9c43ef9c99ae0bc6784c3ff5f92f2d29c895862406a51e2`
- 目标芯片：ESP32-P4 rev3 overlay
- 状态：仅编译验证，未做硬件验证

相较更早的 `Netlist_SCH_6_2026-07-10.net`，本版移除 FEMDRW016G eMMC，并把 GPIO39–GPIO44 连接到 TF 卡座。导出文件残留的 `EMMC_D0` 等历史网名不代表仍有 eMMC。

| 字段 | 值 |
| --- | --- |
| Kconfig | `SI_BOARD_EXOANCHOR_PROTOTYPE_V21` |
| Board ID | `exoanchor-prototype-v2.1` |
| Hostname | `exoanchor-v21` |
| 芯片 overlay | `esp32p4-rev3` |
| Ethernet PHY | DP83825I U19，地址 1，未做真机验证 |

## GPIO 映射

| 功能 | 信号 | GPIO |
| --- | --- | ---: |
| ATX | 电源 / 复位输出 | 5 / 4 |
| ATX 检测 | 12V / 3V3AUX | 0 / 1 |
| 状态 | 定位灯 | 15 |
| Ethernet | MDC / MDIO / RESET / INTR-PWDN | 20 / 21 / 22 / 23 |
| RMII | CRS_DV / RXD0 / RXD1 / RX_ER | 28 / 29 / 30 / 31 |
| RMII | REF_CLK / TX_EN / TXD0 / TXD1 | 32 / 33 / 34 / 35 |
| USB HID FS | DM / DP | 26 / 27 |
| 开发串口 | U1 CH343P RXD / TXD | 37 / 38 |
| TF 卡 | D0–D3 / CLK / CMD / detect | 39–42 / 43 / 44 / 47 |
| MS2109 EEPROM | SCL / SDA | 13 / 14 |
| 次级 CH343P | U17 TXD / RXD | 50 / 51 |

GPIO2/GPIO3 只引到 H3，固件职责未冻结；GPIO6、GPIO7、GPIO16、GPIO17、GPIO45、GPIO46、GPIO48 在该导出中没有第二端点，不能视为已实现能力。TF 卡检测 GPIO47 低有效；本板型不得声明 eMMC 能力。
