#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
CJSON_DIR="$SCRIPT_DIR/vendor/cjson"

# Host tests use the same pinned cJSON source as ESP-IDF 5.5.5, without
# requiring an SDK checkout or a network download on contributors' machines.
for tool in "${CC:-cc}" python3 node; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "host tests require $tool; see the firmware README" >&2
        exit 1
    fi
done
if ! python3 -c 'import sys; raise SystemExit(sys.version_info < (3, 10))'; then
    echo "host tests require Python 3.10+" >&2
    exit 1
fi
if ! node -e 'process.exit(Number(process.versions.node.split(".")[0]) < 18 ? 1 : 0)'; then
    echo "host tests require Node.js 18+" >&2
    exit 1
fi
for source in cJSON.c cJSON.h; do
    if [ ! -f "$CJSON_DIR/$source" ]; then
        echo "host test dependency missing: vendor/cjson/$source" >&2
        exit 1
    fi
done

TEST_BIN="${TMPDIR:-/tmp}/exoanchor-firmware-core-tests-$$"
STORE_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-firmware-settings-store-tests-$$"
SSH_HOSTKEY_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-firmware-ssh-hostkey-tests-$$"
HID_JSON_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-firmware-hid-json-tests-$$"
HID_ABORT_FENCE_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-hid-abort-fence-tests-$$"
HID_OWNER_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-hid-owner-tests-$$"
NETWORK_SETTINGS_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-firmware-network-settings-tests-$$"
PRODUCT_FEATURE_SETTINGS_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-product-feature-settings-tests-$$"
RELAY_PROTOCOL_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-firmware-relay-protocol-tests-$$"
VIDEO_MJPEG_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-firmware-video-mjpeg-tests-$$"
VIDEO_FRAME_STORE_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-firmware-video-frame-store-tests-$$"
PAGE_CONTEXT_CHECKPOINT_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-firmware-page-context-checkpoint-tests-$$"
AGENT_TASK_RUNTIME_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-agent-task-runtime-tests-$$"
AGENT_TASK_SERVICE_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-agent-task-service-tests-$$"
AGENT_TASK_CANCEL_GATE_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-agent-task-cancel-gate-tests-$$"
AGENT_EXECUTION_GUARD_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-agent-execution-guard-tests-$$"
AGENT_VERIFIER_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-agent-verifier-tests-$$"
AGENT_REQUEST_BROKER_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-agent-request-broker-tests-$$"
AGENT_EVENT_STORE_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-agent-event-store-tests-$$"
AGENT_HISTORY_WRITER_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-agent-history-writer-tests-$$"
AGENT_ENDPOINT_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-agent-endpoint-tests-$$"
AGENT_RESULT_ERROR_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-agent-result-error-tests-$$"
DIAGNOSTICS_FORMATTER_TEST_BIN="${TMPDIR:-/tmp}/exoanchor-diagnostics-formatter-tests-$$"
AGENT_EVENT_STORE_TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/exoanchor-agent-event-store.XXXXXX")

trap 'rm -f "$TEST_BIN" "$STORE_TEST_BIN" "$SSH_HOSTKEY_TEST_BIN" "$HID_JSON_TEST_BIN" "$HID_ABORT_FENCE_TEST_BIN" "$HID_OWNER_TEST_BIN" "$NETWORK_SETTINGS_TEST_BIN" "$PRODUCT_FEATURE_SETTINGS_TEST_BIN" "$RELAY_PROTOCOL_TEST_BIN" "$VIDEO_MJPEG_TEST_BIN" "$VIDEO_FRAME_STORE_TEST_BIN" "$PAGE_CONTEXT_CHECKPOINT_TEST_BIN" "$AGENT_TASK_RUNTIME_TEST_BIN" "$AGENT_TASK_SERVICE_TEST_BIN" "$AGENT_TASK_CANCEL_GATE_TEST_BIN" "$AGENT_EXECUTION_GUARD_TEST_BIN" "$AGENT_VERIFIER_TEST_BIN" "$AGENT_REQUEST_BROKER_TEST_BIN" "$AGENT_EVENT_STORE_TEST_BIN" "$AGENT_HISTORY_WRITER_TEST_BIN" "$AGENT_ENDPOINT_TEST_BIN" "$AGENT_RESULT_ERROR_TEST_BIN" "$DIAGNOSTICS_FORMATTER_TEST_BIN"; rm -rf "$AGENT_EVENT_STORE_TEST_DIR"' EXIT INT TERM

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$PROJECT_DIR/main/core" \
    "$PROJECT_DIR/main/core/action_event.c" \
    "$PROJECT_DIR/main/core/agent_router.c" \
    "$PROJECT_DIR/main/core/authorization.c" \
    "$PROJECT_DIR/main/core/boot_key_sequence.c" \
    "$PROJECT_DIR/main/core/device_observation_utils.c" \
    "$PROJECT_DIR/main/core/host_display_mode.c" \
    "$PROJECT_DIR/main/core/hid_ascii.c" \
    "$PROJECT_DIR/main/core/model_provider.c" \
    "$PROJECT_DIR/main/core/network_config.c" \
    "$PROJECT_DIR/main/core/discovery_rate_limit.c" \
    "$PROJECT_DIR/main/core/product_identity.c" \
    "$PROJECT_DIR/main/core/utf8_utils.c" \
    "$PROJECT_DIR/main/core/version_utils.c" \
    "$PROJECT_DIR/main/core/storage_path.c" \
    "$PROJECT_DIR/main/core/terminal_control_state.c" \
    "$PROJECT_DIR/main/core/resource_operation_state.c" \
    "$SCRIPT_DIR/test_core.c" \
    -o "$TEST_BIN"

