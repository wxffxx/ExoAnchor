# Architecture

Architecture documents describe boundaries between hardware, firmware, dashboard, the embedded Agent, and external clients. Hardware and embedded-Agent details stay in `device/`; external adapters stay in `integrations/`.

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
embedded Agent / bounded device API
  <-> observations / actions / safety / logs / playbooks
Human operator / optional MCP client
```

The device layer should report real capabilities. The agent layer may choose actions, but it must not assume power control or power-state detection exists on hardware variants that do not expose those signals.
