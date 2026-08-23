#!/usr/bin/env python3
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def require(text: str, fragment: str, label: str) -> None:
    if fragment not in text:
        raise AssertionError(f"{label}: missing {fragment!r}")


kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
config = (ROOT / "main/config/app_config.h").read_text(encoding="utf-8")
web = (ROOT / "main/services/web_server.c").read_text(encoding="utf-8")
device = (ROOT / "main/services/device_http.c").read_text(encoding="utf-8")
shell = (ROOT / "main/www/assets/ui-shell.js").read_text(encoding="utf-8")
core = (ROOT / "main/www/assets/ui-core.js").read_text(encoding="utf-8")
overview = (ROOT / "main/www/index.html").read_text(encoding="utf-8")
settings = (ROOT / "main/www/settings.html").read_text(encoding="utf-8")
build_script = (ROOT / "tools/build-firmware.sh").read_text(encoding="utf-8")
h264_defaults = (ROOT / "configs/profiles/sdkconfig.defaults.dev-h264").read_text(
    encoding="utf-8"
)
root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
profile_asset = (ROOT / "main/services/web/build_profile_asset_module.inc").read_text(encoding="utf-8")
product_features = (ROOT / "main/services/web/product_features_http_module.inc").read_text(encoding="utf-8")

for fragment in (
    "choice SI_BUILD_PROFILE",
    "config SI_BUILD_PROFILE_DEV",
    "config SI_BUILD_PROFILE_STABLE",
    "config SI_EMBEDDED_AGENT",
    "config SI_UART_TERMINAL",
):
    require(kconfig, fragment, "Kconfig")

require(cmake, "if(CONFIG_SI_EMBEDDED_AGENT)", "CMake")
for source in (
    "adapters/agent_http_routes.c",
    "application/agent_request_broker.c",
    "application/agent_skill_service.c",
    "core/agent_router.c",
    "core/model_provider.c",
):
    conditional = cmake.split("if(CONFIG_SI_EMBEDDED_AGENT)", 1)[1]
    require(conditional, source, "conditional Agent sources")
require(cmake, "www/agent.html", "conditional Agent page")
require(cmake, "agent_skill_embed", "conditional Agent Skills")
require(cmake, "if(CONFIG_SI_UART_TERMINAL)", "conditional UART Terminal assets")
require(cmake, '"www/terminal.html"', "conditional UART Terminal page")
require(cmake, '"www/vendor/xterm/xterm.js"', "conditional xterm runtime")

