# Research

This directory tracks open research questions and points to source material. Raw PDFs, schematics, screenshots, and vendor bundles live in [../ref/](../ref/).

## Active Questions

| Topic | Current source |
|---|---|
| ESP32-P4 + MS2109 bring-up risks | [../ref/work-notes/hardware/ESP32P4_MS2109_HARDWARE_TODO_zh.md](../ref/work-notes/hardware/ESP32P4_MS2109_HARDWARE_TODO_zh.md) |
| UPD720202 USB3 host design | [../ref/work-notes/hardware/UPD720202_DESIGN_NOTES_zh.md](../ref/work-notes/hardware/UPD720202_DESIGN_NOTES_zh.md) |
| LT6911C CSI firmware feasibility | [../ref/work-notes/lt6911c-esp32p4-csi-firmware/README_zh.md](../ref/work-notes/lt6911c-esp32p4-csi-firmware/README_zh.md) |
| LT6911C Keil library symbols | [../ref/work-notes/lt6911c-esp32p4-csi-firmware/LT6911C_LIB_SYMBOLS_zh.md](../ref/work-notes/lt6911c-esp32p4-csi-firmware/LT6911C_LIB_SYMBOLS_zh.md) |

## Research Note Policy

- Keep raw source material in `docs/ref/`.
- Keep conclusions short and dated when the hardware may change.
- Mark assumptions clearly when they come from datasheets rather than board tests.
- Promote stable conclusions into `device/` docs only after they affect a real hardware path.