"$TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/application" \
    -I"$PROJECT_DIR/main/core" \
    -I"$PROJECT_DIR/main/infrastructure" \
    "$PROJECT_DIR/main/application/console_credentials.c" \
    "$PROJECT_DIR/main/core/hid_ascii.c" \
    "$PROJECT_DIR/main/infrastructure/settings_store.c" \
    "$PROJECT_DIR/main/infrastructure/settings_schema.c" \
    "$PROJECT_DIR/main/infrastructure/secret_store.c" \
    "$SCRIPT_DIR/test_settings_store.c" \
    -o "$STORE_TEST_BIN"

"$STORE_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/services" \
    "$PROJECT_DIR/main/services/ssh_hostkey_store.c" \
    "$SCRIPT_DIR/test_ssh_hostkey_store.c" \
    -o "$SSH_HOSTKEY_TEST_BIN"

"$SSH_HOSTKEY_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/application" \
    -I"$PROJECT_DIR/main/core" \
    -I"$PROJECT_DIR/main/infrastructure" \
    "$PROJECT_DIR/main/application/network_settings.c" \
    "$PROJECT_DIR/main/core/network_config.c" \
    "$PROJECT_DIR/main/core/product_identity.c" \
    "$PROJECT_DIR/main/infrastructure/settings_store.c" \
    "$SCRIPT_DIR/test_network_settings.c" \
    -o "$NETWORK_SETTINGS_TEST_BIN"

"$NETWORK_SETTINGS_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/application" \
    -I"$PROJECT_DIR/main/infrastructure" \
    "$PROJECT_DIR/main/application/device_settings.c" \
    "$SCRIPT_DIR/test_product_feature_settings.c" \
    -o "$PRODUCT_FEATURE_SETTINGS_TEST_BIN"

"$PRODUCT_FEATURE_SETTINGS_TEST_BIN" failure-retry
"$PRODUCT_FEATURE_SETTINGS_TEST_BIN" missing-defaults
"$PRODUCT_FEATURE_SETTINGS_TEST_BIN" read-failure-retry

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$PROJECT_DIR/main/core" \
    "$PROJECT_DIR/main/core/relay_protocol.c" \
    "$SCRIPT_DIR/test_relay_protocol.c" \
    -o "$RELAY_PROTOCOL_TEST_BIN"

"$RELAY_PROTOCOL_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$PROJECT_DIR/main/drivers" \
    "$PROJECT_DIR/main/drivers/video_mjpeg.c" \
    "$SCRIPT_DIR/test_video_mjpeg.c" \
    -o "$VIDEO_MJPEG_TEST_BIN"

"$VIDEO_MJPEG_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/drivers" \
    "$PROJECT_DIR/main/drivers/video_frame_store.c" \
    "$SCRIPT_DIR/test_video_frame_store.c" \
    -o "$VIDEO_FRAME_STORE_TEST_BIN"

"$VIDEO_FRAME_STORE_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/core" \
    -I"$CJSON_DIR" \
    "$PROJECT_DIR/main/core/agent_page_context_checkpoint.c" \
    "$CJSON_DIR/cJSON.c" \
    "$SCRIPT_DIR/test_agent_page_context_checkpoint.c" \
    -o "$PAGE_CONTEXT_CHECKPOINT_TEST_BIN"

"$PAGE_CONTEXT_CHECKPOINT_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/adapters" \
    -I"$PROJECT_DIR/main/core" \
    -I"$PROJECT_DIR/main/drivers" \
    -I"$CJSON_DIR" \
    "$PROJECT_DIR/main/adapters/hid_json.c" \
    "$CJSON_DIR/cJSON.c" \
    "$SCRIPT_DIR/test_hid_json.c" \
    -o "$HID_JSON_TEST_BIN"

