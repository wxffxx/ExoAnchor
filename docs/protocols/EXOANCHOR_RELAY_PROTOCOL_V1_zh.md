# ExoAnchor Relay Protocol v1（EXAR/1）

状态：草案；线协议版本：1；实现许可证：MIT。

## 设计边界

EXAR/1 是 ExoAnchor 自有的设备反向连接协议，不兼容 FRP，也不复用 FRP、yamux 或其他反向代理项目的源码、消息名称、常量和线协议。

它只采用通用网络方法：设备主动建立出站 TLS 连接，服务端在该连接上创建带流量控制的逻辑流，双方搬运逻辑流中的字节。协议只允许访问设备自身的固定服务，不提供任意 `IP:port` 转发。

首版服务白名单：

| 服务 ID | 设备端目标 | 用途 |
| ---: | --- | --- |
| 1 | 本机 HTTP 服务（80） | UI、API、HID/UART WebSocket |
| 2 | 本机视频 HTTP 服务（81） | MJPEG |

因此，即使 relay 凭据泄漏，协议本身也不能被用于扫描或访问设备所在局域网的其他主机。

## 分层

```text
浏览器 --HTTPS/WSS--> 公网 Relay
                         |
                         | EXAR/1 over TLS（设备主动出站）
                         v
                    ExoAnchor MCU
                         |
                         +--> 固定本机 HTTP 服务
                         +--> 固定本机 MJPEG 服务
```

TLS 层负责加密、完整性和服务端身份验证；EXAR/1 负责设备身份验证、逻辑流、流量控制、心跳和关闭语义。不得使用明文 EXAR/1。

## 固定帧头

所有多字节整数使用网络字节序。帧头固定为 20 字节：

| 偏移 | 长度 | 字段 | 约束 |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `EXAR` |
| 4 | 1 | version | `1` |
| 5 | 1 | type | 见消息表 |
| 6 | 2 | flags | 当前仅 DATA 允许 `FIN=0x0001` |
| 8 | 4 | stream_id | 会话消息为 0；流消息非 0 |
| 12 | 4 | sequence | 发送方向单调递增，用于诊断和状态机检查 |
| 16 | 4 | payload_size | 最大 16 KiB |

帧解析必须处理任意拆包和粘包；一次 socket 读取不等于一个帧。任何未知类型、未知 flag、非法长度或非法 stream ID 都应关闭 TLS 会话，而不是尝试跳过。

## 会话建立与设备认证

1. 设备完成 TLS 握手，必须校验 relay 证书链和主机名；生产环境建议同时固定私有 CA。
2. 设备发送 `HELLO`：`capabilities:u32 || device_nonce[32] || device_id_len:u8 || device_id`。
3. relay 返回一次性 `CHALLENGE`：32 字节密码学随机数。挑战只在当前 TLS 连接有效，并应在短时间后过期。
4. 设备发送 `AUTH`：

   ```text
   HMAC-SHA256(
       per_device_secret,
       "EXAR-AUTH-V1" || 0x00 || HELLO_payload || CHALLENGE_payload
   )
   ```

5. relay 用恒定时间比较 HMAC。成功后返回 `READY`；失败只返回通用认证错误并断开，不暴露设备是否存在。

设备 secret 必须逐台随机生成，存放在 secret store 中，不得复用 Web 登录密码，不得写入日志、崩溃转储或普通设置导出。relay 数据库只向认证进程暴露该 secret；后续可迁移到安全元件或双向 TLS，不改变帧层。

`READY` payload 为 `session_id[16] || heartbeat_ms:u32 || max_streams:u16 || reserved:u16`。心跳范围为 5–120 秒，并发流上限为 1–32。

## 消息表

| type | 值 | stream_id | payload |
| --- | ---: | ---: | --- |
| HELLO | `0x01` | 0 | capabilities、设备 nonce、设备 ID |
| CHALLENGE | `0x02` | 0 | 32 字节 relay nonce |
| AUTH | `0x03` | 0 | 32 字节 HMAC |
| READY | `0x04` | 0 | 会话参数 |
| OPEN | `0x10` | 非 0 | `service:u8 || reserved[3] || relay_rx_window:u32` |
| OPEN_OK | `0x11` | 非 0 | `device_rx_window:u32` |
| DATA | `0x12` | 非 0 | 原始字节；可带 FIN |
| WINDOW | `0x13` | 非 0 | 新增接收额度 `credit:u32`，必须非 0 |
| CLOSE | `0x14` | 非 0 | `code:u16 || reason`，总长 2–130 |
| PING | `0x20` | 0 | 8 字节随机值 |
| PONG | `0x21` | 0 | 原样返回 PING payload |
| ERROR | `0x22` | 0 或非 0 | `code:u16 || reason`，总长 2–130 |

只有 relay 可以发起 `OPEN`。设备收到后先检查服务白名单和并发限额，再连接本机固定端口；成功后返回 `OPEN_OK`，失败返回该 stream 的 `ERROR`/`CLOSE`。

## 背压与内存上限

每个方向独立维护发送额度：

- `OPEN` 告知设备 relay 可以接收多少字节。
- `OPEN_OK` 告知 relay 设备可以接收多少字节。
- 接收方消费数据后用 `WINDOW` 返还额度。
- 发送方额度为 0 时必须停止读取其上游，不能无限缓存。
- 单个 WINDOW 增量不得超过 4 MiB，累计额度必须使用饱和检查，禁止整数回绕。

建议 ESP32-P4 首版使用 4 个并发流、每流 32 KiB 窗口、16 KiB 最大 DATA，并从 PSRAM 分配 payload/队列。MJPEG 应采用小窗口自然背压；慢浏览器不得拖垮控制 API 或 HID WebSocket。

## 必须实现的状态与故障处理

- 状态顺序固定为 `TLS -> HELLO -> CHALLENGE -> AUTH -> READY`；乱序消息直接断开。
- READY 前拒绝所有流消息。
- stream ID 在一次会话中不得复用。
- PING 超过两个周期未收到匹配 PONG，关闭并指数退避重连；退避加入随机抖动。
- 网络变化、TLS 错误或协议错误时关闭全部本地 socket，清零 HMAC 临时数据，再重连。
- relay 对设备、用户、IP、并发流、带宽和认证失败次数分别限流。
- 浏览器侧仍使用 ExoAnchor 原有登录/权限体系；relay 身份不得自动等价为设备管理员权限。

## 实现切片

当前仓库首个切片只包含纯 C 帧编解码、payload 结构校验、认证 transcript 构造和 host 单测。后续实现顺序：

1. MCU `esp-tls` 会话状态机与证书校验。
2. mbedTLS HMAC 适配及逐设备 secret 配置/轮换。
3. 固定服务 socket adapter、每流窗口和公平调度。
4. 独立 relay 服务及浏览器入口。
5. 将 KVM 页面的视频 URL 改为 relay 可路由的同源地址，避免现有 `:81` 绝对端口绕过 relay。

正式发布前应做协议模糊测试、断线/慢连接压力测试、secret 生命周期审计和第三方法务复核。
