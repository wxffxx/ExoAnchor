# 2026-10-08 OTA 闭环硬件在环实验

- 时间：2026-10-08
- 固件：基线为 factory 分区中的 `0.87.6-dev`；实验镜像为 Dev 构建 `0.87.6-otaA/B/C/FAIL-1008`，
  最终镜像 `0.87.6-ota-1008`（提交 `07bdc615`）
- 硬件：ExoAnchor PrototypeV2.4（ESP32-P4 rev3.2，32 MB PSRAM @ 250 MHz，16 MiB Flash），分区为
  factory、ota_0、ota_1 各 4 MiB，开启回滚
- 修复：[`ota_runtime_module.inc`](../../device/ESP32P4/firmware/v0.86.6-dev/main/services/web/ota_runtime_module.inc)、
  [`ota_settings_module.inc`](../../device/ESP32P4/firmware/v0.86.6-dev/main/services/web/ota_settings_module.inc)、
  [`web_server.c`](../../device/ESP32P4/firmware/v0.86.6-dev/main/services/web_server.c)；
  提交 `07bdc615` 经 PR #9 于 2026-10-09 合入 main（合并提交 `a2205802`）

## 1. 结论

| 项目 | 结果 |
| --- | --- |
| 网络 OTA 全流程（上传、SHA-256 校验、切槽、重启、回读） | A、B、C、最终版四次升级均回读到新版本和 `valid` |
| 网络中断续传 | 设备已写 8192 字节时断开 TCP，按查询到的偏移续传成功；旧偏移和提前 finish 被拒绝 |
| 上传中途硬复位 | 已写 4096 字节时复位，旧版本正常启动，未完成会话被清除 |
| 首次启动故障回滚 | 故障版在启动确认前重启，启动程序自动回滚到上一版；串口日志可见全过程 |
| 错误包拒绝 | 匿名上传、畸形 JSON、超分区大小、错误 SHA-256、截断镜像均被拒绝，启动分区不变 |
| 缺陷：截断包借用旧镜像尾部 | B 版接受 4096 字节截断包并切换启动分区；C 版修复后拒绝 |
| 缺陷：废弃会话不自动清理 | A 版 310 s 后仍 busy；C 版 310.2 s 后已自动清理，期间未重启 |
| 缺陷：默认凭据 403 分支返回成功 | 主机回归复现并修复；未在真板上验证 |
| 独立 Flash 校验 | 最终应用、原 factory 应用、原启动程序和分区表与预期一致 |

三处修复都已在 main：当前 main 的 `ota_runtime_module.inc` 中有按接收字节数限定的二次镜像校验
（`received_image`，`.size = written`）和 `ota_upload_session_watchdog_task`（任务名 `ota_expiry`），
`web_server.c` 启动时调用 `ota_upload_session_watchdog_start()`，`ota_settings_module.inc` 的默认凭据
分支在 403 发送成功时返回 `ESP_ERR_INVALID_STATE`；三个主机回归在 `scripts/check-all.sh` 中运行。
故障注入代码只用于本实验，没有进入仓库。

## 2. 测试方法

**链路**：主机脚本经设备 HTTPS API 登录，用分块上传接口（start / chunk / finish / status / abort /
reboot）升级，块大小 64 KiB，finish 时设备返回整包 SHA-256。每次上传前核对 `/api/capabilities`
中的板型为 PrototypeV2.4。

**串口只做旁路**：用于读取芯片身份、测试前备份、记录启动日志、RTS 硬复位和 `verify_flash`；
没有通过串口写入应用来代替 OTA。测试前读出完整 16 MiB Flash 作为恢复备份
（SHA-256 `5e60455d…`）。

**镜像**：五个 Dev 构建，版本号各不相同，以便在串口和 API 中区分：

| 镜像 | 版本 | 大小（字节） | SHA-256 前缀 | 用途 |
| --- | --- | ---: | --- | --- |
| A | `0.87.6-otaA-1008` | 3115840 | `863853d8` | 从 factory 升级的第一版 |
| FAIL | `0.87.6-otaFAIL-1008` | 3114256 | `37735b1f` | 在启动确认前打印注入标记并 `esp_restart()` |
| B | `0.87.6-otaB-1008` | 3112368 | `d18a6383` | 复现缺陷 |
| C | `0.87.6-otaC-1008` | 3112560 | `9bf1adce` | 修复版 |
| 最终版 | `0.87.6-ota-1008` | 3112544 | `1bd2dc3d` | 经修复后的服务完整升级 |

