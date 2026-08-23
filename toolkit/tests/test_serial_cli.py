import unittest
from unittest.mock import patch

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.serial_cli import DiagnosticConsole


class _FakeSerialPort:
    def __init__(self, *, acknowledge_sync=True):
        self.acknowledge_sync = acknowledge_sync
        self.pending = bytearray()
        self.writes = []

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        return False

    def reset_input_buffer(self):
        self.pending.clear()

    def write(self, payload):
        self.writes.append(payload)
        joined = b"".join(self.writes)
        if joined.endswith(b"sync\r\n"):
            if self.acknowledge_sync:
                self.pending.extend(b"stale exo> sync\r\nsync ok\r\nexo> ")
            self.writes.clear()
            self.writes.append(b"sync\r\n")
        elif joined.endswith(b"status\r\n"):
            self.pending.extend(b"status\r\nstatus ok\r\nexo> ")
            self.writes.clear()
            self.writes.extend((b"sync\r\n", b"status\r\n"))
        return len(payload)

    def flush(self):
        return None

    def read(self, size):
        if not self.pending:
            return b""
        chunk = bytes(self.pending[:size])
        del self.pending[:size]
        return chunk


class _FakeSerialModule:
    SerialException = OSError

    def __init__(self, device):
        self.device = device

    def Serial(self, *args, **kwargs):
        return self.device


class DiagnosticConsoleTests(unittest.TestCase):
    def test_sync_acknowledgement_precedes_caller_command(self):
        device = _FakeSerialPort()
        with patch(
            "exoanchor_toolkit.serial_cli._serial_module",
            return_value=_FakeSerialModule(device),
        ):
            result = DiagnosticConsole("/dev/test", timeout=0.1).run("status")

        self.assertEqual(device.writes, [b"sync\r\n", b"status\r\n"])
        self.assertEqual(result.output, "status ok")

    def test_sync_response_deadline_starts_after_paced_write(self):
        device = _FakeSerialPort()
        clock = [0.0]

        def slow_host_sleep(_seconds):
            clock[0] += 0.2

        with (
            patch(
                "exoanchor_toolkit.serial_cli._serial_module",
                return_value=_FakeSerialModule(device),
            ),
            patch(
                "exoanchor_toolkit.serial_cli.time.monotonic",
                side_effect=lambda: clock[0],
            ),
            patch(
                "exoanchor_toolkit.serial_cli.time.sleep",
                side_effect=slow_host_sleep,
            ),
        ):
            result = DiagnosticConsole("/dev/test", timeout=0.1).run("status")

        self.assertEqual(result.output, "status ok")

    def test_command_is_not_sent_when_sync_cannot_be_confirmed(self):
        device = _FakeSerialPort(acknowledge_sync=False)
        with patch(
            "exoanchor_toolkit.serial_cli._serial_module",
            return_value=_FakeSerialModule(device),
        ):
            with self.assertRaisesRegex(ToolkitError, "cannot synchronize"):
                DiagnosticConsole("/dev/test", timeout=0.01).run("network show")

        self.assertEqual(device.writes, [b"sync\r\n"])


if __name__ == "__main__":
    unittest.main()
