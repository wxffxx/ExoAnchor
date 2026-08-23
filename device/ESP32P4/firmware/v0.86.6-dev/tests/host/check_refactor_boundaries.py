#!/usr/bin/env python3
"""Prevent the Web refactor from regressing back into a transport god module."""

from pathlib import Path
import hashlib
import re
import sys


PROJECT = Path(__file__).resolve().parents[2]
MAIN = PROJECT / "main"
WEB_SERVER = MAIN / "services" / "web_server.c"
WEB_MODULES = MAIN / "services" / "web"
DEVICE_HTTP = MAIN / "services" / "device_http.c"
STATUS_WS = MAIN / "services" / "status_ws.c"
HTTP_API = MAIN / "adapters" / "http_api.c"
HTTP_API_HEADER = MAIN / "adapters" / "http_api.h"
BASE_SETTINGS_HTTP = MAIN / "services" / "web" / "base_settings_http_module.inc"
SSH_HTTP = MAIN / "services" / "web" / "ssh_module.inc"
OBSERVATION_SERVICE = MAIN / "application" / "device_observation_service.c"
SETTINGS_STORE = MAIN / "infrastructure" / "settings_store.c"
SETTINGS_SCHEMA = MAIN / "infrastructure" / "settings_schema.c"
SECRET_STORE = MAIN / "infrastructure" / "secret_store.c"
CONSOLE_CREDENTIALS = MAIN / "application" / "console_credentials.c"
AGENT_API_SETTINGS = MAIN / "application" / "agent_api_settings.c"
AGENT_TOOLS_SETTINGS = MAIN / "application" / "agent_tools_settings.c"
POWER_CONTROL = MAIN / "drivers" / "power_control.c"
APP_MAIN = MAIN / "app" / "app_main.c"
OTA_SETTINGS_HTTP = MAIN / "services" / "web" / "ota_settings_module.inc"
ASYNC_HTTP = MAIN / "services" / "web" / "async_http_module.inc"
CMAKE = MAIN / "CMakeLists.txt"
PROJECT_CMAKE = PROJECT / "CMakeLists.txt"
SDKCONFIG_DEFAULTS = PROJECT / "sdkconfig.defaults"
PROTOTYPE_V23_DEFAULTS = (
    PROJECT / "configs" / "boards" /
    "sdkconfig.defaults.exoanchor-prototype-v2.3"
)
PROTOTYPE_V23_EEPROM_EMULATOR = (
    PROJECT / "configs" / "boards" /
    "sdkconfig.defaults.exoanchor-prototype-v2.3-ms2109-eeprom-emulator"
)
UVC_ISOC_OVERRIDE = PROJECT / "patches" / "usb_host_uvc-2.5.1" / "uvc_isoc.c"
UVC_ISOC_UPSTREAM_SHA256 = (
    "c709685f2814697c568620c7c2b7974e3e3167a92308ddd324c46db082de1ccc"
)
UVC_ISOC_UPSTREAM_LOSS_BLOCK_SHA256 = (
    "fe01926202a8afab6ef438711d53494e6cdb288572b58be1eddac77a27c2e113"
)
UVC_ISOC_FIXED_LOSS_BLOCK_SHA256 = (
    "ee76adf961af8b576fdf99bafdceb0ac75cc42a197174fb1efc4b7b799ad12ac"
)
UVC_ISOC_UPSTREAM_FID_BLOCK_SHA256 = (
    "37b780f9a5e1b22e0d04207d716bf3789f1dfbc2234502eee4a169bef170d412"
)
UVC_ISOC_FIXED_FID_BLOCK_SHA256 = (
    "5343bfd077e3a44f9e3958ba767e0eed242a4e70c442fc9e9cee88e0f018b1e3"
)

