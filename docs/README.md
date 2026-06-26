# ExoAnchor Documentation

This directory is the project-level documentation map. Device-specific wiring and firmware notes live next to the code under `device/`; agent-runtime notes live under `exoanchor-agent/`; this directory keeps cross-cutting direction, architecture, process, research, and reference material.

## Start Here

| Area | File | Purpose |
|---|---|---|
| Project direction | [project/PROJECT_DIRECTION_zh.md](project/PROJECT_DIRECTION_zh.md) | Current product shape, focus, and non-goals |
| Project roadmap | [project/ROADMAP.md](project/ROADMAP.md) | Hardware, firmware, agent, and docs milestones |
| Architecture | [architecture/README.md](architecture/README.md) | System boundary and architecture index |
| Model strategy | [architecture/MODEL_STRATEGY_zh.md](architecture/MODEL_STRATEGY_zh.md) | LLM provider and local-model strategy |
| Process | [process/README.md](process/README.md) | Documentation ownership and maintenance rules |
| Research | [research/README.md](research/README.md) | Current research questions and source notes |
| Reference material | [ref/README.md](ref/README.md) | Datasheets, schematics, extracted material, vendor bundles |

## Current Documentation Shape

- Keep the root [README.md](../README.md) as the shortest runnable overview.
- Keep hardware bring-up docs beside the hardware variant, for example [../device/ESP32P4/waveshare-nano-diy/BuildGuide.md](../device/ESP32P4/waveshare-nano-diy/BuildGuide.md).
- Keep firmware build and flash notes beside the firmware, for example [../device/ESP32P4/firmware/README.md](../device/ESP32P4/firmware/README.md).
- Keep agent runtime details inside [../exoanchor-agent/](../exoanchor-agent/).
- Use this `docs/` tree only for project-level material that crosses module boundaries.

## Active Mainline

The current mainline is:

```text
Waveshare ESP32-P4-NANO DIY
  -> shared ESP32-P4 firmware
  -> embedded KVM dashboard
  -> exoanchor-agent runtime later attaches above the device layer
```

The target hardware line remains `device/ESP32P4/exoanchor-esp32p4x/`, but documentation should not assume its power-control and power-state hardware is already validated.
