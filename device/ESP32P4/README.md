# ESP32-P4 Platform

English | [简体中文](README_zh.md)

ESP32-P4 is ExoAnchor's current device platform. This directory maintains the
software-facing board mappings, firmware versions, and runtime capabilities.
Schematics, PCB/CAD, BOMs, assembly instructions, and manufacturing material
are maintained in
[ExoAnchor-Hardware](https://github.com/wxffxx/ExoAnchor-Hardware/tree/main/ESP32P4)
instead.

## Directory layout

```text
device/ESP32P4/
├── boards/
│   └── IMPLEMENTATION_PROFILES_zh.md  # Board, firmware, silicon, and validation matrix
└── firmware/
    ├── README.md                       # Firmware version index
    ├── v0.86.6-dev/                    # Current Dev source tree
    └── v0.86-stable-kvm/               # Independent pure-KVM source tree
```

The software repository does not duplicate the README for each physical board.
Use the corresponding Hardware repository directory for wiring, manufacturing,
or physical inspection.

## Physical implementations

| # | Implementation | Firmware combination | Current status |
| ---: | --- | --- | --- |
| 1 | ESP32-P4 development board with external capture | `waveshare-p4-nano + rev1` | Frozen known-good reference |
| 2 | Waveshare NANO with simple expansion board | Currently shares `waveshare-p4-nano + rev1` | Prototype; expansion capabilities are not fully modeled |
| 3 | Prototype0 | `exoanchor-prototype0 + rev3` | Broad hardware-verified reference |
| 4 | PrototypeV2.3 | `exoanchor-prototype-v2.3 + rev3` | b4-labeled assembly using the b6 mapping; bring-up verified |
| 4A | PrototypeV2.4 / V2.4a6 | `exoanchor-prototype-v2.4 + rev3` | Product profile available; `a6` denotes only the PCB layer count; flashing and HIL remain pending |

See the
[board and firmware profile matrix](boards/IMPLEMENTATION_PROFILES_zh.md) for
GPIO sources, netlist provenance, capabilities, and evidence levels.
PrototypeV2.1 is a compile-only design reference, while legacy
`exoanchor-esp32p4x` is a frozen compatibility profile. Neither represents a
new current physical implementation.

`exoanchor-prototype-v2.4-ms-test` is a dedicated MS2109/EEPROM hardware
validation image, not the V2.4 product profile.

## Current capability boundary

| Implementation | UVC | HID | Ethernet | Target UART | ATX/power state |
| --- | --- | --- | --- | --- | --- |
| Direct development board | Known good | GPIO26/27 | IP101GRI | None | None |
| NANO expansion | Supported | Expansion-dependent | IP101GRI | None | Expansion-dependent and not fully modeled |
| Prototype0 | Verified | Verified | IP101GRI verified | None | PWR/RST/12V/3V3AUX verified |
| PrototypeV2.3 | Modes verified; image-quality issue remains open | Bring-up verified | DP83825I bring-up | GPIO50/51 verified | Derived from netlist; product validation incomplete |
| PrototypeV2.4 / V2.4a6 | Profile configured; HIL pending | GPIO26/27; HIL pending | DP83825I; HIL pending | GPIO50/51; HIL pending | PWR/RST/12V/3V3AUX configured only; HIL pending |

The Dashboard and `GET /api/capabilities` must reflect the active profile and
must not advertise another board's capabilities.

## Build entry points

The current development runtime is `0.87.6-dev`. Its source directory retains
the name `v0.86.6-dev` because the tree evolved from the hardened 0.86.6
baseline.

- Build the current Dev firmware:
  [`firmware/v0.86.6-dev/README.md`](firmware/v0.86.6-dev/README.md)
- Build the independent pure-KVM firmware:
  [`firmware/v0.86-stable-kvm/README.md`](firmware/v0.86-stable-kvm/README.md)
- Select a version and review upgrade boundaries:
  [`firmware/README.md`](firmware/README.md)

The rev3/ECO7 profiles in the current Dev tree, including PrototypeV2.4, use
ESP-IDF v5.5.5 or a later compatible version. The frozen rev1/Waveshare path
uses ESP-IDF v5.4.x. Switching board or silicon profiles requires a fresh build
directory and generated configuration.

## Runtime entry points

```text
http://<board-ip>/
http://<board-ip>/kvm
http://<board-ip>/agent
http://<board-ip>/terminal
http://<board-ip>/settings
```

Replace the local bootstrap credential on first boot. Shared documentation must
not publish credential values.