**网络中断**：请求头声明 `Content-Length: 65536`，只发 8192 字节后关闭真实 TCP 连接；再查询
`upload/status` 得到设备侧偏移，故意用偏移 0 重发、提前调用 finish，确认两者都被拒绝后，从设备
给出的偏移续传到结束。

**故障用例**：每项之后检查启动分区不变、`busy=false`、没有活动会话。截断镜像取一个完整镜像的
前 4096 字节，并给出这 4096 字节的正确 SHA-256。

**废弃会话**：开始一个 4096 字节的会话后不再发任何上传请求，等待 310 s（会话期限 300 s），之后
只读 `/api/ota/status` 和 `/api/automation/status`，不访问 `upload/status`；比较前后 `boot_id` 排除重启。

**回滚**：串口记录启动日志，确认先出现故障版版本号和注入标记、再出现 A 版版本号，不只凭网页上
的版本号推断。

## 3. 问题与根因

### 3.1 截断包借用目标槽里的旧镜像尾部

现象：B 版运行在 ota_1，目标槽 ota_0 里还是 A。上传 A 的前 4096 字节，finish 返回 200、
`reboot_required=true`，启动分区被切到 ota_0。同一用例在 A 版上返回 400
`ESP_ERR_OTA_VALIDATE_FAILED`；缺陷只在目标槽保留同一旧镜像时出现。

根因：`esp_ota_end` 校验的是整个分区。OTA 按顺序擦写，只覆盖了前 4096 字节，后面仍是同一镜像的
旧内容，整个分区看上去是一个完整有效的镜像。

修复：`ota_finish_update` 增加参数 `written`，在 `esp_ota_end` 之后用 `esp_image_verify` 再校验一次，
范围限定为本次实际接收的字节数（包括镜像校验和、哈希和需要的签名）；失败时不设置启动分区。
分块上传、整包上传和 manifest 下载三条路径共用这一处。

验证：主机回归 `test_ota_image_extent_runtime.py` 在旧代码上失败；C 版真板上用 B 的前 4096 字节
重做，分块接口返回 400 `ESP_ERR_IMAGE_INVALID`，整包接口返回 500 `ESP_ERR_IMAGE_INVALID`，启动
分区均不变。

### 3.2 废弃的上传会话要等下一个请求才清理

现象：A 版开始会话后不再请求，310.1 s 后 `/api/ota/status` 仍为 `busy=true`；访问 `upload/status`
后会话才被清除。

根因：300 s 无进展期限只在上传相关请求中检查。客户端放弃后不再有请求，会话、维护占用
（“固件上传维护中”，阻止手动操作）和视频暂停会一直保留。

修复：新增独立任务 `ota_expiry`（4096 字节栈），每 1 s 以不等待的方式尝试取得 OTA 锁，取到后按
原有 300 s 期限清理会话、释放维护占用并解除视频暂停。持锁与写入和 finish 互斥，不打断正在处理
的请求；Flash 清理不放在共享的 `esp_timer` 任务里。

验证：C 版等待 310.2 s，不访问上传接口，`busy=false`，维护操作清空，状态消息为
“chunked OTA session expired and was aborted”，`boot_id` 前后相同。主机回归
`test_ota_session_runtime.py` 覆盖锁被占用时跳过、到期只清理一次。

### 3.3 默认凭据的 403 分支返回成功

根因：`ota_require_auth` 在用户仍使用默认凭据时发送 403，但直接返回发送函数的结果；发送成功
即为 `ESP_OK`，调用方会当作认证通过。

修复：403 发送成功时返回 `ESP_ERR_INVALID_STATE`，发送失败时返回发送错误。

验证：主机回归 `test_ota_auth_runtime.py` 修复前失败、修复后通过。验证这一分支需要把真板账户
重置为默认凭据，本轮没有做；真板只实测了匿名上传的 401。

## 4. 最终实测

**升级链路**（按时间顺序）：

