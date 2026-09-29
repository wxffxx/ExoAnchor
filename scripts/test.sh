#!/bin/sh
# Offline validation only; never discovers, configures, or flashes a device.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PYTHON="${PYTHON:-python3}"
export PYTHON
"$PYTHON" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else "Repository tests require Python 3.10+; set PYTHON to its executable")'
"$ROOT/device/ESP32P4/firmware/v0.86.6-dev/tests/host/run.sh"
"$ROOT/device/ESP32P4/firmware/v0.86-stable-kvm/tests/host/run.sh"
(cd "$ROOT/integrations/exoanchor-mcp" && "$PYTHON" -m unittest discover -s tests -q)
(cd "$ROOT/toolkit" && "$PYTHON" -m unittest discover -s tests -q)
