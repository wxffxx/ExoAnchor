# 项目方向

ExoAnchor 当前应被视为一个低成本、可复现的裸机 BMC/KVM 项目，而不是泛用自动化聊天工具。核心价值是：当被控机器的系统、网络或 SSH 不可信时，仍然能通过物理控制面完成观察和恢复。

## 当前主线

短期主线是 ESP32-P4 KVM：

- `device/ESP32P4/waveshare-nano-diy/`：当前最快可复现路径，不做 PCB，不提供 ATX 电源控制。
- `device/ESP32P4/firmware/`：所有 ESP32-P4 形态共用的 ESP-IDF 固件。
- `device/ESP32P4/exoanchor-esp32p4x/`：完整自研板目标，承载视频、HID、电源控制和电源状态检测。
- ESP32-P4 固件内 Agent：负责观察、动作、安全确认和日志；仓库不再维护上位机 Python Agent runtime。
- `integrations/exoanchor-mcp/`：可选外部 MCP 适配器，只通过受限设备 API 接入。

## 近期重点

1. 让 Waveshare NANO DIY 方案可重复搭建、可稳定烧录、可稳定找到 IP。
2. 固件侧先稳定 UVC 视频、GPIO26/GPIO27 USB HID、有线网络、基础 dashboard。
3. 文档明确不同硬件形态的能力边界，尤其不要把 DIY 方案写成支持 ATX 电源控制。
4. Agent 层在 ESP32-P4 内完成结构化 observation、权限和持久化；MCP 只验证外部客户端的工具契约。
5. 参考资料继续沉到 `docs/ref/`，只在项目文档中引用结论。

## 非目标

- 不把 ESP32-P4 DIY 方案包装成完整 BMC；它没有 PWR/RST/12V/3V3AUX 后端。
- 不优先追求本地小模型完成复杂系统管理任务。
- 不把旧硬件目录原样恢复到顶层；旧路线若仍有价值，应整理到 `device/` 的当前形态下。
- 不在 `docs/` 里堆放原始 PDF、构建产物或临时截图；这些归 `docs/ref/` 或本地忽略目录。

## 成熟形态

成熟后的项目应该是两层：

- Device 层简单可靠，负责视频采集、HID 输出、网络访问和板级电源/状态后端。
- 设备内 Agent 可审计、可恢复，负责高层决策、SSH/KVM 通道选择、安全确认、日志和 playbook。

Device 层不要承担复杂智能决策；Agent 层不要假设设备能力已经存在，必须读取具体硬件形态的能力矩阵。
