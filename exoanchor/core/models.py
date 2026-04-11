"""
Core Data Models — Pydantic models for tasks, actions, and execution state.
"""

import json
import time
import uuid
from enum import Enum
from typing import Optional, Any
from pydantic import BaseModel, Field


# ── Agent Modes ─────────────────────────────────────────────────

class AgentMode(str, Enum):
    MANUAL = "manual"           # Human-triggered only, no background monitoring
    PASSIVE = "passive"         # Watchdog: monitor + auto-recover on anomaly
    SEMI_ACTIVE = "semi_active" # Passive + scheduled/conditional skill execution


# ── Task Models ─────────────────────────────────────────────────

class TaskState(str, Enum):
    PENDING = "pending"
    RUNNING = "running"
    PAUSED = "paused"
    COMPLETED = "completed"
    FAILED = "failed"
    ABORTED = "aborted"


class TaskRequest(BaseModel):
    """API request to create a task."""
    skill_name: str
    params: dict[str, Any] = Field(default_factory=dict)
    mode: str = "guided"  # "guided" | "scripted"


class TaskStatus(BaseModel):
    """Current status of a running/completed task."""
    task_id: str
    skill_name: str
    state: TaskState
    progress: float = 0.0       # 0.0 - 1.0
    current_step: int = 0
    total_steps: Optional[int] = None
    current_checkpoint: Optional[str] = None
    started_at: float
    elapsed: float = 0.0
    error: Optional[str] = None
    result: Optional[dict] = None


# ── Tool Observations ─────────────────────────────────────────

class ToolObservation(BaseModel):
    """Structured result returned by a tool execution."""

    tool_name: str
    success: bool = True
    exit_status: Optional[int] = None
    stdout: str = ""
    stderr: str = ""
    output: str = ""
    parsed: dict[str, Any] = Field(default_factory=dict)
    artifacts: dict[str, Any] = Field(default_factory=dict)
    channel: str = ""
    error: Optional[str] = None
    timestamp: float = Field(default_factory=time.time)


# ── Plan Runtime Models ────────────────────────────────────────

class RunState(str, Enum):
    PENDING = "pending"
    RUNNING = "running"
    WAITING_CONFIRMATION = "waiting_confirmation"
    PAUSED = "paused"
    COMPLETED = "completed"
    FAILED = "failed"
    ABORTED = "aborted"


class PlanStepState(str, Enum):
    PENDING = "pending"
    RUNNING = "running"
    WAITING_CONFIRMATION = "waiting_confirmation"
    DONE = "done"
    FAILED = "failed"
    SKIPPED = "skipped"


class PlanStep(BaseModel):
    """Normalized plan step definition."""
    id: str
    description: str
    tool: str = "shell.exec"
    args: dict[str, Any] = Field(default_factory=dict)
    command: str = ""
    dangerous: bool = False


class PlanStepStatus(PlanStep):
    """Execution status for a single plan step."""
    status: PlanStepState = PlanStepState.PENDING
    output: str = ""
    error: Optional[str] = None
    exit_status: Optional[int] = None
    observation: Optional[ToolObservation] = None
    started_at: Optional[float] = None
    finished_at: Optional[float] = None
    eval_action: Optional[str] = None
    eval_reason: Optional[str] = None


class PlanRunStatus(BaseModel):
    """Persistent status snapshot for a backend plan run."""
    run_id: str
    goal: str
    state: RunState = RunState.PENDING
    steps: list[PlanStepStatus] = Field(default_factory=list)
    current_step_index: int = 0
    waiting_step_id: Optional[str] = None
    created_at: float = Field(default_factory=time.time)
    started_at: Optional[float] = None
    updated_at: float = Field(default_factory=time.time)
    completed_at: Optional[float] = None
    error: Optional[str] = None
    supervised: bool = False
    react_mode: str = "on_fail"
    model: str = ""
    source: str = "llm"
    metadata: dict[str, Any] = Field(default_factory=dict)
    total_steps: int = 0
    completed_steps: int = 0


class Task:
    """Internal task representation."""

    def __init__(self, skill_name: str, params: dict = None, mode: str = "guided", metadata: dict = None):
        self.task_id = str(uuid.uuid4())[:8]
        self.skill_name = skill_name
        self.params = params or {}
        self.mode = mode
        self.metadata = metadata or {}
        self.state = TaskState.PENDING
        self.started_at = 0.0
        self.completed_at = 0.0
        self.error: Optional[str] = None
        self.result: Optional[dict] = None

    def to_status(self, progress: float = 0.0, step: int = 0, checkpoint: str = None) -> TaskStatus:
        elapsed = time.time() - self.started_at if self.started_at else 0
        return TaskStatus(
            task_id=self.task_id,
            skill_name=self.skill_name,
            state=self.state,
            progress=progress,
            current_step=step,
            current_checkpoint=checkpoint,
            started_at=self.started_at,
            elapsed=elapsed,
            error=self.error,
            result=self.result,
        )


# ── Screen State ────────────────────────────────────────────────

class UIElement(BaseModel):
    """Structured UI element detected on screen."""

    role: str = ""
    label: str = ""
    text: str = ""
    bounds: dict[str, Any] = Field(default_factory=dict)
    confidence: float = 0.0


