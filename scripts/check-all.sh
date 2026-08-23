#!/bin/sh
set -eu

REPO_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MCP_PYTHON=${MCP_PYTHON:-python3}

run_toolkit_tests() {
    if [ -n "${TOOLKIT_PYTHON:-}" ]; then
        (
            cd "$REPO_DIR/toolkit"
            PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=. \
                "$TOOLKIT_PYTHON" -m unittest discover -s tests -v
        )
        return
    fi

    if command -v uv >/dev/null 2>&1; then
        (
            cd "$REPO_DIR/toolkit"
            PYTHONDONTWRITEBYTECODE=1 uv run --locked \
                python -m unittest discover -s tests -v
        )
        return
    fi

    if ! python3 -c 'import sys; raise SystemExit(sys.version_info < (3, 10))'; then
        echo "Toolkit tests require Python 3.10+ or uv" >&2
        exit 1
    fi
    (
        cd "$REPO_DIR/toolkit"
        PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=. \
            python3 -m unittest discover -s tests -v
    )
}

"$REPO_DIR/device/ESP32P4/firmware/v0.86-stable-kvm/tests/host/run.sh"
"$REPO_DIR/device/ESP32P4/firmware/v0.86.6-dev/tests/host/run.sh"

(
    cd "$REPO_DIR/integrations/exoanchor-mcp"
    PYTHONDONTWRITEBYTECODE=1 "$MCP_PYTHON" -m unittest discover -s tests -v
)

run_toolkit_tests
"$REPO_DIR/scripts/check-repository-hygiene.sh"

if git -C "$REPO_DIR" rev-parse --verify main >/dev/null 2>&1; then
    git -C "$REPO_DIR" diff --check main..HEAD
else
    git -C "$REPO_DIR" diff --check HEAD
fi

echo "all repository checks: PASS"
