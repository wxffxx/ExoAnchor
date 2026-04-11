"""
Skill Recorder — Record human operations and convert to reusable Skills.

Hooks into HID operations to capture:
  - Screenshot before each action
  - The HID action itself
  - Timing between actions
  
After recording stops, optionally uses Vision LLM to generalize
raw mouse coordinates into semantic descriptions.
"""

import os
import time
import json
import logging
from typing import Optional

import yaml

logger = logging.getLogger("exoanchor.skills.recorder")


class SkillRecorder:
    """
    Records human HID operations and generates a Skill YAML file.
    
    Usage:
        recorder.start("my_workflow")
        # ... user operates via web KVM panel ...
        # HID events are captured via on_hid_action() hook
        skill = await recorder.stop()
    """

    def __init__(self, vision_adapter=None, save_dir: str = "/tmp/agent_recordings"):
        self.vision = vision_adapter
        self.save_dir = save_dir
        self.recording = False
        self.name: str = ""
        self.steps: list[dict] = []
        self._start_time: float = 0

    @property
    def is_recording(self) -> bool:
        return self.recording

    def start(self, name: str):
        """Start recording operations."""
        self.recording = True
        self.name = name
        self.steps = []
        self._start_time = time.time()
        os.makedirs(self.save_dir, exist_ok=True)
        logger.info(f"Recording started: '{name}'")

    async def on_hid_action(self, action: dict):
        """
        Hook called on every HID action.
        Should be integrated into the HID WebSocket handler.
        """
        if not self.recording:
            return

        step = {
            "timestamp": time.time(),
            "relative_time": time.time() - self._start_time,
            "action": action,
        }

        # Capture screenshot if vision adapter available
        if self.vision:
            try:
                jpeg = await self.vision.get_snapshot_jpeg(quality=70)
                screenshot_path = os.path.join(
                    self.save_dir, f"{self.name}_step{len(self.steps)}.jpg"
                )
                with open(screenshot_path, "wb") as f:
                    f.write(jpeg)
                step["screenshot"] = screenshot_path
            except Exception as e:
                logger.debug(f"Screenshot capture failed: {e}")

        self.steps.append(step)

    async def stop(self) -> dict:
        """
        Stop recording and generate a Skill.
        Returns the generated skill data as a dict.
        """
        self.recording = False
        duration = time.time() - self._start_time
        logger.info(f"Recording stopped: '{self.name}', {len(self.steps)} steps, {duration:.1f}s")

        if not self.steps:
            return {"skill": {"name": self.name, "steps": []}}

        # Generate skill YAML
        skill_data = self._generate_skill()

        # Save recording data for reference
        recording_path = os.path.join(self.save_dir, f"{self.name}_recording.json")
        with open(recording_path, "w") as f:
            # Remove screenshot binary data for JSON serialization
            serializable_steps = []
            for s in self.steps:
                step_copy = {**s}
                step_copy.pop("screenshot_frame", None)
                serializable_steps.append(step_copy)
            json.dump(serializable_steps, f, indent=2, ensure_ascii=False)

        return skill_data

    def _generate_skill(self) -> dict:
        """Convert recorded steps into a Skill YAML structure."""
        skill_steps = []

        for i, step in enumerate(self.steps):
            action = step["action"]
            wait = 0.5  # Default wait

            # Calculate wait from timing
            if i < len(self.steps) - 1:
                wait = round(self.steps[i + 1]["relative_time"] - step["relative_time"], 1)
                wait = max(0.2, min(wait, 10.0))  # Clamp to reasonable range

            skill_step = {
                "id": f"step_{i}",
                "action": self._normalize_action(action),
                "wait": wait,
            }

            skill_steps.append(skill_step)

        return {
            "skill": {
                "name": self.name,
                "description": f"Recorded workflow: {self.name}",
                "mode": "scripted",
                "tags": ["recorded"],
                "steps": skill_steps,
                "safety": {
                    "max_steps": len(skill_steps) * 2,  # Allow some retry room
                    "max_duration": int(self.steps[-1]["relative_time"] * 3),
                },
            }
        }

    def _normalize_action(self, action: dict) -> dict:
        """Normalize a raw HID action into a Skill action format."""
        action_type = action.get("type", "")

        if action_type == "keyboard":
            key = action.get("key", "")
            modifiers = action.get("modifiers", [])
            if modifiers:
                return {"type": "key_press", "key": key, "modifiers": modifiers}
            return {"type": "key_press", "key": key}

        elif action_type == "mouse_move":
            return {
                "type": "mouse_move",
                "x": action.get("x", 0),
                "y": action.get("y", 0),
            }

        elif action_type == "mouse_click" or action_type == "click":
            return {
                "type": "mouse_click",
                "button": action.get("button", "left"),
                "x": action.get("x"),
                "y": action.get("y"),
            }

        elif action_type == "type":
            return {"type": "type_text", "text": action.get("text", "")}

        # Fallback: pass through
        return action

    def get_status(self) -> dict:
        """Get recorder status."""
        return {
            "recording": self.recording,
            "name": self.name,
            "steps_recorded": len(self.steps),
            "duration": time.time() - self._start_time if self.recording else 0,
        }
