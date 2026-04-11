"""
Adapter Interfaces — Host-agnostic abstractions for HID, Video, GPIO.

Each KVM Host implements these interfaces so the Agent framework
can operate without knowing the underlying hardware.
"""

from abc import ABC, abstractmethod
from typing import Optional
import numpy as np
import logging

logger = logging.getLogger("exoanchor.adapters")


class HIDAdapterInterface(ABC):
    """HID adapter — each Host implements this to provide keyboard/mouse control."""

    @abstractmethod
    async def send_key(self, key: str, modifiers: Optional[list[str]] = None) -> None:
        """Send a single key press (press + release)."""
        ...

    @abstractmethod
    async def type_string(self, text: str, interval: float = 0.05) -> None:
        """Type a string character by character."""
        ...

    @abstractmethod
    async def move_mouse(self, x: int, y: int, absolute: bool = False) -> None:
        """Move mouse by (x, y) relative, or to (x, y) absolute."""
        ...

    @abstractmethod
    async def click(self, button: str = "left", x: Optional[int] = None, y: Optional[int] = None) -> None:
        """Click mouse button. Optionally move to (x, y) first."""
        ...

    @abstractmethod
    async def release_all(self) -> None:
        """Release all pressed keys and mouse buttons."""
        ...


class VideoAdapterInterface(ABC):
    """Video adapter — each Host implements this to provide screen capture."""

    @abstractmethod
    async def get_frame(self) -> np.ndarray:
        """Capture a single frame as a numpy array (BGR, HWC)."""
        ...

    @abstractmethod
    async def get_snapshot_jpeg(self, quality: int = 85) -> bytes:
        """Capture a single frame as JPEG bytes."""
        ...

    def is_available(self) -> bool:
        """Whether video capture is available."""
        return True


class GPIOAdapterInterface(ABC):
    """GPIO adapter — each Host implements this for power control."""

    @abstractmethod
    async def power_action(self, action: str) -> dict:
        """Execute power action: 'on', 'off', 'reset'."""
        ...

    @abstractmethod
    async def get_power_status(self) -> dict:
        """Get current power status of the target machine."""
        ...


# ═══════════════════════════════════════════════════════════════
# Mock Adapters — for development/testing on Mac or any non-KVM host
# ═══════════════════════════════════════════════════════════════

class MockHIDAdapter(HIDAdapterInterface):
    """Mock HID — logs actions instead of sending USB HID commands."""

    async def send_key(self, key: str, modifiers: Optional[list[str]] = None) -> None:
        mod_str = f"+{'+'.join(modifiers)}" if modifiers else ""
        logger.info(f"[MOCK HID] key_press: {key}{mod_str}")

    async def type_string(self, text: str, interval: float = 0.05) -> None:
        logger.info(f"[MOCK HID] type_string: '{text}'")

    async def move_mouse(self, x: int, y: int, absolute: bool = False) -> None:
        mode = "abs" if absolute else "rel"
        logger.info(f"[MOCK HID] mouse_move: ({x}, {y}) {mode}")

    async def click(self, button: str = "left", x: Optional[int] = None, y: Optional[int] = None) -> None:
        pos = f" at ({x}, {y})" if x is not None else ""
        logger.info(f"[MOCK HID] click: {button}{pos}")

    async def release_all(self) -> None:
        logger.info("[MOCK HID] release_all")


class MockVideoAdapter(VideoAdapterInterface):
    """Mock Video — returns black frames or loads test images."""

    def __init__(self, width: int = 1280, height: int = 720, test_image_path: str = None):
        self.width = width
        self.height = height
        self.test_image_path = test_image_path
        self._test_frame = None

    async def get_frame(self) -> np.ndarray:
        if self.test_image_path and self._test_frame is None:
            try:
                import cv2
                self._test_frame = cv2.imread(self.test_image_path)
            except ImportError:
                from PIL import Image
                img = Image.open(self.test_image_path)
                self._test_frame = np.array(img)[:, :, ::-1]  # RGB→BGR

        if self._test_frame is not None:
            return self._test_frame.copy()

        return np.zeros((self.height, self.width, 3), dtype=np.uint8)

    async def get_snapshot_jpeg(self, quality: int = 85) -> bytes:
        frame = await self.get_frame()
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

    def is_available(self) -> bool:
        return True

    def set_test_image(self, path: str):
        """Load a test image for mock capture."""
        self.test_image_path = path
        self._test_frame = None


class MockGPIOAdapter(GPIOAdapterInterface):
    """Mock GPIO — logs power actions."""

    def __init__(self):
        self._power_state = "unknown"

    async def power_action(self, action: str) -> dict:
        logger.info(f"[MOCK GPIO] power_{action}")
        if action == "on":
            self._power_state = "on"
        elif action == "off":
            self._power_state = "off"
        elif action == "reset":
            self._power_state = "on"
        return {"ok": True, "action": action}

    async def get_power_status(self) -> dict:
        return {"power": self._power_state}
