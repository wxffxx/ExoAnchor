#!/usr/bin/env bash
set -euo pipefail

PROFILE="dev"
BOARD="exoanchor-prototype-v2.3"
BUILD_DIR=""
REUSE_SDKCONFIG=0
STABLE_H264_CANDIDATE=0

usage() {
  cat <<'EOF'
Usage:
  build-firmware.sh [--profile dev|stable] [--board BOARD] [--build-dir DIR]
                    [--reuse-sdkconfig] [--stable-h264-candidate]

Profiles:
  dev       Embedded Agent runtime plus external MCP support.
  stable    External MCP support without the embedded Agent runtime.

Boards:
  exoanchor-prototype-v2.3 (default)
  exoanchor-prototype-v2.4 (V2.4 product profile, ESP32-P4 rev3)
  exoanchor-prototype-v2.4-ms-test (dedicated MS2109 hardware test)
  exoanchor-prototype-v2.1
  exoanchor-prototype0
  exoanchor-esp32p4x
  waveshare-p4-nano

Configuration:
  Official builds regenerate sdkconfig from the selected defaults on every
  invocation. Use --reuse-sdkconfig only for an intentional local menuconfig
  experiment; release-critical video settings are still verified afterwards.

  --stable-h264-candidate
      Build an offline H.264 candidate from the Stable composition. This is
      accepted only with --profile stable --board exoanchor-prototype-v2.3;
      normal Stable builds remain MJPEG-only.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)
      PROFILE="${2:?missing value for --profile}"
      shift 2
      ;;
    --board)
      BOARD="${2:?missing value for --board}"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="${2:?missing value for --build-dir}"
      shift 2
      ;;
    --reuse-sdkconfig)
      REUSE_SDKCONFIG=1
      shift
      ;;
    --stable-h264-candidate)
      STABLE_H264_CANDIDATE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[build-firmware] unknown option: $1" >&2
      usage >&2
      exit 64
      ;;
  esac
done

case "$PROFILE" in
  dev|stable) ;;
  *)
    echo "[build-firmware] profile must be dev or stable" >&2
    exit 64
    ;;
esac

case "$BOARD" in
  exoanchor-prototype-v2.4|exoanchor-prototype-v2.4-ms-test|exoanchor-prototype-v2.3|exoanchor-prototype-v2.1|exoanchor-prototype0)
    SILICON="esp32p4-rev3"
    ;;
  exoanchor-esp32p4x|waveshare-p4-nano)
    SILICON="esp32p4-rev1"
    ;;
  *)
    echo "[build-firmware] unsupported board: $BOARD" >&2
    exit 64
    ;;
esac

if [[ "$STABLE_H264_CANDIDATE" -eq 1 ]] &&
   [[ "$PROFILE" != "stable" ||
      "$BOARD" != "exoanchor-prototype-v2.3" ||
      "$SILICON" != "esp32p4-rev3" ]]; then
  echo "[build-firmware] --stable-h264-candidate only supports --profile stable --board exoanchor-prototype-v2.3 (esp32p4-rev3)" >&2
  exit 64
fi

if [[ "$BOARD" == "exoanchor-prototype-v2.4-ms-test" &&
      "$PROFILE" != "stable" ]]; then
  echo "[build-firmware] the V2.4 MS test image uses the lean Stable composition plus UART0 diagnostics" >&2
  echo "[build-firmware] rebuild with --profile stable --board exoanchor-prototype-v2.4-ms-test" >&2
  exit 64
fi

if ! command -v idf.py >/dev/null 2>&1; then
  IDF_EXPORT="${SI_IDF_EXPORT:-${HOME}/esp/esp-idf-v5.5.5/export.sh}"
  if [[ ! -f "$IDF_EXPORT" ]]; then
    echo "[build-firmware] ESP-IDF is not active and export script was not found: $IDF_EXPORT" >&2
    echo "[build-firmware] activate ESP-IDF or set SI_IDF_EXPORT" >&2
    exit 69
  fi
  # shellcheck source=/dev/null
  . "$IDF_EXPORT"
fi

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

