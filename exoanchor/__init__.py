"""
ExoAnchor — KVM Agent Framework for SimpleIPMI

A generalized computer-use agent that operates target machines through
KVM hardware (HID + Video) and SSH, with a reusable Skill system.

Modes:
  - Manual: Human-triggered skill execution only
  - Passive: Watchdog monitoring + auto-recovery
  - Semi-Active: Passive + scheduled/conditional skill execution
"""

__version__ = "0.1.0"
