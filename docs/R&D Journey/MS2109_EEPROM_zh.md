# 2026-08-23 / 2026-10-29 MS2109 AT24C16 EEPROM（U5）：写保护控制、外接编程器烧录与经 P4 只读备份

- 时间：第一阶段 2026-08-23 16:12–18:40；第二阶段 2026-10-29 10:4x（均为北京时间）
- 硬件：ExoAnchor PrototypeV2.4（ESP32-P4 rev3.2，MS2109 + AT24C16 U5）两块，下文称
  V2.4 板 A (2026-08-23) 和 V2.4 板 B (2026-10-29)；外接 CH341 编程器（VID:PID `1A86:5512`）
- 固件：板 A 依次刷入 `0.87.6-dev` 的 r35、r36、r37（ESP-IDF 5.5.5）；板 B 实验时临时刷入
  `0.88.0-v2.4-ms-test`，结束后恢复原固件 `0.88.0-dev`
- 镜像：厂商 Demo 包中的 `MS2109_24C16_Demo_V1.8.1`（2048 字节，补 0xFF），SHA-256
  `2dd56419dedd50ab843b4cd1a0282c9e4e8e849d90a9481bf1c65399680b4715`；镜像本身不随本文发布
- 板 B 的经 P4 读出过程另见 `P4_IN_SYSTEM_PROGRAMMING_zh.md` 第 3.2 节

## 1. 结论

| 项目 | 板 | 结果 |
| --- | --- | --- |
| GPIO16 写保护控制（`eeprom-wp status/write-enable/protect`） | A | r37 已验证：首次读取、开漏拉低、开漏释放和板上上拉都有实机结果 |
| r36 `eeprom-wp status` 误报 GPIO16 为低 | A | 已查明原因，r37 修复，修复后首次读取为 `level=1 protected` |
| 用外接 CH341 给空白 U5 写入 Demo V1.8.1 | A | 已验证：写前双读一致（全 0xFF），工具内校验、独立回读、恢复写保护后再读都与锁定镜像一致 |
| 写后首次上电 | A | 整板 power-on 启动，MS2109 两路电源上电，UVC 枚举出 55 个模式；当时 CH341 仍连在板上，不算拆除治具后的独立冷启动 |
| 写后实际取得 MJPEG 帧、KVM 画面 | A | 未验证 |
| MS2109 确实加载了新 EEPROM | A | 未验证 |
| 经 P4 探测 8 个块地址（0x50–0x57） | B | 已验证，`address_mask=0xff` |
| 经 P4 整片读出 2048 字节 | B | 已验证，两份逐字节相同，SHA-256 与 Demo V1.8.1 锁定镜像一致 |
| 实验后恢复原固件、NVS 不变 | B | 已验证 |
| 经 P4 写入 U5 | — | 从未测试 |
| `ms2109-eeprom-tool.py dump` | B | 失败（串口同步问题），工具未修 |
| 板 A 早期预检时 CH341 读出全 0x00 的原因 | A | 未查明 |

两块板的 U5 现在都是 Demo V1.8.1。要区分清楚：**U5 的写入只用外接 CH341 编程器做过（板 A）；经 P4
写入 U5 从未测试过**。P4 这条路径目前只验证了读出（板 B）。

## 2. 方法与安全设计

### 2.1 连接

按 V2.4A4 网表：

| 功能 | 器件/网络 | ESP32-P4 |
| --- | --- | --- |
| MS2109 主电源开关 | U7 TPS22918，ON 高有效 | GPIO13 |
| MS2109 1.2V 使能 | U10 TLV62569，EN 高有效 | GPIO18 |
| AT24C16 写保护 | U5 WP，高电平保护，板上 4.7k 上拉 | GPIO16 |
| AT24C16 时钟 | MS_SCL，经 R31 0Ω | GPIO47 |
| AT24C16 数据 | MS_SDA，经 R61 0Ω | GPIO48 |

