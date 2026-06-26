# ExoAnchor 文档地图

这里放项目级文档。设备接线、固件构建这类和代码/硬件形态绑定很强的内容，继续放在 `device/` 对应目录；Agent runtime 细节继续放在 `exoanchor-agent/`；`docs/` 只承载跨模块的方向、架构、流程、研究和参考资料。

## 入口

| 分类 | 文件 | 用途 |
|---|---|---|
| 项目方向 | [project/PROJECT_DIRECTION_zh.md](project/PROJECT_DIRECTION_zh.md) | 当前产品形态、重点和非目标 |
| 项目路线 | [project/ROADMAP.md](project/ROADMAP.md) | 硬件、固件、Agent、文档的阶段规划 |
| 架构 | [architecture/README.md](architecture/README.md) | 系统边界和架构索引 |
| 模型策略 | [architecture/MODEL_STRATEGY_zh.md](architecture/MODEL_STRATEGY_zh.md) | LLM provider 与本地模型策略 |
| 流程 | [process/README.md](process/README.md) | 文档维护和归档规则 |
| 研究 | [research/README.md](research/README.md) | 当前调研问题和资料入口 |
| 参考资料 | [ref/README.md](ref/README.md) | datasheet、原理图、提取物、厂商包 |

## 当前文档边界

- 根目录 [README_zh.md](../README_zh.md) 只保留最快能读懂和跑起来的概览。
- 硬件 bring-up 文档放在对应硬件形态旁边，例如 [../device/ESP32P4/waveshare-nano-diy/BuildGuide.md](../device/ESP32P4/waveshare-nano-diy/BuildGuide.md)。
- 固件构建/烧录说明放在固件旁边，例如 [../device/ESP32P4/firmware/README.md](../device/ESP32P4/firmware/README.md)。
- Agent runtime 的实现路线放在 [../exoanchor-agent/](../exoanchor-agent/)。
- 只有跨硬件、固件、Agent 的内容才放进这个 `docs/` 树。

## 当前主线

```text
Waveshare ESP32-P4-NANO DIY
  -> 共享 ESP32-P4 固件
  -> 板载 KVM dashboard
  -> 后续由 exoanchor-agent 接入更高层恢复工作流
```

目标自研板仍然是 `device/ESP32P4/exoanchor-esp32p4x/`，但文档里不能把它的电源控制和电源状态检测写成已验证能力。
