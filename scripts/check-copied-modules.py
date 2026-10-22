#!/usr/bin/env python3
"""Keep modules deliberately copied between independently installable packages identical.

Toolkit and the MCP bridge are installed separately, so shared transport and
parsing helpers are copied rather than imported. A fix to one copy must reach
the other; this check fails until both match.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
COPIES = (
    ('toolkit/exoanchor_toolkit/json_limits.py', 'integrations/exoanchor-mcp/exoanchor_mcp/json_limits.py'),
    ('toolkit/exoanchor_toolkit/tls_transport.py', 'integrations/exoanchor-mcp/exoanchor_mcp/tls_transport.py'),
)


def main() -> int:
    drift = [pair for pair in COPIES if (ROOT / pair[0]).read_bytes() != (ROOT / pair[1]).read_bytes()]
    for left, right in drift:
        print(f'copied modules differ: {left} != {right}', file=sys.stderr)
    if drift:
        return 1
    print(f'copied Toolkit/MCP modules identical ({len(COPIES)} pairs): PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