MIGRATED_SETTINGS_OWNERS = {
    MAIN / "application" / "auth_service.c": (
        '"si_auth"', '"username"', '"password_hash"', '"login_count"',
    ),
    MAIN / "application" / "device_settings.c": (
        '"si_device"', '"label"', '"si_session"', '"auto_logout"',
        '"auto_minutes"', '"si_mcp"', '"enabled"',
    ),
    MAIN / "application" / "agent_prompt_service.c": (
        '"si_agent_prompt"', '"system"',
    ),
    MAIN / "application" / "video_control.c": (
        '"si_video_cfg"', '"kvm_width"', '"kvm_height"',
        '"kvm_fps_x100"', '"quality"', '"always_on"', '"preview_fps100"',
    ),
    AGENT_API_SETTINGS: (
        '"si_agent_api"', '"active"', '"provider"', '"endpoint"', '"model"',
        '"api_key"',
    ),
    AGENT_TOOLS_SETTINGS: (
        '"si_agent_tools"', '"web_en"', '"web_prof"', '"web_ep"',
        '"web_model"', '"web_key"',
        '"web_strategy"',
    ),
}


def function_line_count(text: str, name: str) -> int:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", text, re.S)
    if not match:
        return 0
    start = match.start()
    brace = text.find("{", match.start())
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1].count("\n") + 1
    return 0


def cmake_bracket_block(text: str, variable: str) -> str:
    """Return one exact CMake ``set(name [=[...]=])`` payload."""
    pattern = rf"set\({re.escape(variable)} \[=\[(.*?)\n\]=\]\)"
    matches = re.findall(pattern, text, re.DOTALL)
    return matches[0] if len(matches) == 1 else ""


