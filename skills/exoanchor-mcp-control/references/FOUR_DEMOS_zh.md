# ExoAnchor MCP 四项演示运行手册

本文定义 Codex 与 DeepSeek Harness 共用的演示合同。它是一份执行手册，不会扩大
用户授权。所有工具均来自 canonical `integrations/exoanchor-mcp`。

## 共同预检

每次演示开始前依次完成：

1. 从 `local-profiles/BOARD_IDENTITY_REGISTRY.toml` 读取目标登记；
2. `exoanchor_capabilities`；
3. `exoanchor_status`；
4. 按通道调用 `exoanchor_snapshot` 或 `exoanchor_uart_status`；
5. 核对设备 ID、固件、视频/HID/UART 初始化、access mode、tool policy 和 lease；
6. 保持 KVM 或 Terminal 页面打开，确认自动控制标识、人工输入冻结与 Stop 可用。

DSH 的公开工具名前缀为 `mcp__exoanchor__`。下文为便于阅读使用原始
`exoanchor_*` 名称。

任何非幂等调用在传输中断后没有结果，都标记为 `outcome_unknown`。此时只允许
读取 fresh status、snapshot、UART journal 或目标服务状态来判断外部结果；在完成
独立读回前不得重复动作，无法读回时停止并交还人工控制。

## Demo 01：KVM 自动登录

目标：从可见的 Linux 登录页进入 shell，凭据不经过模型或 MCP 参数。

1. 人工确认 KVM 页面确实是目标机登录页。DSH 当前不能直接视觉理解 JPEG，不能
   用图片占位符代替该确认。
2. 调 `exoanchor_snapshot`，保存本次服务器生命周期中的 `observation_id`。
3. 调 `exoanchor_console_login`：
   - `expected_device_id` 为已核验设备；
   - `expected_observation_id` 为刚取得的 observation；
   - `credential_ref=console://default`；
   - 登录页可用 `stage=both`、`between=enter`。
4. 独立获取新 snapshot，由人工确认已进入 shell。
5. 再读 status，确认 lease 释放、HID 无 failure、视频持续且人工输入恢复。

验收：起始页、终态页、secret redaction、控制标识、人工冻结、Stop 和恢复全部有
同轮证据。历史成功不能替代本轮录制。

## Demo 02：KVM 配置 UART

目标：保留 KVM 恢复通道，通过 KVM shell 幂等配置 Linux serial-getty，再由
ExoAnchor UART journal 独立看到登录提示。

1. 通过短小 KVM 命令现场解析 `/dev/serial/by-id/`，再解析实际 TTY；禁止硬编码
   `ttyACM0` 或假定 USB 位置就是身份。
2. 先读取 `systemctl cat serial-getty@<tty>.service` 和现有 drop-in。存在未知配置
   时停止，不能覆盖。
3. 期望 drop-in 的核心语义：device unit 绑定、`Restart=always`，以及
   `agetty --noreset --noclear --keep-baud 115200,9600 %I vt220`。
4. sudo 提示出现时，先 fresh snapshot，再用
   `exoanchor_console_login(stage=password)` 引用 `console://default`。不得键入密码。
5. 逐步执行 daemon-reload、enable/start、状态与 journal 读回。
6. `exoanchor_uart_status` 确认 UART1/115200/8N1；从 cursor `0` 读取一次 retained
   journal，之后只从 `next_cursor` 继续，直到看到真实 login prompt。

验收：现场 adapter 身份、service/drop-in、systemd 状态、UART login prompt、KVM
持续可用以及人工 Stop/恢复证据一致。

## Demo 03：UART 安装基础软件

默认包集：`ca-certificates curl jq openjdk-21-jre-headless`。

1. 保持 Terminal 页为输出观察者。
2. 从 UART 最新 cursor 读取。若看到 `login:`，用 `exoanchor_uart_write` 发送非秘密
   用户名；看到尾部精确 password prompt 后，调用
   `exoanchor_uart_authenticate(credential_ref=console://default)`。
3. 用唯一标记加 `id -un` 验证进入 shell。
4. 只读检查 OS、架构、磁盘和现有包版本。
5. 需要 sudo 时，先发送单个 `sudo -v`，读到精确提示后调用
   `exoanchor_uart_authenticate(credential_ref=auto://sudo)`。
6. 安装使用一个可读回的有界任务；命令末尾打印唯一 marker 与真实 exit code。
   安装进行中按 `next_cursor` 轮询，不重复发送安装命令。
7. 独立验证：

```sh
dpkg-query -W -f='${Package}\t${Status}\t${Version}\n' ca-certificates curl jq openjdk-21-jre-headless
curl --version | head -n 1
jq --version
java -version
```

验收：四个包均为 `install ok installed`，marker exit code 为 0，Terminal 实时同步，
人工输入在 MCP 写入期冻结、Stop 可用，结束后人工输入恢复。APT 仍运行或结果未知
时不得重发安装命令。

## Demo 04：UART 安装 Minecraft Server 1.20.1

默认实现为 Mojang vanilla 1.20.1。1.20.1 使用 Java 17 作为演示运行时；Demo 03 的
JDK 21 保留用于展示，不用它代替版本兼容性检查。

这一步会创建用户、目录、systemd 服务并需要接受 Mojang EULA。执行前必须获得
用户对“安装 vanilla 1.20.1、接受 EULA、仅 localhost 监听”的明确确认。

1. 只读检查 Java 17、`minecraft` 用户、`/opt/minecraft/1.20.1`、相关 systemd
   unit、25565 监听和磁盘。发现未知既有对象时停止，不覆盖。
2. 通过官方 version manifest 解析 `1.20.1` metadata，再从 metadata 读取 server
   JAR URL 和 SHA-1。禁止使用随机镜像或硬编码未知散列。
3. 安装 `openjdk-17-jre-headless`，创建系统用户与专用目录，下载到临时文件，
   校验 SHA-1 后再原子移动为 `server.jar`。
4. 将 `eula=true` 作为用户已明确确认后的独立步骤；默认
   `server-ip=127.0.0.1`，不改防火墙、不做公网暴露。
5. 新建任务专用 systemd unit；若同名 unit 已存在则先展示并停止。设置合理内存
   上限、工作目录、非 root 用户和自动重启。
6. daemon-reload、enable/start 后用 UART marker 轮询，禁止因启动慢而重复创建。
7. 独立验证：unit active、日志出现 Done、进程为 minecraft 用户、server JAR
   SHA-1 与官方 metadata 一致、仅 `127.0.0.1:25565` 监听。
8. KVM 仍应可作为恢复通道；Terminal 输出持续同步；用户 Stop 后 automation 不得
   重获 lease。

验收不包括公网连通、端口转发、关闭 online-mode、安装模组或修改防火墙。需要这些
能力时另开任务并单独授权。

## 证据矩阵

| 维度 | 每项必须记录 |
|---|---|
| 身份 | registry 记录、configured/observed device ID、固件 |
| 起始状态 | fresh snapshot 或 UART cursor/status |
| 控制权 | owner、generation/lease、页面标识、人工冻结、Stop |
| 动作 | 精确工具名、非秘密参数摘要、marker/idempotency 信息 |
| 终态 | 独立 snapshot/journal/status/service/package 读回 |
| Secret | credential reference、redaction/non-export flags，不记内容 |
| 恢复 | lease 释放、人工输入恢复、无卡键、KVM/Terminal 仍在线 |

任何一格缺失都只能记为部分证据，不能宣称实机验收完成。
