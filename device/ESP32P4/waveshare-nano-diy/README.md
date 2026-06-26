# Waveshare ESP32-P4-NANO DIY

**[中文](README_zh.md)** | **Build guide:** [BuildGuide.md](BuildGuide.md)

This is the fastest ExoAnchor ESP32-P4 bring-up path. It uses an off-the-shelf Waveshare ESP32-P4-NANO, an MS2109-class HDMI USB capture card, Ethernet, and four USB 2.0 HID wires. No custom PCB and no soldering are required.

## Role

Use this variant when you want to validate the core KVM loop quickly:

- HDMI video from the target machine
- keyboard and mouse control through USB HID
- Ethernet dashboard access
- shared ESP32-P4 firmware bring-up

Do not use this variant when the requirement is remote ATX power control or power-state detection. The DIY wiring has no PWR/RST/12V/3V3AUX backend.

## Capability Boundary

| Feature | Status |
| --- | --- |
| UVC video | Supported through the ESP32-P4 high-speed USB-A host port |
| USB HID | Supported through GPIO26(D-) and GPIO27(D+) |
| Ethernet | Supported through the NANO RJ45/IP101GRI path |
| Web dashboard | Supported by the shared firmware |
| OTA upload UI | Present in firmware, validation in progress |
| ATX power control | Not available |
| Power detect 12V | Not available |
| Power standby detect 3V3AUX | Not available |

If remote boot is needed, configure Wake-on-LAN on the target machine or use external power-control wiring outside this DIY design.

## Hardware

- Waveshare ESP32-P4-NANO
- MS2109 HDMI USB capture card
- HDMI cable
- RJ45 Ethernet cable
- USB-C data cable for flashing and UART logs
- USB 2.0 cable or breakout with 5V, GND, D+, and D- available
- Mac, Windows, or Linux flashing host with the ESP32-P4 ESP-IDF toolchain

Avoid MS2109S capture cards for this path; current testing has shown compatibility issues.

## Wiring Summary

| Connection | Destination |
| --- | --- |
| MS2109 USB | ESP32-P4-NANO USB-A host port |
| Target HDMI output | MS2109 HDMI input |
| RJ45 / IP101GRI | DHCP LAN |
| USB HID D+ | GPIO27 |
| USB HID D- | GPIO26 |
| USB HID 5V | 5V |
| USB HID GND | GND |
| USB-C | Flashing host during setup/debug |

Check the 5V pin before powering the board. Do not connect the target USB 5V wire to 3V3.

## Firmware

This board uses the shared ESP32-P4 firmware:

[../firmware](../firmware/)

Typical build flow:

```bash
cd device/ESP32P4/firmware
. "$HOME/esp/esp-idf-v5.4/export.sh"
idf.py set-target esp32p4
idf.py build
./tools/flash-monitor.sh /dev/cu.usbmodem5B5E1314701 --wait-ip --exit-on-ip
```

Replace the serial port with the local device shown by your OS. The Waveshare NANO commonly appears through a CH343 USB-to-UART bridge.

## Bring-up Checklist

1. Serial log has no boot loop, panic, or USB init failure.
2. Ethernet receives a DHCP address.
3. The web UI opens at the board IP.
4. HDMI video appears in the KVM page.
5. Keyboard and mouse actions affect the target machine.
6. USB-C can be removed after flashing if the target USB HID 5V powers the board reliably.

Detailed assembly, safety checks, flashing notes, and troubleshooting live in [BuildGuide.md](BuildGuide.md).
