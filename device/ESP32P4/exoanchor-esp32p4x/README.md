# ExoAnchor ESP32-P4x

This is the full custom ESP32-P4 board target. It is the intended complete hardware design for video, HID, power control, and power-state detection.

## Capability Boundary

| Feature | Status |
|---------|--------|
| UVC video | Supported |
| USB HID | Supported through the board-defined USB FS path |
| Ethernet | Supported through the board-defined Ethernet path |
| ATX power control | Supported by design |
| Power detect 12V | Supported by design |
| Power Standby detect 3V3AUX | Supported by design |

The web dashboard's four compact states map naturally to this board:

- `Video`: UVC capture online, standby, or offline.
- `USB`: HID online, standby, or offline.
- `Power`: PCIe/ATX 12V present or absent.
- `Power Standby`: 3V3AUX present or absent.

Use the shared firmware in `../firmware`. Board-specific GPIO and sensing backends should be added here once the schematic is locked.
