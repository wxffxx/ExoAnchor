# DeepSeek Harness × ExoAnchor MCP

本目录把本机 DeepSeek Harness（DSH）连接到 canonical ExoAnchor MCP。它不包含
第二套控制实现：DSH 官方 `@deepseek-ai/dsh-mcp-client` 只负责启动同一个 Python
stdio server，并把工具公开为 `mcp__exoanchor__exoanchor_*`。

## 当前兼容边界

- 已按本机 DSH `0.1.0-rc.6` 配置格式制作。
- UART、status、logs 等文本结果可由 DSH 完整观察。
- MCP 客户端虽保留 JPEG block，但当前 DeepSeek chat adapter 给模型的是图片
  占位符。因此 KVM 演示必须保留人工页面观察与画面确认；DSH 不得声称自己识别
  了截图。
- 本覆盖层保持 DSH 自身文件沙箱为 `read-only`。`--supervised` 只打开设备 MCP
  写门，设备侧权限模式、tool policy、fresh observation、租约与确认门仍然生效。

## 准备环境

密码文件仅保存设备 Web/API 登录密码，内容不能写入 shell history、patch 或任务
文本。文件不得具有 group/other 权限：

```bash
chmod 600 "$HOME/.codex/secrets/exoanchor-password"
export EXOANCHOR_BASE_URL='http://<设备地址>'
export EXOANCHOR_DEVICE_ID='<已核验设备 ID>'
export EXOANCHOR_USERNAME='<设备 API 用户名>'
export EXOANCHOR_PASSWORD_FILE="$HOME/.codex/secrets/exoanchor-password"
```

先只组合配置，不启动模型、MCP 子进程或设备请求：

```bash
./scripts/run-dsh.sh --read-only --check > /tmp/exoanchor-dsh-config.txt
```

检查输出中存在 `mcp-exoanchor`、`@deepseek-ai/dsh-mcp-client`、canonical MCP cwd
和 `exoanchor-mcp-control` skill root；不要把包含私有地址的 dump 发布出去。

## 只读冒烟

```bash
./scripts/run-dsh.sh --read-only -- \
  '加载 exoanchor-mcp-control；只调用 capabilities、status、UART status，不得写入。报告工具门与当前 owner。'
```

只读冒烟通过后，才为用户明确要求的演示开启设备写门：

```bash
./scripts/run-dsh.sh --supervised -- \
  '加载 exoanchor-mcp-control，并完整读取 FOUR_DEMOS_zh.md。执行 Demo 01；每个依赖画面的步骤等待人工确认，用户 Stop 后立即终止。'
```

四项演示的公开步骤与验收矩阵：

- [`../../skills/exoanchor-mcp-control/references/FOUR_DEMOS_zh.md`](../../skills/exoanchor-mcp-control/references/FOUR_DEMOS_zh.md)
- 操作者自己的设备身份、提示词扩展和现场记录必须保存在仓库外的受控工作区，
  不得复制进公开提交。

## 故障处理

- `failOnStartupError` 会让连接/工具发现错误直接终止启动，避免得到一个假正常
  DSH 会话。
- MCP stdio 崩溃最多退避重连 3 次。写调用丢失结果时仍按 `outcome_unknown`
  处理：先独立读回，不能依赖重连盲目重发。
- 更改 URL、设备 ID、密码文件或代码后，启动新的 DSH 进程；不要假定旧 stdio
  子进程已经读取新环境。
- 任意 SSH、未校验 SSH host、持久化 SSH 输出默认关闭；确需开启时单独审查，
  不修改本演示覆盖层来图省事。
