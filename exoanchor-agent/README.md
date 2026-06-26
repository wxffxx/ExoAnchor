# ExoAnchor Agent

ExoAnchor 的 Agent 层。它通过 KVM 硬件（HID + 视频采集 + 电源控制）和 SSH 操作被控主机。

ExoAnchor 的发展重点是可靠闭环，而不是泛用聊天自动化：服务端持久化 runtime、结构化 observation、可审计日志、安全确认，以及面向裸机恢复的技能/playbook。

项目级方向见 [../docs/project/PROJECT_DIRECTION_zh.md](../docs/project/PROJECT_DIRECTION_zh.md)，ExoAnchor 详细 runtime 路线见 [ROADMAP.md](ROADMAP.md)。

模型策略见 [../docs/architecture/MODEL_STRATEGY_zh.md](../docs/architecture/MODEL_STRATEGY_zh.md)：MVP 不要求本地小模型完成系统管理任务，DeepSeek V4、Gemini 和 OpenAI 都应保留为一等 provider。

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
