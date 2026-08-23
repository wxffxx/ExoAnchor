#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf '%s\n' \
    'Run DeepSeek Harness with the canonical ExoAnchor MCP over stdio.' \
    '' \
    'Usage:' \
    '  run-dsh.sh [--read-only|--supervised] [--check] [--] [task]' \
    '' \
    'Required environment:' \
    '  EXOANCHOR_BASE_URL       Authenticated device base URL' \
    '  EXOANCHOR_DEVICE_ID      Exact configured device ID' \
    '  EXOANCHOR_USERNAME       Device web/API user' \
    '  EXOANCHOR_PASSWORD_FILE  Private password file (no group/other bits)' \
    '' \
    'Modes:' \
    '  --read-only   Reject device mutations (default).' \
    '  --supervised  Enable the MCP write gate; all device gates still apply.' \
    '  --check       Print composed DSH config and exit without MCP/model/device I/O.'
}

# Run DeepSeek Harness with the canonical ExoAnchor MCP over stdio.
#
# Usage:
#   run-dsh.sh [--read-only|--supervised] [--check] [--] [task]
#
# Required environment:
#   EXOANCHOR_BASE_URL       Authenticated device base URL
#   EXOANCHOR_DEVICE_ID      Exact configured device ID
#   EXOANCHOR_USERNAME       Device web/API user
#   EXOANCHOR_PASSWORD_FILE  Private password file (no group/other bits)
#
# Modes:
#   --read-only   Device mutations are rejected by the MCP bridge (default).
#   --supervised  Device writes are enabled, but device policy, access mode,
#                 lease, fresh-observation and explicit-confirmation gates
#                 still apply.
#   --check       Compose and print the DSH config, then exit. No MCP process,
#                 model request or device request is started.

mode=read-only
check_only=0
while (($#)); do
  case "$1" in
    --read-only)
      mode=read-only
      shift
      ;;
    --supervised)
      mode=supervised
      shift
      ;;
    --check)
      check_only=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      printf 'run-dsh: unknown option: %s\n' "$1" >&2
      exit 2
      ;;
    *)
      break
      ;;
  esac
done

for command_name in dsh python3; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'run-dsh: required command not found: %s\n' "$command_name" >&2
    exit 1
  fi
done

required_names=(
  EXOANCHOR_BASE_URL
  EXOANCHOR_DEVICE_ID
  EXOANCHOR_USERNAME
  EXOANCHOR_PASSWORD_FILE
)
for required_name in "${required_names[@]}"; do
  if [[ -z "${!required_name:-}" ]]; then
    printf 'run-dsh: required environment is unset: %s\n' "$required_name" >&2
    exit 2
  fi
done

case "$EXOANCHOR_BASE_URL" in
  http://*|https://*) ;;
  *)
    printf 'run-dsh: EXOANCHOR_BASE_URL must be an absolute HTTP(S) URL\n' >&2
    exit 2
    ;;
esac
if [[ "$EXOANCHOR_DEVICE_ID" =~ [[:space:]] ]]; then
  printf 'run-dsh: EXOANCHOR_DEVICE_ID must not contain whitespace\n' >&2
  exit 2
fi
if [[ ! -f "$EXOANCHOR_PASSWORD_FILE" || ! -r "$EXOANCHOR_PASSWORD_FILE" ]]; then
  printf 'run-dsh: EXOANCHOR_PASSWORD_FILE must be a readable regular file\n' >&2
  exit 2
fi

if secret_mode=$(stat -f '%Lp' "$EXOANCHOR_PASSWORD_FILE" 2>/dev/null); then
  :
elif secret_mode=$(stat -c '%a' "$EXOANCHOR_PASSWORD_FILE" 2>/dev/null); then
  :
else
  printf 'run-dsh: cannot inspect EXOANCHOR_PASSWORD_FILE permissions\n' >&2
  exit 2
fi
case "$secret_mode" in
  *00) ;;
  *)
    printf 'run-dsh: EXOANCHOR_PASSWORD_FILE must not grant group/other permissions (current mode %s)\n' "$secret_mode" >&2
    exit 2
    ;;
esac

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
export EXOANCHOR_MCP_ROOT
EXOANCHOR_MCP_ROOT=$(cd "$script_dir/.." && pwd -P)
repo_root=$(cd "$EXOANCHOR_MCP_ROOT/../.." && pwd -P)
export EXOANCHOR_SKILL_ROOT="$repo_root/skills"
patch_file="$EXOANCHOR_MCP_ROOT/docs/dsh/exoanchor.patch.yml"

if [[ ! -f "$patch_file" || ! -f "$EXOANCHOR_SKILL_ROOT/exoanchor-mcp-control/SKILL.md" ]]; then
  printf 'run-dsh: integration patch or ExoAnchor skill is missing\n' >&2
  exit 1
fi

export EXOANCHOR_STATE_DIR="${EXOANCHOR_STATE_DIR:-$HOME/.dsh/exoanchor-mcp-state}"
mkdir -p "$EXOANCHOR_STATE_DIR"
chmod 700 "$EXOANCHOR_STATE_DIR"

if [[ "$mode" == supervised ]]; then
  export EXOANCHOR_ALLOW_WRITE=1
else
  export EXOANCHOR_ALLOW_WRITE=0
fi
export EXOANCHOR_CONTROL_OWNER=mcp
export DSH_PERMISSION_MODE=read-only
export DSH_TOOLS_MODE=native
export DSH_TELEMETRY_MODE=DISABLED

dsh_profile="${DSH_PROFILE:-headless}"
if ((check_only)); then
  exec dsh --profile "$dsh_profile" --patch "$patch_file" --dump-config
fi

task="${*:-加载 exoanchor-mcp-control 技能，只执行 ExoAnchor 只读预检，并报告设备身份、能力和当前控制租约；不得执行写操作。}"
exec dsh --profile "$dsh_profile" --patch "$patch_file" "$task"
