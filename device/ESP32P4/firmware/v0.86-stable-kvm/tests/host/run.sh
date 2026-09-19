#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
TEST_BIN="${TMPDIR:-/tmp}/exoanchor-stable-kvm-core-tests-$$"
STORE_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-stable-kvm-settings-store-tests-$$"

trap 'rm -f "$TEST_BIN" "$STORE_TEST_BIN"' EXIT INT TERM

"${CC:-cc}" \
    -std=c11 -Wall -Wextra -Werror \
    -I"$PROJECT_DIR/main/core" \
    "$PROJECT_DIR/main/core/device_observation_utils.c" \
    "$SCRIPT_DIR/test_core.c" \
    -o "$TEST_BIN"
"$TEST_BIN"

"${CC:-cc}" \
    -std=c11 -Wall -Wextra -Werror \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/infrastructure" \
    "$PROJECT_DIR/main/infrastructure/settings_store.c" \
    "$PROJECT_DIR/main/infrastructure/secret_store.c" \
    "$SCRIPT_DIR/test_settings_store.c" \
    -o "$STORE_TEST_BIN"
"$STORE_TEST_BIN"

"${PYTHON:-python3}" "$SCRIPT_DIR/check_web_contract.py"
