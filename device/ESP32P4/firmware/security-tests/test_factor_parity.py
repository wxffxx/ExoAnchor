#!/usr/bin/env python3
"""Keep the authentication-factor sources identical across firmware trees.

The firmware trees stay independent (no shared component or symlink), so the
TOTP and passkey implementations are deliberately copied. A fix applied to one
tree must reach the other; this check fails until both copies match.
"""
from pathlib import Path
import sys

FIRMWARE = Path(__file__).resolve().parents[1]
TREES = ('v0.86.6-dev', 'v0.86-stable-kvm')
SHARED = (
    'main/application/passkey.c',
    'main/application/passkey.h',
    'main/application/two_step.c',
    'main/application/two_step.h',
    'main/services/web/passkey_http_module.inc',
    'main/services/web/two_step_http_module.inc',
)


def main() -> int:
    drift = [name for name in SHARED
             if len({(FIRMWARE / tree / name).read_bytes() for tree in TREES}) != 1]
    for name in drift:
        print(f'authentication factor copies differ: {name}', file=sys.stderr)
    if drift:
        return 1
    print(f'authentication factor copies identical across {len(TREES)} trees: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
