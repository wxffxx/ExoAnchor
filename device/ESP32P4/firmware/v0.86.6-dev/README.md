# ExoAnchor 开发固件

当前版本：**0.87.6-dev「IndigoShore / 靛蓝海岸」**。

本目录是 ESP32-P4 平台的唯一开发主线，包含 KVM、Web 控制台、嵌入式 Agent、SSH、TF 文件管理和外部 MCP 接口。版本号以 [`CMakeLists.txt`](CMakeLists.txt) 中的 `PROJECT_VER` 为准；正式历史见[固件版本历史](../docs/FIRMWARE_VERSION_HISTORY_zh.md)。

## 目录边界

```text
main/
├── app/          # 组合根与启动顺序
├── adapters/     # HTTP、JSON 与 WebSocket 协议适配
├── application/  # 认证、设置、视频与控制租约
├── config/       # 源码配置边界
├── core/         # 与传输无关、可做主机测试的逻辑
├── drivers/      # Ethernet、HID、电源、TF 与 UVC 驱动
├── services/     # 诊断、SSH、Web、Agent 与 OTA 服务
└── www/          # 嵌入式 Web 资源

configs/
├── boards/       # 板型 overlay
└── silicon/      # ESP32-P4 芯片版本 overlay

tests/host/       # 本地主机测试
```

板级 GPIO 必须先在 `main/Kconfig.projbuild` 定义，再由 `main/config/board_config.h` 暴露给驱动。不要在驱动中加入私有 `CONFIG_*` 回退值或硬编码板级 GPIO。

0.87.4 起，主线的 Locator LED 支持可选第二 GPIO：单 GPIO 板仍只控制亮灭；双 GPIO 板可在 Settings 的高级设置中切换哪一路为高电平，以交换灯色/方向。选择会保存到 NVS；板型必须通过 `CONFIG_SI_POWER_LOCATOR_RETURN_GPIO` 明确提供第二路，不允许在运行时推断硬件拓扑。

构建配置的组合规则见 [`configs/README.md`](configs/README.md)，源码配置职责见 [`main/config/README.md`](main/config/README.md)，板型实现状态以 [`../../boards/IMPLEMENTATION_PROFILES_zh.md`](../../boards/IMPLEMENTATION_PROFILES_zh.md) 为准。

## 构建前提

- Prototype0、PrototypeV2.1、PrototypeV2.3、PrototypeV2.4 使用 ESP-IDF 5.5.5
  和 rev3 overlay。
- 旧版 ESP32-P4x 与 Waveshare ESP32-P4-NANO 使用 ESP-IDF 5.4.x 和 rev1 overlay。
- 更换板型时必须生成新的 `sdkconfig`，不能跨板型复制构建产物或生成配置。

先进入本目录，再选择唯一一组板型命令。首选 `tools/build-firmware.sh`，它会核对
ESP-IDF 版本、板型/profile、敏感配置、视频内存合同与产物符号。例如当前 V2.4
Dev 产品组合的可复现入口为：

```bash
SI_IDF_EXPORT="$HOME/esp/esp-idf-v5.5.5/export.sh" \
  ./tools/build-firmware.sh \
  --profile dev \
  --board exoanchor-prototype-v2.4 \
  --build-dir build-exoanchor-prototype-v2.4-dev
```

下面保留展开后的手工配置示例，便于审查 overlay 组成。

### Prototype0（rev3，已验证参考）

```bash
. "$HOME/esp/esp-idf-v5.5.5/export.sh"
idf.py \
  -B build-exoanchor-prototype0-idf5.5.5 \
  -D SDKCONFIG=build-exoanchor-prototype0-idf5.5.5/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/boards/sdkconfig.defaults.exoanchor-prototype0;configs/silicon/sdkconfig.defaults.esp32p4-rev3" \
  set-target esp32p4
idf.py -B build-exoanchor-prototype0-idf5.5.5 build
```

该配置使用 IP101GRI，只接受 ESP32-P4 rev3.00–3.99。当前 Dev 树对 rev3 profile
强制要求 ESP-IDF 5.5.5 或更新版本；烧录工具建议使用 esptool 5.3.0。

### PrototypeV2.1（rev3，仅编译参考）

```bash
. "$HOME/esp/esp-idf-v5.5.5/export.sh"
idf.py \
  -B build-exoanchor-prototype-v2.1-rev3 \
  -D SDKCONFIG=build-exoanchor-prototype-v2.1-rev3/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/boards/sdkconfig.defaults.exoanchor-prototype-v2.1;configs/silicon/sdkconfig.defaults.esp32p4-rev3" \
  set-target esp32p4
idf.py -B build-exoanchor-prototype-v2.1-rev3 build
```

该配置使用 DP83825I。它是基于 V2.1b 网表保留的编译参考，不是当前串口连接的验证板。

### PrototypeV2.3（rev3，bring-up 主线）

```bash
. "$HOME/esp/esp-idf-v5.5.5/export.sh"
idf.py \
  -B build-exoanchor-prototype-v2.3-rev3 \
  -D SDKCONFIG=build-exoanchor-prototype-v2.3-rev3/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/boards/sdkconfig.defaults.exoanchor-prototype-v2.3;configs/silicon/sdkconfig.defaults.esp32p4-rev3" \
  set-target esp32p4
idf.py -B build-exoanchor-prototype-v2.3-rev3 build
```

