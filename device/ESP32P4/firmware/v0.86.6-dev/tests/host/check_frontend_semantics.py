#!/usr/bin/env python3
"""Keep known frontend semantic debt from growing while it is being removed.

This is deliberately a ceiling-based gate.  The current UI still contains
legacy badges, optimistic READY labels, concrete telemetry fallbacks and
unfinished product surfaces.  Each matching occurrence is already covered by
the frontend remediation roadmap; new occurrences fail the host test, while
removing existing debt lowers the reported count without requiring an
allowlist edit.
"""

from dataclasses import dataclass
from pathlib import Path
import re
import sys


PROJECT_DIR = Path(__file__).resolve().parents[2]
WWW_DIR = PROJECT_DIR / "main" / "www"


@dataclass(frozen=True)
class DebtRule:
    name: str
    relative_path: str
    pattern: str
    ceiling: int
    flags: int = 0


RULES = (
    # Status-shaped decoration inventory from FE-02/FE-10.
    DebtRule("overview badge classes", "index.html",
             r'class="[^"]*\bbadge\b[^"]*"', 0),
    DebtRule("KVM badge classes", "kvm.html",
             r'class="[^"]*\bbadge\b[^"]*"', 0),
    DebtRule("Agent badge classes", "agent.html",
             r'class="[^"]*\bbadge\b[^"]*"', 0),
    DebtRule("Terminal badge classes", "terminal.html",
             r'class="[^"]*\bbadge\b[^"]*"', 0),
    DebtRule("Settings badge classes", "settings.html",
             r'class="[^"]*\bbadge\b[^"]*"', 0),
    DebtRule("Settings scope pills", "settings.html",
             r'class="[^"]*\bscope-pill\b[^"]*"', 0),

    # Unsupported optimistic claims from FE-02/FE-03/FE-04/FE-07.
    DebtRule("Overview READY literals", "index.html", r"\bREADY\b", 0),
    DebtRule("Agent READY literals", "agent.html", r"\bREADY\b", 0),
    DebtRule("Terminal READY literals", "terminal.html", r"\bREADY\b", 0),
    DebtRule("Settings READY literals", "settings.html", r"\bREADY\b", 0),
    DebtRule("shared assistant READY literals", "assets/ui-shell.js",
             r"\bREADY\b", 0),
    DebtRule("unconditional all-normal claim", "index.html",
             r"一切正常", 0),

    # Concrete values that currently masquerade as observations (FE-02).
    DebtRule("Overview PHY fallback", "index.html", r"IP101GRI", 0),
    DebtRule("Settings PHY fallback", "settings.html", r"IP101GRI", 0),
    DebtRule("Overview core-count fallback", "index.html",
             r"\bpcpu\.cores\s*\|\|\s*2\b", 0),
    DebtRule("Overview unknown speed with concrete unit", "index.html",
             r'["\']--M["\']', 0),
    DebtRule("Settings unknown speed with concrete unit", "settings.html",
             r'["\']--M["\']', 0),

    # Chip model used as a product/device identity fallback (FE-03/FE-10).
    DebtRule("Overview ESP32-P4 identity", "index.html", r"ESP32-P4", 0),
    DebtRule("KVM ESP32-P4 identity", "kvm.html", r"ESP32-P4", 0),
    DebtRule("Agent ESP32-P4 identity", "agent.html", r"ESP32-P4", 0),
    DebtRule("Settings ESP32-P4 identity", "settings.html", r"ESP32-P4", 0),
    DebtRule("shared shell ESP32-P4 identity", "assets/ui-shell.js",
             r"ESP32-P4", 0),

    # Implementation/development detail leakage (FE-03/FE-05/FE-06/FE-07).
    DebtRule("Terminal local architecture injection", "terminal.html",
             r"\[local\]", 0),
    DebtRule("KVM transport-shaped no-signal copy", "kvm.html",
             r"waiting for USB UVC frame", 0),
    DebtRule("assistant fixed dry-run promotion", "assets/ui-shell.js",
             r"默认安全预演", 0),
    DebtRule("Agent reserved registry surfaces", "agent.html",
             r"REGISTRY RESERVED", 0),
    DebtRule("Agent planned implementation labels", "agent.html",
             r"\bPLANNED\b", 0),
)


def main() -> int:
    failures: list[str] = []
    debt_total = 0
    for rule in RULES:
        path = WWW_DIR / rule.relative_path
        if not path.exists():
            failures.append(f"{rule.name}: missing file {rule.relative_path}")
            continue
        text = path.read_text(encoding="utf-8")
        count = len(re.findall(rule.pattern, text, rule.flags))
        debt_total += count
        if count > rule.ceiling:
            failures.append(
                f"{rule.name}: {count} occurrences exceeds debt ceiling "
                f"{rule.ceiling} in {rule.relative_path}"
            )

    if failures:
        print("frontend semantics debt gate failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1

    print(
        "frontend semantics debt gate: PASS "
        f"({debt_total} known occurrences; ceilings may only decrease)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
