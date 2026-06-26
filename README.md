# ExoAnchor

**[中文文档](README_zh.md)**

An [RCOS](https://rcos.io) project.

ExoAnchor is an open-source, low-cost, AI-ready bare-metal BMC/KVM project with complete software and hardware design. Its goal is to keep physical machines controllable through a physical control plane when SSH, system networking, or services are unavailable: view the screen, send keyboard/mouse input, enter BIOS, handle boot failures, and recover with power control when available.

Hardware repository: [wxffxx/ExoAnchor-Hardware](https://github.com/wxffxx/ExoAnchor-Hardware)

The project has two layers:

- **Device layer:** ESP32-P4 and other hardware variants provide video input, USB HID output, Ethernet access, and board-level control signals. This layer includes complete hardware/software design and an easy-to-reproduce DIY path.
- **Agent layer:** `exoanchor-agent/` provides the service-side runtime for observations, actions, safety confirmation, logs, and recovery playbooks.

## Quick Start: Waveshare ESP32P4 NANO DIY

1. Read the hardware guide first: [BuildGuide.md](device/ESP32P4/waveshare-nano-diy/BuildGuide.md).
2. Wire the target host USB HID cable:

| USB signal | ESP32-P4-NANO pin |
| --- | --- |
| D+ | GPIO27 |
| D- | GPIO26 |
| 5V | 5V |
| GND | GND |

3. Plug the MS2109 HDMI capture card into the ESP32-P4-NANO USB-A port.
4. Connect the target host HDMI output to the capture card.
5. Connect RJ45 Ethernet.
6. Build and flash the shared firmware.

## Current Focus

The latest hardware is the ESP32-P4 KVM platform under [device/ESP32P4](device/ESP32P4/).

The world's first design to complete a full KVM task with an MCU.

| Path | Status | Use case |
| --- | --- | --- |
| [device/ESP32P4/waveshare-nano-diy](device/ESP32P4/waveshare-nano-diy/) | Active bring-up | No custom PCB, no soldering, quick video + keyboard/mouse + Ethernet |
| [device/ESP32P4/waveshare-nano-expansion](device/ESP32P4/waveshare-nano-expansion/) | Prototype | Waveshare NANO + expansion board |
| [device/ESP32P4/exoanchor-esp32p4x](device/ESP32P4/exoanchor-esp32p4x/) | Custom board | Full video, HID, power control, and power-state detection |

The fastest path to a working device is the Waveshare ESP32-P4-NANO DIY build:

- Waveshare ESP32-P4-NANO
- MS2109 HDMI USB capture card
- RJ45 Ethernet
- USB 2.0 HID wiring on GPIO26/GPIO27
- Shared ESP-IDF firmware from [device/ESP32P4/firmware](device/ESP32P4/firmware/)

Assembly guide: [device/ESP32P4/waveshare-nano-diy/BuildGuide.md](device/ESP32P4/waveshare-nano-diy/BuildGuide.md).

## What The DIY Build Can Do

| Feature | Status |
| --- | --- |
| HDMI video capture | Supported through an MS2109-class UVC capture card on the ESP32-P4 USB-A host port |
| Keyboard/mouse | Supported through USB HID on GPIO26/GPIO27 |
| Ethernet | Supported through the Waveshare NANO RJ45/IP101GRI path |
| Web UI | Supported by the shared ESP32-P4 firmware |
| OTA/update UI | Present in firmware, still being validated |
| ATX power control | Not supported by the DIY build |
| 12V / 3V3AUX power-state detection | Not supported by the DIY build |

The DIY build is intentionally simple. It is good for validating video, keyboard/mouse, networking, and firmware flow. It cannot press the target host power button or detect standby rails. If remote boot is needed, use Wake-on-LAN, manual operation, or a future expansion/custom board.

## Architecture

### ESP32-P4 NANO DIY KVM

```text
                         ┌────────────────────────────────┐
User browser ─ Ethernet →│ RJ45 + IP101GRI PHY            │
                         │ ESP32-P4 + firmware + Web UI   │
                         │                                │
                         │  USB-A host ─────┐             │
                         │  GPIO27 D+  ─┐   │             │
                         │  GPIO26 D-  ─┼───┼──────────┐  │
                         │  5V / GND   ─┘   │          │  │
                         └──────────────────┼──────────┼──┘
                                            │          │
                                      ┌─────▼─────┐    │
                                      │ MS2109    │    │
                                      │ HDMI UVC  │    │
                                      └─────┬─────┘    │
                                            │          │
Target machine HDMI out ────────────────────┘          │
Target machine USB-A  ◀────────────────────────────────┘
```

In the DIY version, the ESP32-P4-NANO is the whole KVM device:

- video enters through the MS2109 capture card on the USB-A host port
- keyboard and mouse leave through the GPIO26/GPIO27 USB HID wiring
- users connect through the onboard IP101GRI PHY to access the dashboard over Ethernet
- after bring-up, USB-C is only used for flashing and serial logs

This wiring has no ATX power-control path. The target host must be powered manually, through Wake-on-LAN, or by another external controller.

## Repository Layout

```text
ExoAnchor/
├── device/
│   ├── ESP32P4/
│   │   ├── firmware/                  # Shared ESP-IDF firmware
│   │   ├── waveshare-nano-diy/        # No-PCB Waveshare NANO build
│   │   ├── waveshare-nano-expansion/  # Expansion-board prototype
│   │   └── exoanchor-esp32p4x/        # Full custom board target
│   ├── ArmLinux/                      # ARM Linux device path
│   ├── ESP32S3/                       # ESP32-S3 control/reference path
│   ├── STM32F103/                     # UART/control bridge path
│   └── ESP32C6/                       # Lightweight remote-switch path
├── exoanchor-agent/                   # Python agent runtime and dashboard integration
└── docs/                              # Project direction, architecture, process, research, and references
```

## Documentation

Start with these documents:

- [device/README.md](device/README.md): device family overview
- [device/ESP32P4/README.md](device/ESP32P4/README.md): ESP32-P4 platform overview
- [device/ESP32P4/waveshare-nano-diy/BuildGuide.md](device/ESP32P4/waveshare-nano-diy/BuildGuide.md): no-PCB assembly guide
- [device/ESP32P4/firmware/README.md](device/ESP32P4/firmware/README.md): shared firmware build/flash guide
- [exoanchor-agent/README.md](exoanchor-agent/README.md): agent runtime overview
- [docs/README.md](docs/README.md): project documentation map

## License

[MIT](LICENSE)
