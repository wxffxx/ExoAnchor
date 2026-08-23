# ExoAnchor PrototypeV0 板型配置

Prototype0 是第一代自研板验证参考，不是 PrototypeV2.1。

## 依据与身份

- 网表：`Netlist_Prototype_v0_2026-07-14.net`
- SHA-256：`3ff99c0d8de57df435d843389f619838d44c4707d92ae849c2e8273dad5f7baf`
- 实测芯片：ESP32-P4 rev3.2；Flash 16 MB；PSRAM 32 MB
- 实测网络：IP101 后端、PHY 地址 1，可完成 Link Up 与 DHCP

| 字段 | 值 |
| --- | --- |
| Kconfig | `SI_BOARD_EXOANCHOR_PROTOTYPE0` |
| Board ID | `exoanchor-prototype0` |
| 显示名称 | `ExoAnchor PrototypeV0` |
| Hostname | `exoanchor-p0` |
| 芯片 overlay | `esp32p4-rev3` |
| Ethernet PHY | IP101GRI U4，地址 1 |

## GPIO 映射

| 功能 | 信号 | GPIO |
| --- | --- | ---: |
| Ethernet | MDC / MDIO / RESET / INTR | 20 / 21 / 22 / 23 |
| RMII | CRS_DV / RXD0 / RXD1 | 28 / 29 / 30 |
| RMII | REF_CLK / TX_EN / TXD0 / TXD1 | 32 / 33 / 34 / 35 |
| USB HID FS | DM / DP | 26 / 27 |
| TF 卡 | detect / D0–D3 / CLK / CMD | 16 / 39–42 / 43 / 44 |
| ATX | 电源 / 复位输出 | 1 / 2 |
| 状态 | 定位灯 / 12V / 3V3AUX | 3 / 4 / 5 |
| 开发串口 | CH343P RXD / TXD | 37 / 38 |

GPIO37/GPIO38 只属于开发、烧录与调试串口，不能向 Agent 或 Web UI 报告为被控端串口能力。

TF 卡检测 GPIO16 低有效且已通过真机验证。C108 是时钟到地的调谐电容，装配时会使 SDMMC 在 OCR 前超时；移除后原生四线 SDMMC 可正常挂载，因此 Prototype0 的生产与返修配置必须将 C108 设为 DNP。

CH343P 的 DTR/RTS 经 U2 接入 EN/BOOT。esptool 5.3.0 已验证自动进入 ROM 下载模式并完成烧录；esptool 4.11 无法可靠驱动 ECO7 flasher stub，不应据此判断下载电路故障。