def main() -> int:
    failures = []
    web_text = WEB_SERVER.read_text(encoding="utf-8")
    web_lines = len(web_text.splitlines())
    if web_lines > 800:
        failures.append(f"web_server.c grew beyond the 800-line assembly budget: {web_lines}")
    if re.search(r"\bnvs_(?:open|get_|set_|erase|commit)", web_text):
        failures.append("web_server.c directly accesses NVS")

    socket_match = re.search(
        r"^CONFIG_LWIP_MAX_SOCKETS=(\d+)$",
        SDKCONFIG_DEFAULTS.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if not socket_match or int(socket_match.group(1)) < 48:
        failures.append("LWIP socket pool is below the dual-HTTP-server budget")
    for marker in (
        "SI_MAIN_HTTP_MAX_OPEN_SOCKETS 24",
        "SI_STREAM_HTTP_MAX_OPEN_SOCKETS 4",
        "SI_HTTPD_INTERNAL_SOCKETS_PER_SERVER 3",
        "SI_NETWORK_SOCKET_HEADROOM 8",
        "_Static_assert(CONFIG_LWIP_MAX_SOCKETS >= SI_HTTP_SOCKET_BUDGET_REQUIRED",
    ):
        if marker not in web_text:
            failures.append(f"HTTP socket budget guard missing marker: {marker}")

    prototype_v23_text = PROTOTYPE_V23_DEFAULTS.read_text(encoding="utf-8")
    for marker in (
        "Netlist_SCH_ESP32P4_Prototype_V2.3b6_1_2026-07-25.tel",
        "0ce2340e77fa5e267ef1eb04ab4fb7ea71ba6c3f9917ba7715f6579ec71b85fa",
        "CONFIG_SI_MS2109_EEPROM_WP_GPIO=16",
        "# CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE is not set",
        "CONFIG_SI_POWER_BUTTON_GPIO=4",
        "CONFIG_SI_RESET_BUTTON_GPIO=5",
        "CONFIG_SI_TARGET_UART_RX_GPIO=50",
        "CONFIG_SI_TARGET_UART_TX_GPIO=51",
        "CONFIG_SI_TF_CARD_DETECT_GPIO=-1",
    ):
        if marker not in prototype_v23_text:
            failures.append(f"PrototypeV2.3b6 board contract missing marker: {marker}")
    if "CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE=y" in prototype_v23_text:
        failures.append("PrototypeV2.3b6 normal profile enables EEPROM emulator beside U5")
    # Host contract checks must be reproducible from tracked inputs.  The
    # generated sdkconfig is intentionally ignored and may not exist in a
    # clean checkout, so inspect the checked-in defaults instead.
    for config_path in (
        SDKCONFIG_DEFAULTS,
        PROTOTYPE_V23_DEFAULTS,
        PROJECT / "configs" / "boards" /
        "sdkconfig.defaults.exoanchor-prototype-v2.1",
        MAIN / "config" / "board_config.h",
        MAIN / "Kconfig.projbuild",
    ):
        if "SI_POWER_RESERVE" in config_path.read_text(encoding="utf-8"):
            failures.append(
                f"removed Reserved GPIO config residue remains: {config_path.name}"
            )

    if not PROTOTYPE_V23_EEPROM_EMULATOR.exists():
        failures.append("PrototypeV2.3 U5-depopulated EEPROM emulator overlay is missing")
    else:
        emulator_text = PROTOTYPE_V23_EEPROM_EMULATOR.read_text(encoding="utf-8")
        for marker in (
            "CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE=y",
            "CONFIG_SI_MS2109_EEPROM_SCL_GPIO=47",
            "CONFIG_SI_MS2109_EEPROM_SDA_GPIO=48",
        ):
            if marker not in emulator_text:
                failures.append(
                    f"PrototypeV2.3 EEPROM emulator overlay missing marker: {marker}"
                )

    for promoted in (DEVICE_HTTP, STATUS_WS, OBSERVATION_SERVICE):
        if not promoted.exists():
            failures.append(f"promoted observation/Web module missing: {promoted.name}")
    for obsolete in (WEB_MODULES / "device_http_module.inc",
                     WEB_MODULES / "status_ws_module.inc"):
        if obsolete.exists():
            failures.append(f"obsolete transitional module still exists: {obsolete.name}")

    for store in (
        SETTINGS_STORE, SETTINGS_SCHEMA, SECRET_STORE, CONSOLE_CREDENTIALS
    ):
        if not store.exists():
            failures.append(f"persistent store facade missing: {store.name}")

    cmake_text = CMAKE.read_text(encoding="utf-8")
    for source in (
        "application/device_observation_service.c",
        "application/console_credentials.c",
        "core/device_observation_utils.c",
        "services/device_http.c",
        "services/status_ws.c",
        "infrastructure/settings_store.c",
        "infrastructure/settings_schema.c",
        "infrastructure/secret_store.c",
    ):
        if f'"{source}"' not in cmake_text:
            failures.append(f"promoted source missing from CMake: {source}")

    project_cmake_text = PROJECT_CMAKE.read_text(encoding="utf-8")
    if (
        "idf_component_get_property(si_uvc_component espressif__usb_host_uvc COMPONENT_LIB)"
        not in project_cmake_text
        or "usb_host_uvc" not in cmake_text
    ):
        failures.append("video capture must use Espressif's registered usb_host_uvc component")
    for marker in (
        'set(si_uvc_upstream_isoc "${si_uvc_component_dir}/uvc_isoc.c")',
        f'"{UVC_ISOC_UPSTREAM_SHA256}"',
        'file(SHA256 "${si_uvc_upstream_isoc}" si_uvc_isoc_actual_sha256)',
        "if(NOT si_uvc_isoc_actual_sha256 STREQUAL si_uvc_251_isoc_sha256)",
        'file(READ "${si_uvc_upstream_isoc}" si_uvc_isoc_source)',
        'string(FIND "${si_uvc_isoc_source}" "${si_uvc_loss_upstream}"',
        'string(REPLACE "${si_uvc_loss_upstream}" "${si_uvc_loss_fixed}"',
        'string(FIND "${si_uvc_isoc_loss_patched}" "${si_uvc_sof_upstream}"',
        'string(REPLACE "${si_uvc_sof_upstream}" "${si_uvc_sof_fixed}"',
        'set(si_uvc_patched_isoc "${si_uvc_patch_dir}/uvc_isoc.c")',
        'file(WRITE "${si_uvc_patched_isoc}" "${si_uvc_isoc_patched}")',
        'list(REMOVE_ITEM si_uvc_sources "${si_uvc_upstream_isoc}")',
        'list(APPEND si_uvc_sources "${si_uvc_patched_isoc}")',
        'message(STATUS "Using strict MS2109 FID-boundary compatibility fix")',
    ):
        if marker not in project_cmake_text:
            failures.append(f"guarded MS2109 FID-boundary patch missing marker: {marker}")

    upstream_loss_block = cmake_bracket_block(
        project_cmake_text, "si_uvc_loss_upstream"
    )
    fixed_loss_block = cmake_bracket_block(
        project_cmake_text, "si_uvc_loss_fixed"
    )
    if not upstream_loss_block or not fixed_loss_block:
        failures.append(
            "UVC packet-loss handling must replace one exact bracket-quoted upstream block"
        )
    else:
        if hashlib.sha256(upstream_loss_block.encode("utf-8")).hexdigest() != (
            UVC_ISOC_UPSTREAM_LOSS_BLOCK_SHA256
        ):
            failures.append("the exact usb_host_uvc 2.5.1 packet-loss anchor changed")
        if hashlib.sha256(fixed_loss_block.encode("utf-8")).hexdigest() != (
            UVC_ISOC_FIXED_LOSS_BLOCK_SHA256
        ):
            failures.append("the reviewed active-frame packet-loss replacement changed")
        for marker in (
            "case USB_TRANSFER_STATUS_TIMED_OUT:",
            "case USB_TRANSFER_STATUS_SKIPPED:",
            "uvc_host_frame_t *loss_frame =",
            "const bool completed_mjpeg_waiting_for_fid =",
            "loss_frame->data_len >= 2",
            "loss_frame->data[loss_frame->data_len - 2] == JPEG_MARKER",
            "loss_frame->data[loss_frame->data_len - 1] == 0xD9U",
            "if (loss_frame && !completed_mjpeg_waiting_for_fid)",
            "uvc_stream->single_thread.skip_current_frame = true;",
            "goto next_isoc_packet;",
        ):
            if marker not in fixed_loss_block:
                failures.append(
                    f"active UVC frame loss must taint the complete frame: {marker}"
                )

    upstream_fid_block = cmake_bracket_block(
        project_cmake_text, "si_uvc_sof_upstream"
    )
    fixed_fid_block = cmake_bracket_block(project_cmake_text, "si_uvc_sof_fixed")
    if not upstream_fid_block or not fixed_fid_block:
        failures.append(
            "MS2109 compatibility must replace one exact bracket-quoted upstream FID block"
        )
    else:
        upstream_block_sha256 = hashlib.sha256(
            upstream_fid_block.encode("utf-8")
        ).hexdigest()
        fixed_block_sha256 = hashlib.sha256(
            fixed_fid_block.encode("utf-8")
        ).hexdigest()
        if upstream_block_sha256 != UVC_ISOC_UPSTREAM_FID_BLOCK_SHA256:
            failures.append(
                "the exact usb_host_uvc 2.5.1 FID replacement anchor changed"
            )
        if fixed_block_sha256 != UVC_ISOC_FIXED_FID_BLOCK_SHA256:
            failures.append(
                "the reviewed MS2109 FID-boundary replacement block changed"
            )
        for marker in (
            "const bool start_of_frame =",
            "if (start_of_frame) {",
            'ESP_EARLY_LOGW(TAG, "missed EoF")',
            "uvc_frame_reset(uvc_stream->dynamic.current_frame);",
        ):
            if marker not in upstream_fid_block:
                failures.append(
                    f"upstream FID replacement anchor is incomplete: {marker}"
                )
        for marker in (
            "uvc_host_frame_t *previous_frame = NULL;",
            "uvc_stream->dynamic.current_frame = NULL;",
            "!uvc_stream->single_thread.skip_current_frame",
            "previous_frame->data_len >= 4",
            "previous_frame->data[0] == JPEG_MARKER",
            "previous_frame->data[1] == JPEG_SOI",
            "previous_frame->data[previous_frame->data_len - 2] == JPEG_MARKER",
            "previous_frame->data[previous_frame->data_len - 1] == 0xD9U",
            "return_previous = uvc_stream->constant.frame_cb(",
            "uvc_host_frame_return(uvc_stream, previous_frame);",
            "payload_data_len < 2",
        ):
            if marker not in fixed_fid_block:
                failures.append(
                    f"strict FID-boundary ownership/marker guard missing: {marker}"
                )
        if fixed_fid_block.count("previous_frame->data[") != 4:
            failures.append(
                "FID fallback may inspect only the previous frame's two SOI and two terminal EOI bytes"
            )
        if fixed_fid_block.count("payload_data[") != 2:
            failures.append(
                "FID fallback may inspect only the new payload's two SOI bytes"
            )
        for forbidden_pattern in (
            r"\b(?:for|while)\s*\(",
            r"\bmem(?:chr|cmp|cpy|move|set)\s*\(",
            r"\bstr(?:str|chr)\s*\(",
            r"\b(?:malloc|calloc|realloc|heap_caps_malloc)\s*\(",
            r"\b(?:scan|search|trim|repair|append|concat|insert)\w*\s*\(",
            r"previous_frame->data_len\s*=(?!=)",
            r"previous_frame->data\s*\[[^]]+\]\s*=(?!=)",
            r"payload_data\s*\[[^]]+\]\s*=(?!=)",
        ):
            if re.search(forbidden_pattern, fixed_fid_block, re.IGNORECASE):
                failures.append(
                    "FID fallback must not scan, allocate, mutate, trim, repair, or insert JPEG bytes: "
                    + forbidden_pattern
                )

    isoc_patch_start = project_cmake_text.find("# MS2109 toggles the UVC Frame ID")
    isoc_patch_end = project_cmake_text.find(
        "# esp_h264 1.3.7", isoc_patch_start
    )
    isoc_patch = project_cmake_text[isoc_patch_start:isoc_patch_end]
    if (
        isoc_patch_start < 0
        or isoc_patch_end < 0
        or isoc_patch.count("string(REPLACE") != 2
        or isoc_patch.count("file(WRITE") != 1
    ):
        failures.append(
            "UVC compatibility must generate one source from exact packet-loss and FID-block replacements"
        )
    for forbidden in (
        "si_uvc_loss_status_",
        "si_uvc_compat_isoc",
        "isoc_add_mjpeg_frame_data",
        "isoc_mjpeg_trim_to_eoi",
        "isoc_find_jpeg_marker",
        "mjpeg_complete_len",
    ):
        if forbidden in project_cmake_text:
            failures.append(
                f"custom UVC assembler/repair logic remains outside the bounded FID patch: {forbidden}"
            )
    local_uvc_isoc_sources = []
    local_uvc_assembler_sources = []
    custom_assembler_markers = (
        "isoc_add_mjpeg_frame_data",
        "isoc_mjpeg_trim_to_eoi",
        "isoc_find_jpeg_marker",
        "mjpeg_complete_len",
    )
    for root_name in ("main", "components", "patches"):
        source_root = PROJECT / root_name
        if not source_root.exists():
            continue
        local_uvc_isoc_sources.extend(
            path for path in source_root.rglob("uvc_isoc.c") if path.is_file()
        )
        for pattern in ("*.c", "*.h", "*.inc"):
            for path in source_root.rglob(pattern):
                if not path.is_file():
                    continue
                source_text = path.read_text(encoding="utf-8")
                if any(marker in source_text for marker in custom_assembler_markers):
                    local_uvc_assembler_sources.append(path)
    if UVC_ISOC_OVERRIDE.exists() or local_uvc_isoc_sources:
        failures.append(
            "repository-local usb_host_uvc/uvc_isoc.c override must not be shipped: "
            + ", ".join(
                str(path.relative_to(PROJECT)) for path in local_uvc_isoc_sources
            )
        )
    if local_uvc_assembler_sources:
        failures.append(
            "repository-local UVC MJPEG assembler/repair source must not be shipped: "
            + ", ".join(
                str(path.relative_to(PROJECT))
                for path in local_uvc_assembler_sources
            )
        )

    direct_nvs = re.compile(r"\bnvs_(?:open|close|get_|set_|erase|commit)")
    settings_schema_text = SETTINGS_SCHEMA.read_text(encoding="utf-8")
    for marker in (
        '"si_schema"', '"version"', '"last_good"', '"pending"',
        "SI_SETTINGS_SCHEMA_CURRENT_VERSION",
        "run_migrations",
        "ESP_ERR_NOT_SUPPORTED",
    ):
        if marker not in settings_schema_text:
            failures.append(f"settings schema contract missing marker: {marker}")
    if '#include "nvs.h"' in settings_schema_text or direct_nvs.search(
        settings_schema_text
    ):
        failures.append("settings schema bypasses settings_store")
    for owner, compatibility_literals in MIGRATED_SETTINGS_OWNERS.items():
        text = owner.read_text(encoding="utf-8")
        if '#include "nvs.h"' in text or direct_nvs.search(text):
            failures.append(f"migrated settings owner accesses NVS directly: {owner.name}")
        missing_literals = [value for value in compatibility_literals if value not in text]
        if missing_literals:
            failures.append(
                f"{owner.name} changed persisted namespace/key without migration: "
                + ", ".join(missing_literals)
            )
        oversized_keys = [value for value in compatibility_literals
                          if len(value.strip('"')) > 15]
        if oversized_keys:
            failures.append(
                f"{owner.name} has NVS namespace/key longer than 15 characters: "
                + ", ".join(oversized_keys)
            )

    auth_text = (MAIN / "application" / "auth_service.c").read_text(encoding="utf-8")
    for marker in (
        "si_secret_store_get_string(&store, AUTH_PASSWORD_HASH_KEY",
        "si_secret_store_set_string(&store, AUTH_PASSWORD_HASH_KEY",
        "si_secret_store_clear",
    ):
        if marker not in auth_text:
            failures.append(f"auth secret boundary missing marker: {marker}")
    console_credentials_text = CONSOLE_CREDENTIALS.read_text(encoding="utf-8")
    if '#include "nvs.h"' in console_credentials_text or direct_nvs.search(
        console_credentials_text
    ):
        failures.append("Console credential service bypasses settings/secret stores")
    for marker in (
        '"si_console"', '"username"', '"password"',
        "si_settings_store_open_read",
        "si_settings_store_open_write",
        "si_secret_store_is_configured",
        "si_secret_store_set_string",
        "si_secret_store_get_string",
    ):
        if marker not in console_credentials_text:
            failures.append(f"Console credential service missing marker: {marker}")
    for owner, secret_key in (
        (AGENT_API_SETTINGS, "LEGACY_SECRET_KEY"),
        (AGENT_TOOLS_SETTINGS, "WEB_SECRET_KEY"),
    ):
        text = owner.read_text(encoding="utf-8")
        for marker in (
            "si_secret_store_is_configured",
            "si_secret_store_get_string",
            "si_secret_store_set_string",
            "si_secret_store_erase",
        ):
            if marker not in text:
                failures.append(
                    f"{owner.name} secret boundary missing marker: {marker}"
                )
        if secret_key not in text:
            failures.append(
                f"{owner.name} compatibility secret key missing: {secret_key}"
            )
    ssh_http_text = SSH_HTTP.read_text(encoding="utf-8")
    if '#include "nvs.h"' in ssh_http_text or direct_nvs.search(ssh_http_text):
        failures.append("SSH settings/credential HTTP module bypasses stores")
    for marker in (
        "si_settings_store_open_read",
        "si_settings_store_open_write",
        "si_secret_store_is_configured",
        "si_secret_store_get_string",
        "si_secret_store_set_string",
        "si_secret_store_erase",
    ):
        if marker not in ssh_http_text:
            failures.append(f"SSH settings/secret boundary missing marker: {marker}")

    for owner, markers in (
        (
            POWER_CONTROL,
            (
                '"si_power"', '"pwr_btn"', '"pwr_btn_ah"',
                "si_settings_store_open_read",
                "si_settings_store_open_write",
            ),
        ),
        (
            APP_MAIN,
            (
                '"si_boot"', '"reset_image"',
                "si_settings_store_open_read",
                "si_settings_store_open_write",
            ),
        ),
        (
            OTA_SETTINGS_HTTP,
            (
                "OTA_NAMESPACE", "OTA_MANIFEST_URL_KEY",
                "si_settings_store_open_read",
                "si_settings_store_open_write",
            ),
        ),
    ):
        text = owner.read_text(encoding="utf-8")
        if '#include "nvs.h"' in text or direct_nvs.search(text):
            failures.append(
                f"migrated runtime settings owner accesses NVS directly: {owner.name}"
            )
        for marker in markers:
            if marker not in text:
                failures.append(
                    f"{owner.name} settings-store boundary missing marker: {marker}"
                )

    async_http_text = ASYNC_HTTP.read_text(encoding="utf-8")
    for marker in (
        "DEFINE_LONG_HTTP_WRAPPER(power_action_async_handler",
        "DEFINE_LONG_HTTP_WRAPPER(snapshot_async_handler",
    ):
        if marker not in async_http_text:
            failures.append(f"blocking HTTP isolation missing marker: {marker}")
    for route_marker in (
        '"/api/power/action", HTTP_POST,\n                 power_action_async_handler',
        '"/api/snapshot", HTTP_GET,\n                 snapshot_async_handler',
    ):
        if route_marker not in web_text:
            failures.append(
                "blocking HTTP handler registered synchronously: "
                + route_marker.split(",")[0]
            )

    http_api_text = HTTP_API.read_text(encoding="utf-8")
    http_api_header_text = HTTP_API_HEADER.read_text(encoding="utf-8")
    base_settings_text = BASE_SETTINGS_HTTP.read_text(encoding="utf-8")
    for marker in (
        "SI_HTTP_SESSION_COOKIE_MAX_LEN 160",
        "char *cookie, size_t cookie_size",
    ):
        if marker not in http_api_header_text:
            failures.append(f"session cookie lifetime contract missing marker: {marker}")
    if "char cookie[160]" in http_api_text:
        failures.append("session cookie points at helper-local stack storage")
    if base_settings_text.count(
        "char session_cookie[SI_HTTP_SESSION_COOKIE_MAX_LEN]"
    ) != 2:
        failures.append("session cookie callers do not own both response-lifetime buffers")

    driver_calls = re.compile(
        r"\bsi_(?:video_get_status|video_get_modes|video_control_get_status|"
        r"hid_get_status|power_get_status|net_get_status|storage_get_tf_status)\s*\("
    )
    forbidden_driver_headers = {
        '"video_input.h"', '"video_control.h"', '"hid_device.h"',
        '"power_control.h"', '"net_manager.h"', '"storage_manager.h"',
    }
    for module in (DEVICE_HTTP, STATUS_WS):
        if not module.exists():
            continue
        text = module.read_text(encoding="utf-8")
        direct_headers = sorted(header for header in forbidden_driver_headers if header in text)
        if direct_headers:
            failures.append(
                f"{module.name} includes device drivers directly: {', '.join(direct_headers)}"
            )
        if driver_calls.search(text):
            failures.append(f"{module.name} calls a device status driver directly")

    forbidden_transport = []
    for folder in ("application", "core", "drivers", "infrastructure"):
        for path in (MAIN / folder).rglob("*.[ch]"):
            text = path.read_text(encoding="utf-8")
            if '#include "esp_http_server.h"' in text or '#include "web_server.h"' in text:
                forbidden_transport.append(str(path.relative_to(PROJECT)))
    if forbidden_transport:
        failures.append("lower layers depend on Web transport: " + ", ".join(forbidden_transport))

    module_files = sorted(WEB_MODULES.glob("*.inc"))
    module_lines = sum(len(path.read_text(encoding="utf-8").splitlines()) for path in module_files)
    for path in module_files:
        lines = len(path.read_text(encoding="utf-8").splitlines())
        if lines > 1500:
            failures.append(f"transitional Web module exceeds 1500 lines: {path.name}={lines}")

    run_engine = (WEB_MODULES / "agent_run_engine_module.inc").read_text(encoding="utf-8")
    run_lines = function_line_count(run_engine, "agent_run_execute_to_json")
    if run_lines == 0 or run_lines > 400:
        failures.append(f"agent_run_execute_to_json exceeds its 400-line transition budget: {run_lines}")

    if failures:
        print("refactor boundary check failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print(
        f"refactor boundaries: PASS (web entry {web_lines} lines, "
        f"{len(module_files)} transition modules/{module_lines} lines, "
        f"agent run {run_lines} lines)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
