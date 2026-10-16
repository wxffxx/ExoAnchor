# 通行密钥（Passkey / WebAuthn）

Dev 和 Stable KVM 支持在浏览器中添加、登录和删除通行密钥。Apple 用户可通过系统通行密钥界面使用 Face ID、Touch ID 或设备解锁；实现采用标准 WebAuthn，也允许兼容的其他验证器。设备只保存凭据 ID、公钥、签名计数器和名称，不接收私钥或生物识别数据。

这是独立的快捷登录方式：通行密钥登录要求验证器完成用户验证，不再要求输入账户密码或 TOTP。原有密码登录仍必须通过已启用的 TOTP / 一次性恢复码。Toolkit 和 MCP 继续使用原有密码、动态码与会话令牌流程，不直接调用浏览器通行密钥。

## 访问网址与 HTTPS

1. 使用稳定的 **HTTPS 主机名**，不要用 IP 地址注册。默认设备证书包含 `exoanchor-<设备 MAC>.local` 的 DNS 名称；实际名称以设备证书为准。局域网需要能解析该名称。
2. 按 [HTTPS 配对指南](SECURITY_TRANSPORT_zh.md) 核对设备证书，并在使用的系统 / 浏览器中建立信任。仅跳过证书错误页不能作为通行密钥可用性的保证。
3. 也可以通过自行管理的 HTTPS 域名和反向代理访问设备。代理必须验证上游设备证书，保留外部 `Host` 与 `Origin`，并正确转发现有 WebSocket / 视频服务。新增功能不会自动配置 DNS、证书、系统信任或代理。
4. 打开设置中的“通行密钥”，输入当前密码和已启用的验证码，点击“绑定当前网址”。保存后所有旧会话退出，重新登录后添加通行密钥。

域名可以解析到局域网 IP，不要求把设备开放到公网。RP ID 固定为绑定网址的完整主机名；协议、主机名、非默认端口组成的 Origin 必须完全一致。只接受规范化的小写 ASCII / punycode HTTPS Origin，默认 443 端口应省略。

网址绑定是管理员操作。服务端不会根据匿名登录请求、`Host` 或转发头自动更换 RP ID。已有密钥时不能更换绑定网址；先删除所有密钥，再绑定新网址并重新注册。更换域名、端口或设备不会自动迁移已有密钥。

## 添加、登录与删除

- 先完成本地管理员账户创建。管理通行密钥要求有效的浏览器会话、最近 60 秒内登录、当前密码，以及已启用的 TOTP / 恢复码。刚用于登录的动态码不可再次使用，需等待下一组或使用未用过的恢复码。
- 点击“添加通行密钥”，按系统提示创建。必须支持可发现凭据（resident key）及用户验证。创建请求有 120 秒有效期，并绑定当前会话和账户凭据代次。
- 登录页点击“使用通行密钥登录”。取消或超时后可以重试，或继续用密码登录。
- 最多保存 4 个凭据，每个凭据 ID 最大 256 字节，名称最多 48 UTF-8 字节（页面限制 16 个 UTF-16 字符）。同一 iCloud 钥匙串同步的密钥通常是同一个凭据，无需为每台 Apple 设备重复添加。
- 添加、删除和更换绑定网址均撤销所有浏览器 / MCP 会话，作废未完成的认证请求。删除设备上的凭据不会自动删除 Apple“密码”中的副本；被删除的副本无法再登录本设备。
- 保留账户密码和离线恢复码。通行密钥存储故障会阻止通行密钥登录，密码＋现有两步验证路径仍独立可用。

## 服务端验证与持久化

实现依据 [WebAuthn 注册与断言验证流程](https://www.w3.org/TR/webauthn-3/#sctn-rp-operations)：

- 256 位随机挑战，单次使用，使用单调时钟判定有效期；最多 4 个未完成请求（1 个注册、3 个登录），注册槽位保留给已认证管理员；匿名登录选项请求至少间隔 1 秒。
- 严格验证 ceremony 类型、挑战、完整 Origin、RP ID 哈希、UP / UV 标志、账户 user handle、凭据 ID 和 ES256 签名；拒绝跨源 iframe、重复 JSON 字段和畸形 CBOR。
- 仅请求 ES256（P-256 / SHA-256）和 `none` attestation，不验证 Apple 设备型号或硬件来源。服务端解析 COSE 公钥，不信任客户端额外提供的公钥字段。
- 支持同步凭据的零签名计数器；非零计数器必须递增。备份资格标志必须保持一致，备份状态标志必须与资格相容。
- 配置、公钥、随机 user handle 和计数器组成一个有版本的 NVS 记录。写入失败时关闭该认证路径，避免在不确定的持久化状态下继续认证；重启后重新加载记录。没有新增私钥存储，也没有改变既有 NVS 加密配置。
- 因子写入与密码替换共用凭据互斥锁；因子变化先撤销会话，再持久化。登录结果还需通过会话创建时的代次检查。
- JSON / CBOR 输入具有大小与深度限制；请求体最大 6143 字节，clientDataJSON / attestationObject / authenticatorData 各最多 1024 字节。

## HTTP 接口

`GET /api/auth/passkeys`：仅浏览器会话，返回 `storage_ok`、`enabled`、`origin`、`limit` 与 `credentials[{id,label}]`。公开的 `/api/auth/status` 中 `passkey` 只包含可用状态、绑定网址及容量，不包含凭据列表、公钥或账户 user handle。

`POST /api/auth/passkeys` 接收扁平 JSON 对象，字段值必须是字符串，且要求 HTTP `Origin` 与 `Host` 匹配：

| action | 输入 | 结果 |
| --- | --- | --- |
| `configure` | `current_password`, 可选 `otp` | 绑定当前 HTTPS Origin；必须没有已注册凭据 |
| `register_options` | `current_password`, 可选 `otp`, `label` | WebAuthn creation options；二进制字段以无填充 base64url 返回 |
| `register_finish` | `type`, `rawId`, `clientDataJSON`, `attestationObject` | 校验并保存，随后要求重新登录 |
| `login_options` | 无 | WebAuthn request options；使用可发现凭据 |
| `login_finish` | `type`, `rawId`, `clientDataJSON`, `authenticatorData`, `signature`, `userHandle` | 验证后创建浏览器会话与 Secure / HttpOnly Cookie |
| `remove` | `current_password`, 可选 `otp`, `id` | 删除指定凭据，随后要求重新登录 |

`type` 必须为 `public-key`；所有二进制响应字段使用无填充 base64url。登录选项与登录完成允许匿名访问，其余操作仅限已登录浏览器，MCP 会话不能管理密钥。

## 验证边界

`./scripts/check-all.sh` 包含两版生产 C 验证器和真实 ES256 签名的主机回归，以及实际 HTTP 处理器的授权和输入检查。浏览器验收使用 Chromium 虚拟验证器，经 HTTPS 本地测试服务把浏览器生成的凭据提交给生产 C 验证器。常规 HTTP UI mock 明确拒绝注册，不模拟“验证成功”。

这些验证证明软件协议和页面流程，不等于 Apple 真机或 ESP32-P4 设备验收。iPhone / Mac 的 iCloud 钥匙串交互、真实设备证书信任、mDNS 解析、网络时序和实际断电写入仍需在目标环境验收。参考 [Apple Passkey 文档](https://developer.apple.com/documentation/authenticationservices/supporting-passkeys)。