"$HID_JSON_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -pthread \
    -I"$PROJECT_DIR/main/core" \
    "$PROJECT_DIR/main/core/hid_abort_fence.c" \
    "$SCRIPT_DIR/test_hid_abort_fence.c" \
    -o "$HID_ABORT_FENCE_TEST_BIN"

"$HID_ABORT_FENCE_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$PROJECT_DIR/main/core" \
    "$PROJECT_DIR/main/core/hid_owner.c" \
    "$SCRIPT_DIR/test_hid_owner.c" \
    -o "$HID_OWNER_TEST_BIN"

"$HID_OWNER_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -pedantic \
    -I"$PROJECT_DIR/main/core" \
    "$PROJECT_DIR/main/core/agent_task_runtime.c" \
    "$SCRIPT_DIR/test_agent_task_runtime.c" \
    -o "$AGENT_TASK_RUNTIME_TEST_BIN"

"$AGENT_TASK_RUNTIME_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -pedantic \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/application" \
    -I"$PROJECT_DIR/main/core" \
    -I"$PROJECT_DIR/main/infrastructure" \
    -I"$PROJECT_DIR/main/config" \
    -I"$CJSON_DIR" \
    "$PROJECT_DIR/main/core/agent_task_runtime.c" \
    "$PROJECT_DIR/main/core/agent_verifier.c" \
    "$PROJECT_DIR/main/core/authorization.c" \
    "$CJSON_DIR/cJSON.c" \
    "$SCRIPT_DIR/test_agent_task_service.c" \
    -o "$AGENT_TASK_SERVICE_TEST_BIN"

"$AGENT_TASK_SERVICE_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -pedantic \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/application" \
    -I"$PROJECT_DIR/main/core" \
    -I"$PROJECT_DIR/main/infrastructure" \
    -I"$PROJECT_DIR/main/config" \
    -I"$CJSON_DIR" \
    "$PROJECT_DIR/main/core/agent_task_runtime.c" \
    "$PROJECT_DIR/main/core/agent_verifier.c" \
    "$PROJECT_DIR/main/core/authorization.c" \
    "$CJSON_DIR/cJSON.c" \
    "$SCRIPT_DIR/test_agent_task_service_cancel_after_started_gate.c" \
    -o "$AGENT_TASK_CANCEL_GATE_TEST_BIN"

"$AGENT_TASK_CANCEL_GATE_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$PROJECT_DIR/main/core" \
    "$PROJECT_DIR/main/core/authorization.c" \
    "$PROJECT_DIR/main/core/agent_execution_guard.c" \
    "$SCRIPT_DIR/test_agent_execution_guard.c" \
    -o "$AGENT_EXECUTION_GUARD_TEST_BIN"

"$AGENT_EXECUTION_GUARD_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$PROJECT_DIR/main/core" \
    "$PROJECT_DIR/main/core/agent_verifier.c" \
    "$SCRIPT_DIR/test_agent_verifier.c" \
    -o "$AGENT_VERIFIER_TEST_BIN"

"$AGENT_VERIFIER_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/application" \
    "$SCRIPT_DIR/test_agent_request_broker.c" \
    -o "$AGENT_REQUEST_BROKER_TEST_BIN"

"$AGENT_REQUEST_BROKER_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -D_DARWIN_C_SOURCE \
    -DSI_AGENT_EVENT_STORE_HOST_TEST \
    -DSI_AGENT_EVENT_STORE_DIR=\"$AGENT_EVENT_STORE_TEST_DIR\" \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/infrastructure" \
    -I"$PROJECT_DIR/main/config" \
    -I"$CJSON_DIR" \
    "$PROJECT_DIR/main/infrastructure/agent_event_store.c" \
    "$CJSON_DIR/cJSON.c" \
    "$SCRIPT_DIR/test_agent_event_store.c" \
    -o "$AGENT_EVENT_STORE_TEST_BIN"

"$AGENT_EVENT_STORE_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/application" \
    -I"$PROJECT_DIR/main/infrastructure" \
    -I"$CJSON_DIR" \
    "$CJSON_DIR/cJSON.c" \
    "$SCRIPT_DIR/test_agent_history_writer.c" \
    -o "$AGENT_HISTORY_WRITER_TEST_BIN"

"$AGENT_HISTORY_WRITER_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -DSI_AGENT_API_VALIDATION_HOST_TEST \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/application" \
    "$PROJECT_DIR/main/application/agent_api_settings.c" \
    "$SCRIPT_DIR/test_agent_endpoint_validation.c" \
    -o "$AGENT_ENDPOINT_TEST_BIN"

