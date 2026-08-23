![ExoAnchor](assets/brand/png/exoanchor-readme-banner-1600x400.png)

# ExoAnchor

**An Another [RCOS](https://rcos.io/) project**

**[English](README.md)**

ExoAnchor 是一个采用 [MIT License](LICENSE) 开源、以从固件到生产资料完整开放为目标的全功能 KVM。它是一个基于 MCU 和 RTOS、以 AI Agent 为可选能力的嵌入式硬件项目。

> 当前开发固件：`0.87.6-dev IndigoShore`。

**我们承诺将使用MIT许可开放从研发到制造的一切资料。**

ExoAnchor 起源于 Rensselaer Polytechnic Institute 的 RCOS 社团。最初，我们
只是想为实验室和个人设备寻找一套负担得起、能够进行远程维护与故障恢复的 KVM
方案。然而，当时符合需求的成熟产品成本超出了我们的预算，封闭的软硬件设计也
限制了定制、维修和二次开发。于是，我们决定从零开始自己做一套。

最初的目标很简单：以尽可能低的成本，实现可靠的视频采集、键盘鼠标控制、电源
管理和远程恢复。随着开发不断深入，ExoAnchor 逐渐不再只是一个低成本替代方案，
而成为一项关于开放硬件、嵌入式系统与现实设备智能控制的长期实验。

ExoAnchor 目前以 ESP32-P4 为核心，使用 MCU 和 RTOS 承载 KVM 的基础能力，
未来也会探索和支持更多硬件平台。AI Agent 是建立在设备能力之上的可选控制层，
而不是设备正常工作的前提；所有涉及现实设备的操作都应当具有明确的权限边界，
并接受人的授权与监督。

对 ExoAnchor 来说，“开源”不只是一个营销符号。我们承诺使用 MIT License
开放从研发到制造的完整资料，包括软件、固件、原理图、PCB 源工程、BOM、网表、
结构 CAD 源工程、复刻指南以及相关设计文档。

我们不打算为制造、修改或商业使用设置人为的知识壁垒——如果一套设计真正属于
开放社区，那么任何人都应该能够将它复刻出来，并深入理解它为何如此设计、又是
如何实现的。

如今，随着 AI Agent 的普及，我们希望让这套低成本、完全开放的 KVM 方案走向
更多人，也让更多人看到：Agent 不只存在于浏览器和软件接口中，它同样可以在明确
授权与监督下感知、操作并接管现实设备。

除了集成化的 PCIe 形态自研板，我们也将提供高度简易的 DIY 方案，完全使用货架产品，无需焊接或任何非常见工具。

即使目标机器的操作系统、SSH、系统网络或服务失效，仍可通过独立网络入口查看画面、发送键盘和鼠标输入，并在板型实际支持时使用 UART、复位和电源控制。

目标机器不需要安装 ExoAnchor 软件。手动 KVM 始终保持独立可用；设备 API、设备内 Agent 和外部 MCP 控制器是在这条确定性控制面之上的可选能力。

## 从固件到生产资料的开放

**不止开放固件。从固件到生产资料，全部开放。**

下表按可复刻性列出各层实际公开的内容与当前状态。所有 ExoAnchor 原创文件均采用
MIT License 分发，使任何人都能检查、修改、复刻、制造和销售自己的硬件。

| 开放层级 | 公开内容 | 当前状态 |
| --- | --- | --- |
| 应用与控制面 | Dashboard、设备 API、设备内 Agent 和可选 MCP 适配器 | ✅ 已公开，持续开发 |
| MCU 固件 | 视频、HID、网络、UART、电源控制、配置和构建脚本 | ✅ Stable 与 Dev 源码已公开 |
| 原理图、PCB | 全套原理图与设计说明 PDF、PADS 与 Altium Designer 可编辑工程 | 🚧 Double checking, Release soon |
| BOM、网表、复刻指南 | 版本化 BOM、Protel 2.0 网表以及构建、烧录、装配与验收说明 | 🚧 BOM 与网表已公开；复刻指南持续补齐 |
| EasyEDA 云端源工程 | [V2.4a6 原理图与 PCB 在线源工程](https://oshwhub.com/team_dsksgpyk/project_akawewur) | ⏳ 待发布 |
| 结构设计 | 可编辑的结构 CAD 源工程 | ⏳ 待发布 |

> ⚠️ 项目仍处于开发和硬件验证阶段，请在使用、复刻或制造前谨慎检查。
> 如发现任何问题，欢迎通过 [GitHub Issues](https://github.com/wxffxx/ExoAnchor/issues) 反馈。

这是一项资料开放承诺，并非代表已经开源，在所有产物验证完毕之前，我们不会释放任何未经过验证和检验的资料。每个硬件修订版实际公开了什么、验证到什么程度，以
[ExoAnchor-Hardware 硬件发布目录](https://github.com/wxffxx/ExoAnchor-Hardware/tree/main/ESP32P4/exoanchor-p4-v2.4a6)
为准。公开原理图、PCB 工程和生产资料，不等于已经完成量产验证；复刻与验收仍应遵循对应修订版的指南和验证状态。

## 核心能力

| 能力 | 说明 |
| --- | --- |
| 视频 | 通过 MS2109 类 UVC 采集路径接收目标机 HDMI 输出 |
| 键盘和鼠标 | 模拟 USB HID，可覆盖 BIOS、启动和操作系统阶段 |
| 独立网络 | ESP32-P4 自身通过以太网提供 Dashboard 和设备 API |
| UART | 只在真实接线并验证的板型上提供目标机串口 |
| 电源和状态 | 只在实现对应线路的板型上提供 PWR、RST、12V 和 3V3AUX |
| 设备内 Agent | 在固件策略、请求批准和控制租约约束下编排观察与动作 |
| 外部控制 | 可选 MCP 适配器直接使用设备 API，不接管设备内 Agent 状态 |

每个板型只报告自己的真实能力。可编译 profile、原理图草案和其他板型的测试结果
不能代替当前硬件的验证证据。

## 硬件基线

| # | 实现 | 状态 | Feature |
| ---: | --- | --- | --- |
| 1 | ESP32-P4 开发板 + 外接 USB HDMI 采集卡 | ✅早期原型，资料已全部开源  | 货架产品 |
| 2 | Waveshare ESP32-P4-NANO + 简易扩展板 | ✅早期原型，资料已全部开源 | 货架产品 |
| 3 | ExoAnchor PrototypeV0 | ✅已完全验证| 核心KVM可行性验证板  |
| 4 | ExoAnchor PrototypeV2.3 | 🚧 bring-up 已验证，量产/HIL 未闭环 | 集成化 PCIe 控制板 |
| 4A | ExoAnchor PrototypeV2.4 / V2.4a6 | 🚧 正式 Dev profile 已建立，HIL 待完成 | 集成化 PCIe 控制板 |
| 4C | ExoAnchor PrototypeV2.4C | 🚧设计中 | HDMI2CSI可行性验证板 |

各板型对应的固件配置、ESP32-P4 芯片版本和验证状态见
[ESP32-P4 板型实现矩阵](device/ESP32P4/boards/IMPLEMENTATION_PROFILES_zh.md)。

项目方确认 V2.4a6 的 `a6` 只表示 PCB 层数，原理图与 ESP32-P4 映射和 V2.4
相同；两者共用 `exoanchor-prototype-v2.4 + esp32p4-rev3` 产品组合。独立的
`exoanchor-prototype-v2.4-ms-test` 含破坏性 EEPROM 测试能力，不是产品固件；
Production TypeC 和 TypeW 也不能与 V2.4 profile 混用。

## 从选型到烧录

### 1. 选择硬件、固件和工具链

| 硬件 | 固件 | 构建组合 | ESP-IDF | 入口 |
| --- | --- | --- | --- | --- |
| Waveshare ESP32-P4-NANO DIY | Dev；也可选纯 KVM Stable | `waveshare-p4-nano + esp32p4-rev1` | 5.4.x | [装配与烧录指南](https://github.com/wxffxx/ExoAnchor-Hardware/tree/main/ESP32P4/simple-diy/waveshare-nano-diy) |
| ExoAnchor PrototypeV0 | Dev 或 Stable | `exoanchor-prototype0 + esp32p4-rev3` | Dev 5.5.5；Stable 5.5.4 | [Dev 构建](device/ESP32P4/firmware/v0.86.6-dev/README.md#prototype0rev3已验证参考) · [Stable 构建](device/ESP32P4/firmware/v0.86-stable-kvm/README.md#构建-prototype0) |
| ExoAnchor PrototypeV2.3 | Dev | `exoanchor-prototype-v2.3 + esp32p4-rev3` | 5.5.5 | [V2.3 构建](device/ESP32P4/firmware/v0.86.6-dev/README.md#prototypev23rev3bring-up-主线) |
| ExoAnchor PrototypeV2.4 / V2.4a6 | Dev | `exoanchor-prototype-v2.4 + esp32p4-rev3` | 5.5.5 | [V2.4 构建与能力边界](device/ESP32P4/firmware/v0.86.6-dev/README.md) |

Dev 是当前 KVM + 设备内 Agent 开发线，运行版本为 `0.87.6-dev`；Stable 是不含
设备内 Agent 的独立纯 KVM 源码线。所有固件版本、升级要求和完整入口见
[固件版本说明](device/ESP32P4/firmware/README.md)。

不同板型和芯片版本不能共用 `sdkconfig` 或 build 目录。烧录前必须确认实物板型，
不要把默认配置烧入未核对的硬件。

### 2. 让 AI Agent 获取源码、构建并烧录

把板卡的开发/烧录 USB 口连接到电脑，在准备存放项目的目录中启动 Codex 或其他
具备终端能力的 AI Agent，然后复制下面的提示词。先替换方括号中的硬件和固件
信息；串口不确定时只请求只读发现，在显式端口和完整板卡身份确认前不要授权烧录。

```text
请从公开仓库获取 ExoAnchor，并协助我完成复刻、构建和烧录。

我的环境：
- 硬件：[Waveshare ESP32-P4-NANO DIY / ExoAnchor PrototypeV0 / ExoAnchor PrototypeV2.3 / ExoAnchor PrototypeV2.4 或 V2.4a6]
- 固件：[Dev / Stable KVM]
- 操作系统：[macOS / Linux / Windows]
- 烧录串口：[实际端口 / 未知——只发现，不烧录]
- 源码位置：[在当前目录新建 ExoAnchor / 使用现有 ExoAnchor 目录]

请完成以下任务：
1. 如果指定位置还没有源码，执行 git clone https://github.com/wxffxx/ExoAnchor.git；如果已有仓库，先确认它确实是 ExoAnchor，不覆盖其中的本地修改。
2. 进入仓库后先阅读 README_zh.md、device/ESP32P4/firmware/README.md，以及所选固件目录的 README。
3. 根据实物确定唯一正确的 board profile、芯片版本 overlay 和 ESP-IDF 版本；代码与配置文件是最终依据。无法确认板型时停止，不要猜测。
4. 检查本机 ESP-IDF 和串口环境。缺少工具链时说明需要安装的准确版本；任何系统级安装都先征得我的同意。
5. 保留工作区中的已有修改，不执行 git reset、checkout 覆盖、强制 pull 或其他破坏性清理。
6. 为所选板型创建独立 build 目录和 sdkconfig，执行 set-target 和完整 build；不得复用其他板型的生成配置。
7. 构建失败时定位首个有效错误并修复明确属于本次构建的问题，不要通过关闭安全检查或改用其他板型配置绕过错误。
8. 只用只读方式识别候选串口和芯片信息。任何写入前，必须让操作者确认实物型号，并让精确 CH343 USB serial、eFuse MAC、silicon revision、固件 profile 和未占用的显式端口与板卡身份记录全部匹配；缺项或不一致立即停止。
9. 只有构建与完整身份记录都匹配硬件后，才可把显式端口和 --build-dir 传给 tools/flash-monitor.sh，完成有线全量烧录、串口监视和 DHCP 地址提取；写命令不得接收自动选择的端口。
10. 最后报告源码提交、固件版本、板型、芯片版本、ESP-IDF、build 目录、串口、设备 IP，以及 Ethernet、UVC、HID 的启动状态和所有异常。

安全限制：
- 不向被控主机发送键盘、鼠标、电源或复位动作。
- 不使用 force、擦除整片 Flash 或覆盖其他板型构建目录，除非我明确批准。
- 不输出或写入共享文档中的口令、Token、私钥和本机绝对凭据路径。
```

### 3. 手动获取源码、构建与烧录

```bash
git clone https://github.com/wxffxx/ExoAnchor.git
cd ExoAnchor
```

安装选型表中对应的 ESP-IDF，然后按所选固件和板型执行
[Dev 构建说明](device/ESP32P4/firmware/v0.86.6-dev/README.md)或
[Stable 构建说明](device/ESP32P4/firmware/v0.86-stable-kvm/README.md)中的命令。
构建完成后，在所选固件目录中执行：

```bash
./tools/flash-monitor.sh <PORT> \
  --build-dir <build-dir> \
  --wait-ip \
  --exit-on-ip
```

`<PORT>` 是系统识别到的实际串口，`<build-dir>` 必须与构建命令的 `idf.py -B`
参数完全一致。分区升级要求和直接使用 `idf.py` 的方式见
[固件烧录入口](device/ESP32P4/firmware/README.md#烧录规则)。

### 4. 访问与验收

| 地址 | 用途 |
| --- | --- |
| `http://<board-ip>/` | Dashboard |
| `http://<board-ip>/kvm` | 视频与键鼠控制 |
| `http://<board-ip>/agent` | 设备内 Agent，仅 Dev |
| `http://<board-ip>/terminal` | 目标机 UART，仅 Dev 且取决于板型 |
| `http://<board-ip>/settings` | 设备设置 |

确认 Dashboard 可访问、Ethernet 已获取地址、KVM 有画面且 HID 可用。首次启动
必须替换本地 bootstrap 凭据；共享文档不发布凭据值。

## 项目目录

```text
ExoAnchor/
├── assets/brand/                    # 品牌和 Web 资产
├── device/ESP32P4/
│   ├── boards/                      # 板型与固件 profile 权威映射
│   └── firmware/                    # Stable 与 Dev 独立固件树
├── docs/
│   ├── reproduction/                # 制造与复刻
│   └── guides/                      # 实用指南
├── integrations/exoanchor-mcp/      # 可选外部 MCP 控制器
│   ├── exoanchor_mcp/               # canonical stdio 服务
│   └── skills/exoanchor-mcp-control/ # Codex 与 DSH 共用操作技能
└── LICENSE

ExoAnchor-Hardware/                  # 独立硬件源工程仓库
├── ESP32P4/
│   ├── exoanchor-p4-v2.4a6/         # 当前集成化 PCIe 自研板
│   ├── simple-diy/                  # 货架产品简易 DIY 方案
│   └── archive/                     # 历史硬件资料
├── assets/brand/
└── LICENSE
```

`ExoAnchor-Hardware` 是与主仓库并列的
[独立硬件仓库](https://github.com/wxffxx/ExoAnchor-Hardware)，不是
`ExoAnchor/` 的子目录。正式文档入口见[文档地图](docs/README_zh.md)。

## 开发验证

在仓库根目录运行完整的纯主机门禁：

```bash
./scripts/check-all.sh
```

该入口依次覆盖两套固件、MCP、Toolkit、文档卫生和 Git 空白检查。Toolkit
要求 Python 3.10+；本机有 `uv` 时会自动使用锁定环境。该门禁不能替代真机 HIL。

## AI 使用披露

ExoAnchor 在研发过程中广泛使用Ai Agent。AI 参与了代码与文档编写、测试、硬件资料整理、原理图审查、研究和视觉资产制作。

使用 AI 不改变贡献内容的许可与责任边界。

## 许可证

ExoAnchor 原创软件、固件和文档采用 [MIT License](LICENSE)。除非具体文件另有
声明，本许可适用于 ExoAnchor 贡献者有权授权的全部原创资料，包括软件、固件、
脚本、测试、配置、文档、规范、图表、原创媒体资源、原理图、PCB 与 CAD 源工程、
BOM、网表、结构模型和生产资料，并允许依据覆盖范围内的设计文件制造和销售硬件。

复制或实质性使用时须保留 `LICENSE` 中的版权与许可声明。第三方产品和资料仍遵循
各自条款，文件级声明优先。MIT License 不授予 ExoAnchor 名称、Logo、商标或
第三方资料的权利，也不代表监管批准、安全认证或特定硬件用途适用性。覆盖资料及
据此制造的硬件在法律允许的最大范围内按“原样”提供，不附带任何保证。

本项目不会提供有关MS2109或者LT6911UXC的资料，因为他们并非公开资料，其传播受到芯片厂商的限制。

### Why MIT All?

我不依靠这个项目赚钱。我有自己的研究主线，ExoAnchor 甚至算是一个半业余项目，
所以我当然可以选择将它全部开源。

同时，全部开源并不代表我们不会推出量产产品。我们会尽快推出能让大家直接购买和
使用的实用版本。

我更希望大家能从中学到如何把一个想法变成产品。我认为，这比这个项目本身的价值
更大。
