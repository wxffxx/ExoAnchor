# HTTPS、首次配对与账号迁移

Dev 和 Stable 的管理入口使用 HTTPS 443；独立视频入口使用 HTTPS/WSS 444。Web 页面使用相对地址和 Secure cookie。认证存储或 TLS 身份读取失败时，设备拒绝启动对应服务。

## 首次使用

1. 通过可信的本地 UART0 查看首次生成的用户名和 32 位十六进制初始密码。公共构建的密码为空时，每台设备生成独立的 128 bit 随机密码。
2. 从同一 UART0 保存 `BEGIN CERTIFICATE` 至 `END CERTIFICATE` 的完整 PEM，并核对串口给出的 SHA-256 指纹。这里输出的是公开证书，私钥保留在设备中。
3. 浏览器访问 `https://<设备地址>/`。按照浏览器或系统证书管理流程信任这份已核对的设备证书，再登录并替换初始密码。证书含首次启动时的 IPv4；更改地址或 hostname 后，浏览器可能需要更新证书身份。不要通过关闭证书校验解决地址不匹配。
4. MCP 和 Toolkit 可直接配对同一 PEM。配对同时验证证书链、签名、有效期和完整叶证书指纹；DHCP 地址变更后依然用该指纹识别设备。

```sh
export EXOANCHOR_TLS_CERTIFICATE_FILE="$PWD/exoanchor-device.pem"
export EXOANCHOR_BASE_URL='https://<设备地址>'
```

未设置证书文件时，客户端使用系统 CA 和 hostname 校验。Toolkit 的裸 IPv4 地址默认解析为 HTTPS 443；GUI 中的本地回环 HTTP 服务仅提供本机界面。

`tools/h264-accept.py` 使用同一配对策略，HTTP 请求与 WebSocket 升级均先校验证书。根目录旧诊断脚本也默认 HTTPS；其 WSS 使用普通 hostname 校验，设备地址须与证书 SAN 一致。运行验收工具会实际控制设备，需按当前硬件验收规程执行。

## 旧固件兼容

新版固件不提供明文管理入口。连接旧固件需要显式选择：MCP/诊断工具设置 `EXOANCHOR_ALLOW_INSECURE_HTTP=1` 并使用 `http://<设备地址>`；Toolkit 输入完整 `http://<设备地址>`。这些旧模式仍然传输明文凭据，仅用于由操作者明确选择的旧设备环境；客户端不会自动降级或转发设备请求的重定向。

## 账号与会话

- 改账号需要 Browser 会话、设置能力，以及最近 60 秒内的重新登录。MCP 会话无法修改账号；设置表单会先用当前密码登录。
- Stable 使用带随机盐的 PBKDF2-SHA256（60,000 次），将旧 SHA-256 记录在成功验证后迁移。迁移保持当前登录有效；修改凭据则撤销所有既有会话。
- 新登录生成独立的 256 bit 随机 token。Stable 会话最长 24 小时，并遵守设备配置的空闲退出时间；注销在设备端撤销会话。网络中断时注销请求可能无法到达设备，服务端期限继续生效。
- Stable 登录在一分钟内失败五次后锁定 30 秒。Dev 保留其现有限流并移除账号入口的匿名密码校验路径。密码计算在登录 worker 执行。
- Dev 中旧的共享 bootstrap `admin/admin` 会在启动时更换为随机初始密码，并通过 UART0 交付。读取损坏或无法保存凭据时，需要本地恢复。

## 依赖补丁与验证

UVC 的解析与打印安全补丁见[补丁说明](../../device/ESP32P4/firmware/security-patches/usb_host_uvc-2.5.1/README.md)。Dev 的 libssh2 保留原有大包长度补丁，并加入 ETM 首块解密前的长度检查。构建验证固定输入的 SHA-256，并编译受审查的补丁源码。

整仓入口 `scripts/check-all.sh` 包含账号状态、授权副作用、已释放日志参数、真实回环 TLS 和客户端响应边界回归。账号测试需要 C 编译器、pkg-config 和 OpenSSL。USB 描述符测试还需要完成组件解析后的 ESP-IDF 工作区：

```sh
python3 device/ESP32P4/firmware/security-tests/test_auth_runtime.py
python3 device/ESP32P4/firmware/security-tests/test_http_security_runtime.py
python3 device/ESP32P4/firmware/security-tests/test_tls_clients.py
python3 device/ESP32P4/firmware/security-tests/test_uvc_descriptors.py
```

这些测试和固件构建验证源码与本机协议行为。真机 TLS 握手、视频吞吐、USB 热插拔、凭据迁移和 GPIO/HID 行为仍需硬件验收。