U5 由 `3V3_SYS` 供电，关掉 MS2109 后 EEPROM 仍有电。P4 因此可以先关 MS2109 再独占 I²C 总线；不能在
MS2109 上电时强行驱动这条共享总线。

### 2.2 产品固件里的写保护命令（r36 起）

r36 把 `eeprom-wp` 串口命令从 MS2109 破坏性测试功能中解耦出来。普通 V2.4 产品配置仍不包含 EEPROM
读写驱动、EEPROM HTTP 接口或 EEPROM 模拟器，ELF 中也确认没有这些符号和 `ms-eeprom program-b64`。

- `eeprom-wp status`：只读当前 WP 输入电平；
- `eeprom-wp write-enable`：以开漏输出拉低 WP，持续到执行 `protect` 或重启；
- `eeprom-wp protect`：释放开漏输出，由板上上拉恢复写保护。

`write-enable` 和 `protect` 配置 GPIO 后立即回读物理电平，与目标不符时返回
`ESP_ERR_INVALID_RESPONSE`，不把 GPIO API 返回成功当作 WP 已切换的证据。

### 2.3 外接 CH341 写入事务（2026-08-23，板 A）

- P4 每次写入前都过一遍身份门（串口序列号、芯片版本、eFuse MAC、Flash 型号与容量、目标 profile、
  端口无人占用），不执行 `erase-all`，不写 NVS、TF 或 OTA 槽；另一块 V2.4（板 B）当天未复位、未访问。
- U5 只做单颗事务。以下各项都是强制门：锁定镜像和工具的 SHA-256；写前两次读取必须一致；工具内校验；
  批量工具独立回读；恢复写保护后再读一遍。
- 写入前事先取得了"允许覆盖非空内容"的授权，但实际写前为空白，没有覆盖非空内容。

锁定项：

| 项目 | SHA-256 |
| --- | --- |
| 镜像 `MS2109_24C16_Demo_V1.8.1_CH341_2KiB_FFpad.bin`（2048 字节） | `2dd56419dedd50ab843b4cd1a0282c9e4e8e849d90a9481bf1c65399680b4715` |
| CH341 工具 | `62b54b1fe1a83bd61b3e17d103d910e5940934db612dfb4a210a3684e7b9c2bc` |
| 批量工具 v1.1.0 | `32b1496cd28dc806cec1d83a669af871f51ae91dddc19bf0a9bfd926405c043e` |

### 2.4 ms-test 固件的安全事务（2026-10-29，板 B）

`exoanchor-prototype-v2.4-ms-test` 是独立的硬件验证固件，不属于产品固件。每次读写 U5 都按以下顺序，
失败路径同样执行清理：

1. 记录 MS2109 当前电源状态，先关 U10 的 1.2V，再关 U7 的 `3V3_MS`；
2. 等电源和共享总线稳定；
3. P4 接管 GPIO47/48 作为 I²C 主机；
4. 只有写入时才拉低 GPIO16，读取始终保持写保护；
5. 写入后做全量回读比较；
6. 释放 GPIO16，由板上 4.7k 上拉恢复写保护；
7. 删除 I²C 主机，释放 GPIO47/48；
8. 事务前为上电状态时，先恢复 `3V3_MS`，再恢复 1.2V。

写入命令 `ms-eeprom program-b64` 必须是完整 2048 字节并以 `CONFIRM` 结尾；不提供整片擦除、任意偏移
写入或无确认写入。设计上按 16 字节页写入、逐页等待写周期，设备端全量回读，主机再独立读一遍比较 CRC32
和逐字节内容；固件不内置也不自动选择任何 MS2109 镜像。这部分写入路径至今没有上板。

本次范围为只读：不写 EEPROM、NVS 和分区表，实验后恢复原固件。开始前确认板子没有正在运行的 ExoAnchor
会话、串口无人占用。

