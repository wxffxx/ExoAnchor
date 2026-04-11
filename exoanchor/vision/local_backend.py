"""
Local Vision Backend — Zero-cost pixel-level screen detection.

No API calls needed. Handles:
  - Black screen detection
  - Frozen screen detection  
  - Blue screen / kernel panic detection
  - Simple brightness/color analysis
"""

import logging
import numpy as np

from .base import VisionBackend
from ..core.models import ScreenState

logger = logging.getLogger("exoanchor.vision.local")


class LocalVisionBackend(VisionBackend):
    """
    Local screen analysis using simple pixel operations.
    
    Used by passive mode for zero-cost monitoring, and as
    fallback when Vision API is unavailable.
    """

    async def analyze(self, frame: np.ndarray, context=None,
                      goal: str = "", checkpoints: list = None) -> ScreenState:
        """Analyze frame using local pixel checks."""
        
        if self._is_black(frame):
            return ScreenState(type="off", description="Screen is black/off", confidence=0.98)

        if self._is_blue_screen(frame):
            return ScreenState(
                type="error",
                description="Blue screen detected (possible kernel panic or BSOD)",
                error="blue_screen",
                confidence=0.9,
            )

        if self._is_mostly_white(frame):
            return ScreenState(type="bright", description="Screen is mostly white/bright", confidence=0.75)

        # Can't determine more without LLM
        brightness = float(np.mean(frame))
        return ScreenState(
            type="unknown",
            description=f"Screen active (mean brightness: {brightness:.0f}/255)",
            confidence=0.35,
        )

    def _is_black(self, frame: np.ndarray, threshold: float = 10) -> bool:
        """Detect black/off screen."""
        return float(np.mean(frame)) < threshold

    def _is_blue_screen(self, frame: np.ndarray) -> bool:
        """
        Detect blue screen (Linux kernel panic or Windows BSOD).
        Blue screens have high blue channel relative to red/green.
        """
        if len(frame.shape) != 3 or frame.shape[2] < 3:
            return False
        
        # BGR format
        b, g, r = frame[:, :, 0], frame[:, :, 1], frame[:, :, 2]
        mean_b = float(np.mean(b))
        mean_g = float(np.mean(g))
        mean_r = float(np.mean(r))
        
        # Blue dominant: blue > 100, blue > 2x red, blue > 1.5x green
        if mean_b > 100 and mean_b > 2 * mean_r and mean_b > 1.5 * mean_g:
            return True
        
        return False

    def _is_mostly_white(self, frame: np.ndarray, threshold: float = 220) -> bool:
        """Detect mostly white screen."""
        return float(np.mean(frame)) > threshold

    def get_brightness(self, frame: np.ndarray) -> float:
        """Get average brightness (0-255)."""
        return float(np.mean(frame))

    def get_pixel_diff(self, frame1: np.ndarray, frame2: np.ndarray) -> float:
        """Get normalized pixel difference between two frames (0.0-1.0)."""
        if frame1.shape != frame2.shape:
            return 1.0
        return float(np.mean(np.abs(
            frame1.astype(np.float32) - frame2.astype(np.float32)
        )) / 255.0)