class CandidateAction(BaseModel):
    """A machine-actionable proposal produced by the vision layer."""

    type: str = "wait"
    tool: Optional[str] = None
    args: dict[str, Any] = Field(default_factory=dict)
    key: Optional[str] = None
    keys: list[str] = Field(default_factory=list)
    text: Optional[str] = None
    x: Optional[int] = None
    y: Optional[int] = None
    button: Optional[str] = None
    modifiers: list[str] = Field(default_factory=list)
    interval: Optional[float] = None
    power_action: Optional[str] = None
    duration: Optional[float] = None
    reason: str = ""
    confidence: float = 0.0
    precondition_screen_types: list[str] = Field(default_factory=list)
    expected_screen_type: Optional[str] = None
    expected_checkpoint: Optional[str] = None
    expected_text: Optional[str] = None
    wait_for_change: bool = False
    wait_for_stable: bool = False

class ScreenState(BaseModel):
    """Structured representation of what's on the target machine's screen."""
    type: str = "unknown"         # "off", "bios", "os_desktop", "os_login", "error", "unknown"
    description: str = ""         # Human-readable description
    error: Optional[str] = None   # Error type if screen.type == "error"
    elements: list[str] = Field(default_factory=list)  # Detected UI elements
    ui_elements: list[UIElement] = Field(default_factory=list)
    focused_region: str = ""
    candidate_actions: list[CandidateAction] = Field(default_factory=list)
    confidence: float = 0.0
    checkpoint: Optional[str] = None
    progress_hint: Optional[float] = None
    safety_alert: Optional[str] = None
    raw_response: Optional[Any] = None  # Raw vision response (for debugging)

    @classmethod
    def from_api_response(cls, response: dict) -> "ScreenState":
        """Parse Vision API response into ScreenState."""
        elements = response.get("elements", []) or []
        ui_elements = response.get("ui_elements", []) or []
        candidate_actions = response.get("candidate_actions", []) or []
        if not candidate_actions and response.get("next_action"):
            candidate_actions = [response.get("next_action")]

        return cls(
            type=response.get("screen_type") or response.get("screen_state", "unknown"),
            description=response.get("observations", ""),
            error=response.get("error") or response.get("safety_alert"),
            elements=response.get("elements", []),
            ui_elements=[item if isinstance(item, UIElement) else UIElement(**item) for item in ui_elements if isinstance(item, dict)],
            focused_region=response.get("focused_region", ""),
            candidate_actions=[
                item if isinstance(item, CandidateAction) else CandidateAction(**item)
                for item in candidate_actions
                if isinstance(item, dict)
            ],
            confidence=float(response.get("confidence", 0.0) or 0.0),
            checkpoint=response.get("checkpoint"),
            progress_hint=response.get("progress"),
            safety_alert=response.get("safety_alert"),
            raw_response=response,
        )

    def signature(self) -> str:
        """Compact fingerprint used for stuck/loop detection."""
        ui_labels = [item.label or item.text for item in self.ui_elements[:6] if item.label or item.text]
        payload = {
            "type": self.type,
            "focused_region": self.focused_region,
            "elements": list(self.elements[:8]),
            "ui": ui_labels,
            "checkpoint": self.checkpoint,
            "description": self.description[:120],
        }
        return json.dumps(payload, ensure_ascii=False, sort_keys=True)


# ── Execution Step ──────────────────────────────────────────────

class StepRecord(BaseModel):
    """Record of a single step in execution history."""
    step_number: int
    timestamp: float
    screen_state: Optional[ScreenState] = None
    action_type: str = ""
    action_detail: str = ""
    action_result: str = ""
    channel_used: str = "hid"     # "hid" | "ssh"
    observation: Optional[ToolObservation] = None
    screenshot_path: Optional[str] = None  # Path to saved screenshot


# ── Service Monitor Models ──────────────────────────────────────

class ServiceType(str, Enum):
    SYSTEMD = "systemd"
    PROCESS = "process"
    DOCKER = "docker"


class ServiceConfig(BaseModel):
    """Configuration for a monitored service."""
    name: str
    type: ServiceType = ServiceType.SYSTEMD
    unit: Optional[str] = None          # systemd unit name
    process: Optional[str] = None       # process name for pgrep
    match: Optional[str] = None         # process arg match string
    container: Optional[str] = None     # docker container name
    check_port: Optional[int] = None    # TCP port to verify
    check_url: Optional[str] = None     # HTTP URL to verify
    on_down: str = "notify"             # "notify" | "restart" | "run_skill:<name>"
    max_restarts: int = 3
    cooldown: int = 60                  # Seconds between restart attempts
    on_max_restarts: str = "notify"     # What to do after max restarts exhausted

    # Runtime state (not from config)
    restart_count: int = 0
    last_restart: float = 0.0
    last_status: Optional[bool] = None


# ── Trigger Models ──────────────────────────────────────────────

class TriggerType(str, Enum):
    BLACK_SCREEN = "black_screen"
    FROZEN_SCREEN = "frozen_screen"
    BLUE_TINT = "blue_tint"
    SSH_CHECK = "ssh_check"
    SERVICE = "service"


class TriggerEvent(BaseModel):
    """An event fired when a trigger condition is met."""
    trigger_name: str
    trigger_type: TriggerType
    timestamp: float = Field(default_factory=time.time)
    description: str = ""
    action: str = ""
    resolved: bool = False


# ── Agent Status ────────────────────────────────────────────────

class AgentStatus(BaseModel):
    """Overall agent status for API responses."""
    mode: AgentMode
    passive_running: bool = False
    current_task: Optional[TaskStatus] = None
    ssh_connected: bool = False
    ssh_target: str = ""
    services_monitored: int = 0
    recent_triggers: list[TriggerEvent] = Field(default_factory=list)
    uptime: float = 0.0