require(config, '#define SI_BUILD_PROFILE "stable"', "build profile macro")
require(config, "#define SI_CFG_EMBEDDED_AGENT_ENABLED 1", "Agent feature macro")
require(config, "#define SI_CFG_UART_TERMINAL_ENABLED 1", "UART Terminal feature macro")
require(web, "#if SI_CFG_EMBEDDED_AGENT_ENABLED", "web runtime gate")
require(web, "#if SI_CFG_UART_TERMINAL_ENABLED", "UART Terminal route gate")
require(device, '"embedded_agent"', "capability manifest")
require(device, '"external_mcp"', "capability manifest")
require(core, "const FEATURES = Object.freeze({", "shared UI feature manifest")
require(core, "available: FEATURES.embeddedAgent", "Agent action-mirror profile gate")
require(core, 'if (!this.available || typeof listener !== "function")', "Agent action-mirror subscription gate")
require(shell, "const embeddedAgent = UI.features.embeddedAgent", "shell Agent profile gate")
require(shell, "uartTerminal", "UART Terminal shell profile gate")
require(overview, "const EMBEDDED_AGENT=UI.features.embeddedAgent", "Overview Agent profile gate")
require(overview, "if(!EMBEDDED_AGENT)return", "Overview Agent request gate")
require(overview, "if(EMBEDDED_AGENT)UI.lifecycle.interval", "Overview Agent polling gate")
require(settings, "stable-profile", "Settings profile gate")
require(settings, "MCP 工具能力授权", "Stable Settings copy")
require(root_cmake, 'set(SI_VERSION_BASE "0.87.6")', "version base")
require(root_cmake, 'set(PROJECT_VER "${SI_VERSION_BASE}-dev")', "default Dev version")
require(build_script, 'dev) FIRMWARE_VERSION="${VERSION_BASE}-dev"', "Dev version")
require(build_script, 'stable) FIRMWARE_VERSION="${VERSION_BASE}-Stable"', "Stable version")
require(
    build_script,
    'FIRMWARE_VERSION="${VERSION_BASE}-Stable-H264-Candidate"',
    "Stable H.264 candidate version",
)
require(build_script, '-D "PROJECT_VER=$FIRMWARE_VERSION"', "profile version injection")
require(build_script, 'actual=$ACTUAL_VERSION', "built version verification")
require(cmake, 'PUBLIC SI_BMC_VERSION="${PROJECT_VER}"', "firmware version definition")
require(profile_asset, r'\",version:\"" SI_BMC_VERSION', "UI version injection")
require(profile_asset, 'uartTerminal:', "UART Terminal UI profile injection")
require(build_script, "Stable build unexpectedly contains the UART Terminal", "Stable UART Terminal exclusion")
require(build_script, "Stable build contains excluded runtime symbol", "Stable linked-symbol audit")
require(build_script, "_binary_terminal_html_start", "UART Terminal asset-symbol audit")
require(product_features, "bool agent_enabled = SI_CFG_EMBEDDED_AGENT_ENABLED", "effective Agent feature state")
require(build_script, 'CONFIG_SI_AUTH_PASSWORD=""', "factory credential build gate")
require(build_script, 'CONFIG_SI_AUTH_USERNAME="admin"', "factory username build gate")
require(build_script, 'rm -f -- "$SDKCONFIG_PATH" "${SDKCONFIG_PATH}.old"', "deterministic sdkconfig regeneration")
require(build_script, "video baseline mismatch", "generated Stable video baseline audit")
require(build_script, "CONFIG_SI_VIDEO_UVC_URB_SIZE=10240", "Stable UVC URB-size audit")
require(build_script, "CONFIG_SI_VIDEO_UVC_JPEG_BUFFER_SIZE=2097152", "Stable JPEG ceiling audit")
require(build_script, "Stable/non-H.264 build does not match the Stable video memory baseline", "Stable memory audit")
require(build_script, "--stable-h264-candidate", "Stable H.264 candidate CLI")
require(
    build_script,
    '"$BOARD" != "exoanchor-prototype-v2.3"',
    "Stable H.264 candidate board gate",
)
require(
    build_script,
    'DEFAULTS="${DEFAULTS};configs/profiles/sdkconfig.defaults.dev-h264"',
    "Stable H.264 candidate resource overlay",
)
require(
    build_script,
    "H.264 build is missing runtime symbol",
    "H.264 linked-symbol audit",
)
require(
    build_script,
    "non-H.264 build contains excluded runtime symbol",
    "non-H.264 linked-symbol audit",
)
require(kconfig, 'default ""', "empty shared bootstrap default")
require(root_cmake, "CVE-2026-55200", "libssh2 security backport")
require(root_cmake, "si_libssh2_transport_sha256", "libssh2 source hash guard")
require(
    root_cmake,
    "si_http_client_555_sha256",
    "ESP-IDF async response-read source hash guard",
)
require(
    root_cmake,
    "ret == ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT",
    "ESP-IDF async response poll timeout remains resumable",
)
for fragment, label in (
    (
        "client->connection_info.method != HTTP_METHOD_HEAD",
        "async HEAD response must not enter body-wait retry",
    ),
    (
        "client->response->status_code / 100 != 1",
        "async informational response must not enter body-wait retry",
    ),
    (
        "client->response->status_code != 204",
        "async 204 response must not enter body-wait retry",
    ),
    (
        "client->response->status_code != 304",
        "async 304 response must not enter body-wait retry",
    ),
):
    require(root_cmake, fragment, label)
