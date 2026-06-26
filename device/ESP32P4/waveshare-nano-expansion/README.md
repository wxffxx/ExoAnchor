# Waveshare NANO Expansion

This variant uses a Waveshare ESP32-P4-NANO with an add-on board. It is a prototype hardware path between direct DIY wiring and the full custom ExoAnchor board.

## Capability Boundary

| Feature | Status |
|---------|--------|
| UVC video | Supported through the ESP32-P4 high-speed USB host port |
| USB HID | Supported through GPIO26(DM) and GPIO27(DP), unless the expansion board remaps it |
| Ethernet | Supported through the NANO RJ45/IP101GRI path |
| ATX power control | Reserved by expansion hardware |
| Power detect 12V | Reserved if wired |
| Power Standby detect 3V3AUX | Not available on the current expansion board |

The important limitation is standby: this expansion board cannot detect 3V3AUX, so `Power Standby` should not be treated as a real backend state for this hardware.

Use the shared firmware in `../firmware`.
