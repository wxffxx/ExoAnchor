# HTTPS、首次配对与账号迁移

Dev 和 Stable 的管理入口使用 HTTPS 443；独立视频入口使用 HTTPS/WSS 444。Web 页面使用相对地址和 Secure cookie。认证存储或 TLS 身份读取失败时，设备拒绝启动对应服务。

## 首次使用

1. 将新设备连接到可信的本地网络，浏览器打开 `https://<设备地址>/`。设备使用本机生成的自签名证书，首次访问按浏览器流程确认该设备连接。
2. 页面直接显示“创建管理员账号”。输入用户名、新密码和确认密码；无需初始密码、调试线或串口日志。
3. 创建成功后进入设备界面。初始化入口在设备端关闭并持久化，后续及重启后均使用正常登录。未初始化设备无法登录或创建控制会话。
4. MCP/Toolkit 连接时，在设备 Settings 的账户区域点击“下载设备证书”，将这份 PEM 用于后续证书验证。私钥保留在设备中；公开证书也可从 `/api/auth/certificate` 下载。

首次归属采用可信局域网中的首次认领，证书采用首次信任。这个阶段没有设备外的身份凭据；其他能访问未初始化设备的网络节点也能竞争首次认领。因此首次配置须在操作者控制的网络完成。已创建账号的设备不会重新开放初始化入口；跨站表单、缺少/过期初始化票据和 MCP 请求均不能进入创建流程。

后续客户端同时验证证书链、签名、有效期和完整叶证书指纹。配对文件固定设备身份，DHCP 地址变更后继续检查同一指纹。

```sh
export EXOANCHOR_TLS_CERTIFICATE_FILE="$PWD/exoanchor-device.pem"
export EXOANCHOR_BASE_URL='https://<设备地址>'
```

未设置证书文件时，客户端使用系统 CA 和 hostname 校验。Toolkit 的裸 IPv4 地址默认解析为 HTTPS 443；GUI 中的本地回环 HTTP 服务仅提供本机界面。首次信任自签名设备不等于已有公共 CA 或硬件出厂身份认证。

`tools/h264-accept.py` 使用同一配对策略，HTTP 请求与 WebSocket 升级均先校验证书。根目录旧诊断脚本也默认 HTTPS；其 WSS 使用普通 hostname 校验，设备地址须与证书 SAN 一致。运行验收工具会实际控制设备，需按当前硬件验收规程执行。

## 旧固件兼容

新版固件不提供明文管理入口。连接旧固件需要显式选择：MCP/诊断工具设置 `EXOANCHOR_ALLOW_INSECURE_HTTP=1` 并使用 `http://<设备地址>`；Toolkit 输入完整 `http://<设备地址>`。这些旧模式仍然传输明文凭据，仅用于由操作者明确选择的旧设备环境；客户端不会自动降级或转发设备请求的重定向。

## 账号与会话

- 改账号需要 Browser 会话、设置能力，以及最近 60 秒内的重新登录。MCP 会话无法修改账号；设置表单会先用当前密码登录。
- Stable 使用带随机盐的 PBKDF2-SHA256（60,000 次），将旧 SHA-256 记录在成功验证后迁移。迁移保持当前登录有效；修改凭据则撤销所有既有会话。
- 新登录生成独立的 256 bit 随机 token。Stable 会话最长 24 小时，并遵守设备配置的空闲退出时间；注销在设备端撤销会话。网络中断时注销请求可能无法到达设备，服务端期限继续生效。
- Stable 登录在一分钟内失败五次后锁定 30 秒。Dev 保留其现有限流并移除账号入口的匿名密码校验路径。密码计算在登录 worker 执行。
- Dev 中旧的共享 bootstrap `admin/admin` 会在启动时失效并进入网页初始化状态；不会输出初始密码。读取损坏或无法保存凭据时，设备拒绝认证服务。定制构建可预置独立密码，并在首次登录后更改。

## 依赖补丁与验证

UVC 的解析与打印安全补丁见[补丁说明](../../device/ESP32P4/firmware/security-patches/usb_host_uvc-2.5.1/README.md)。Dev 的 libssh2 保留原有大包长度补丁，并加入 ETM 首块解密前的长度检查。构建验证固定输入的 SHA-256，并编译受审查的补丁源码。

整仓入口 `scripts/check-all.sh` 包含账号状态、一次性网页初始化与跨站票据、授权副作用、已释放日志参数、真实回环 TLS 和客户端响应边界回归。账号测试需要 C 编译器、pkg-config 和 OpenSSL。USB 描述符测试还需要完成组件解析后的 ESP-IDF 工作区：

```sh
python3 device/ESP32P4/firmware/security-tests/test_auth_runtime.py
python3 device/ESP32P4/firmware/security-tests/test_http_security_runtime.py
python3 device/ESP32P4/firmware/security-tests/test_tls_clients.py
node device/ESP32P4/firmware/security-tests/test_browser_setup.mjs
python3 device/ESP32P4/firmware/security-tests/test_uvc_descriptors.py
```

这些测试和固件构建验证源码与本机协议行为。真机 TLS 握手、视频吞吐、USB 热插拔、凭据迁移和 GPIO/HID 行为仍需硬件验收。
