#!/bin/sh
set -eu

toolkit_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

case "$(uname -s 2>/dev/null || true)" in
  Darwin|Linux) ;;
  *)
    echo "This launcher supports macOS and Linux. Use the Windows EXE on Windows." >&2
    exit 2
    ;;
esac

if [ "$#" -eq 0 ]; then
  set -- gui
fi

if command -v uv >/dev/null 2>&1; then
  exec uv run --project "$toolkit_dir" exoanchor-toolkit "$@"
fi

if command -v exoanchor-toolkit >/dev/null 2>&1; then
  exec exoanchor-toolkit "$@"
fi

echo "ExoAnchor Toolkit requires uv or an installed exoanchor-toolkit command." >&2
echo "Install uv from https://docs.astral.sh/uv/ and run this script again." >&2
exit 2
