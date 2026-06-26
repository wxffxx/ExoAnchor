from exoanchor.skills import SkillBase, param
from exoanchor.core.plan_ir import ExecutablePlan, ExecutableStep


class PythonSystemSnapshot(SkillBase):
    name = "python_system_snapshot"
    description = "Use the Python skill runtime to collect a quick system snapshot"
    tags = ["builtin", "python", "system", "diagnostics"]
    params = {
        "include_disk": param(bool, default=True, description="Include disk usage in the snapshot"),
        "include_memory": param(bool, default=True, description="Include memory usage in the snapshot"),
    }

    async def execute(self, ctx):
        steps = [
            ExecutableStep(
                id="kernel",
                description="查看系统内核信息",
                tool="shell.exec",
                args={"command": "uname -a"},
            )
        ]

        if ctx.task.params.get("include_disk", True):
            steps.append(ExecutableStep(
                id="disk",
                description="查看磁盘使用情况",
                tool="shell.exec",
                args={"command": "df -h | head -10"},
            ))

        if ctx.task.params.get("include_memory", True):
            steps.append(ExecutableStep(
                id="memory",
                description="查看内存使用情况",
                tool="shell.exec",
                args={"command": "free -h"},
            ))

        return ExecutablePlan(
            goal="采集系统快照",
            steps=steps,
            source=f"skill:{self.name}",
            metadata={"generated_by": "python_skill"},
        )