## 3. 实验过程与发现

### 3.1 第一阶段（2026-08-23，板 A）

**r35：新板首次烧录（16:12–16:23）**

- 身份门通过：ESP32-P4 v3.2，16 MiB SPI Flash（manufacturer `c8` / device `4018`），目标 profile
  `exoanchor-prototype-v2.4`。
- 烧录前整片备份 16 MiB（16777216 字节），SHA-256
  `dffab0dd410657cb30c7b2fd7f2586a4792e8472e58882b3532581f8111a646d`，逐字节均为 0xFF，没有需要保留的出厂内容。
- 写入 r35（应用 3070224 字节，SHA-256 `e0eaa16a…f0cf`），DIO、80 MHz、16 MiB。四段工具内 hash 校验和
  独立 `verify_flash` 全部通过。
- 启动正常：32 MiB PSRAM 250 MHz，以太网 100 Mbps 全双工，根页面 HTTP 200。现场没有 TF 卡，SDMMC
  初始化以 `ESP_ERR_TIMEOUT` 结束。本次没有测 KVM、MS2109 视频、HID 等功能。

**r36：写保护命令（17:21–18:18）**

- 主机测试和官方 V2.4 Dev 构建通过，应用 3070992 字节（SHA-256 `c2742f60…0995`）。刷入后四段工具内 hash
  和独立 `verify_flash` 通过。
- 首次实机 `eeprom-wp status` 在从未执行 `write-enable` 的情况下报告
  `gpio=16 level=0 write=enabled result=ESP_OK`（见 4.1）。随后只执行了增强安全性的 `eeprom-wp protect`，
  返回 `gpio=16 level=1 write=protected result=ESP_OK`，说明开漏释放和板上上拉有效。
- r36 当即停用，没有用它解锁或写入 U5。
- 同一时段外接 CH341 对 U5 的预检双读为 2048 字节全 0x00（见 4.2）。

**r37：修复与写保护闭环（18:18–18:40）**

- `status` 路径先调用 `gpio_input_enable()`，不改变现有开漏输出状态。主机测试和官方构建通过；应用
  3070992 字节，SHA-256 `30d903fe53448242a7e4adc403862e277646e25c5c920968015b5033f3f01be1`。
- 写入前再次确认芯片版本和 eFuse MAC。DIO、80 MHz、16 MiB、460800 baud；四段工具内 hash 和独立
  `verify_flash` 全部一致。

| 阶段 | 串口结果 |
| --- | --- |
| r37 首次 `status` | `gpio=16 level=1 write=protected result=ESP_OK` |
| 写入前 `write-enable` | `gpio=16 level=0 write=enabled result=ESP_OK` |
| 写入后 `protect` | `gpio=16 level=1 write=protected result=ESP_OK` |
| 最终再次 `status` | `gpio=16 level=1 write=protected result=ESP_OK` |

**用外接 CH341 写入 U5**

1. 写前两次读取完全一致，均为全 0xFF，SHA-256
   `d0ff1b294b5288d1ae1421eadf5b2d38a8752b76d472ff30bed9028e25b1c5b8`。
2. 以 100 kHz、256 个 8 字节 page 写入锁定镜像；工具内 2048 字节校验 PASS。
3. 批量工具独立回读 `post-read.bin`，与目标逐字节一致，SHA-256 为锁定值。
4. 执行 `protect` 恢复写保护后，再读 `post-protected-read.bin`，仍与目标逐字节一致，SHA-256 为锁定值。

事务结果 PASS。写后看到的那次在线状态（版本、以太网、HTTP 200）来自 U5 写入前的启动，不能作为新
EEPROM 被加载的证据。

**写后首次上电**

- 设备出现新的启动，auth-status 接口报告 `reset_reason=power-on`，说明整板重新上电。
- 串口确认：r37 正常运行；GPIO16 仍为 `level=1 / protected / ESP_OK`；MS2109 的 GPIO13 3.3 V 开关和
  GPIO18 1.2 V 使能都为 on，顺序 `3v3-first`，结果 `ESP_OK`。