| 步骤 | 操作 | 结果 |
| --- | --- | --- |
| 1 | factory `0.87.6-dev` → A（ota_0），含网络中断 | 续传完成，SHA-256 一致，重启后 A / ota_0 / `valid` |
| 2 | A 上的故障用例与废弃会话 | 错误包均被拒绝；废弃会话 310.1 s 后仍 busy（复现 3.2） |
| 3 | A 上传 4096 字节后 RTS 硬复位 | A 正常启动，`busy=false`，无活动会话 |
| 4 | A → FAIL（ota_1） | 故障版启动约 4.6 s 时在确认前重启，启动程序回滚到 A / ota_0 / `valid` |
| 5 | A → B（ota_1），含网络中断 | B / ota_1 / `valid`；随后故障用例中复现 3.1 |
| 6 | B → C（ota_0） | C / ota_0 / `valid`；3.1、3.2 修复复测通过 |
| 7 | C → 最终版（ota_1），含网络中断 | 最终版 / ota_1 / `valid`，无活动上传 |

**故障用例**：

| 用例 | A 版 | B 版 | C 版 |
| --- | --- | --- | --- |
| 匿名上传 | 401 | 401 | 401 |
| 畸形 JSON | 400 | 400 | 400 |
| 超分区大小（0x400001 字节） | 400 `ESP_ERR_INVALID_SIZE` | 400 `ESP_ERR_INVALID_SIZE` | 400 `ESP_ERR_INVALID_SIZE` |
| 错误 SHA-256 | 400 `ESP_ERR_INVALID_CRC` | 400 `ESP_ERR_INVALID_CRC` | 400 `ESP_ERR_INVALID_CRC` |
| 截断镜像，分块接口 | 400 `ESP_ERR_OTA_VALIDATE_FAILED` | 200，被接受并切换启动分区 | 400 `ESP_ERR_IMAGE_INVALID` |
| 截断镜像，整包接口 | — | — | 500 `ESP_ERR_IMAGE_INVALID` |
| 主动 abort | 恢复空闲 | 未执行（脚本在上一项中止） | 恢复空闲 |
| 废弃会话 310 s | 仍 busy | 未执行 | 已自动清理 |

**最终状态**：`0.87.6-ota-1008`，running = boot = ota_1，`valid`，无活动上传。最终镜像 3112544 字节，
SHA-256 `1bd2dc3d…`。

**独立 Flash 校验**（esptool `verify_flash`，全部 digest 一致）：

| 范围 | 对照 |
| --- | --- |
| 0x2000 起 28672 字节（启动程序与分区表） | 测试前备份 |
| 0x20000 起 3070224 字节（factory 应用） | 测试前备份 |
| 0x820000 起 3112544 字节（ota_1） | 最终镜像 |

**设置保持**：账户凭据（同一凭据可登录）、设备标签、主机名、IPv4、OTA 设置，以及 Dev / 内置
Agent / 外部 MCP 构建开关在整条链路后保持不变。这是对选定设置的语义核对，不代表 NVS 字节完全不变。

**软件验证**：Dev 和 Stable（V2.4）构建通过；Stable 只做了编译验证，真板运行的是 Dev。完整
`check-all` 在 Python 3.11 下通过，包括 130 个 MCP 测试、126 个 Toolkit 测试和新增的 3 个 OTA
运行测试（把固件中的实际函数抽出来配桩编译运行）。

## 5. 遗留问题

- 只测了硬复位和启动故障，没有测真实物理断电、电压跌落、Flash 损坏和多轮寿命循环。
- 续传只针对同一次启动中的会话；硬复位后需要重新开始，不支持跨重启续传。
- HTTPS manifest 远程拉取没有做端到端实测；整包接口只做了截断拒绝，完整升级以分块接口为主。
- 测试客户端对这块已核验身份的板使用自签名 TLS 且不校验证书，没有证明证书信任的部署流程。
- 固件签名、镜像内板型绑定和请求总时限还没有处理。本轮是一块开发板上的验证，不代表 OTA 已达到
  正式发布的要求。
- 默认凭据 403 分支只有主机回归，没有真板验证。
- 基线固件的 Agent Task Service 日志目录和 SD 访问已有降级；本轮只保留功能开关，没有验收完整的
  Agent 运行能力。
- 默认 uv 环境的 Python 3.12.13 下有 3 个 Toolkit 深层 JSON 测试失败，相关代码本轮未改动，不计入
  OTA 结果。2026-10-16 的提交 `7acdf06f` 给 Toolkit 的 GUI 传输和 release 元数据解析改用与 Python
  版本无关的 JSON 嵌套深度限制；本文没有在 Python 3.12 下复跑确认。
