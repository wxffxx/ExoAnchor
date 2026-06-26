from .models import AgentMode, AgentStatus, Task, TaskState, TaskStatus
from .context import ExecutionContext

__all__ = [
    "AgentMode",
    "AgentStatus",
    "Task",
    "TaskState",
    "TaskStatus",
    "ExecutionContext",
    "PassiveMonitor",
    "SemiActiveExecutor",
]


def __getattr__(name):
    if name == "PassiveMonitor":
        from .passive import PassiveMonitor
        return PassiveMonitor
    if name == "SemiActiveExecutor":
        from .executor import SemiActiveExecutor
        return SemiActiveExecutor
    raise AttributeError(name)
