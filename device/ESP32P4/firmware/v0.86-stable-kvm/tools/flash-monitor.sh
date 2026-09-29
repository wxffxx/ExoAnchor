#!/usr/bin/env bash
set -euo pipefail

PORT="${ESPPORT:-}"
BAUD="115200"
FLASH_BAUD="115200"
BUILD_DIR="${IDF_BUILD_DIR:-build}"
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
  --build-dir DIR    ESP-IDF build directory. Default: build.
  --no-monitor       Flash only; do not start monitor.
  -h, --help         Show this help.

Examples:
  ./tools/flash-monitor.sh /dev/cu.usbmodemDEVICE
  ./tools/flash-monitor.sh /dev/cu.usbmodemDEVICE --build-dir build-prototype0 --wait-ip --exit-on-ip
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
  echo "[flash-monitor] Pass the exact serial PORT explicitly or set ESPPORT." >&2
  exit 64
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
  # shellcheck source=/dev/null
  . "$HOME/esp/esp-idf-v5.4/export.sh"
fi

cd "$(dirname "$0")/.."

if [[ "$WAIT_IP" -eq 1 && -f "$BUILD_DIR/sdkconfig" ]]; then
  eth_enabled="$(grep -E '^CONFIG_SI_ETH_ENABLE=' "$BUILD_DIR/sdkconfig" | head -n1 | cut -d= -f2- || true)"
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
