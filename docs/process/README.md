# Process

This directory defines how documentation should be maintained after the repository restructure.

## Documentation Rules

- Put runnable bring-up steps beside the hardware or firmware they operate on.
- Put agent runtime internals inside `exoanchor-agent/`.
- Put cross-cutting project direction, architecture, roadmap, and research here.
- Put raw datasheets, schematics, extracted text, and vendor bundles under `docs/ref/`.
- Do not put generated build output, `.DS_Store`, SDK caches, or personal machine paths in tracked docs.

## When Adding A New Document

1. Decide whether it belongs beside code/hardware or in `docs/`.
2. Add it to the nearest README index.
3. State status clearly: tested, design target, hypothesis, or reference.
4. Link to raw sources in `docs/ref/` instead of duplicating large source text.
5. Keep hardware capability claims aligned with the active matrix in `device/ESP32P4/README.md`.

## Current Priority

The current documentation priority is not volume; it is link correctness and capability truthfulness.

Broken links should be fixed before adding more prose. Hardware features should be described as tested only after bring-up confirms them.
