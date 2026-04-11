# ExoAnchor Roadmap

ExoAnchor 现在的主要问题不是单点 bug，而是 agent 主链还没有形成稳定闭环。

当前状态更接近：

- 一个会把自然语言翻成 `plan` 的聊天层
- 一个能执行 YAML skill 的执行器
- 一个在前端里跑计划循环的 UI

要把它做成“真正可用的 agent”，核心目标是把这三条链统一成一个服务端 runtime。

## 总体目标

把 ExoAnchor 从“前端驱动的计划执行器”重构为“服务端持久化 agent runtime”。

最终形态应具备：

- 服务端负责任务编排、执行、恢复，而不是前端
- `plan`、`skill`、`tool call` 走同一条执行主链
- 所有工具调用都返回结构化 observation
- 任务状态、步骤日志、关键事实、artifact 可持久化
- 支持人工确认、失败恢复、断点续跑、后台运行

## 现阶段最核心的缺口

1. `plan` 执行在前端，缺少服务端 orchestrator
2. `skill_call` 会被翻译回前端 plan，没有进入原生 executor
3. 工具层以字符串命令为主，缺少强类型 schema
4. 执行上下文仅在内存中，无法恢复任务
5. step evaluation 主要依赖 LLM 猜字符串 output
6. 安全机制主要是轻量保护，不是后端强约束

## 设计原则

1. 先做稳定 runtime，再增强模型能力
2. 先做结构化 observation，再做更复杂的自主规划
3. 先让服务端能持续执行，再考虑更强的 UI 体验
4. 所有危险动作必须经过服务端安全闸门
5. 所有 agent 行为都必须可追踪、可重放、可恢复

## Phase 0: Stabilize

目标：让当前版本从“容易坏”变成“可调试、可复现”。

交付物：

- 统一 SSH backend 策略，默认优先系统 `ssh`
- 所有 shell 执行返回统一结构：
  - `success`
  - `exit_status`
  - `stdout`
  - `stderr`
  - `output`
- 为每个计划步骤写入结构化日志
- 为每次失败保留原始命令、退出码、截断后的输出
- 把前端 `plan` 执行结果同步到后端任务历史

建议改动：

- 新增 `runs/` 或 `data/runs/` 目录保存任务记录
- 为 `/api/agent/ssh/exec` 和 `/api/agent/ssh/stream` 统一响应 schema
- 为 `test_server.py` 的 plan 接口增加 request/response logging

验收标准：

- 任意计划失败后，可以在本地定位到失败步骤、命令、输出、退出码
- 相同任务重复执行时，日志结构一致
- UI 刷新后至少还能看到最近一次执行结果

## Phase 1: Build A Real Orchestrator

目标：把 `plan` 从前端搬到后端，建立真正的任务运行时。

交付物：

- 新增服务端 `PlanRun` / `StepRun` 模型
- 新增后端计划执行器 `PlanExecutor`
- 新增任务状态流转：
  - `pending`
  - `running`
  - `waiting_confirmation`
  - `paused`
  - `failed`
  - `completed`
  - `aborted`
- 前端只负责展示和发送确认，不再自己驱动计划循环
- WebSocket 只订阅服务端事件流

建议实现：

1. 新建 `exoanchor/core/plan_executor.py`
2. 新建 `exoanchor/core/run_store.py`
3. 给 `/api/agent/task` 增加新的任务类型：
   - `plan`
   - `skill`
   - `guided_task`
4. 给 `/api/agent/task/{id}` 提供：
   - 查询状态
   - 暂停
   - 恢复
   - 中止
   - 获取步骤日志

关键重构点：

- [test_server.py](/Users/wxffxx/Documents/26s/SI_server/simpleipmi_restructure/test_server.py) 只负责把自然语言解析成计划，不再驱动计划执行
- [index.html](/Users/wxffxx/Documents/26s/SI_server/simpleipmi_restructure/exoanchor/dashboard/index.html) 去掉前端 `while` 执行主循环
- 后端负责：
  - 执行步骤
  - 判断成功失败
  - 决定是否进入 step evaluation
  - 写入任务状态
  - 广播实时事件

验收标准：

- 关闭浏览器后任务仍可继续执行
- 刷新页面后能重新附着到当前任务
- 危险步骤确认由后端控制，不依赖前端本地变量

## Phase 2: Unify Plan / Skill / Tool Call

目标：把自然语言计划、技能系统、工具调用统一到一条执行链。

交付物：

- `skill_call` 直接交给 executor，而不是翻译回前端 plan
- YAML skill、Python skill、LLM plan 都转换为统一的内部 IR
- 定义统一 Step schema，例如：
  - `tool`
  - `args`
  - `expect`
  - `retry`
  - `on_failure`
  - `requires_confirmation`

建议实现：

1. 新增 `exoanchor/core/plan_ir.py`
2. 把以下输入统一转换为 `ExecutablePlan`
   - LLM `plan`
   - YAML `skill`
   - Python `skill`
   - trigger recovery flow
3. 让 executor 只执行一种内部表示，不区分来源

需要补的能力：

- `SkillStore` 支持 Python skill 注册与加载
- `SemiActiveExecutor` 支持 `python` mode
- skill 参数校验、默认值填充、schema 校验统一放到后端

验收标准：

- 同一个任务既可以来自 LLM 计划，也可以来自内置 skill，但最后都进入同一执行器
- skill 调用不再依赖 UI 的 plan 翻译逻辑
- 新增一种 skill 类型时，不需要改前端

## Phase 3: Structured Tools And Observations

目标：减少“字符串命令 + LLM 猜测”的脆弱性。

交付物：

- 工具调用标准化
- observation 标准化
- step evaluation 不再只看原始 output 字符串

