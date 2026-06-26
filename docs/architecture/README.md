# Architecture

Architecture documents describe boundaries between hardware, firmware, dashboard, and agent runtime. Hardware-specific details stay in `device/`; agent internals stay in `exoanchor-agent/`.

## Documents

| File | Purpose |
|---|---|
| [SYSTEM_BOUNDARY_zh.md](SYSTEM_BOUNDARY_zh.md) | Device layer, firmware layer, dashboard, and agent boundary |
| [MODEL_STRATEGY_zh.md](MODEL_STRATEGY_zh.md) | Model provider strategy for the agent layer |

## Current Boundary Summary

```text
Target machine
  <-> HDMI / USB / power signals
ESP32-P4 device firmware
  <-> dashboard / API / status
exoanchor-agent
  <-> observations / actions / safety / logs / playbooks
Human operator
```

The device layer should report real capabilities. The agent layer may choose actions, but it must not assume power control or power-state detection exists on hardware variants that do not expose those signals.