- UVC 初始化成功，枚举出 55 个模式，active 为 1920×1080 MJPEG，target 为 1920×1080@25。当时没有采集
  owner，streaming/capture 为 0。
- 但外接 CH341 仍在 USB 上枚举，这次启动不能算"拆除烧录治具后的独立冷启动"。KVM 页面停在本地账户
  登录框，没有读取或代填凭据，因此没有建立视频 owner，也没有取得帧。
- 改用不需要登录的 `video-h264-test`，在采集开始前因内部连续内存不足退出（见 4.3），同样没有取得帧。

按检查点的结论：要拔出 CH341（先拆 SCL/SDA，最后拆 GND）、整板断电至少 5 秒并确认 MS2109 的 3.3 V / 1.2 V
下降后再上电，再由操作者登录 KVM 页面验收实际 MJPEG 帧和画面，才能签署 MS2109 加载新 EEPROM。

### 3.2 第二阶段（2026-10-29，板 B）：经 P4 只读备份

**实验固件**：`./tools/build-firmware.sh --profile stable --board exoanchor-prototype-v2.4-ms-test
--build-dir build-v24-mstest`，版本 `0.88.0-v2.4-ms-test`，app
`a60a914781139e5e5de5e8cbb40d9124482221782d5a921176ae00caf2fa83fb`（2155056 字节）。第一次构建失败，见 4.5。

**过程**：

1. NVS 只读快照：0x9000，24 KiB，SHA-256
   `02653103192f54bcdd9fbeb56543b70bc054996960374ed99a794c6fdaf8f684`。
2. 身份核对（ESP32-P4 v3.2，MAC 与登记值一致）通过后，按段备份 bootloader、分区表、otadata、phy、app
   （原 app `a38d752279d1aadb…`，0.88.0-dev，4 MiB 分区）。ms-test 的分区表与板上一致，不重写。
3. 刷 ms-test：只写 bootloader 和 app，`verify_flash` 一致后复位。启动日志有
   `V2.4 MS test ready 3v3=GPIO13 1v2=GPIO18 wp=GPIO16 scl=GPIO47 sda=GPIO48`。
4. `ms-switch status`：`switch_level=1 core_level=1 command=on sequence=3v3-first last=ESP_OK`。
5. `ms-eeprom status`：`type=AT24C16 size=2048 page=16 wp=protected scl_level=1 sda_level=1
   address_mask=0xff probe=ESP_OK`，8 个块地址全部应答。
6. `ms-eeprom dump-b64` 两遍，都返回 `ESP_OK`，逐字节相同：SHA-256
   `2dd56419dedd50ab843b4cd1a0282c9e4e8e849d90a9481bf1c65399680b4715`，CRC32 `3e1888f5`。读出过程中没有
   EEPROM 写入，写保护始终有效。

**镜像内容**：

| 偏移 | 内容 | 解读 |
| --- | --- | --- |
| 0x000 | `A5 5A` | MS2109 EEPROM 有效标记 |
| 0x002 | `02 AE` | 补丁代码长度 0x2AE = 686 字节 |
| 0x004 | `01 00` | 配置字节 |
| 0x00C | `20 22 12 28` | BCD 日期 2022-12-28 |
| 0x030 起 | `02 CC 03 …` | 8051 代码，首条 LJMP 0xCC03，加载地址 0xCC00 |
| 0x2E1 之后 | 全 0xFF | 未使用 |

- 共 695 个非 0xFF 字节，分 8 段。没有 ASCII 或 UTF-16 字符串，看不到自定义 USB 描述符。
- 与厂商资料里的五份参考镜像（JZY-HDMI-V1.4、L-331-R1.2、SFX_VCMS_0101_V2、noname_1、OZC4）都不相同。
- 当天晚些时候比对发现，SHA-256 与第一阶段写入板 A 的锁定镜像完全相同，即板 B 的 U5 也是厂商 Demo 包里的
  `MS2109_24C16_Demo_V1.8.1`。

