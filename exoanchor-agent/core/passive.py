"""
Passive Monitor — Watchdog mode for continuous monitoring.

Monitors:
  - Screen state (black screen, frozen screen, kernel panic)
  - Service health via SSH (systemd, process, docker, ports, HTTP)
  
Triggers recovery actions when anomalies are detected.
"""

import asyncio
import time
import logging
from typing import Optional

import numpy as np

from .models import (
    ServiceConfig,
    ServiceType,
    TriggerEvent,
    TriggerType,
)

logger = logging.getLogger("exoanchor.passive")


class PassiveMonitor:
    """
    Passive watchdog mode.

    Continuously monitors the target machine for anomalies:
      - Visual: black screen, frozen screen, error screens
      - Services: systemd units, processes, docker containers, ports, HTTP endpoints

    When an anomaly is detected, the configured recovery action is triggered.
    """

    def __init__(self, vision_adapter, ssh_manager, action_driver, config: dict):
        self.vision = vision_adapter
        self.ssh = ssh_manager
        self.action = action_driver
        self.config = config

        # Polling
        self.poll_interval: float = config.get("poll_interval", 5)

        # Visual triggers
        self.visual_config = config.get("local_triggers", {})

        # Service monitoring
        self.services: list[ServiceConfig] = []
        self._load_services(config.get("services", {}))

        # SSH triggers (custom commands)
        self.ssh_triggers = config.get("ssh_triggers", {})

        # State
        self.running = False
        self._task: Optional[asyncio.Task] = None
        self.prev_frame: Optional[np.ndarray] = None
        self.prev_frame_time: float = 0
        self.frozen_since: float = 0
        self.black_since: float = 0
        self.trigger_history: list[TriggerEvent] = []
        self._skill_runner = None  # Set via set_skill_runner()

    def set_skill_runner(self, runner):
        """Inject skill runner for executing recovery skills."""
        self._skill_runner = runner

    def _load_services(self, services_config: dict):
        """Load service monitor configs from YAML."""
        for name, cfg in services_config.items():
            svc = ServiceConfig(name=name, **cfg)
            self.services.append(svc)
        if self.services:
            logger.info(f"Loaded {len(self.services)} service monitors: "
                        f"{[s.name for s in self.services]}")

    # ── Lifecycle ───────────────────────────────────────────────

    async def start(self):
        """Start the monitoring loop."""
        if self.running:
            logger.warning("Passive monitor already running")
            return
        self.running = True
        self._task = asyncio.create_task(self._monitor_loop())
        logger.info(f"Passive monitor started (poll every {self.poll_interval}s)")

    async def stop(self):
        """Stop the monitoring loop."""
        self.running = False
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None
        logger.info("Passive monitor stopped")

    # ── Main Loop ───────────────────────────────────────────────

    async def _monitor_loop(self):
        """Core monitoring loop."""
        while self.running:
            try:
                await self._check_cycle()
            except Exception as e:
                logger.error(f"Monitor cycle error: {e}")
            await asyncio.sleep(self.poll_interval)

    async def _check_cycle(self):
        """Single monitoring cycle."""
        now = time.time()

        # ── Visual checks (local, zero-cost) ──
        if self.vision.is_available():
            try:
                frame = await self.vision.get_frame()
                await self._check_visual(frame, now)
                self.prev_frame = frame
                self.prev_frame_time = now
            except Exception as e:
                logger.debug(f"Visual check skipped: {e}")

        # ── Service checks (via SSH) ──
        if self.ssh and self.ssh.has_shell and self.services:
            await self._check_services()

        # ── Custom SSH triggers ──
        if self.ssh and self.ssh.has_shell and self.ssh_triggers:
            await self._check_ssh_triggers()

    # ── Visual Checks ──────────────────────────────────────────

    async def _check_visual(self, frame: np.ndarray, now: float):
        """Check screen for visual anomalies."""

        # Black screen detection
        black_cfg = self.visual_config.get("black_screen", {})
        if black_cfg.get("enabled", True):
            threshold = black_cfg.get("threshold", 10)
            timeout = black_cfg.get("timeout", 60)
            mean_brightness = float(np.mean(frame))

            if mean_brightness < threshold:
                if self.black_since == 0:
                    self.black_since = now
                elif now - self.black_since > timeout:
                    await self._fire_trigger(
                        "black_screen", TriggerType.BLACK_SCREEN,
                        f"Screen black for {timeout}s (brightness={mean_brightness:.1f})",
                        black_cfg.get("action", "power_cycle")
                    )
                    self.black_since = 0  # Reset after action
            else:
                self.black_since = 0

        # Frozen screen detection
        frozen_cfg = self.visual_config.get("frozen_screen", {})
        if frozen_cfg.get("enabled", True) and self.prev_frame is not None:
            threshold = frozen_cfg.get("threshold", 0.001)
            timeout = frozen_cfg.get("timeout", 120)

            if frame.shape == self.prev_frame.shape:
                diff = float(np.mean(np.abs(
                    frame.astype(np.float32) - self.prev_frame.astype(np.float32)
                )) / 255.0)

                if diff < threshold:
                    if self.frozen_since == 0:
                        self.frozen_since = now
                    elif now - self.frozen_since > timeout:
                        await self._fire_trigger(
                            "frozen_screen", TriggerType.FROZEN_SCREEN,
                            f"Screen frozen for {timeout}s (diff={diff:.6f})",
                            frozen_cfg.get("action", "power_cycle")
                        )
                        self.frozen_since = 0
                else:
                    self.frozen_since = 0

    # ── Service Checks ──────────────────────────────────────────

    async def _check_services(self):
        """Check all monitored services."""
        for svc in self.services:
            alive = await self._check_single_service(svc)

            if alive:
                if svc.last_status is False:
                    logger.info(f"Service '{svc.name}' recovered")
                svc.last_status = True
                continue

            # Service is down
            if svc.last_status is not False:
                logger.warning(f"Service '{svc.name}' is DOWN")
            svc.last_status = False

            await self._handle_service_down(svc)

    async def _check_single_service(self, svc: ServiceConfig) -> bool:
        """Check if a single service is alive. Returns True if healthy."""
        try:
            # Base check
            if svc.type == ServiceType.SYSTEMD:
                ok, output = await self.ssh.run_check(
                    f"systemctl is-active {svc.unit or svc.name}"
                )
                if not ok or "active" not in output:
                    return False

            elif svc.type == ServiceType.PROCESS:
                match = svc.match or svc.process or svc.name
                ok, _ = await self.ssh.run_check(f"pgrep -f '{match}'")
                if not ok:
                    return False

            elif svc.type == ServiceType.DOCKER:
                container = svc.container or svc.name
                ok, output = await self.ssh.run_check(
                    f"docker inspect -f '{{{{.State.Running}}}}' {container}"
                )
                if not ok or "true" not in output.lower():
                    return False

            # Port check (optional)
            if svc.check_port:
                ok, _ = await self.ssh.run_check(
                    f"ss -tlnp | grep -q ':{svc.check_port} '"
                )
                if not ok:
                    return False

            # HTTP check (optional)
            if svc.check_url:
                ok, output = await self.ssh.run_check(
                    f"curl -sf -o /dev/null -w '%{{http_code}}' --max-time 5 {svc.check_url}"
                )
                if not ok or output.strip() not in ("200", "201", "204", "301", "302"):
                    return False

            return True

        except Exception as e:
            logger.debug(f"Service check error for '{svc.name}': {e}")
            return False

    async def _handle_service_down(self, svc: ServiceConfig):
        """Handle a service being down."""
        now = time.time()

        # Cooldown check
        if now - svc.last_restart < svc.cooldown:
            return

        # Max restarts check
        if svc.restart_count >= svc.max_restarts:
            await self._fire_trigger(
                f"service_{svc.name}_max_restarts",
                TriggerType.SERVICE,
                f"Service '{svc.name}' exceeded max restarts ({svc.max_restarts})",
                svc.on_max_restarts,
            )
            svc.restart_count = 0  # Reset counter
            return

        # Execute recovery action
        svc.restart_count += 1
        svc.last_restart = now

        await self._fire_trigger(
            f"service_{svc.name}_down",
            TriggerType.SERVICE,
            f"Service '{svc.name}' is down (attempt {svc.restart_count}/{svc.max_restarts})",
            svc.on_down,
            params={"service": svc.unit or svc.container or svc.name},
        )

    # ── Custom SSH Triggers ────────────────────────────────────

    async def _check_ssh_triggers(self):
        """Check custom SSH-based triggers."""
        for name, cfg in self.ssh_triggers.items():
            try:
                command = cfg.get("command", "")
                condition = cfg.get("condition", "")
                action = cfg.get("action", "notify")

                ok, output = await self.ssh.run_check(command, timeout=10)
                if not ok:
                    continue

                # Evaluate condition
                if condition and self._eval_condition(condition, output):
                    await self._fire_trigger(
                        name, TriggerType.SSH_CHECK,
                        f"SSH trigger '{name}': {output.strip()}",
                        action,
                        params=cfg.get("params", {}),
                    )
            except Exception as e:
                logger.debug(f"SSH trigger '{name}' error: {e}")

    def _eval_condition(self, condition: str, output: str) -> bool:
        """Safely evaluate a trigger condition string."""
        try:
            return bool(eval(condition, {"__builtins__": {}}, {
                "output": output.strip(),
                "int": int,
                "float": float,
                "len": len,
            }))
        except Exception:
            return False

    # ── Trigger Handler ─────────────────────────────────────────

    async def _fire_trigger(
        self,
        name: str,
        trigger_type: TriggerType,
        description: str,
        action: str,
        params: dict = None,
    ):
        """Fire a trigger and execute its recovery action."""
        event = TriggerEvent(
            trigger_name=name,
            trigger_type=trigger_type,
            description=description,
            action=action,
        )
        self.trigger_history.append(event)
        # Keep last 100 events
        if len(self.trigger_history) > 100:
            self.trigger_history = self.trigger_history[-100:]

        logger.warning(f"TRIGGER [{name}]: {description} → action: {action}")

        try:
            await self._execute_action(action, params or {})
            event.resolved = True
        except Exception as e:
            logger.error(f"Trigger action failed: {e}")

    async def _execute_action(self, action: str, params: dict):
        """Execute a trigger's recovery action."""
        from .models import Task
        from ..action.driver import Action

        if action == "notify":
            # Just log — notification system can be added later
            logger.info("Trigger action: notify (logged)")

        elif action == "power_cycle":
            logger.warning("Trigger action: power_cycle")
            await self.action.execute(Action(type="power", power_action="reset"))

        elif action == "restart":
            service = params.get("service", "")
            if service and self.ssh.has_shell:
                await self.ssh.run(f"sudo systemctl restart {service}", timeout=30)
                logger.info(f"Trigger action: restarted service '{service}'")

        elif action.startswith("shell:"):
            command = action[6:]
            if self.ssh.has_shell:
                await self.ssh.run(command, timeout=30)
                logger.info(f"Trigger action: shell '{command}'")

        elif action.startswith("run_skill:"):
            skill_name = action[10:]
            if self._skill_runner:
                await self._skill_runner(skill_name, params)
                logger.info(f"Trigger action: ran skill '{skill_name}'")
            else:
                logger.warning(f"Cannot run skill '{skill_name}': no skill runner set")

        elif action == "log_and_reboot":
            logger.warning("Trigger action: log_and_reboot")
            # Save screenshot for debugging
            try:
                jpeg = await self.vision.get_snapshot_jpeg()
                ts = int(time.time())
                path = f"/tmp/agent_trigger_{ts}.jpg"
                with open(path, "wb") as f:
                    f.write(jpeg)
                logger.info(f"Screenshot saved: {path}")
            except Exception:
                pass
            await self.action.execute(Action(type="power", power_action="reset"))

    # ── Status ──────────────────────────────────────────────────

    def get_status(self) -> dict:
        """Get passive monitor status."""
        return {
            "running": self.running,
            "poll_interval": self.poll_interval,
            "services": [
                {
                    "name": s.name,
                    "type": s.type.value,
                    "status": "up" if s.last_status else ("down" if s.last_status is False else "unknown"),
                    "restart_count": s.restart_count,
                }
                for s in self.services
            ],
            "recent_triggers": [
                {
                    "name": t.trigger_name,
                    "type": t.trigger_type.value,
                    "description": t.description,
                    "timestamp": t.timestamp,
                    "resolved": t.resolved,
                }
                for t in self.trigger_history[-10:]
            ],
        }