VERSION_BASE="$(sed -n 's/^set(SI_VERSION_BASE "\([^"]*\)")$/\1/p' CMakeLists.txt)"
if [[ -z "$VERSION_BASE" ]]; then
  echo "[build-firmware] missing SI_VERSION_BASE in CMakeLists.txt" >&2
  exit 70
fi
if [[ "$STABLE_H264_CANDIDATE" -eq 1 ]]; then
  FIRMWARE_VERSION="${VERSION_BASE}-Stable-H264-Candidate"
elif [[ "$BOARD" == "exoanchor-prototype-v2.4-ms-test" ]]; then
  FIRMWARE_VERSION="${VERSION_BASE}-v2.4-ms-test"
else
  case "$PROFILE" in
    dev) FIRMWARE_VERSION="${VERSION_BASE}-dev" ;;
    stable) FIRMWARE_VERSION="${VERSION_BASE}-Stable" ;;
  esac
fi
if [[ -z "$BUILD_DIR" ]]; then
  if [[ "$STABLE_H264_CANDIDATE" -eq 1 ]]; then
    BUILD_DIR="build-${BOARD}-stable-h264-candidate"
  else
    BUILD_DIR="build-${BOARD}-${PROFILE}"
  fi
fi
if [[ "$BUILD_DIR" == /* ]]; then
  BUILD_DIR_PATH="$BUILD_DIR"
else
  BUILD_DIR_PATH="$PROJECT_DIR/$BUILD_DIR"
fi
SDKCONFIG_PATH="$BUILD_DIR_PATH/sdkconfig"

DEFAULTS="sdkconfig.defaults;configs/boards/sdkconfig.defaults.${BOARD};configs/silicon/sdkconfig.defaults.${SILICON};configs/profiles/sdkconfig.defaults.${PROFILE}"
H264_BUILD=0
if [[ "$PROFILE" == "dev" && "$SILICON" == "esp32p4-rev3" ]] ||
   [[ "$STABLE_H264_CANDIDATE" -eq 1 ]]; then
  H264_BUILD=1
  DEFAULTS="${DEFAULTS};configs/profiles/sdkconfig.defaults.dev-h264"
fi

if [[ "$REUSE_SDKCONFIG" -eq 0 ]]; then
  # sdkconfig is generated state, not an input to an official product build.
  # Keeping it silently preserves values from older defaults and previously
  # selected profiles, which can change UVC DMA topology without changing the
  # build command. Recreate it deterministically while retaining object files.
  rm -f -- "$SDKCONFIG_PATH" "${SDKCONFIG_PATH}.old"
fi

echo "[build-firmware] board=$BOARD silicon=$SILICON profile=$PROFILE stable_h264_candidate=$STABLE_H264_CANDIDATE version=$FIRMWARE_VERSION"
echo "[build-firmware] build_dir=$BUILD_DIR_PATH"
idf.py -B "$BUILD_DIR_PATH" \
  -D "IDF_TARGET=esp32p4" \
  -D "PROJECT_VER=$FIRMWARE_VERSION" \
  -D "SDKCONFIG=$SDKCONFIG_PATH" \
  -D "SDKCONFIG_DEFAULTS=$DEFAULTS" build

SDKCONFIG="$SDKCONFIG_PATH"
if [[ ! -f "$SDKCONFIG" ]]; then
  echo "[build-firmware] missing generated sdkconfig: $SDKCONFIG" >&2
  exit 70
fi

case "$PROFILE" in
  dev) EXPECTED_PROFILE="CONFIG_SI_BUILD_PROFILE_DEV=y" ;;
  stable) EXPECTED_PROFILE="CONFIG_SI_BUILD_PROFILE_STABLE=y" ;;
esac
if ! grep -qx "$EXPECTED_PROFILE" "$SDKCONFIG"; then
  echo "[build-firmware] generated sdkconfig does not match profile: $PROFILE" >&2
  exit 70
fi
if ! grep -qx 'CONFIG_SI_AUTH_PASSWORD=""' "$SDKCONFIG"; then
  echo "[build-firmware] refusing a shared build-time web password" >&2
  echo "[build-firmware] leave CONFIG_SI_AUTH_PASSWORD empty to use the restricted factory-claim flow" >&2
  exit 70
fi
if ! grep -qx 'CONFIG_SI_AUTH_USERNAME="admin"' "$SDKCONFIG"; then
  echo "[build-firmware] official factory-claim flow requires username admin" >&2
  exit 70
fi
for expected_video_config in \
  'CONFIG_SI_VIDEO_ENABLE=y' \
  'CONFIG_SI_VIDEO_WIDTH=1920' \
  'CONFIG_SI_VIDEO_HEIGHT=1080' \
  'CONFIG_SI_VIDEO_UVC_FPS=25' \
  'CONFIG_SI_VIDEO_UVC_FRAME_BUFFERS=3' \
  'CONFIG_SI_VIDEO_UVC_URBS=4' \
  'CONFIG_SI_VIDEO_UVC_URB_SIZE=10240' \
  'CONFIG_SI_VIDEO_UVC_JPEG_BUFFER_SIZE=2097152'; do
  if ! grep -qx "$expected_video_config" "$SDKCONFIG"; then
    echo "[build-firmware] video baseline mismatch: expected $expected_video_config" >&2
    echo "[build-firmware] use a fresh configuration or remove --reuse-sdkconfig" >&2
    exit 70
  fi
done
if [[ "$H264_BUILD" -eq 1 ]]; then
  if ! grep -qx 'CONFIG_SI_VIDEO_H264_EXPERIMENT=y' "$SDKCONFIG" ||
     ! grep -qx 'CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=262144' "$SDKCONFIG"; then
    echo "[build-firmware] H.264 build is missing its codec resource contract" >&2
    exit 70
  fi
else
  if grep -qx 'CONFIG_SI_VIDEO_H264_EXPERIMENT=y' "$SDKCONFIG" ||
     ! grep -qx 'CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=150000' "$SDKCONFIG"; then
    echo "[build-firmware] Stable/non-H.264 build does not match the Stable video memory baseline" >&2
    exit 70
  fi
fi
if [[ "$PROFILE" == "dev" ]] &&
   ! grep -qx "CONFIG_SI_EMBEDDED_AGENT=y" "$SDKCONFIG"; then
  echo "[build-firmware] Dev build is missing CONFIG_SI_EMBEDDED_AGENT" >&2
  exit 70
fi
if [[ "$PROFILE" == "dev" ]]; then
  for expected_agent_tls_config in \
    'CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y' \
    'CONFIG_MBEDTLS_DYNAMIC_BUFFER=y' \
    'CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384' \
    'CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048' \
    'CONFIG_MBEDTLS_AES_C=y' \
    'CONFIG_MBEDTLS_GCM_C=y' \
    'CONFIG_SPIRAM_USE_MALLOC=y' \
    'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096'; do
    if ! grep -qx "$expected_agent_tls_config" "$SDKCONFIG"; then
      echo "[build-firmware] embedded Agent TLS memory contract mismatch: expected $expected_agent_tls_config" >&2
      exit 70
    fi
  done
  if grep -qx 'CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y' "$SDKCONFIG"; then
    echo "[build-firmware] embedded Agent TLS buffers are still forced into internal RAM" >&2
    exit 70
  fi
  if ! grep -qx '# CONFIG_MBEDTLS_HARDWARE_AES is not set' "$SDKCONFIG"; then
    echo "[build-firmware] Dev Agent build must avoid DMA-dependent hardware AES" >&2
    exit 70
  fi
  for forbidden_agent_tls_config in \
    'CONFIG_MBEDTLS_HARDWARE_GCM=y' \
    'CONFIG_MBEDTLS_AES_USE_INTERRUPT=y'; do
    if grep -qx "$forbidden_agent_tls_config" "$SDKCONFIG"; then
      echo "[build-firmware] Dev Agent crypto fallback unexpectedly enabled: $forbidden_agent_tls_config" >&2
      exit 70
    fi
  done
fi
if [[ "$PROFILE" == "stable" ]] &&
   grep -qx "CONFIG_SI_EMBEDDED_AGENT=y" "$SDKCONFIG"; then
  echo "[build-firmware] Stable build unexpectedly contains the embedded Agent" >&2
  exit 70
fi
if [[ "$PROFILE" == "dev" ]] &&
   grep -qx "CONFIG_SI_TARGET_UART_ENABLE=y" "$SDKCONFIG" &&
   ! grep -qx "CONFIG_SI_UART_TERMINAL=y" "$SDKCONFIG"; then
  echo "[build-firmware] Dev target UART build is missing CONFIG_SI_UART_TERMINAL" >&2
  exit 70
fi
if grep -qx "CONFIG_SI_VIDEO_H264_EXPERIMENT=y" "$SDKCONFIG" &&
   ! grep -qx "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=262144" "$SDKCONFIG"; then
  echo "[build-firmware] H.264 build is missing the protected internal/DMA heap" >&2
  exit 70
fi
if [[ "$PROFILE" == "stable" ]] &&
   grep -qx "CONFIG_SI_UART_TERMINAL=y" "$SDKCONFIG"; then
  echo "[build-firmware] Stable build unexpectedly contains the UART Terminal" >&2
  exit 70
fi
if grep -qx "CONFIG_SI_ETH_ENABLE=y" "$SDKCONFIG" &&
   ! grep -qx "CONFIG_LWIP_AUTOIP=y" "$SDKCONFIG"; then
  echo "[build-firmware] Ethernet build is missing CONFIG_LWIP_AUTOIP" >&2
  echo "[build-firmware] Refusing a product build without DHCP/AutoIP recovery" >&2
  exit 70
fi
if [[ "$BOARD" != "exoanchor-prototype-v2.4-ms-test" ]] &&
   grep -qx "CONFIG_SI_MS2109_TEST_ENABLE=y" "$SDKCONFIG"; then
  echo "[build-firmware] refusing product build with V2.4 MS2109 EEPROM test controls" >&2
  exit 70
fi
if [[ "$BOARD" == "exoanchor-prototype-v2.4-ms-test" ]]; then
  if ! grep -qx "CONFIG_SI_MS2109_TEST_ENABLE=y" "$SDKCONFIG"; then
    echo "[build-firmware] V2.4 MS test build is missing its hardware test controls" >&2
    exit 70
  fi
  if grep -qx "CONFIG_SI_MS2109_POWER_ENABLE=y" "$SDKCONFIG"; then
    echo "[build-firmware] refusing V2.4 EEPROM test build with production MS2109 power driver" >&2
    exit 70
  fi
  if grep -qx "CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE=y" "$SDKCONFIG"; then
    echo "[build-firmware] refusing V2.4 physical EEPROM build with emulator enabled" >&2
    exit 70
  fi
  for expected_gpio in \
    'CONFIG_SI_MS2109_SWITCH_GPIO=13' \
    'CONFIG_SI_MS2109_CORE_ENABLE_GPIO=18' \
    'CONFIG_SI_MS2109_EEPROM_WP_GPIO=16' \
    'CONFIG_SI_MS2109_EEPROM_SCL_GPIO=47' \
    'CONFIG_SI_MS2109_EEPROM_SDA_GPIO=48'; do
    if ! grep -qx "$expected_gpio" "$SDKCONFIG"; then
      echo "[build-firmware] V2.4 MS test GPIO mismatch: expected $expected_gpio" >&2
      exit 70
    fi
  done
fi
if [[ "$BOARD" == "exoanchor-prototype-v2.4" ]]; then
  if ! grep -qx "CONFIG_SI_MS2109_POWER_ENABLE=y" "$SDKCONFIG"; then
    echo "[build-firmware] PrototypeV2.4 product build is missing MS2109 power sequencing" >&2
    exit 70
  fi
  if grep -qx "CONFIG_SI_MS2109_TEST_ENABLE=y" "$SDKCONFIG" ||
     grep -qx "CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE=y" "$SDKCONFIG"; then
    echo "[build-firmware] refusing PrototypeV2.4 product build with MS2109 EEPROM test/emulator enabled" >&2
    exit 70
  fi
  for expected_setting in \
    'CONFIG_SI_BOARD_EXOANCHOR_PROTOTYPE_V24=y' \
    'CONFIG_SI_SILICON_TARGET="esp32p4-rev3"' \
    'CONFIG_ESP32P4_REV_MIN_300=y' \
    'CONFIG_SI_ETH_PHY_DP83825=y' \
    'CONFIG_SI_MS2109_SWITCH_GPIO=13' \
    'CONFIG_SI_MS2109_CORE_ENABLE_GPIO=18' \
    'CONFIG_SI_MS2109_EEPROM_WP_GPIO=16' \
    'CONFIG_SI_MS2109_EEPROM_SCL_GPIO=47' \
    'CONFIG_SI_MS2109_EEPROM_SDA_GPIO=48' \
    'CONFIG_SI_POWER_BUTTON_GPIO=4' \
    'CONFIG_SI_RESET_BUTTON_GPIO=5' \
    'CONFIG_SI_POWER_DETECT_GPIO=0' \
    'CONFIG_SI_STANDBY_DETECT_GPIO=1' \
    'CONFIG_SI_POWER_LOCATOR_GPIO=17' \
    'CONFIG_SI_POWER_LOCATOR_RETURN_GPIO=-1' \
    'CONFIG_SI_TF_CARD_DETECT_GPIO=-1' \
    'CONFIG_SI_TARGET_UART_ENABLE=y' \
    'CONFIG_SI_TARGET_UART_PORT=1' \
    'CONFIG_SI_TARGET_UART_RX_GPIO=50' \
    'CONFIG_SI_TARGET_UART_TX_GPIO=51' \
    'CONFIG_SI_HID_USB_DM_GPIO=26' \
    'CONFIG_SI_HID_USB_DP_GPIO=27'; do
    if ! grep -qx "$expected_setting" "$SDKCONFIG"; then
      echo "[build-firmware] PrototypeV2.4 product profile mismatch: expected $expected_setting" >&2
      exit 70
    fi
  done
fi
PROJECT_DESCRIPTION="$BUILD_DIR_PATH/project_description.json"
if [[ ! -f "$PROJECT_DESCRIPTION" ]]; then
  echo "[build-firmware] missing project description: $PROJECT_DESCRIPTION" >&2
  exit 70
fi
ACTUAL_VERSION="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["project_version"])' "$PROJECT_DESCRIPTION")"
if [[ "$ACTUAL_VERSION" != "$FIRMWARE_VERSION" ]]; then
  echo "[build-firmware] version mismatch: expected=$FIRMWARE_VERSION actual=$ACTUAL_VERSION" >&2
  exit 70
fi

FIRMWARE_ELF="$BUILD_DIR_PATH/si_esphost_esp32p4.elf"
NM_TOOL="$(command -v riscv32-esp-elf-nm || command -v riscv32-esp-elf-gcc-nm || true)"
if [[ ! -f "$FIRMWARE_ELF" || -z "$NM_TOOL" ]]; then
  echo "[build-firmware] cannot audit profile symbols" >&2
  exit 70
fi
PROFILE_SYMBOLS="$($NM_TOOL "$FIRMWARE_ELF")"
STRINGS_TOOL="$(command -v strings || true)"
if [[ -z "$STRINGS_TOOL" ]]; then
  echo "[build-firmware] cannot audit firmware strings" >&2
  exit 70
fi
FIRMWARE_STRINGS="$($STRINGS_TOOL -a "$FIRMWARE_ELF")"
if [[ "$PROFILE" == "dev" ]]; then
  for symbol in agent_run_start; do
    if ! grep -Eq "[[:space:]]${symbol}$" <<<"$PROFILE_SYMBOLS"; then
      echo "[build-firmware] Dev build is missing runtime symbol: $symbol" >&2
      exit 70
    fi
  done
  if grep -qx "CONFIG_SI_UART_TERMINAL=y" "$SDKCONFIG"; then
    for symbol in terminal_handler _binary_terminal_html_start; do
      if ! grep -Eq "[[:space:]]${symbol}$" <<<"$PROFILE_SYMBOLS"; then
        echo "[build-firmware] Dev UART Terminal build is missing runtime symbol: $symbol" >&2
        exit 70
      fi
    done
  fi
else
  for symbol in agent_run_start terminal_handler _binary_terminal_html_start xterm_css_handler; do
    if grep -Eq "[[:space:]]${symbol}$" <<<"$PROFILE_SYMBOLS"; then
      echo "[build-firmware] Stable build contains excluded runtime symbol: $symbol" >&2
      exit 70
    fi
  done
fi

MS2109_EEPROM_SYMBOLS=(
  si_ms2109_test_probe_eeprom
  si_ms2109_test_read_eeprom
  si_ms2109_test_program_eeprom
  ms2109_eeprom_probe_handler
  ms2109_eeprom_read_handler
  ms2109_eeprom_program_handler
)
MS2109_EEPROM_TEST_STRINGS=(
  "/api/ms2109/eeprom/probe"
  "/api/ms2109/eeprom/read"
  "/api/ms2109/eeprom/program"
  "ms-eeprom program-b64"
  "PROGRAM MS2109 EEPROM"
)
MS2109_EEPROM_WP_STRINGS=(
  "eeprom-wp write-enable"
)
if [[ "$BOARD" == "exoanchor-prototype-v2.4-ms-test" ]]; then
  for symbol in "${MS2109_EEPROM_SYMBOLS[@]}"; do
    if ! grep -Eq "[[:space:]]${symbol}$" <<<"$PROFILE_SYMBOLS"; then
      echo "[build-firmware] V2.4 MS test build is missing EEPROM symbol: $symbol" >&2
      exit 70
    fi
  done
  for marker in "${MS2109_EEPROM_TEST_STRINGS[@]}"; do
    if ! grep -Fq "$marker" <<<"$FIRMWARE_STRINGS"; then
      echo "[build-firmware] V2.4 MS test build is missing EEPROM surface marker: $marker" >&2
      exit 70
    fi
  done
else
  for symbol in "${MS2109_EEPROM_SYMBOLS[@]}"; do
    if grep -Eq "[[:space:]]${symbol}$" <<<"$PROFILE_SYMBOLS"; then
      echo "[build-firmware] product build contains excluded MS2109 EEPROM symbol: $symbol" >&2
      exit 70
    fi
  done
  for marker in "${MS2109_EEPROM_TEST_STRINGS[@]}"; do
    if grep -Fq "$marker" <<<"$FIRMWARE_STRINGS"; then
      echo "[build-firmware] product build contains excluded MS2109 EEPROM surface marker: $marker" >&2
      exit 70
    fi
  done
fi

if grep -Eq '^CONFIG_SI_MS2109_EEPROM_WP_GPIO=[0-9]+$' "$SDKCONFIG"; then
  for marker in "${MS2109_EEPROM_WP_STRINGS[@]}"; do
    if ! grep -Fq "$marker" <<<"$FIRMWARE_STRINGS"; then
      echo "[build-firmware] EEPROM-WP-capable build is missing UART marker: $marker" >&2
      exit 70
    fi
  done
else
  for marker in "${MS2109_EEPROM_WP_STRINGS[@]}"; do
    if grep -Fq "$marker" <<<"$FIRMWARE_STRINGS"; then
      echo "[build-firmware] build without EEPROM WP GPIO contains UART marker: $marker" >&2
      exit 70
    fi
  done
fi

if grep -qx "CONFIG_SI_MS2109_POWER_ENABLE=y" "$SDKCONFIG"; then
  for symbol in si_ms2109_power_get_status si_ms2109_power_set \
                si_ms2109_power_cycle ms2109_power_handler; do
    if ! grep -Eq "[[:space:]]${symbol}$" <<<"$PROFILE_SYMBOLS"; then
      echo "[build-firmware] MS2109 power-control build is missing symbol: $symbol" >&2
      exit 70
    fi
  done
elif [[ "$BOARD" == "exoanchor-prototype-v2.4-ms-test" ]]; then
  for symbol in si_ms2109_test_set_power si_ms2109_test_cycle_power \
                ms2109_power_handler; do
    if ! grep -Eq "[[:space:]]${symbol}$" <<<"$PROFILE_SYMBOLS"; then
      echo "[build-firmware] V2.4 test build is missing power-control symbol: $symbol" >&2
      exit 70
    fi
  done
fi

if [[ "$H264_BUILD" -eq 1 ]]; then
  for symbol in si_h264_stream_initialize si_h264_stream_ws_handler; do
    if ! grep -Eq "[[:space:]]${symbol}$" <<<"$PROFILE_SYMBOLS"; then
      echo "[build-firmware] H.264 build is missing runtime symbol: $symbol" >&2
      exit 70
    fi
  done
else
  for symbol in si_h264_stream_initialize si_h264_stream_ws_handler; do
    if grep -Eq "[[:space:]]${symbol}$" <<<"$PROFILE_SYMBOLS"; then
      echo "[build-firmware] non-H.264 build contains excluded runtime symbol: $symbol" >&2
      exit 70
    fi
  done
fi

echo "[build-firmware] verified profile=$PROFILE stable_h264_candidate=$STABLE_H264_CANDIDATE version=$FIRMWARE_VERSION"
