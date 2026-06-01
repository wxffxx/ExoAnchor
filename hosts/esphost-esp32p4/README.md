# SI BMC - ESP32-P4 Host

Status: HID + MIPI-CSI video-input bring-up baseline.

This directory contains the ESP32-P4 host firmware source for the Waveshare ESP32-P4-NANO. The current firmware intentionally ignores GPIO and ATX power control so bring-up can focus on two paths:

- TinyUSB boot-keyboard and boot-mouse HID migrated from the ESP32-S3 firmware, with the KVM page exposing keyboard, joystick, and touchpad controls over `/api/ws/hid`.
- Embedded dashboard/KVM pages migrated from the CM4 host path, with ESP32-P4 RJ45 Ethernet and MIPI-CSI video input exposed as JPEG snapshots. The KVM page uses snapshot refresh so HID WebSocket traffic is not blocked by a long-lived stream request.

The default video source is now an OV5647-style 2-lane MIPI-CSI camera on the Waveshare ESP32-P4-NANO CSI connector. The firmware initializes the camera over SCCB/I2C, captures RAW8 through CSI/ISP as RGB565, and encodes snapshots/streams with the hardware JPEG encoder.

Verified build: `idf.py build` succeeds with app binary size `0xa7aa0` bytes and about 35% free space in the default 1MB app partition.

Run `./tools/serve-ui.py --port 5080` to test the embedded dashboard/KVM pages locally with mocked status, video, and HID WebSocket endpoints before flashing them to the ESP32-P4. Mouse `mousemove` accepts either normalized `dx/dy` or raw HID counts via `{"unit":"hid","dx":18,"dy":-6}`. Touchpad dragging uses browser Pointer Lock when available so the control-side pointer stays pinned while relative movement is sent to the target host; browsers without Pointer Lock fall back to hiding the local cursor and keeping the touchpad marker centered while dragging.

Use `./tools/flash-monitor.sh PORT --wait-ip --exit-on-ip` to flash, read the serial log, print the detected Ethernet DHCP IP, and save it to `build/last_ip.txt`.

See [README_zh.md](README_zh.md) for active wiring, configuration, build, flash, and endpoint notes.
