# ExoAnchor Devices

English | [简体中文](README_zh.md)

This directory contains the software-facing definitions for supported
ExoAnchor device platforms. Hardware design files, schematics, PCB/CAD, BOMs,
assembly instructions, and manufacturing material live in the separate
[ExoAnchor-Hardware](https://github.com/wxffxx/ExoAnchor-Hardware) repository.

## Platforms

| Platform | Role | Documentation |
| --- | --- | --- |
| ESP32-P4 | Current platform for KVM, embedded Agent, terminal, storage, and external MCP access | [Platform overview](ESP32P4/README.md) |

## Repository boundary

The software repository contains:

- board and silicon build profiles;
- firmware source trees and version history;
- host-side tests, build tools, and runtime capability contracts.

It does not contain supplier assets, per-device credentials, authoritative
board identity records, or manufacturing packages. A buildable profile is not
proof that a physical board has passed hardware validation.

Before flashing, follow the target firmware's documented identity and build
rules. Never infer a physical board identity from its IP address, hostname,
serial path, or currently installed firmware.
