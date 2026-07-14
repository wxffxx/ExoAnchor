# ExoAnchor Documentation

This directory is the project-level documentation map. Device-specific wiring, firmware, and embedded Agent notes live next to the code under `device/`; external adapters live under `integrations/`; this directory keeps cross-cutting direction, architecture, process, research, and reference material.

## Start Here

| Area | File | Purpose |
|---|---|---|
| Project direction | [project/PROJECT_DIRECTION_zh.md](project/PROJECT_DIRECTION_zh.md) | Current product shape, focus, and non-goals |
| Project roadmap | [project/ROADMAP.md](project/ROADMAP.md) | Hardware, firmware, agent, and docs milestones |
| Architecture | [architecture/README.md](architecture/README.md) | System boundary and architecture index |
| Model strategy | [architecture/MODEL_STRATEGY_zh.md](architecture/MODEL_STRATEGY_zh.md) | LLM provider and local-model strategy |
| Process | [process/README.md](process/README.md) | Documentation ownership and maintenance rules |
| Research | [research/README.md](research/README.md) | Current research questions and source notes |
| Reference material | `docs/ref/` (local, ignored) | Datasheets, schematics, extracted material, vendor bundles |

## Current Documentation Shape

- Keep the root [README.md](../README.md) as the shortest runnable overview.
- Keep hardware bring-up docs beside the hardware variant, for example [../device/ESP32P4/waveshare-nano-diy/BuildGuide.md](../device/ESP32P4/waveshare-nano-diy/BuildGuide.md).
- Keep firmware build and flash notes beside the firmware, for example [../device/ESP32P4/firmware/README.md](../device/ESP32P4/firmware/README.md).
- Keep embedded Agent implementation details beside the ESP32-P4 firmware; keep external protocol adapters under `integrations/`.
- Use this `docs/` tree only for project-level material that crosses module boundaries.

## Active Mainline

The current mainline is:

```text
Waveshare ESP32-P4-NANO DIY
  -> shared ESP32-P4 firmware
  -> embedded KVM dashboard
  -> optional external MCP clients attach through the bounded device API
```

The target hardware line remains `device/ESP32P4/exoanchor-esp32p4x/`, but documentation should not assume its power-control and power-state hardware is already validated.
