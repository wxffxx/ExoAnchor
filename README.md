![ExoAnchor](assets/brand/png/exoanchor-readme-banner-1600x400.png)

# ExoAnchor

**An Another [RCOS](https://rcos.io/) project**

**[中文](README_zh.md)**

ExoAnchor is a fully featured, AI Agent-based KVM project released entirely as
open source under the [MIT License](LICENSE).

> Current development firmware: `0.87.2-dev IndigoShore`. The project is in
> active development and hardware validation; this is not an RC, production
> release, or manufacturing-readiness claim.

It is built around the ESP32-P4 and keeps a physical machine reachable when
its operating system, SSH access, network stack, or services are unavailable.
Operators can view the screen and send keyboard and mouse input through an
independent network path, with UART, reset and power control available only on
boards that implement them.

The target machine does not need ExoAnchor software. Manual KVM remains
independently usable; the device API, embedded Agent and external MCP controller
are optional layers above the deterministic control plane.

Schematics, PCB/CAD, BOMs and manufacturing material live in
[ExoAnchor-Hardware](https://github.com/wxffxx/ExoAnchor-Hardware).

## Core capabilities

| Capability | Description |
| --- | --- |
| Video | Captures target HDMI output through an MS2109-class UVC path |
| Keyboard and mouse | Emulates USB HID across BIOS, boot and OS stages |
| Independent network | Serves the Dashboard and device API over ESP32-P4 Ethernet |
| UART | Exposes target serial only on wired and verified board profiles |
| Power and status | Exposes PWR, RST, 12V and 3V3AUX only where implemented |
| Embedded Agent | Orchestrates observations and actions under firmware policy, requests and leases |
| External control | Optional MCP adapter uses device APIs without owning embedded Agent state |

Each board reports only its own real capabilities. A buildable profile, draft
schematic, or test from another board is not hardware evidence.

## Current hardware baseline

| # | Implementation | Status |
| ---: | --- | --- |
| 1 | ESP32-P4 development board with external USB HDMI capture | Frozen known-good reference |
| 2 | Waveshare ESP32-P4-NANO with a simple expansion board | Prototype |
| 3 | ExoAnchor Prototype0 | Broad hardware-verified reference |
| 4 | ExoAnchor PrototypeV2.3 | b4-labelled assembly using b6 mapping; bring-up verified, not production-final |

### PrototypeV2.4A6 preview

<p align="center">
  <img src="assets/readme/exoanchor-prototype-v2.4a6-installed.jpg" alt="ExoAnchor PrototypeV2.4A6 installed inside a desktop chassis" width="49%">
  <img src="assets/readme/exoanchor-prototype-v2.4a6-board-render.png" alt="ExoAnchor PrototypeV2.4A6 board render" width="49%">
</p>

<p align="center"><sub>PrototypeV2.4A6 installed in a desktop chassis (left) and board render (right).</sub></p>

See the
[ESP32-P4 implementation matrix](device/ESP32P4/boards/IMPLEMENTATION_PROFILES_zh.md)
for each board's firmware profile, ESP32-P4 revision, and validation status.

## Architecture

```text
Browser / Embedded Agent / MCP
              │
              │ Ethernet
              ▼
┌──────────────────────────────────────────┐
│ ESP32-P4 ExoAnchor                       │
│ Dashboard · KVM Core · Device API       │
└─────────────┬─────────────┬──────────────┘
              │             │
       HDMI capture/UVC   USB HID
              │             │
              └──── Target machine ──┐
                                     │
                     Optional UART/power
```

KVM Core owns deterministic video, HID, UART, and power behavior. The embedded
Agent uses those capabilities only within firmware authorization and the
hardware's reported capabilities. Manual KVM remains available when the Agent
or model is unavailable.

## From hardware selection to flashing

### 1. Choose hardware, firmware, and toolchain

| Hardware | Firmware | Build combination | ESP-IDF | Entry |
| --- | --- | --- | --- | --- |
| Waveshare ESP32-P4-NANO DIY | Dev; pure-KVM Stable is also available | `waveshare-p4-nano + esp32p4-rev1` | 5.4.x | [Assembly and flashing guide](https://github.com/wxffxx/ExoAnchor-Hardware/tree/main/ESP32P4/reference/waveshare-nano-diy) |
| ExoAnchor Prototype0 | Dev or Stable | `exoanchor-prototype0 + esp32p4-rev3` | 5.5.4 | [Dev build](device/ESP32P4/firmware/v0.86.6-dev/README.md#prototype0rev3已验证参考) · [Stable build](device/ESP32P4/firmware/v0.86-stable-kvm/README.md#构建-prototype0) |
| ExoAnchor PrototypeV2.3 | Dev | `exoanchor-prototype-v2.3 + esp32p4-rev3` | 5.5.4 | [V2.3 build](device/ESP32P4/firmware/v0.86.6-dev/README.md#prototypev23rev3bring-up-主线) |

Dev is the current KVM + embedded Agent development tree and runs
`0.87.2-dev`. Stable is an independent pure-KVM tree without the embedded
Agent. See the [firmware version index](device/ESP32P4/firmware/README.md) for
all versions, upgrade requirements, and detailed entries.

Different board and chip-revision profiles must not share generated `sdkconfig` or
build directories. Confirm the physical board before flashing.

### 2. Ask an AI Agent to get the source, build, and flash

Connect the board's development/flashing USB port, start Codex or another
terminal-capable AI Agent in the directory where the project should be stored,
and copy the prompt below. Replace the bracketed hardware and firmware values
first. Keep “auto-detect” when the serial port is unknown.

```text
Get ExoAnchor from its public repository and help me reproduce, build, and flash a device.

My environment:
- Hardware: [Waveshare ESP32-P4-NANO DIY / ExoAnchor Prototype0 / ExoAnchor PrototypeV2.3]
- Firmware: [Dev / Stable KVM]
- Operating system: [macOS / Linux / Windows]
- Flashing port: [auto-detect / actual port]
- Source location: [create ExoAnchor in the current directory / use an existing ExoAnchor directory]

Complete this task:
1. If the selected location has no source checkout, run git clone https://github.com/wxffxx/ExoAnchor.git. If a repository already exists, confirm that it is ExoAnchor and preserve all local changes.
2. Enter the repository and read README_zh.md, device/ESP32P4/firmware/README.md, and the README in the selected firmware directory.
3. Determine the one correct board profile, chip-revision overlay, and ESP-IDF version from the physical hardware. Code and configuration are authoritative. Stop instead of guessing when the board cannot be identified.
4. Check the local ESP-IDF and serial environment. State the exact required version when the toolchain is missing, and ask before making any system-level installation.
5. Preserve all existing workspace changes. Do not run git reset, overwrite with checkout, force-pull, or perform destructive cleanup.
6. Create a separate build directory and sdkconfig for the selected board, then run set-target and a complete build. Never reuse generated configuration from another board.
7. If the build fails, diagnose the first actionable error and fix only issues clearly in scope. Do not bypass failures by disabling safety checks or selecting another board profile.
8. Identify candidate serial ports and chip information with read-only checks. Stop if the port or chip identity conflicts with the selected board.
9. After the build matches the hardware, run tools/flash-monitor.sh with an explicit --build-dir to perform a complete wired flash, monitor serial output, and extract the DHCP address.
10. Report the source commit, firmware version, board, chip revision, ESP-IDF version, build directory, serial port, device IP, Ethernet/UVC/HID startup state, and every error.

Safety constraints:
- Do not send keyboard, mouse, power, or reset actions to the target host.
- Do not use force, erase the entire Flash, or overwrite another board's build directory without my explicit approval.
- Do not print or add passwords, tokens, private keys, or absolute local credential paths to shared documentation.
```

### 3. Get the source, build, and flash manually

```bash
git clone https://github.com/wxffxx/ExoAnchor.git
cd ExoAnchor
```

Install the ESP-IDF version from the selection table, then follow the commands
in the [Dev build guide](device/ESP32P4/firmware/v0.86.6-dev/README.md) or
[Stable build guide](device/ESP32P4/firmware/v0.86-stable-kvm/README.md) for
the selected board. After the build, run this from the selected firmware
directory:

```bash
./tools/flash-monitor.sh <PORT> \
  --build-dir <build-dir> \
  --wait-ip \
  --exit-on-ip
```

`<PORT>` is the serial port reported by the operating system, and `<build-dir>`
must exactly match the `idf.py -B` argument used during the build. See the
[firmware flashing entry](device/ESP32P4/firmware/README.md#烧录规则) for
partition-upgrade requirements and the direct `idf.py` alternative.

### 4. Open and verify

| URL | Purpose |
| --- | --- |
| `http://<board-ip>/` | Dashboard |
| `http://<board-ip>/kvm` | Video, keyboard, and mouse |
| `http://<board-ip>/agent` | Embedded Agent, Dev only |
| `http://<board-ip>/terminal` | Target UART, Dev only and board-dependent |
| `http://<board-ip>/settings` | Device settings |

Verify that the Dashboard opens, Ethernet has an address, KVM video is present,
and HID works. First boot requires replacing the local bootstrap credential;
shared documentation does not publish its value.

## Repository layout

```text
ExoAnchor/
├── assets/brand/                    # Brand and Web assets
├── device/ESP32P4/
│   ├── boards/                      # Authoritative board/profile mapping
│   └── firmware/                    # Independent Stable and Dev trees
├── docs/
│   ├── reproduction/                # Manufacturing and reproduction
│   ├── guides/                      # Practical guides
│   └── ROADMAP_zh.md                # Reviewed public roadmap
├── integrations/exoanchor-mcp/      # Optional external MCP controller
└── LICENSE
```

Browse the [documentation map](docs/README.md).

## License

Original ExoAnchor software, firmware and documentation are licensed under the
[MIT License](LICENSE). Original hardware designs use the same policy, including
permission to manufacture and sell hardware from covered design files.

Unless a file or directory carries a different notice, that license covers
original software, firmware, scripts, tests, configuration, documentation,
specifications, diagrams, media assets and hardware design files that ExoAnchor
contributors have the right to license. Third-party material remains under its
own terms, and file-specific notices take precedence. The MIT License does not
grant rights to the ExoAnchor name, logos or other trademarks, does not include
an express patent license, and does not imply regulatory approval, safety
certification or fitness for a particular hardware application.
