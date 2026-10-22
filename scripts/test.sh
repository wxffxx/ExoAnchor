#!/bin/sh
# Offline validation only; never discovers, configures, or flashes a device.
# Kept as a compatibility entry point: scripts/check-all.sh is the full gate.
set -eu
exec "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/check-all.sh" "$@"
