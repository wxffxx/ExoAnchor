# SimpleIPMI

**[中文文档](README_zh.md)** 

An [RCOS](https://rcos.io) Project

Open-source, low-cost KVM-over-IP solution. Remotely control physical machines — keyboard, mouse, video, and power — through a web browser, similar to commercial IPMI/BMC systems.

## Features

- **Remote Video** — HDMI capture via USB or CSI, streamed as MJPEG
- **Remote Keyboard & Mouse** — USB HID emulation
- **Power Control** — Optocoupler/relay-isolated power, reset, and force-off
- **Web Dashboard** — Browser-based control panel, zero client installation
- **Flexible Networking** — WiFi AP direct connect, Ethernet LAN, or Tailscale overlay

## Architecture

The project supports three distinct host architectures:

### MCU Host — ESP32-S3 (Available)

Lightweight standalone solution. The ESP32-S3 handles everything in a single chip: WiFi AP (or Ethernet via optional W5500), web server (SPIFFS), and native USB HID. No Linux, no capture card — minimal BOM, single-host control.

```
                          ┌─────────────────────────────────────────┐
                          │            ESP32-S3                     │
                          │                                         │
User ── WiFi AP ─────────→│  AsyncWebServer + WebSocket (SPIFFS)   │
  or                      │         │              │                │
User ── W5500 Ethernet* ─→│    GPIO Control    USB HID (OTG)       │
         (optional)       │     ┌────┴────┐    ┌───┴────┐          │
                          │     │Optocoupler│   │Keyboard│          │
                          │     │PWR  │ RST │   │ Mouse  │          │
                          │     └──┬──┴──┬──┘   └───┬────┘          │
                          └────────┼─────┼──────────┼───────────────┘
                                   │     │          │
                              PWR_BTN RST_BTN    USB ──→ Target Machine

* W5500 SPI Ethernet is optional. Without it, the ESP32-S3 operates
  as a WiFi AP at 192.168.4.1. With W5500, it joins the LAN via DHCP.
```

### ARM Linux Host (Available)

Full-featured KVM solution based on ARM Linux SBCs (CM4, OrangePi). The SBC runs a Python (FastAPI) server with modular subsystems for video, HID, power, terminal, and firmware management.

Two HID modes are supported: UART serial bridge to an external ESP32-S3 (CM4 variant), or native USB OTG gadget (OrangePi variant).

```
                     ┌──────────────────────────────────────────────────┐
                     │               ARM Linux SBC                     │
                     │          FastAPI + WebSocket Server              │
                     │                                                  │
User (Browser) ─────→│  ┌───────────┐ ┌───────────┐ ┌──────────────┐  │
   Ethernet/WiFi     │  │  Video    │ │  HID Mgr  │ │  GPIO Ctrl   │  │
                     │  │  Capture  │ │           │ │              │  │
                     │  │ (OpenCV)  │ │ Mode A:   │ │  PWR_BTN     │  │
                     │  │           │ │  UART ──────────→ ESP32-S3  │  │
                     │  │           │ │  (serial   │ │  RST_BTN     │  │
                     │  │           │ │  protocol) │ │  12V Detect  │  │
                     │  │           │ │           │ │              │  │
                     │  │           │ │ Mode B:   │ └──────┬───────┘  │
                     │  │           │ │  USB OTG  │        │          │
                     │  │           │ │ (/dev/hidg)│        │          │
                     │  └─────┬─────┘ └─────┬─────┘        │          │
                     │  ┌─────┴─────┐ ┌─────┴─────┐        │          │
                     │  │ Terminal  │ │ ESP32 OTA │        │          │
                     │  │ (pty/ssh) │ │ Flasher   │        │          │
                     │  └───────────┘ └───────────┘        │          │
                     └────────┼───────────┼────────────────┼──────────┘
                              │           │                │
                         USB Capture   USB HID       Optocoupler/Relay
                         (MS2109)    (KB + Mouse)    (PWR / RST / 12V)
                              │           │                │
                         HDMI out ←── Target Machine ──→ Motherboard
```

### Composite Host (WIP)

Central management server running on Ubuntu (x86/ARM). Manages multiple KVM hosts and USB capture cards simultaneously, providing a unified web panel for multi-target-machine control.

## Repository Structure

```
simpleipmi/
├── hosts/                            # KVM host firmware & software
│   ├── esphost-esp32s3/              #   ESP32-S3 MCU host (WiFi AP + HID)
│   ├── armhost-cm4/                  #   CM4 ARM Linux host (Ethernet + HID Bridge)
│   ├── armhost-orangepi4/            #   OrangePi CM4 host (USB OTG HID)
│   ├── stmhost-f103/                 #   STM32F103 MCU host (WIP)
│   └── esphost-esp32c3(switch_only)/ #   ESP32-C3 power-switch only (WIP)
│
├── hardware/                         # PCB / schematic / BOM design files
│   ├── km/                           #   Keyboard-Mouse HID modules
│   ├── kvm-carrier/                  #   KVM carrier boards
│   ├── coreboard/                    #   SoC core modules
│   └── accessories/                  #   Video capture, relay, etc.
│
├── exoanchor/                           # KVM Agent framework (vision + auto-remediation)
│   ├── core/                         #   Passive monitor + semi-active executor
│   ├── vision/                       #   Screen analysis (local + LLM API)
│   ├── action/                       #   HID/SSH action driver
│   ├── skills/                       #   Skill system (YAML + Python)
│   └── dashboard/                    #   Agent web UI
│
├── composite/                        # Multi-host management system (WIP)
│   └── server/
│
├── shared/                           # Cross-device shared resources
│   └── protocol/                     #   Communication protocol (protocol.h)
│
└── docs/                             # Documentation & manuals
```

## Host Platform Comparison

| | ESP32-S3 | ARM CM4 | OrangePi CM4 | STM32F103 |
|---|---|---|---|---|
| **Architecture** | MCU | ARM Linux | ARM Linux | MCU |
| **Cost** | TBD | TBD | TBD | TBD |
| **Video Capture** | None | USB capture card | USB capture card | None |
| **HID** | Native USB OTG | ESP32-S3 serial bridge | Native USB OTG | Native USB |
| **Networking** | WiFi AP / W5500 Ethernet | Ethernet / WiFi | Ethernet / WiFi | External |
| **Web Panel** | Built-in (SPIFFS) | FastAPI server | FastAPI server | None |
| **Status** | Available | Available | Available | WIP |

## Quick Start

### ARM CM4 Host

**Requirements:** CM4-compatible SBC, ESP32-S3 (HID bridge), USB capture card

```bash
# Deploy server on CM4
cd hosts/armhost-cm4/server
pip install -r requirements.txt
python main.py

# Flash ESP32-S3 HID bridge firmware
cd hosts/armhost-cm4/firmware
pio run -t upload
```

See [hosts/armhost-cm4/docs/DEVELOPMENT.md](hosts/armhost-cm4/docs/DEVELOPMENT.md)

## Hardware Designs

All PCB and schematic files are under `hardware/`, organized by function:

| Category | Description |
|----------|-------------|
| `km/` | Keyboard-mouse HID modules (ESP32-S2, XIAO-ESP32S3, STM32F103) |
| `kvm-carrier/` | KVM carrier boards (PCIe CM4 v1/v2, T113) |
| `coreboard/` | SoC core modules (H616, ARM Linux full-module) |
| `accessories/` | HDMI capture (Toshiba TC358743, MS2109), relay module |

See [hardware/README.md](hardware/README.md)

## Roadmap

**Completed**
- ESP32-S3 standalone MCU host (WiFi AP + optional W5500 Ethernet)
- ARM CM4 KVM host with ESP32-S3 HID bridge
- OrangePi CM4 KVM host with native USB OTG
- Web dashboard (KVM view, terminal, system info)

**TODO**
- STM32F103 low-cost MCU host
- Composite multi-host management system
- H616 coreboard completion

## About RCOS

This project is developed under the [Rensselaer Center for Open Source](https://rcos.io) (RCOS), an organization at Rensselaer Polytechnic Institute that supports student-driven open source software serving the greater good.

## License

MIT
