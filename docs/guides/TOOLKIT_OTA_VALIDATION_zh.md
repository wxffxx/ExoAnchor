# Toolkit 下载与 OTA 客户端验证

本规程验证主机端的 Release 下载、固件包 manifest 校验、网络 OTA 上传和错误恢复。
测试使用临时目录、合成镜像及绑定 `127.0.0.1` 的 HTTP 服务，不连接 GitHub 或物理设备。
它不证明设备端 Flash 写入、启动确认、回滚、断电恢复或固件自行通过 URL 拉取 manifest 的行为。

## 运行离线回归

在仓库根目录准备 Toolkit 和整仓主机测试依赖：

```bash
uv sync --locked --project toolkit --python 3.12
uv pip install --python toolkit/.venv/bin/python \
  -r device/ESP32P4/firmware/v0.86.6-dev/tests/host/requirements.txt
```

仅运行下载、上传和 JSON 边界回归：

```bash
cd toolkit
.venv/bin/python -m unittest \
  tests.test_ota_workflow tests.test_json_limits \
  tests.test_firmware tests.test_http_transport_failures \
  tests.test_gui_transport tests.test_release_transport -v
```

整仓检查从仓库根目录运行；所需 C 编译器、Node.js、OpenSSL 和 `pkg-config`
见[固件验证规程](FIRMWARE_VALIDATION_zh.md)及[传输验证说明](SECURITY_TRANSPORT_zh.md)：

```bash
PYTHON="$PWD/toolkit/.venv/bin/python" \
MCP_PYTHON="$PWD/toolkit/.venv/bin/python" \
TOOLKIT_PYTHON="$PWD/toolkit/.venv/bin/python" \
  ./scripts/check-all.sh
```

Python 兼容性变更应分别使用 3.10、3.11、3.12 运行 Toolkit 全部测试和
`device/ESP32P4/firmware/security-tests/test_tls_clients.py`。后者建立真实的本地
TLS 连接，检查 Toolkit、MCP 和诊断客户端只向通过证书校验的对端发送凭据。
准备环境时可替换上述 `--python` 版本；依赖安装完成后测试本身无需外网。

## 验证内容

| 路径 | 核对结果 |
| --- | --- |
| Release → ZIP → manifest → OTA | 下载器校验归档与各 segment 的 SHA-256，完整上传的字节与合成镜像一致；三轮上传均独立完成；再次下载复用经过复验的缓存。 |
| 归档或 segment 损坏 | 返回明确错误，临时下载被清理，不发布可用缓存，也不开始 OTA。 |
| 校验后文件变化 | 在任何设备请求前，重新核对实际上传字节的长度与 SHA-256；文件增长、截断或等长修改均停止。 |
| 部分写入或整块响应丢失 | 通过真实 socket 断连模拟；先查询状态，只从同一 session、大小、摘要对应的已确认 offset 继续，不能盲目重发原块。 |
| 断连且没有进展 | 当前调用失败并停止，不无限重试；之后的新调用可查询并续传匹配的活动会话。 |
| 会话或板型不匹配 | 不开始新上传、不覆盖其他活动会话、不自动 abort。 |
| 状态字段不合法 | 拒绝布尔值冒充 offset、空或非字符串 session ID、无进展或越界的 offset、非正整数 chunk size。 |
| 新会话分块限制 | 使用 start 响应给出的 chunk size，同时维持客户端每块最多 64 KiB 的限制。 |
| finish 或 reboot 拒绝 | finish 必须同时返回 `ok: true` 和匹配的 SHA-256 才能请求重启；reboot 未确认时不能报告请求成功。 |
| Release 重定向 | HTTP 308 在 Python 3.10 上也能处理；跨 origin 跳转移除凭据，HTTPS 不能降级为 HTTP。 |

JSON 接收入口在解析后、交给业务代码前，以非递归遍历限制为最多 64 层对象/数组。
根对象或数组计一层，字符串里的括号不计层数。限制覆盖 Toolkit 和 MCP 的设备响应、
MCP stdio 消息、GUI 请求、Release 元数据和固件 manifest；本地 manifest 及共用读取
函数处理的构建 JSON 文件最多 1 MiB。
各 HTTP 入口保留各自原有的字节限制，解释器自身的语法、整数长度或递归异常仍会转换为
对应 API 的错误。该策略不依赖某一 Python 版本恰好抛出 `RecursionError`，也不修改全局递归上限。

## 中断后的排查顺序

1. 保留原始固件包并重新运行 Toolkit 的 `firmware verify`。文件不再匹配 manifest 时，
   先恢复或重新获取可信包，不继续上传。
2. 确认目标身份、板型和配对证书后，检查 `/api/ota/upload/status` 的
   `active`、`session_id`、`size`、`sha256`、`offset`。日志中不记录密码、token 或 Cookie。
3. 若活动会话对应同一包，重新执行原上传操作会从报告的 offset 恢复；状态不匹配时停止，
   由操作者确认现有会话用途后再决定是否显式取消。客户端不会替操作者取消其他上传。
4. finish 或 reboot 的响应丢失时，不把“不知道结果”记为“操作未发生”，也不自动重放。
   先检查设备状态。`--no-reboot` 只表示镜像待激活；“Device reboot requested”只表示
   请求获确认，均不代表新镜像已成功启动。

保存源码版本、Python 版本、命令、退出码和完整日志。真实设备的启动、回滚及断电验证
应单独记录，并遵循[固件验证规程](FIRMWARE_VALIDATION_zh.md)的设备身份与恢复条件。
