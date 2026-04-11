"""
API Vision Backend — Uses Vision LLM (GPT-4o / Claude) for semantic screen understanding.

Sends screenshots to the Vision API and gets structured analysis:
  - What's on screen (BIOS menu, OS desktop, error, etc.)
  - What action to take next
  - Progress assessment
"""

import base64
import json
import logging
from typing import Optional

import numpy as np

from .base import VisionBackend
from ..core.models import ScreenState

logger = logging.getLogger("exoanchor.vision.api")

VISION_SYSTEM_PROMPT = """You are a KVM automation agent analyzing screenshots of a target machine.
Your job is to understand what's on screen and recommend the next action.

Always respond in JSON format:
{
  "screen_type": "off | bios_main | bios_submenu | os_login | os_desktop | terminal | installer | error | unknown",
  "observations": "Brief description of what you see on screen",
  "elements": ["list", "of", "visible", "UI", "elements"],
  "ui_elements": [
    {"role": "button | field | menu | label", "label": "text on screen", "text": "raw OCR text", "confidence": 0.0-1.0}
  ],
  "focused_region": "What area currently matters most",
  "candidate_actions": [
    {
      "type": "key_press | key_sequence | type_text | mouse_click | wait | done | error",
      "tool": "optional structured tool name such as hid.key_press or shell.exec",
      "args": {"optional": "tool args"},
      "key": "Enter",
      "keys": ["Down", "Down", "Enter"],
      "text": "text to type",
      "x": 640,
      "y": 360,
      "duration": 2.0,
      "reason": "Why this action",
      "confidence": 0.0-1.0,
      "precondition_screen_types": ["optional", "screen", "types"],
      "expected_screen_type": "optional screen type after action",
      "expected_checkpoint": "optional checkpoint after action",
      "expected_text": "optional text that should appear after action",
      "wait_for_change": true,
      "wait_for_stable": true
    }
  ],
  "next_action": {
    "type": "Legacy single-action fallback. Mirror the first candidate action here."
  },
  "confidence": 0.0-1.0,
  "progress": 0.0-1.0,
  "checkpoint": "name of checkpoint reached, if any",
  "safety_alert": "any concerning observations (high temp warning, error messages, etc.)"
}"""


