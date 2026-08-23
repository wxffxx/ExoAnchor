![ExoAnchor](assets/brand/png/exoanchor-readme-banner-1600x400.png)

# ExoAnchor

**An Another [RCOS](https://rcos.io/) project**

**[中文](README_zh.md)**

ExoAnchor is a fully featured KVM released under the [MIT License](LICENSE),
with the goal of opening the complete path from firmware to fabrication. It is
an MCU- and RTOS-based embedded hardware project with an optional AI Agent
layer.

> Current development firmware: `0.87.6-dev IndigoShore`.

The project currently centers on the ESP32-P4, with additional hardware
platforms under consideration as long as they preserve the same control plane
and safety model. It keeps a physical machine reachable when its operating
system, SSH access, network stack, or services are unavailable. Operators can
view the screen and send keyboard and mouse input through an independent
network path, with UART, reset and power control available only on boards that
implement them.

The target machine does not need ExoAnchor software. Manual KVM remains
independently usable; the device API, embedded Agent and external MCP controller
are optional layers above the deterministic control plane.

## Open from firmware to fabrication

**Not just open firmware. Open from firmware to fabrication.**

For ExoAnchor, "open source" is not a marketing label. It means comprehensively
open-sourcing the software, hardware, and editable 3D model source projects.
All original ExoAnchor files are distributed under the MIT License so anyone
can inspect, modify, reproduce, manufacture, and sell their own hardware.

| Layer | Published material | Current status |
| --- | --- | --- |
| Application and control plane | Dashboard, device API, embedded Agent, and optional MCP adapter | ✅ Published and under active development |
| MCU firmware | Video, HID, networking, UART, power control, configuration, and build scripts | ✅ Stable and Dev source published |
| Schematics and PCB | Complete schematic and design notes PDF plus editable PADS and Altium Designer projects | ✅ Latest published |
| BOM, netlist, and reproduction guide | Versioned BOM, Protel 2.0 netlist, and build, flashing, assembly, and acceptance instructions | 🚧 BOM and netlist published; the reproduction guide is being expanded |
| EasyEDA cloud source project | [Online V2.4a6 schematic and PCB source project](https://oshwhub.com/team_dsksgpyk/project_akawewur) | ⏳ Pending |
| Mechanical design | Editable mechanical CAD source projects | ⏳ Pending |

> ⚠️ This project is still under active development and hardware validation.
> Carefully review all materials before use, reproduction, or manufacturing.
> Please report any problems through
> [GitHub Issues](https://github.com/wxffxx/ExoAnchor/issues).

This is an artifact-release commitment, not a manufacturing-readiness claim.
The
[ExoAnchor-Hardware hardware release directory](https://github.com/wxffxx/ExoAnchor-Hardware/tree/main/ESP32P4/exoanchor-p4-v2.4a6)
is authoritative for the actual publication and validation state of each
hardware revision. Publishing schematics, PCB projects, and manufacturing
materials does not itself establish production readiness; reproduction and
acceptance must follow the guide and validation state for that revision.

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
| 3 | ExoAnchor PrototypeV0 | Broad hardware-verified reference |
| 4 | ExoAnchor PrototypeV2.3 | b4-labelled assembly using b6 mapping; bring-up verified, not production-final |
| 4A | ExoAnchor PrototypeV2.4 / V2.4a6 | Formal Dev product profile available; flashing and HIL not yet completed |

See the
[ESP32-P4 implementation matrix](device/ESP32P4/boards/IMPLEMENTATION_PROFILES_zh.md)
for each board's firmware profile, ESP32-P4 revision, and validation status.

The project confirms that `a6` identifies the V2.4 PCB layer count only; V2.4
and V2.4a6 have the same schematic/GPIO mapping and use
`exoanchor-prototype-v2.4 + esp32p4-rev3`. The separate
`exoanchor-prototype-v2.4-ms-test` profile exposes destructive EEPROM test
controls and is not a product image.

## From hardware selection to flashing

### 1. Choose hardware, firmware, and toolchain

| Hardware | Firmware | Build combination | ESP-IDF | Entry |
| --- | --- | --- | --- | --- |
| Waveshare ESP32-P4-NANO DIY | Dev; pure-KVM Stable is also available | `waveshare-p4-nano + esp32p4-rev1` | 5.4.x | [Assembly and flashing guide](https://github.com/wxffxx/ExoAnchor-Hardware/tree/main/ESP32P4/simple-diy/waveshare-nano-diy) |
| ExoAnchor PrototypeV0 | Dev or Stable | `exoanchor-prototype0 + esp32p4-rev3` | Dev 5.5.5; Stable 5.5.4 | [Dev build](device/ESP32P4/firmware/v0.86.6-dev/README.md#prototype0rev3已验证参考) · [Stable build](device/ESP32P4/firmware/v0.86-stable-kvm/README.md#构建-prototype0) |
| ExoAnchor PrototypeV2.3 | Dev | `exoanchor-prototype-v2.3 + esp32p4-rev3` | 5.5.5 | [V2.3 build](device/ESP32P4/firmware/v0.86.6-dev/README.md#prototypev23rev3bring-up-主线) |
| ExoAnchor PrototypeV2.4 / V2.4a6 | Dev | `exoanchor-prototype-v2.4 + esp32p4-rev3` | 5.5.5 | [V2.4 build and capability boundary](device/ESP32P4/firmware/v0.86.6-dev/README.md) |

Dev is the current KVM + embedded Agent development tree and runs
`0.87.6-dev`. Stable is an independent pure-KVM tree without the embedded
Agent. See the [firmware version index](device/ESP32P4/firmware/README.md) for
all versions, upgrade requirements, and detailed entries.

Different board and chip-revision profiles must not share generated `sdkconfig` or
build directories. Confirm the physical board before flashing.

### 2. Ask an AI Agent to get the source, build, and flash

Connect the board's development/flashing USB port, start Codex or another
terminal-capable AI Agent in the directory where the project should be stored,
and copy the prompt below. Replace the bracketed hardware and firmware values
first. If the serial port is unknown, request read-only discovery and do not
authorize flashing until an explicit port and the complete board identity are confirmed.

```text
Get ExoAnchor from its public repository and help me reproduce, build, and flash a device.

My environment:
- Hardware: [Waveshare ESP32-P4-NANO DIY / ExoAnchor PrototypeV0 / ExoAnchor PrototypeV2.3 / ExoAnchor PrototypeV2.4 or V2.4a6]
- Firmware: [Dev / Stable KVM]
- Operating system: [macOS / Linux / Windows]
- Flashing port: [actual port / unknown — discovery only, no flashing]
- Source location: [create ExoAnchor in the current directory / use an existing ExoAnchor directory]

Complete this task:
1. If the selected location has no source checkout, run git clone https://github.com/wxffxx/ExoAnchor.git. If a repository already exists, confirm that it is ExoAnchor and preserve all local changes.
2. Enter the repository and read README_zh.md, device/ESP32P4/firmware/README.md, and the README in the selected firmware directory.
3. Determine the one correct board profile, chip-revision overlay, and ESP-IDF version from the physical hardware. Code and configuration are authoritative. Stop instead of guessing when the board cannot be identified.
4. Check the local ESP-IDF and serial environment. State the exact required version when the toolchain is missing, and ask before making any system-level installation.
5. Preserve all existing workspace changes. Do not run git reset, overwrite with checkout, force-pull, or perform destructive cleanup.
6. Create a separate build directory and sdkconfig for the selected board, then run set-target and a complete build. Never reuse generated configuration from another board.
7. If the build fails, diagnose the first actionable error and fix only issues clearly in scope. Do not bypass failures by disabling safety checks or selecting another board profile.
8. Identify candidate serial ports and chip information with read-only checks. Before any write, require the operator-confirmed physical model, exact CH343 USB serial, eFuse MAC, silicon revision, firmware profile, and an explicit unoccupied port to match the board identity record. Stop on any missing value or mismatch.
9. Only after the build and full identity record match the hardware, run tools/flash-monitor.sh with the explicit port and --build-dir to perform a complete wired flash, monitor serial output, and extract the DHCP address. Never pass an auto-selected port to a write command.
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
│   └── guides/                      # Practical guides
├── integrations/exoanchor-mcp/      # Optional external MCP controller
│   ├── exoanchor_mcp/               # Canonical stdio server
│   └── skills/exoanchor-mcp-control/ # Shared Codex and DSH operating skill
└── LICENSE

ExoAnchor-Hardware/                  # Independent hardware source repository
├── ESP32P4/
│   ├── exoanchor-p4-v2.4a6/         # Current integrated PCIe design
│   ├── simple-diy/                  # Off-the-shelf simple DIY designs
│   └── archive/                     # Historical hardware material
├── assets/brand/
└── LICENSE
```

`ExoAnchor-Hardware` is an
[independent hardware repository](https://github.com/wxffxx/ExoAnchor-Hardware)
alongside the main repository, not a subdirectory of `ExoAnchor/`. Browse the
[documentation map](docs/README.md).

## Development verification

Run the complete host-only repository gate from the repository root:

```bash
./scripts/check-all.sh
```

It covers both firmware trees, MCP, Toolkit, documentation hygiene, and Git
whitespace checks. Toolkit requires Python 3.10+; when available, `uv` resolves
the locked environment automatically. This gate does not replace board HIL.

## License

Original ExoAnchor software, firmware and documentation are licensed under the
[MIT License](LICENSE). Unless a file says otherwise, this license covers all
original materials that the ExoAnchor contributors have the right to license,
including software, firmware, scripts, tests, configuration, documentation,
specifications, diagrams, original media assets, schematics, PCB and CAD source
projects, BOMs, netlists, mechanical models, and manufacturing materials. It
includes permission to manufacture and sell hardware from covered design files.

Copies or substantial portions must retain the copyright and permission notice
from `LICENSE`. Third-party products and materials retain their own terms, and
file-specific notices take precedence. The MIT License does not grant rights to
ExoAnchor names, logos, trademarks, or third-party material, and does not imply
regulatory approval, safety certification, or fitness for a particular hardware
application. Covered materials and hardware made from them are provided **as
is**, without warranty, to the fullest extent permitted by law.

### Why MIT?

I do not depend on this project for income. I have my own research agenda, and
ExoAnchor is, in a sense, a semi-hobby project. That gives me the freedom to
open-source all of it.

Being fully open source does not mean that we will not release production
hardware. We will bring out a practical version that people can purchase and
use as soon as possible.

More than anything, I hope people can learn from ExoAnchor how to turn an idea
into a product. I believe that lesson is worth more than the project itself.
