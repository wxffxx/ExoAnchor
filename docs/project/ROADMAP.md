# Project Roadmap

This roadmap is scoped to the whole ExoAnchor project. Embedded-Agent internals belong beside the ESP32-P4 firmware; the optional external MCP adapter has its own roadmap under `integrations/exoanchor-mcp/docs/`.

## Phase 0: Re-baseline The Repository

Goal: make the current repository shape understandable after the old docs and legacy directories were removed.

- Keep `device/` as the device-family root.
- Keep embedded-Agent implementation in the ESP32-P4 firmware and external adapters under `integrations/`.
- Keep `docs/` for cross-cutting direction, architecture, process, research, and reference material.
- Fix broken documentation links from root README and agent README.
- Remove local metadata such as `.DS_Store` from docs.

Acceptance:

- A new contributor can start from `README.md` or `README_zh.md` and reach every major active path.
- `docs/README.md` and `docs/README_zh.md` describe the new documentation boundary.

## Phase 1: ESP32-P4 DIY Bring-up

Goal: one reproducible KVM path using Waveshare ESP32-P4-NANO.

- Validate firmware build and flash flow.
- Validate DHCP/IP discovery and web dashboard access.
- Validate MS2109-class UVC video input.
- Validate GPIO26/GPIO27 USB HID output.
- Keep troubleshooting notes in `device/ESP32P4/waveshare-nano-diy/BuildGuide.md`.

Acceptance:

- The DIY build can show target HDMI video and send keyboard/mouse input.
- Documentation clearly states that DIY does not support ATX power control or power-state detection.

## Phase 2: Shared ESP32-P4 Firmware Baseline

Goal: keep one firmware tree usable across DIY, expansion, and custom-board variants.

- Keep variant differences in configuration, not duplicated firmware trees.
- Make dashboard status reflect actual board capability.
- Keep OTA/upload UI validation separate from core KVM validation.
- Add board-level backend only after the matching hardware path exists.

Acceptance:

- `device/ESP32P4/README.md` capability matrix remains true.
- Firmware README includes build, flash, serial, IP discovery, and known limitations.

## Phase 3: Custom Board Definition

Goal: turn `exoanchor-esp32p4x` from target concept into a locked board definition.

- Lock power tree, USB, Ethernet, video capture, HID, PWR/RST, 12V detect, and 3V3AUX detect.
- Keep design notes linked from `docs/ref/work-notes/`.
- Add board-specific GPIO/ADC definitions after schematic decisions are stable.

Acceptance:

- Dashboard `Video`, `USB`, `Power`, and `Power Standby` each map to real hardware signals.
- The board README distinguishes design support from tested support.

## Phase 4: Agent Runtime Closure

Goal: move from front-end-driven plans toward a bounded embedded runtime.

- Stabilize structured observations and run logs.
- Move plan execution into the ESP32-P4 firmware runtime.
- Unify plan, skill, and tool-call execution.
- Add persistent run state, safety gates, and audit logs.

Acceptance:

- Agent tasks survive browser refresh.
- Failed tasks have structured logs and reproducible failure context.
- Dangerous actions are controlled by backend policy.

## Phase 5: Integrated Recovery Workflow

Goal: combine device KVM control and agent-side recovery.

- Use SSH when the OS is healthy.
- Use video/HID/KVM when SSH or networking fails.
- Use power control only on hardware variants that really expose it.
- Keep every recovery attempt auditable.

Acceptance:

- The system can choose between SSH and out-of-band KVM based on observed state.
- Hardware capability limits are enforced by configuration and UI.