if root_cmake.count("ret == ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT") != 1:
    raise AssertionError("async response timeout override must have one guarded template")
require(
    root_cmake,
    "must not be emulated by resending the POST",
    "Provider response retry must never resend a POST",
)

for profile in ("dev", "stable"):
    defaults = ROOT / f"configs/profiles/sdkconfig.defaults.{profile}"
    if not defaults.is_file():
        raise AssertionError(f"missing profile defaults: {defaults}")

dev_defaults = (ROOT / "configs/profiles/sdkconfig.defaults.dev").read_text(encoding="utf-8")
stable_defaults = (ROOT / "configs/profiles/sdkconfig.defaults.stable").read_text(encoding="utf-8")
require(dev_defaults, "CONFIG_SI_UART_TERMINAL=y", "Dev UART Terminal default")
for fragment in (
    "# CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC is not set",
    "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y",
    "CONFIG_MBEDTLS_DYNAMIC_BUFFER=y",
    "CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048",
    "# CONFIG_MBEDTLS_HARDWARE_AES is not set",
):
    require(dev_defaults, fragment, "Dev embedded Agent TLS memory contract")
for fragment in (
    "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y",
    "CONFIG_MBEDTLS_DYNAMIC_BUFFER=y",
    "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384",
    "CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048",
    "CONFIG_MBEDTLS_AES_C=y",
    "CONFIG_MBEDTLS_GCM_C=y",
    "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096",
    "# CONFIG_MBEDTLS_HARDWARE_AES is not set",
    "CONFIG_MBEDTLS_HARDWARE_GCM=y",
    "CONFIG_MBEDTLS_AES_USE_INTERRUPT=y",
):
    require(build_script, fragment, "generated Dev TLS memory audit")
require(stable_defaults, "# CONFIG_SI_UART_TERMINAL is not set", "Stable UART Terminal default")
if "CONFIG_SI_VIDEO_H264_EXPERIMENT=y" in stable_defaults:
    raise AssertionError("ordinary Stable profile must not enable H.264")
require(
    h264_defaults,
    "CONFIG_SI_VIDEO_H264_EXPERIMENT=y",
    "explicit H.264 overlay selection",
)
require(
    h264_defaults,
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=262144",
    "H.264 internal/DMA reserve uses the ESP-IDF 5.5 supported maximum",
)

h264_kconfig = kconfig.split("config SI_VIDEO_H264_EXPERIMENT", 1)[1].split(
    "config SI_VIDEO_H264_BITRATE", 1
)[0]
require(
    h264_kconfig,
    "default y if SI_BUILD_PROFILE_DEV && SI_BOARD_REQUIRES_REV3",
    "Rev3 Dev H.264 default",
)
require(
    h264_kconfig,
    "depends on SI_VIDEO_ENABLE && SI_BOARD_REQUIRES_REV3",
    "Stable candidate H.264 Kconfig availability",
)
if "depends on SI_VIDEO_ENABLE && SI_BUILD_PROFILE_DEV" in h264_kconfig:
    raise AssertionError("H.264 Kconfig is still hard-gated to Dev")

build_path = ROOT / "tools/build-firmware.sh"
help_result = subprocess.run(
    ["bash", str(build_path), "--help"],
    cwd=ROOT,
    check=False,
    capture_output=True,
    text=True,
)
if help_result.returncode != 0 or "--stable-h264-candidate" not in help_result.stdout:
    raise AssertionError("Stable H.264 candidate is missing from CLI help")

for invalid_args in (
    ("--profile", "dev", "--board", "exoanchor-prototype-v2.3"),
    ("--profile", "stable", "--board", "exoanchor-prototype-v2.1"),
):
    result = subprocess.run(
        ["bash", str(build_path), *invalid_args, "--stable-h264-candidate"],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 64 or "only supports --profile stable" not in result.stderr:
        raise AssertionError(
            f"Stable H.264 candidate accepted invalid arguments: {invalid_args!r}"
        )

print("build profile contract: ok")
