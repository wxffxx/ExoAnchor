# ExoAnchor — KVM Agent Framework

SimpleIPMI 的智能层。通过 KVM 硬件（HID + 视频采集）和 SSH 自主操作被控 Ubuntu 主机。

## 运行模式

| 模式 | 空闲行为 | 触发方式 |
|------|---------|---------|
| **Manual (人工)** | 无 | 仅人类手动触发 |
| **Passive (被动)** | 监控画面+服务 | 异常自动恢复 |
| **Semi-Active (半主动)** | 监控+定时预案 | 异常/条件/定时触发 |

## 架构

```
exoanchor/
├── core/           # 核心引擎 (被动监控 + 半主动执行器)
├── vision/         # 视觉后端 (本地检测 + Vision LLM API)
├── action/         # 操作驱动 (HID/SSH 多通道调度)
├── channels/       # SSH 通道管理
├── skills/         # Skill 系统 (加载/保存/录制)
├── safety/         # 安全机制
├── api/            # FastAPI 路由 (供 Host 挂载)
├── skill_library/  # 用户技能库
└── prompts/        # LLM Prompt 模板
```

## Host 集成

```python
from exoanchor.api.routes import create_agent_router
from exoanchor.action.adapters import MockHIDAdapter, MockVideoAdapter, MockGPIOAdapter

# Mac 开发模式
router, agent = create_agent_router(
    hid_adapter=MockHIDAdapter(),
    video_adapter=MockVideoAdapter(),
    gpio_adapter=MockGPIOAdapter(),
    config={"mode": "manual", "target": {"ip": "localhost", "ssh": {"port": 22}}}
)
app.include_router(router, prefix="/api/agent")
```

## 快速测试 (Mac)

```bash
pip install -r requirements.txt
# 在 Host server 中启动，或单独运行测试
python -m pytest tests/
```

## Skill 格式

YAML (声明式):
```yaml
skill:
  name: "restart_nginx"
  mode: "scripted"
  steps:
    - action: {type: "shell", command: "sudo systemctl restart nginx"}
    - action: {type: "shell", command: "systemctl is-active nginx"}
      expect: "active"
```

Python (复杂逻辑):
```python
from exoanchor.skills import SkillBase
class MySkill(SkillBase):
    async def execute(self, ctx):
        await ctx.ssh.run("...")
```
