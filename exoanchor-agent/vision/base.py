"""
Vision Backend — Screen capture and analysis.

Base class + two implementations:
  - LocalBackend: pixel-level checks (black, frozen, blue screen). Zero API cost.
  - APIBackend: Vision LLM (GPT-4o/Claude) for semantic screen understanding.
"""

from abc import ABC, abstractmethod
from typing import Optional
import asyncio
import time
import logging

import numpy as np

logger = logging.getLogger("exoanchor.vision")


class VisionBackend(ABC):
    """Abstract base for screen analysis backends."""

    def __init__(self, video_adapter):
        self.video = video_adapter

    async def capture(self) -> np.ndarray:
        """Capture a single frame from the video adapter."""
        return await self.video.get_frame()

    async def capture_jpeg(self, quality: int = 85) -> bytes:
        """Capture a single frame as JPEG bytes."""
        return await self.video.get_snapshot_jpeg(quality)

    @abstractmethod
    async def analyze(self, frame: np.ndarray, context=None,
                      goal: str = "", checkpoints: list = None):
        """Analyze a frame and return structured screen state."""
        ...

    async def wait_stable(self, timeout: float = 3.0, threshold: float = 0.02) -> bool:
        """Wait until screen stops changing (useful after navigation)."""
        prev = await self.capture()
        deadline = time.time() + timeout
        while time.time() < deadline:
            await asyncio.sleep(0.3)
            curr = await self.capture()
            if prev.shape == curr.shape:
                diff = float(np.mean(np.abs(
                    curr.astype(np.float32) - prev.astype(np.float32)
                )) / 255.0)
                if diff < threshold:
                    return True
            prev = curr
        return False

    async def wait_for_change(self, timeout: float = 60.0, threshold: float = 0.05) -> bool:
        """Wait until screen changes (useful after power cycle)."""
        prev = await self.capture()
        deadline = time.time() + timeout
        while time.time() < deadline:
            await asyncio.sleep(1.0)
            curr = await self.capture()
            if prev.shape == curr.shape:
                diff = float(np.mean(np.abs(
                    curr.astype(np.float32) - prev.astype(np.float32)
                )) / 255.0)
                if diff > threshold:
                    return True
            prev = curr
        return False
