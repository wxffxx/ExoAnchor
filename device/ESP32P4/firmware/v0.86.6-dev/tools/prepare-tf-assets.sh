#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 /Volumes/TF_CARD_OR_MOUNT_ROOT" >&2
  exit 2
fi

target_root="$1"
mkdir -p \
  "${target_root%/}/EA/ASSETS" \
  "${target_root%/}/EA/AGENT" \
  "${target_root%/}/EA/EXPORTS" \
  "${target_root%/}/EA/LOGS" \
  "${target_root%/}/EA/MCP" \
  "${target_root%/}/EA/OTA" \
  "${target_root%/}/EA/SNAPSHOTS"

echo "TF assets prepared at ${target_root%/}/EA"