该配置使用 DP83825I，并启用 GPIO50/GPIO51 上的双速率目标串口。当前映射依据 V2.3b6 网表，但操作方将实物标识为 V2.3b4；在身份差异闭合前只使用通用 `PrototypeV2.3` 名称。标准配置假定板上保留 AT24C16C U5，因此不启用实验性 MS2109 EEPROM 模拟。

如果设备曾用其他板型保存 GPIO 设置，只在一次性迁移构建中把 `configs/boards/sdkconfig.defaults.exoanchor-prototype-v2.3-migrate-legacy-nvs` 追加到 `SDKCONFIG_DEFAULTS`；后续普通构建必须移除该 overlay。

### PrototypeV2.4 / V2.4a6（rev3，产品 profile，HIL 待完成）

```bash
. "$HOME/esp/esp-idf-v5.5.5/export.sh"
idf.py \
  -B build-exoanchor-prototype-v2.4-rev3 \
  -D SDKCONFIG=build-exoanchor-prototype-v2.4-rev3/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/boards/sdkconfig.defaults.exoanchor-prototype-v2.4;configs/silicon/sdkconfig.defaults.esp32p4-rev3" \
  set-target esp32p4
idf.py -B build-exoanchor-prototype-v2.4-rev3 build
```

该组合的 Board ID 为 `exoanchor-prototype-v2.4`。项目方确认 V2.4a6 的 `a6`
只表示 PCB 层数，原理图与 ESP32-P4 映射和 V2.4 相同，因此 V2.4a6 使用同一
产品 profile，不创建 `a6` 专用 Board ID。详细 GPIO 与能力边界见
[`configs/boards/exoanchor-prototype-v2.4.md`](configs/boards/exoanchor-prototype-v2.4.md)。

产品 profile 配置 DP83825I、MS2109 正常两级上电、PWR/RST、12V/3V3AUX、
Locator、TF、目标 UART1 和 HID；它强制关闭 EEPROM 测试驱动与模拟器，不开放
EEPROM UART/HTTP 写入面。`exoanchor-prototype-v2.4-ms-test` 是另一个包含破坏性
EEPROM 维护接口的硬件验证镜像，不能用于普通 Dev 产品构建。

本入口当前只建立可复现的配置契约，不表示 V2.4/V2.4a6 已烧录、完成 HIL 或
可以量产。没有操作者确认的实物型号和匹配的板卡身份记录时，只允许构建与只读
检查，不得烧录。

### 旧版 ExoAnchor ESP32-P4x（rev1，冻结兼容）

```bash
. "$HOME/esp/esp-idf-v5.4/export.sh"
idf.py \
  -B build-exoanchor-esp32p4x \
  -D SDKCONFIG=build-exoanchor-esp32p4x/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/boards/sdkconfig.defaults.exoanchor-esp32p4x;configs/silicon/sdkconfig.defaults.esp32p4-rev1" \
  set-target esp32p4
idf.py -B build-exoanchor-esp32p4x build
```

### Waveshare ESP32-P4-NANO（rev1，冻结兼容）

```bash
. "$HOME/esp/esp-idf-v5.4/export.sh"
idf.py \
  -B build-waveshare-p4-nano \
  -D SDKCONFIG=build-waveshare-p4-nano/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;configs/boards/sdkconfig.defaults.waveshare-p4-nano;configs/silicon/sdkconfig.defaults.esp32p4-rev1" \
  set-target esp32p4
idf.py -B build-waveshare-p4-nano build
```

本地配置使用 `idf.py -B <build-dir> menuconfig`，不要直接修改公共默认配置来保存个人环境差异。

逐板身份和 bring-up 证据只保存在受控开发工作区；正式仓库仅记录可公开复现的
profile 契约。任何烧录都必须重新核对物理
型号、CH343 serial、eFuse MAC、silicon revision 与目标 profile，不能从端口、
IP 或现有固件推断身份。

## UVC 兼容覆盖

开发固件固定使用 `usb_host_uvc 2.5.1`。顶层 CMake 先校验上游源文件 SHA-256，再在构建目录生成受控副本，应用有边界的 cleanup、MS2109 帧组装和 MJPEG 质量协商修正；注册表组件源码本身保持不变。

升级依赖时必须重新审查这些受哈希和片段双重保护的转换，不能把现有修正静默套用到其他版本。第三方来源与许可证边界见本目录的 [`THIRD_PARTY_NOTICES.txt`](THIRD_PARTY_NOTICES.txt)。

## 测试、烧录与访问

运行不依赖硬件的测试：

```bash
./tests/host/run.sh
```

烧录并等待 DHCP 地址：

```bash
./tools/flash-monitor.sh <PORT> \
  --build-dir <build-dir> \
  --wait-ip \
  --exit-on-ip
```

`<build-dir>` 必须与前面构建命令的 `idf.py -B` 参数一致。

可用页面：

```text
http://<设备地址>/
http://<设备地址>/kvm
http://<设备地址>/agent
http://<设备地址>/terminal
http://<设备地址>/settings
```

`/skills` 仅作为兼容重定向，目标是 `/settings#agent`。