"$AGENT_ENDPOINT_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$SCRIPT_DIR/idf_stubs" \
    -I"$PROJECT_DIR/main/core" \
    -I"$CJSON_DIR" \
    "$CJSON_DIR/cJSON.c" \
    "$SCRIPT_DIR/test_agent_result_error.c" \
    -o "$AGENT_RESULT_ERROR_TEST_BIN"

"$AGENT_RESULT_ERROR_TEST_BIN"

"${CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$PROJECT_DIR/main/application" \
    -I"$PROJECT_DIR/main/core" \
    "$PROJECT_DIR/main/core/action_event.c" \
    "$PROJECT_DIR/main/application/diagnostics_service.c" \
    "$SCRIPT_DIR/test_diagnostics_formatter.c" \
    -o "$DIAGNOSTICS_FORMATTER_TEST_BIN"

"$DIAGNOSTICS_FORMATTER_TEST_BIN"
python3 "$SCRIPT_DIR/test_net_manager_runtime.py"
python3 "$SCRIPT_DIR/test_h264_send_runtime.py"
python3 "$SCRIPT_DIR/check_web_contract.py"
python3 "$SCRIPT_DIR/check_agent_auth_liveness_contract.py"
python3 "$SCRIPT_DIR/check_direct_hid_guard_contract.py"
python3 "$SCRIPT_DIR/check_hid_owner_contract.py"
python3 "$SCRIPT_DIR/check_refactor_boundaries.py"
python3 "$SCRIPT_DIR/check_agent_tool_contract.py"
python3 "$SCRIPT_DIR/check_agent_skill_contract.py"
python3 "$SCRIPT_DIR/check_uart_mcp_contract.py"
python3 "$SCRIPT_DIR/check_terminal_control_contract.py"
python3 "$SCRIPT_DIR/check_diag_cli_contract.py"
python3 "$SCRIPT_DIR/check_ms2109_test_contract.py"
python3 "$SCRIPT_DIR/check_ms2109_power_contract.py"
python3 "$SCRIPT_DIR/check_ms2109_composition_boundary.py"
python3 "$SCRIPT_DIR/check_prototype_v24_product_contract.py"
python3 "$SCRIPT_DIR/check_tf_low_speed_fallback_contract.py"
python3 "$SCRIPT_DIR/check_frontend_semantics.py"
python3 "$SCRIPT_DIR/check_settings_review_contract.py"
python3 "$SCRIPT_DIR/check_resource_contract.py"
python3 "$SCRIPT_DIR/check_video_pipeline_contract.py"
python3 "$SCRIPT_DIR/check_build_profiles.py"
python3 "$SCRIPT_DIR/check_network_boot_contract.py"
python3 "$SCRIPT_DIR/check_ssh_persistence_broker_contract.py"
python3 "$SCRIPT_DIR/check_product_feature_contract.py"
python3 "$SCRIPT_DIR/check_page_context_revocation_contract.py"
python3 "$SCRIPT_DIR/check_agent_checkpoint_identity_contract.py"
python3 "$SCRIPT_DIR/check_agent_runtime_isolation_contract.py"
python3 "$SCRIPT_DIR/check_agent_history_writer_contract.py"
python3 "$SCRIPT_DIR/check_agent_runtime_shadow_cutover_gate.py"
python3 "$SCRIPT_DIR/check_agent_degraded_control_contract.py"
python3 "$SCRIPT_DIR/check_agent_http_exact_control_contract.py"
python3 "$SCRIPT_DIR/test_agent_runtime_http_cli.py"
node "$SCRIPT_DIR/test_ui_feature_gating.js"
node "$SCRIPT_DIR/test_ui_lifecycle.mjs"
node "$SCRIPT_DIR/test_ui_auth_requests.mjs"
node "$SCRIPT_DIR/test_ui_shell_agent_gate.mjs"
node "$SCRIPT_DIR/test_ui_shell_status_menus.mjs"
node "$SCRIPT_DIR/test_global_automation_control.mjs"
node "$SCRIPT_DIR/test_kvm_focus_refresh.mjs"
node "$SCRIPT_DIR/test_kvm_agent_video_boundary.mjs"
node "$SCRIPT_DIR/test_kvm_ms_power_state.mjs"
node "$SCRIPT_DIR/test_settings_review_runtime.mjs"
node "$SCRIPT_DIR/test_uart_terminal_handshake.mjs"
node "$SCRIPT_DIR/test_ssh_terminal_handshake.mjs"
node "$SCRIPT_DIR/test_terminal_automation_control.mjs"
