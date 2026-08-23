# 双系统启动目标编排方案

- 状态：设计提案
- 记录日期：2026-07-30
- 适用范围：ExoAnchor ESP32-P4 KVM 固件、Web UI、MCP 与可选 Host Companion

## 1. 背景

当前 ExoAnchor 已具备以下基础能力：

- 通过 GPIO 执行主机 Power/Reset；
- 通过 USB HID 发送 BIOS/Boot Menu 热键；
- 通过 KVM 视频观察启动画面；
- 保存单一的被控端操作系统资料；
- 对高风险操作执行计划、审批、启动、取消和人工确认。

现有 `boot_key_sequence` 只能进入 BIOS Setup 或 Boot Menu，不能表达“下次进入
Linux”或“下次进入 Windows”。现有 Target Profile 也只有一个
`operating_system` 字段，无法准确描述双系统或多系统主机。

## 2. 目标

在 Dashboard、Agent 或 MCP 中提供统一的启动目标：

- 下次启动 Linux；
- 下次启动 Windows；
- 进入 Boot Menu；
- 进入 BIOS Setup。

系统应优先使用一次性的 UEFI `BootNext`，在 Host Companion 不可用时退回
Boot Menu HID，最终以人工 KVM 接管兜底。

不把修改长期默认启动项作为普通切换操作；修改默认启动顺序应是独立、明确审批的
设置操作。

## 3. 三级执行策略

### 3.1 UEFI BootNext

Linux 和 Windows 分别运行一个最小化的 ExoAnchor Host Companion。Companion
负责发现本机 UEFI 启动项，并在收到经过认证和审批的请求后设置一次性
`BootNext`。

这是首选路径，原因如下：

- 只影响下一次启动；
- 不依赖 BIOS 菜单位置或显示分辨率；
- 可以使用稳定的 UEFI 启动项标识；
- 启动后可以由 Companion 主动回报实际操作系统身份。

UEFI 启动项标识是主机本地数据，不应作为跨主机通用预设。首次绑定时必须发现并由
用户确认。

### 3.2 Boot Menu HID

当当前操作系统离线、Companion 不可用或无法设置 `BootNext` 时，复用现有
`boot_key_sequence`：

1. 创建带主机 Profile 的启动计划；
2. 由用户批准 Reset 和发键；
3. 在限定时间内重复发送 F11/F12 等已配置热键；
4. 进入 Boot Menu 后执行该主机专属的选择序列；
5. 转入启动结果验证。

菜单选择序列必须属于具体主机 Profile。不得对未知 BIOS 菜单进行通用的零样本自动
导航，也不得仅依赖未经验证的屏幕绝对坐标。

### 3.3 人工 KVM

出现以下情况时停止自动执行并请求人工 KVM：

- BIOS/Boot Menu 与已保存 Profile 不一致；
- BitLocker、恢复密钥或 Secure Boot 提示；
- HID、视频或电源状态不可用；
- 启动结果无法验证；
- 达到最大尝试次数。

## 4. 建议数据模型

```json
{
  "schema_version": "exoanchor.boot_profile.v1",
  "profile_id": "desk-node",
  "default_target": "linux",
  "boot_targets": [
    {
      "id": "linux",
      "label": "Ubuntu",
      "kind": "uefi",
      "uefi_entry": "0003",
      "verification_identity": "ubuntu-desk-node"
    },
    {
      "id": "windows",
      "label": "Windows 11",
      "kind": "uefi",
      "uefi_entry": "0001",
      "verification_identity": "windows-desk-node"
    }
  ],
  "firmware_entry": {
    "boot_menu_key": "F11",
    "bios_setup_key": "Delete",
    "start_delay_ms": 250,
    "interval_ms": 300,
    "max_attempts": 12,
    "total_timeout_ms": 15000
  },
  "fallback_policy": {
    "allow_hid_menu": true,
    "max_boot_attempts": 1,
    "require_human_on_failure": true
  }
}
```

Target Profile 应从单一 `operating_system` 字段演进为：

- `boot_targets[]`：该主机可进入的系统；
- `observed_target`：最近验证成功的实际系统；
- `requested_target`：当前一次性启动意图；
- `default_target`：主机长期默认目标，仅用于展示和恢复判断。

## 5. 建议 API

```text
GET  /api/host/boot-targets
GET  /api/host/boot-intent
POST /api/host/boot-intent
POST /api/host/boot-intent/{id}/approve
POST /api/host/boot-intent/{id}/start
POST /api/host/boot-intent/{id}/cancel
POST /api/host/boot-intent/{id}/confirm
```

创建请求示例：

```json
{
  "target": "windows",
  "strategy": "auto",
  "reboot": true
}
```

`strategy=auto` 的选择顺序是：

1. Companion + UEFI BootNext；
2. 主机专属 Boot Menu HID；
3. 人工 KVM。

## 6. 状态机

```text
idle
  -> planned
  -> approved
  -> arming_bootnext | entering_boot_menu
  -> rebooting
  -> awaiting_identity
  -> succeeded

任意执行状态
  -> cancelled | failed | human_intervention_required
```

启动意图至少记录：

- `intent_id`；
- 请求目标和实际采用的策略；
- 请求者、审批会话和时间；
- Reset/Reboot 是否已执行；
- 发键次数；
- 验证证据；
- 最终结果和失败原因。

## 7. 启动结果验证

验证优先级如下：

1. Host Companion 使用设备绑定密钥回报系统身份和本次启动 ID；
2. 预先配置的网络服务或 SSH/Windows 管理端点回报身份；
3. UART 启动标识；
4. KVM 画面由用户人工确认。

只检测到主机上电或网络连通不能证明进入了目标操作系统。

## 8. 安全与恢复约束

- 设置 `BootNext`、重启、Reset 和 Boot Menu 自动选择都需要明确审批；
- 默认只允许一次自动重试，禁止形成无限重启循环；
- 保存 `last_known_good_target`，但不在失败后擅自修改长期默认启动顺序；
- Companion 不向 ESP32-P4 返回管理员密码或恢复密钥；
- Companion 请求应使用设备绑定身份、短期 nonce 和防重放校验；
- 发现 BitLocker/Secure Boot/恢复环境时立即停止；
- 人工打开 KVM 时应抢占并终止自动 HID 序列；
- Profile 不匹配或固件菜单变化时应使 HID 菜单自动选择失效。

## 9. 分阶段实现

### MVP 1：主机 Profile 与人工菜单

- 将 Target Profile 扩展为多个 `boot_targets`；
- Dashboard 增加“进入 Linux / Windows / Boot Menu / BIOS”入口；
- 复用现有 `boot_key_sequence` 打开菜单；
- 由用户通过 KVM 完成具体启动项选择和确认。

### MVP 2：主机专属 HID 选择

- 增加 Boot Menu Profile；
- 支持有界的方向键、Enter 和等待序列；
- 加入启动意图状态机、审计和失败停止。

### MVP 3：跨平台 Companion

- Linux 和 Windows 发现 UEFI 启动项；
- 绑定并确认目标名称与启动项；
- 设置一次性 `BootNext`；
- 启动完成后回报系统身份；
- 将 HID 路径降级为离线备用方案。

## 10. 验收条件

- 用户可以明确选择 Linux 或 Windows，且不会永久改变默认启动项；
- 正常情况下仅重启一次；
- 目标系统启动后能提供可审计的身份验证证据；
- Companion 不可用时能进入已配置 Boot Menu；
- 未知 BIOS、恢复界面或验证超时时不会继续盲目发键或重启；
- 人工 KVM 接管能够立即中断自动流程。
