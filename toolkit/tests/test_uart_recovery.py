import unittest

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.ports import PortInfo
from exoanchor_toolkit.uart_recovery import wait_for_uart_runtime


def port(device: str, serial_number: str) -> PortInfo:
    return PortInfo(
        device=device,
        description="ExoAnchor",
        hwid="test",
        vid=0x1A86,
        pid=0x55D3,
        serial_number=serial_number,
        manufacturer="QinHeng",
        likely_exoanchor=True,
    )


class StepClock:
    def __init__(self) -> None:
        self.value = 0.0

    def monotonic(self) -> float:
        return self.value

    def sleep(self, seconds: float) -> None:
        self.value += seconds


class UartRuntimeRecoveryTests(unittest.TestCase):
    def test_reenumeration_ignores_other_board_and_verifies_runtime(self):
        original = port("/dev/cu.old", "board-a")
        other = port("/dev/cu.other", "board-b")
        returned = port("/dev/cu.new", "board-a")
        calls = 0

        def ports_provider():
            nonlocal calls
            calls += 1
            return [other] if calls == 1 else [other, returned]

        def identify(console):
            self.assertEqual(console.port, "/dev/cu.new")
            return {
                "device_id": "ea-p4-test",
                "firmware": "0.87.3-dev",
                "board": "exoanchor-prototype-v2.3",
            }

        clock = StepClock()
        result = wait_for_uart_runtime(
            original,
            expected_version="0.87.3-dev",
            expected_board="exoanchor-prototype-v2.3",
            ports_provider=ports_provider,
            identify_device=identify,
            monotonic=clock.monotonic,
            sleep=clock.sleep,
        )
        self.assertTrue(result["verified"])
        self.assertEqual(result["port"], "/dev/cu.new")
        self.assertEqual(result["device_id"], "ea-p4-test")

    def test_wrong_runtime_version_is_rejected(self):
        original = port("/dev/cu.test", "board-a")
        with self.assertRaisesRegex(ToolkitError, "instead of 0.87.3-dev"):
            wait_for_uart_runtime(
                original,
                expected_version="0.87.3-dev",
                expected_board="exoanchor-prototype-v2.3",
                ports_provider=lambda: [original],
                identify_device=lambda console: {
                    "firmware": "0.87.2-dev",
                    "board": "exoanchor-prototype-v2.3",
                },
            )

    def test_missing_device_times_out_without_selecting_another_board(self):
        original = port("/dev/cu.old", "board-a")
        other = port("/dev/cu.other", "board-b")
        clock = StepClock()
        with self.assertRaisesRegex(ToolkitError, "same device did not return"):
            wait_for_uart_runtime(
                original,
                expected_version="0.87.3-dev",
                expected_board="exoanchor-prototype-v2.3",
                timeout=0.5,
                ports_provider=lambda: [other],
                identify_device=lambda console: {},
                monotonic=clock.monotonic,
                sleep=clock.sleep,
            )


if __name__ == "__main__":
    unittest.main()