class APIVisionBackend(VisionBackend):
    """
    Vision LLM API backend for semantic screen understanding.
    
    Supports OpenAI (GPT-4o) and Anthropic (Claude) APIs.
    """

    def __init__(self, video_adapter, config: dict):
        super().__init__(video_adapter)
        self.provider = config.get("api_provider", "openai")
        self.api_key = config.get("api_key", "")
        self.model = config.get("model", "gpt-4o")
        self.max_tokens = config.get("max_tokens", 1000)
        self.base_url = config.get("base_url")  # Custom endpoint support
        self._client = None

    async def _get_client(self):
        """Lazy-init HTTP client."""
        if self._client is None:
            import httpx
            self._client = httpx.AsyncClient(timeout=30.0)
        return self._client

    async def analyze(self, frame: np.ndarray, context=None,
                      goal: str = "", checkpoints: list = None) -> ScreenState:
        """Send frame to Vision API for analysis."""
        
        if not self.api_key:
            logger.warning("Vision API key not configured, falling back to local")
            from .local_backend import LocalVisionBackend
            local = LocalVisionBackend(self.video)
            return await local.analyze(frame, context, goal, checkpoints)

        # Encode frame as JPEG
        jpeg_bytes = self._encode_jpeg(frame)
        b64_image = base64.b64encode(jpeg_bytes).decode()

        # Build prompt
        user_prompt = self._build_prompt(context, goal, checkpoints)

        # Call API
        try:
            if self.provider == "openai":
                response = await self._call_openai(b64_image, user_prompt)
            elif self.provider == "anthropic":
                response = await self._call_anthropic(b64_image, user_prompt)
            else:
                raise ValueError(f"Unknown API provider: {self.provider}")

            return self._parse_response(response)

        except Exception as e:
            logger.error(f"Vision API call failed: {e}")
            return ScreenState(
                type="unknown",
                description=f"Vision API error: {e}",
                raw_response=str(e),
            )

    def _encode_jpeg(self, frame: np.ndarray, quality: int = 85) -> bytes:
        """Encode numpy frame to JPEG bytes."""
        try:
            import cv2
            _, buf = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, quality])
            return buf.tobytes()
        except ImportError:
            from PIL import Image
            import io
            img = Image.fromarray(frame[:, :, ::-1])  # BGR→RGB
            buf = io.BytesIO()
            img.save(buf, format='JPEG', quality=quality)
            return buf.getvalue()

    def _build_prompt(self, context, goal: str, checkpoints: list) -> str:
        """Build the user prompt with context."""
        parts = []
        
        if goal:
            parts.append(f"Current task goal: {goal}")
        
        if context:
            parts.append(f"Step {context.current_step}, elapsed {context.elapsed:.0f}s")
            if context.checkpoints_reached:
                parts.append(f"Checkpoints reached: {', '.join(context.checkpoints_reached)}")
            recent = context.get_recent_history(5)
            if recent:
                parts.append(f"Recent actions: {json.dumps(recent, ensure_ascii=False)}")
        
        if checkpoints:
            remaining = []
            reached = context.checkpoints_reached if context else set()
            for cp in checkpoints:
                name = cp.get("name", "")
                if name not in reached:
                    hint = cp.get("visual_hint", cp.get("description", ""))
                    remaining.append(f"  - {name}: {hint}")
            if remaining:
                parts.append(f"Remaining checkpoints:\n" + "\n".join(remaining))
        
        parts.append("Analyze the screenshot and respond with JSON as specified.")
        return "\n\n".join(parts)

    async def _call_openai(self, b64_image: str, prompt: str) -> dict:
        """Call OpenAI Vision API."""
        client = await self._get_client()
        url = (self.base_url or "https://api.openai.com") + "/v1/chat/completions"
        
        response = await client.post(
            url,
            headers={
                "Authorization": f"Bearer {self.api_key}",
                "Content-Type": "application/json",
            },
            json={
                "model": self.model,
                "max_tokens": self.max_tokens,
                "messages": [
                    {"role": "system", "content": VISION_SYSTEM_PROMPT},
                    {"role": "user", "content": [
                        {"type": "text", "text": prompt},
                        {"type": "image_url", "image_url": {
                            "url": f"data:image/jpeg;base64,{b64_image}",
                            "detail": "low",
                        }},
                    ]},
                ],
                "response_format": {"type": "json_object"},
            },
        )
        response.raise_for_status()
        data = response.json()
        content = data["choices"][0]["message"]["content"]
        return json.loads(content)

    async def _call_anthropic(self, b64_image: str, prompt: str) -> dict:
        """Call Anthropic Vision API."""
        client = await self._get_client()
        url = (self.base_url or "https://api.anthropic.com") + "/v1/messages"
        
        response = await client.post(
            url,
            headers={
                "x-api-key": self.api_key,
                "anthropic-version": "2023-06-01",
                "Content-Type": "application/json",
            },
            json={
                "model": self.model,
                "max_tokens": self.max_tokens,
                "system": VISION_SYSTEM_PROMPT,
                "messages": [{
                    "role": "user",
                    "content": [
                        {"type": "image", "source": {
                            "type": "base64",
                            "media_type": "image/jpeg",
                            "data": b64_image,
                        }},
                        {"type": "text", "text": prompt},
                    ],
                }],
            },
        )
        response.raise_for_status()
        data = response.json()
        content = data["content"][0]["text"]
        return json.loads(content)

    def _parse_response(self, response: dict) -> ScreenState:
        """Parse API response into ScreenState."""
        return ScreenState.from_api_response(response)

    async def close(self):
        """Close HTTP client."""
        if self._client:
            await self._client.aclose()
            self._client = None