**恢复与验证**：从备份副本运行 `restore_board.py --write`，写回 bootloader、otadata、phy、app，
`verify_flash` 一致。启动后为 `0.88.0-dev`、`exoanchor-prototype-v2.4`，身份检查通过；NVS 再读仍为
`02653103…`；HTTPS 根路径返回 200；MS2109 两路电源上电，UVC 重新枚举出 1920×1080 等 MJPEG 模式。

## 4. 问题

### 4.1 r36 的 `eeprom-wp status` 误报写保护已解除

现象：从未执行 `write-enable`，首次 `status` 却报告 `level=0 write=enabled`。

原因：复位后 GPIO16 的输入路径尚未启用，直接 `gpio_get_level()` 读到的值不能作为状态证据。

处理：r36 立即停用，没有用它动 U5；r37 在 `status` 路径先调用 `gpio_input_enable()`，且不改变现有开漏
输出状态。r37 首次 `status` 为 `level=1 protected`。

### 4.2 早期预检时 CH341 读出全 0x00

r36 阶段外接 CH341 对 U5 的预检双读为 2048 字节全 0x00。当时判断这可能是真实内容，也可能是接线或总线
异常，不能据此认为 EEPROM 可写。正式事务没有复现，正式双读稳定为全 0xFF。早期文件只保留作异常追溯，
不能代替正式写前备份。原因未查明。

### 4.3 写后 `video-h264-test` 内存前置条件失败

板 A 写后首次上电时，`video-h264-test` 在采集开始前退出：`internal=35955 largest=15360
need=24576/18432`，结果 `ESP_ERR_INVALID_STATE`。这次没有访问或改写 U5，也没有取得帧；它是当时运行时
内存前置条件失败，不能作为 MS2109/UVC 图像失败的证据。

### 4.4 `ms2109-eeprom-tool.py dump` 失败

现象：工具的 `dump` 两次都报 "device did not return a complete verified EEPROM dump"，同一工具的 `status`
和 `power status` 成功。

原因：看串口原始数据发现两点：板 B 的 CH343 一打开串口就会复位 P4，命令发出时板子还在启动；启动后输入
缓冲里残留一个杂字节，命令变成 "?ms-eeprom dump-b64"，被回 `unknown command`。

处理：改用临时脚本，打开串口后等到 "CLI ready"，发一个空行清缓冲，再发命令。3.2 第 6 步的两份 dump 就是
这样取得的。工具本身还没改。

### 4.5 ms-test 第一次构建失败

16fb7c20 新增的 `h264-stages` 串口命令没有按 H.264 条件编译，Stable 组成里没有对应类型。修复并提交
8d3e2505 后构建通过。

### 4.6 恢复工具覆盖日志

`restore_board.py` 会覆盖所在备份目录里的日志，所以原固件备份另外复制一份（附 SHA256SUMS），恢复从副本
运行。

## 5. 遗留问题

- 经 P4 写入 U5 从未测试。投入使用前要：用户授权并提供 2048 字节镜像；先修好工具的串口同步（等
  "CLI ready"、发空行、再发命令）；写前再备份；先把读出的同一份镜像原样写回，验证写入路径，再考虑写新内容。
- `ms2109-eeprom-tool.py` 的串口同步问题未修。
- 板 A：拆除 CH341 后的独立冷启动、实际 MJPEG 帧和 KVM 画面验收、MS2109 加载新 EEPROM，都还没有完成。
- 板 A：早期预检读出全 0x00 的原因未查明。
- 板 A：TF、HID、Locator、PWR/RST、detect inputs 以及更广的 Agent/KVM 功能，在本阶段都没有验证。