建议工具层拆分：

- `ssh.exec`
- `ssh.stream`
- `ssh.upload`
- `systemd.status`
- `systemd.restart`
- `docker.ps`
- `docker.restart`
- `file.write`
- `file.read`
- `process.start`
- `process.stop`
- `vision.analyze`
- `hid.key_press`
- `hid.type_text`

建议 observation schema：

- `tool_name`
- `success`
- `exit_status`
- `stdout`
- `stderr`
- `parsed`
- `artifacts`
- `timestamp`

建议重点：

- 常见运维任务优先不用“裸 shell”
- 能结构化的都先结构化
- step evaluator 输入以 observation 为主，原始 output 为辅

验收标准：

- 至少一半常见任务可以不依赖纯字符串 shell 解析
- step evaluation 对失败类型有稳定判断
- 常见错误如 `permission denied`、`unit not found`、`port in use` 能被结构化识别

## Phase 4: Persistent Memory And Recovery

目标：让 agent 真正“记住”和“恢复”。

交付物：

- 任务持久化
- artifact 持久化
- 事实记忆层
- 断点续跑

建议存储内容：

- Task metadata
- Step logs
- SSH output snapshots
- Screenshot references
- Generated files
- Confirmations
- Learned facts

建议新增模块：

- `exoanchor/memory/run_memory.py`
- `exoanchor/memory/fact_store.py`
- `exoanchor/memory/artifact_store.py`

事实记忆示例：

- 目标机 OS 版本
- 已安装的软件
- 常用 workload 路径
- 已部署服务与端口
- 最近一次失败原因

验收标准：

- 服务重启后可恢复未完成任务状态
- agent 能利用历史事实避免重复探测
- 用户询问“刚才为什么失败”时，系统可直接回答

## Phase 5: Safety And Policy Layer

目标：把“安全”从提示词和前端确认，升级为后端强约束。

交付物：

- 服务端政策引擎
- 危险命令分类
- 环境级权限边界
- 审计日志

建议规则：

- 高风险命令必须显式确认：
  - `rm -rf`
  - `mkfs`
  - `reboot`
  - `shutdown`
  - 覆盖系统配置
- 某些目录禁止写入
- 某些命令只能在特定 mode 下执行
- 自动化任务与人工任务使用不同策略

需要重构：

- [guard.py](/Users/wxffxx/Documents/26s/SI_server/simpleipmi_restructure/exoanchor/safety/guard.py) 从轻量检查升级为 policy gate
- confirmation 不再只是 UI 行为，而是 runtime state

验收标准：

- 即使前端被绕过，危险动作仍无法直接执行
- 所有高风险步骤都有审计记录
- Passive / Semi-active 模式下有单独的权限策略

## Phase 6: Better Guided Agent Loop

目标：让视觉 + HID 这条链从 demo 变成可依赖能力。

交付物：

- 更强的 screen state schema
- 更稳定的 checkpoint 判断
- action precondition / postcondition
- stuck recovery 策略库

当前主要问题：

- guided mode 过于依赖单轮 vision 返回 `next_action`
- checkpoint 判断只是 description 文本包含
- loop detection 过于简单

建议升级：

- screen state 输出拆成：
  - `screen_type`
  - `ui_elements`
  - `focused_region`
  - `candidate_actions`
  - `confidence`
- 每个 guided step 必须带预期结果
- recovery 策略参数化，而不是只按一次 `Escape`

验收标准：

- 常见 BIOS / 登录 / 桌面场景能稳定识别
- guided task 的失败类型可解释
- 卡住时能做不止一种恢复动作

## 推荐实施顺序

### 版本 v0.1

先做：

1. 后端 `PlanExecutor`
2. `RunStore` 持久化任务和步骤
3. 前端改为只展示服务端事件

这是最关键的一步。没有它，后面的能力都会继续建在脆弱基础上。

### 版本 v0.2

接着做：

1. `skill_call -> executor`
2. 统一内部 plan IR
3. Python skill runtime

这一步做完，ExoAnchor 才会真正像“框架”，而不是“prompt + 页面脚本”。

### 版本 v0.3

继续做：

1. 结构化 tool layer
2. 结构化 observation
3. 失败分类与恢复策略

这一步做完，agent 的稳定性会比单纯调 prompt 高很多。

### 版本 v1.0

最后做：

1. 记忆与断点恢复
2. 服务端 policy gate
3. guided mode 强化
4. 被动监控与主动执行统一调度

## 第一批建议直接开工的文件

- `exoanchor/core/plan_executor.py`
- `exoanchor/core/run_store.py`
- `exoanchor/core/plan_ir.py`
- `exoanchor/api/routes.py`
- `exoanchor/dashboard/index.html`
- `exoanchor/skills/store.py`
- `exoanchor/core/executor.py`
- `exoanchor/safety/guard.py`

## 第一批里程碑定义

### Milestone A: 后端计划执行

- 前端不再直接驱动步骤执行
- 服务端可以运行一个完整 plan
- 任务可查询、暂停、恢复、中止

### Milestone B: 原生 skill 跑通

- `skill_call` 不再翻译成前端 plan
- YAML skill 和 Python skill 都可进入统一执行链

### Milestone C: 结构化观测

- shell/tool 输出不再只是原始文本
- step evaluator 用 observation 做判断

## 成功标准

如果以下 5 点成立，就说明 ExoAnchor 已经从“几乎不可用”进入“可用 agent”阶段：

1. 一个复杂任务可以在服务端独立跑完
2. 前端刷新不会中断任务
3. 失败后能准确知道哪一步、为什么失败
4. `plan` 和 `skill` 使用同一执行主链
5. 危险动作由后端统一拦截和审计
