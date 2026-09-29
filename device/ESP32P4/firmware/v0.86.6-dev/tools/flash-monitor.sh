#!/usr/bin/env bash
set -euo pipefail

PORT="${ESPPORT:-}"
BAUD="115200"
FLASH_BAUD="115200"
BUILD_DIR="${IDF_BUILD_DIR:-build-exoanchor-prototype-v2.3-rev3}"
WAIT_IP=0
EXIT_ON_IP=0
IP_TIMEOUT=60
NO_MONITOR=0

usage() {
  cat <<'EOF'
Usage:
  flash-monitor.sh [PORT] [options]

Options:
  --wait-ip          After flashing, read the serial log and extract the board IP.
  --exit-on-ip       Exit after the first IP is found. Implies --wait-ip.
  --ip-timeout SEC   Seconds to wait for an IP before failing. Default: 60.
  --baud BAUD        Serial baud rate used for IP monitor. Default: 115200.
  --flash-baud BAUD  Flash baud rate. Default: 115200.
  --build-dir DIR    ESP-IDF build directory. Default: build-exoanchor-prototype-v2.3-rev3.
  --no-monitor       Flash only; do not start monitor.
  -h, --help         Show this help.

Examples:
  ./tools/flash-monitor.sh /dev/cu.usbmodemDEVICE --wait-ip --exit-on-ip
  ./tools/flash-monitor.sh /dev/cu.usbmodemDEVICE --build-dir build-exoanchor-prototype0-idf5.5.5
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --wait-ip)
      WAIT_IP=1
      shift
      ;;
    --exit-on-ip)
      WAIT_IP=1
      EXIT_ON_IP=1
      shift
      ;;
    --ip-timeout)
      IP_TIMEOUT="${2:?missing value for --ip-timeout}"
      shift 2
      ;;
    --baud)
      BAUD="${2:?missing value for --baud}"
      shift 2
      ;;
    --flash-baud)
      FLASH_BAUD="${2:?missing value for --flash-baud}"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="${2:?missing value for --build-dir}"
      shift 2
      ;;
    --no-monitor)
      NO_MONITOR=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 64
      ;;
    *)
      PORT="$1"
      shift
      ;;
  esac
done

if [[ -z "$PORT" ]]; then
  shopt -s nullglob
  serial_candidates=(/dev/cu.usbmodem*)
  shopt -u nullglob
  if [[ "${#serial_candidates[@]}" -eq 1 ]]; then
    PORT="${serial_candidates[0]}"
  elif [[ "${#serial_candidates[@]}" -eq 0 ]]; then
    echo "[flash-monitor] No USB serial device found; pass PORT explicitly." >&2
    exit 66
  else
    echo "[flash-monitor] Multiple USB serial devices found; refusing automatic selection:" >&2
    printf '  %s\n' "${serial_candidates[@]}" >&2
    echo "[flash-monitor] Pass the intended PORT explicitly." >&2
    exit 64
  fi
fi

if [[ ! -e "$PORT" ]]; then
  echo "[flash-monitor] Serial device does not exist: $PORT" >&2
  exit 66
fi
if command -v lsof >/dev/null 2>&1; then
  busy_processes="$(lsof -t "$PORT" 2>/dev/null || true)"
  if [[ -n "$busy_processes" ]]; then
    echo "[flash-monitor] Serial device is already in use: $PORT (PID $busy_processes)" >&2
    exit 75
  fi
fi

if [[ -z "${IDF_PATH:-}" ]]; then
  IDF_EXPORT="${SI_IDF_EXPORT:-$HOME/esp/esp-idf-v5.5.5/export.sh}"
  if [[ ! -f "$IDF_EXPORT" ]]; then
    echo "[flash-monitor] ESP-IDF export script not found: $IDF_EXPORT" >&2
    echo "[flash-monitor] Set SI_IDF_EXPORT to the ESP-IDF version required by dependencies.lock." >&2
    exit 69
  fi
  # shellcheck source=/dev/null
  . "$IDF_EXPORT"
fi

cd "$(dirname "$0")/.."

if [[ ! -f "$BUILD_DIR/sdkconfig" ]]; then
  echo "[flash-monitor] Refusing to flash without an existing board-specific sdkconfig: $BUILD_DIR/sdkconfig" >&2
  echo "[flash-monitor] Configure the intended board profile first, then pass its directory with --build-dir." >&2
  exit 66
fi

board_id="$(grep -E '^CONFIG_SI_BOARD_ID=' "$BUILD_DIR/sdkconfig" | head -n1 | cut -d= -f2- | tr -d '\"' || true)"
if [[ -z "$board_id" ]]; then
  echo "[flash-monitor] Refusing to flash: CONFIG_SI_BOARD_ID is missing from $BUILD_DIR/sdkconfig" >&2
  exit 65
fi
echo "[flash-monitor] target board: $board_id (build directory: $BUILD_DIR)"

eth_enabled="$(grep -E '^CONFIG_SI_ETH_ENABLE=' "$BUILD_DIR/sdkconfig" | head -n1 | cut -d= -f2- || true)"
if [[ "$eth_enabled" == "y" ]] &&
   ! grep -qx "CONFIG_LWIP_AUTOIP=y" "$BUILD_DIR/sdkconfig"; then
  echo "[flash-monitor] Refusing Ethernet firmware without CONFIG_LWIP_AUTOIP." >&2
  echo "[flash-monitor] Rebuild from current defaults instead of flashing a stale build directory." >&2
  exit 65
fi

if [[ "$WAIT_IP" -eq 1 ]]; then
  if [[ "$eth_enabled" != "y" ]]; then
    echo "[flash-monitor] WARNING: CONFIG_SI_ETH_ENABLE is not enabled; no Ethernet IP will be assigned."
  fi
fi

idf.py -B "$BUILD_DIR" -p "$PORT" -b "$FLASH_BAUD" flash

if [[ "$NO_MONITOR" -eq 1 ]]; then
  exit 0
fi

if [[ "$WAIT_IP" -eq 1 ]]; then
  ip_monitor_args=(
    --port "$PORT"
    --baud "$BAUD"
    --timeout "$IP_TIMEOUT"
    --output "$BUILD_DIR/last_ip.txt"
  )
  if [[ "$EXIT_ON_IP" -eq 1 ]]; then
    ip_monitor_args+=(--exit-on-ip)
  fi
  python tools/serial-ip-monitor.py "${ip_monitor_args[@]}"
else
  idf.py -B "$BUILD_DIR" -p "$PORT" monitor
fi
