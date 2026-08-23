import unittest

from exoanchor_toolkit.errors import ToolkitError
from exoanchor_toolkit.ports import (
    PortInfo,
    capture_port_identity,
    match_port_identity,
    resolve_port,
)


def port(
    name: str,
    likely: bool,
    *,
    serial_number: str | None = None,
) -> PortInfo:
    return PortInfo(
        device=name,
        description="test",
        hwid="test",
        vid=0x1A86 if likely else None,
        pid=0x55D3 if likely else None,
        serial_number=serial_number,
        manufacturer=None,
        likely_exoanchor=likely,
    )


class PortResolutionTests(unittest.TestCase):
    def test_explicit_port_is_preserved(self):
        self.assertEqual(resolve_port("/dev/example", []), "/dev/example")

    def test_unique_likely_port_is_selected(self):
        self.assertEqual(
            resolve_port("auto", [port("/dev/a", False), port("/dev/b", True)]),
            "/dev/b",
        )

    def test_no_likely_port_is_rejected(self):
        with self.assertRaisesRegex(ToolkitError, "no ExoAnchor-like"):
            resolve_port(None, [port("/dev/a", False)])

    def test_ambiguous_ports_are_rejected(self):
        with self.assertRaisesRegex(ToolkitError, "multiple ExoAnchor-like"):
            resolve_port("auto", [port("/dev/a", True), port("/dev/b", True)])

    def test_identity_follows_same_usb_device_after_path_change(self):
        original = port("/dev/cu.old", True, serial_number="board-a")
        other = port("/dev/cu.other", True, serial_number="board-b")
        returned = port("/dev/cu.new", True, serial_number="board-a")
        self.assertEqual(
            match_port_identity(original, [other, returned]),
            returned,
        )

    def test_identity_without_serial_number_never_guesses_another_port(self):
        original = port("/dev/cu.original", True)
        self.assertIsNone(
            match_port_identity(original, [port("/dev/cu.other", True)])
        )

    def test_capture_requires_exact_selected_path(self):
        selected = port("/dev/cu.selected", True, serial_number="board-a")
        self.assertEqual(
            capture_port_identity("/dev/cu.selected", [selected]),
            selected,
        )
        with self.assertRaisesRegex(ToolkitError, "disappeared"):
            capture_port_identity("/dev/cu.missing", [selected])


if __name__ == "__main__":
    unittest.main()
