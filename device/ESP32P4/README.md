# ESP32-P4 Platform

This directory contains the active ESP32-P4 UVC KVM platform. The firmware is shared, while the board directories describe hardware variants and their power/status capability boundaries.

## Layout

```text
ESP32P4/
├── firmware/                  # Shared ESP-IDF firmware
├── waveshare-nano-diy/        # Waveshare ESP32-P4-NANO direct wiring
├── waveshare-nano-expansion/  # Waveshare ESP32-P4-NANO with add-on board
└── exoanchor-esp32p4x/        # Full custom ExoAnchor board
```

## Capability Matrix

| Variant | UVC video | USB HID | Ethernet | ATX power control | Power detect 12V | Power Standby detect 3V3AUX | Role |
|---------|-----------|---------|----------|-------------------|------------------|------------------------------|------|
| [waveshare-nano-diy](waveshare-nano-diy/) | Yes | GPIO26/27 | IP101GRI 100M | No | No | No | No-PCB bring-up and simple KVM |
| [waveshare-nano-expansion](waveshare-nano-expansion/) | Yes | GPIO26/27 | IP101GRI 100M | Reserved by expansion hardware | Reserved if wired | No | Prototype with add-on board |
| [exoanchor-esp32p4x](exoanchor-esp32p4x/) | Yes | Board-defined USB FS | Board-defined Ethernet | Yes | Yes | Yes | Full target board |

Dashboard status should reflect this matrix. `Video` and `USB` are live firmware states today. `Power` and `Power Standby` are UI placeholders until the matching board-level GPIO/ADC backend is added.

## Build

```bash
cd device/ESP32P4/firmware
. "$HOME/esp/esp-idf-v5.4/export.sh"
idf.py set-target esp32p4
idf.py build
```

## Flash And Find IP

```bash
cd device/ESP32P4/firmware
./tools/flash-monitor.sh /dev/cu.usbmodem5B5E1314701 --wait-ip --exit-on-ip
```

The script writes the detected Ethernet DHCP IP to `build/last_ip.txt`. Adjust the serial port to the local `ls /dev/cu.usbmodem*` result.

## Current Firmware Baseline

- USB UVC MJPEG input, tested with MS2109-class HDMI capture.
- ESP32-P4 USB FS HID on GPIO26(DM) and GPIO27(DP) for the Waveshare NANO variants.
- Embedded dashboard, KVM, settings, OTA upload, and reserved OTA check flow.
- Default login is `admin` / `admin`; the web UI asks for a password change when still using the default.
