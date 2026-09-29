# 固件构建配置

通用 ESP-IDF 和应用默认值位于 `../sdkconfig.defaults`；`boards/` 选择板型，`silicon/` 选择互斥的 ESP32-P4 芯片版本。生成新 `sdkconfig` 时必须同时指定板型和芯片版本 overlay：

```bash
idf.py \
  -B build-exoanchor-prototype0-rev3 \
  -D SDKCONFIG=build-exoanchor-prototype0-rev3/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/boards/sdkconfig.defaults.exoanchor-prototype0;configs/silicon/sdkconfig.defaults.esp32p4-rev3" \
  set-target esp32p4
```

物理实现与构建配置不是一一对应关系，映射以[板型实现矩阵](../../../boards/IMPLEMENTATION_PROFILES_zh.md)为准。当前规则如下：

- PrototypeV2.3/rev3 是 bring-up 主线；实物标识 b4、现有电气来源 b6，映射仍未冻结。
- PrototypeV2.4/rev3 的正式产品入口是 `exoanchor-prototype-v2.4`；V2.4a6 的
  `a6` 只表示 PCB 层数，原理图映射不变，因此不建立单独 `a6` profile。当前
  只建立配置契约，HIL 尚未完成。
- `exoanchor-prototype-v2.4-ms-test` 是独立的 MS2109 电源/EEPROM 硬件验证配置，
  包含破坏性维护能力，不得用于普通 V2.4 Dev 产品构建或发布。
- Prototype0/rev3 是验证范围更完整的硬件参考。
- PrototypeV2.1/rev3 仅作编译与设计参考。
- 旧版 ESP32-P4x 和 Waveshare NANO 的 rev1 配置冻结维护。
- 直连 Waveshare NANO 与简单扩展板共用 `waveshare-p4-nano` 基础配置。

禁止把一个板型/芯片组合生成的 `sdkconfig` 用于另一个组合，因为生成值会覆盖条件式 Kconfig 默认值。

V2.4 产品 profile 的完整 GPIO、产品能力和测试镜像边界见
[`boards/exoanchor-prototype-v2.4.md`](boards/exoanchor-prototype-v2.4.md)。普通
V2.4 与 `v2.4-ms-test` 是独立身份；板名、端口、IP 或现有固件自报不能作为
实物识别依据。

V2.3 标准配置遵循 b6 网表并假定物理 AT24C16C U5 存在，因此关闭 MS2109 EEPROM 模拟。只有确认 U5 已移除的实验板，才能在普通板型 overlay 后追加 `boards/sdkconfig.defaults.exoanchor-prototype-v2.3-ms2109-eeprom-emulator`。
