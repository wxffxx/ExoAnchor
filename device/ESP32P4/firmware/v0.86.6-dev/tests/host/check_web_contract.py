#!/usr/bin/env python3
"""Check that the embedded UI, firmware routes, and local mock stay aligned."""

from pathlib import Path
import re
import sys


PROJECT_DIR = Path(__file__).resolve().parents[2]
SDKCONFIG_DEFAULTS = PROJECT_DIR / "sdkconfig.defaults"
APP_MAIN = PROJECT_DIR / "main" / "app" / "app_main.c"
WWW_DIR = PROJECT_DIR / "main" / "www"
WEB_SERVER = PROJECT_DIR / "main" / "services" / "web_server.c"
DEVICE_HTTP = PROJECT_DIR / "main" / "services" / "device_http.c"
SSH_MODULE = PROJECT_DIR / "main" / "services" / "web" / "ssh_module.inc"
CONSOLE_CREDENTIALS_MODULE = PROJECT_DIR / "main" / "services" / "web" / "console_credentials_module.inc"
CONSOLE_LOGIN_MCP_MODULE = PROJECT_DIR / "main" / "services" / "web" / "console_login_mcp_module.inc"
CONSOLE_CREDENTIALS_SERVICE = PROJECT_DIR / "main" / "application" / "console_credentials.c"
SSH_CLIENT = PROJECT_DIR / "main" / "services" / "ssh_client.c"
AGENT_HTTP_ROUTES = PROJECT_DIR / "main" / "adapters" / "agent_http_routes.c"
HTTP_API = PROJECT_DIR / "main" / "adapters" / "http_api.c"
AUTH_SERVICE = PROJECT_DIR / "main" / "application" / "auth_service.c"
AGENT_TOOL_DISPATCH = PROJECT_DIR / "main" / "services" / "web" / "agent_tool_dispatch_module.inc"
AGENT_ASK_USER_TOOL = PROJECT_DIR / "main" / "services" / "web" / "agent_ask_user_tool_module.inc"
AGENT_SSH_TOOL = PROJECT_DIR / "main" / "services" / "web" / "agent_ssh_tool_module.inc"
OTA_SETTINGS = PROJECT_DIR / "main" / "services" / "web" / "ota_settings_module.inc"
OTA_RUNTIME = PROJECT_DIR / "main" / "services" / "web" / "ota_runtime_module.inc"
AGENT_REQUEST = PROJECT_DIR / "main" / "services" / "web" / "agent_request_module.inc"
AGENT_RUN_ENGINE = PROJECT_DIR / "main" / "services" / "web" / "agent_run_engine_module.inc"
AGENT_API_SETTINGS = PROJECT_DIR / "main" / "application" / "agent_api_settings.c"
AGENT_API_SETTINGS_H = PROJECT_DIR / "main" / "application" / "agent_api_settings.h"
MODEL_PROVIDER = PROJECT_DIR / "main" / "core" / "model_provider.c"
AGENT_DATA = PROJECT_DIR / "main" / "services" / "web" / "agent_data_http_module.inc"
AGENT_BROKER_HTTP = PROJECT_DIR / "main" / "services" / "web" / "agent_broker_http_module.inc"
AGENT_RUN_HTTP = PROJECT_DIR / "main" / "services" / "web" / "agent_run_http_module.inc"
AGENT_RUN_CHECKPOINT = PROJECT_DIR / "main" / "services" / "web" / "agent_run_checkpoint_module.inc"
AGENT_RUN_TASK = PROJECT_DIR / "main" / "services" / "web" / "agent_run_task_module.inc"
AGENT_TASK_SERVICE = PROJECT_DIR / "main" / "application" / "agent_task_service.c"
AGENT_EVENT_STORE = PROJECT_DIR / "main" / "infrastructure" / "agent_event_store.c"
AGENT_EXECUTION_GUARD = PROJECT_DIR / "main" / "core" / "agent_execution_guard.c"
AGENT_VERIFIER = PROJECT_DIR / "main" / "core" / "agent_verifier.c"
AGENT_REPOSITORY = PROJECT_DIR / "main" / "infrastructure" / "agent_repository.c"
AGENT_REQUEST_BROKER = PROJECT_DIR / "main" / "application" / "agent_request_broker.c"
AGENT_REQUEST_BROKER_H = PROJECT_DIR / "main" / "application" / "agent_request_broker.h"
ACTION_EVENT_H = PROJECT_DIR / "main" / "core" / "action_event.h"
AGENT_TOOL_CORE = PROJECT_DIR / "main" / "services" / "web" / "agent_tool_core_module.inc"
AGENT_DEVICE_TOOLS = PROJECT_DIR / "main" / "services" / "web" / "agent_device_tools_module.inc"
AGENT_UART_TOOLS = PROJECT_DIR / "main" / "services" / "web" / "agent_uart_tool_module.inc"
AGENT_ACTION_EVENT_RUNTIME = PROJECT_DIR / "main" / "services" / "web" / "agent_action_event_runtime_module.inc"
AGENT_ROUTER = PROJECT_DIR / "main" / "core" / "agent_router.c"
TARGET_UART = PROJECT_DIR / "main" / "drivers" / "target_uart.c"
UART_MODULE = PROJECT_DIR / "main" / "services" / "web" / "uart_module.inc"
ACCESS_MODE_HTTP = PROJECT_DIR / "main" / "services" / "web" / "access_mode_http_module.inc"
ASYNC_HTTP = PROJECT_DIR / "main" / "services" / "web" / "async_http_module.inc"
POWER_CONTROL = PROJECT_DIR / "main" / "drivers" / "power_control.c"
PARTITIONS = PROJECT_DIR / "partitions.csv"
EXPECTED_HTTP_ROUTES = PROJECT_DIR / "tests" / "host" / "expected_http_routes.txt"
UI_MOCK = PROJECT_DIR / "tools" / "serve-ui.py"
SHELL_CSS_FILE = WWW_DIR / "assets" / "ui-shell.css"
BRAND_MARK_FILE = WWW_DIR / "assets" / "exoanchor-ui-mark.svg"
FAVICON_FILE = WWW_DIR / "assets" / "favicon.svg"
BRAND_SOURCE_DIR = PROJECT_DIR.parents[3] / "assets" / "brand" / "web"
CORE_CSS = "/assets/ui-core.css"
CORE_JS = "/assets/ui-core.js"
SHELL_CSS = "/assets/ui-shell.css"
SHELL_JS = "/assets/ui-shell.js"

API_PATTERN = re.compile(r"/api/[A-Za-z0-9_./?-]+")
FIRMWARE_ROUTE_PATTERN = re.compile(
    r'register_uri\([^,]+,\s*"([^"]+)",\s*(HTTP_[A-Z]+)'
)
DIRECT_FIRMWARE_ROUTE_PATTERN = re.compile(
    r'httpd_uri_t\s+\w+\s*=\s*\{(?:(?!\};).)*?'
    r'\.uri\s*=\s*"([^"]+)"(?:(?!\};).)*?'
    r'\.method\s*=\s*(HTTP_[A-Z]+)',
    re.S,
)
ROUTE_TABLE_PATTERN = re.compile(r'\.uri\s*=\s*"(/api/[^"]+)"')
AGENT_ROUTE_ENTRY_PATTERN = re.compile(
    r'\{\.uri\s*=\s*"([^"]+)",\s*\.method\s*=\s*(HTTP_[A-Z]+),'
)
EXPECTED_AGENT_ROUTES = {
    ("/api/settings/agent-api", "HTTP_GET"),
    ("/api/settings/agent-api", "HTTP_POST"),
    ("/api/settings/agent-api/models", "HTTP_GET"),
    ("/api/settings/agent-prompt", "HTTP_GET"),
    ("/api/settings/agent-prompt", "HTTP_POST"),
    ("/api/settings/agent-tools", "HTTP_GET"),
    ("/api/settings/agent-tools", "HTTP_POST"),
    ("/api/agent/sessions", "HTTP_GET"),
    ("/api/agent/sessions", "HTTP_POST"),
    ("/api/agent/sessions/delete", "HTTP_POST"),
    ("/api/agent/history", "HTTP_GET"),
    ("/api/agent/history", "HTTP_POST"),
    ("/api/agent/history/clear", "HTTP_POST"),
    ("/api/agent/memory", "HTTP_GET"),
    ("/api/agent/memory", "HTTP_POST"),
    ("/api/agent/memory/clear", "HTTP_POST"),
    ("/api/agent/data/clear", "HTTP_POST"),
    ("/api/agent/run", "HTTP_POST"),
    ("/api/agent/run/abort", "HTTP_POST"),
    ("/api/agent/run/events", "HTTP_GET"),
    ("/api/agent/run/pause", "HTTP_POST"),
    ("/api/agent/run/resume", "HTTP_POST"),
    ("/api/agent/run/steer", "HTTP_POST"),
    ("/api/agent/run/status", "HTTP_GET"),
    ("/api/agent/run/cancel", "HTTP_POST"),
    ("/api/agent/requests", "HTTP_GET"),
    ("/api/agent/requests/decision", "HTTP_POST"),
    ("/api/agent/requests/cancel", "HTTP_POST"),
    ("/api/agent/context-catalog", "HTTP_GET"),
}


def normalized_api_paths(text: str) -> set[str]:
    return {match.split("?", 1)[0] for match in API_PATTERN.findall(text)}


def expected_http_routes() -> set[tuple[str, str]]:
    routes = set()
    for line in EXPECTED_HTTP_ROUTES.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        method, uri = line.split(maxsplit=1)
        routes.add((uri, method))
    return routes


def main() -> int:
    frontend_paths: set[str] = set()
    failures = []
    for page in sorted(WWW_DIR.glob("*.html")):
        text = page.read_text(encoding="utf-8")
        frontend_paths.update(normalized_api_paths(text))
        if CORE_CSS not in text or CORE_JS not in text:
            failures.append(f"{page.name} does not load the shared UI core")
        if SHELL_CSS not in text or SHELL_JS not in text:
            failures.append(f"{page.name} does not load the shared UI shell")
        if '<link rel="icon" href="/favicon.svg" type="image/svg+xml">' not in text:
            failures.append(f"{page.name} does not use the ExoAnchor favicon")
        if 'id="appShell"' not in text or "ExoAnchorShell.mount(" not in text:
            failures.append(f"{page.name} does not mount the shared UI shell")
        if '<nav class="top"' in text:
            failures.append(f"{page.name} still contains a page-specific top navigation")
        for duplicate in ("const API={", "const AuthUI={", "const Session={"):
            if duplicate in text:
                failures.append(f"{page.name} still defines duplicate frontend infrastructure: {duplicate}")
        if 'id="authModal"' in text or 'id="logoutModal"' in text:
            failures.append(f"{page.name} still embeds a page-specific auth/logout modal")
        if "switch-control" in re.sub(r"<style>.*?</style>", "", text, flags=re.S):
            failures.append(f"{page.name} still uses the legacy switch markup")

    kvm_text = (WWW_DIR / "kvm.html").read_text(encoding="utf-8")
    video_style = re.search(r"\.screen img\{([^}]*)\}", kvm_text)
    if not video_style:
        failures.append("KVM video sizing rule is missing")
    else:
        declarations = video_style.group(1).replace(" ", "")
        for marker in (
            "position:absolute",
            "width:calc(100%-16px)",
            "height:calc(100%-16px)",
            "min-width:0",
            "min-height:0",
            "object-fit:contain",
        ):
            if marker not in declarations:
                failures.append(f"KVM video sizing guard missing: {marker}")
    for marker in ("displayModeSelect", "display-pixel", "display-native", "si_kvm_display_mode"):
        if marker in kvm_text:
            failures.append(f"KVM obsolete display-mode option remains: {marker}")
    for marker in (
        'new BroadcastChannel("exoanchor-kvm-video")',
        "function stopVideoForPeer()",
        "function claimVideo(",
        'videoClaimChannel.postMessage({type:"claim"',
        'window.addEventListener("focus"',
        "await API.ensure()",
        "void touchKvm(false,true)",
        "startKvmLease({rotate:true,resetStream:true,delay:0})",
    ):
        if marker not in kvm_text:
            failures.append(f"KVM cross-tab stream handoff missing: {marker}")
    hidden_branch = re.search(
        r"if\(document\.hidden\)\{kvmWindowBlurred=true;(.*?)return\}",
        kvm_text,
    )
    if not hidden_branch:
        failures.append("KVM visibility-loss cleanup branch is missing")
    elif "releaseKvmLease" in hidden_branch.group(1):
        failures.append("KVM visibility loss still releases the warm device lease")

    shell_js_text = SHELL_CSS_FILE.with_name("ui-shell.js").read_text(encoding="utf-8")
    shell_css_text = SHELL_CSS_FILE.read_text(encoding="utf-8")
    if not BRAND_MARK_FILE.is_file() or BRAND_MARK_FILE.read_bytes() != (
        BRAND_SOURCE_DIR / "exoanchor-ui-mark.svg"
    ).read_bytes():
        failures.append("embedded UI mark does not match the prepared brand asset")
    if not FAVICON_FILE.is_file() or FAVICON_FILE.read_bytes() != (
        BRAND_SOURCE_DIR / "favicon.svg"
    ).read_bytes():
        failures.append("embedded favicon does not match the prepared brand asset")
    for marker in (
        'const BRAND_MARK_HTML = \'<img src="/assets/exoanchor-ui-mark.svg" alt="">\';',
        '<div class="logo" aria-hidden="true">\' + BRAND_MARK_HTML',
        '<span aria-hidden="true">\' + BRAND_MARK_HTML',
        '<span class="ea-assistant-mark" aria-hidden="true">\' + BRAND_MARK_HTML',
    ):
        if marker not in shell_js_text:
            failures.append(f"shared shell brand integration missing {marker}")
    if "ASSISTANT_MARK" in shell_js_text or '<div class="logo">EA</div>' in shell_js_text:
        failures.append("shared shell still renders the temporary EA text mark")
    for marker in ("navigationPending", "location.assign(destination)"):
        if marker in shell_js_text:
            failures.append(f"shared navigation still blocks native browser routing: {marker}")
    for marker in (
        'id="agentNav"',
        "data-agent-href=",
        'aria-disabled="true" tabindex="-1"',
        'UI.api.get("/api/settings/product-features")',
        'window.addEventListener("exoanchor:product-features-changed"',
        'new BroadcastChannel(',
        '"message", handleAgentFeatureBroadcast',
        'window.addEventListener("focus", refreshAgentAvailabilityOnResume)',
        'document.visibilityState === "visible"',
        'link.removeAttribute("href")',
        'link.setAttribute("href", link.dataset.agentHref || "/agent")',
        "agentAvailabilityGeneration += 1",
        "generation === agentAvailabilityGeneration",
        'link.getAttribute("aria-disabled") === "true"',
    ):
        if marker not in shell_js_text:
            failures.append(f"shared Agent runtime navigation gate missing {marker}")
    for marker in (
        ".ea-shell .nav a.agent-disabled",
        '.ea-shell .nav a[aria-disabled="true"]',
    ):
        if marker not in shell_css_text:
            failures.append(f"shared Agent disabled navigation styling missing {marker}")
    agent_page_text = (WWW_DIR / "agent.html").read_text(encoding="utf-8")
    for marker in ("if(cfg)refreshAgentModels()", "setAgentModelFallback(agentProfile.value);refreshAgentModels()"):
        if marker in agent_page_text:
            failures.append(f"Agent page still auto-enumerates remote models: {marker}")
    agent_data_text = AGENT_DATA.read_text(encoding="utf-8")
    for marker in ("agent_cloud_get_json(", "esp_http_client_"):
        if marker in agent_data_text:
            failures.append(f"model-list HTTP route still blocks on cloud I/O: {marker}")
    for marker in (
        "si_agent_repository_clear_all(&cleared)",
        "session.principal != SI_PRINCIPAL_BROWSER",
        "AGENT_DATA_CLEAR_CONFIRMATION",
        "agent_run_state_is_active(s_agent_run_job.state)",
        '"mcp_exposed", false',
    ):
        if marker not in agent_data_text:
            failures.append(f"manual Agent data deletion boundary missing {marker}")
    if '"name", "agent_data_clear"' in (
        PROJECT_DIR / "main" / "services" / "device_http.c"
    ).read_text(encoding="utf-8"):
        failures.append("destructive Agent data deletion leaked into MCP tool capabilities")

    settings_page_text = (WWW_DIR / "settings.html").read_text(encoding="utf-8")
    for marker in (
        'new CustomEvent("exoanchor:product-features-changed"',
        'new BroadcastChannel(PRODUCT_FEATURES_CHANNEL_NAME)',
        'type:"embedded-agent-updated"',
    ):
        if marker not in settings_page_text:
            failures.append(
                f"Settings product-feature updates do not notify the shared shell: {marker}"
            )
    agent_api_settings_text = AGENT_API_SETTINGS.read_text(encoding="utf-8")
    agent_api_settings_h_text = AGENT_API_SETTINGS_H.read_text(encoding="utf-8")
    model_provider_text = MODEL_PROVIDER.read_text(encoding="utf-8")
    agent_request_text = AGENT_REQUEST.read_text(encoding="utf-8")
    agent_run_engine_text = AGENT_RUN_ENGINE.read_text(encoding="utf-8")
    agent_ssh_tool_text = AGENT_SSH_TOOL.read_text(encoding="utf-8")
    for source_name, source_text in (
        ("Agent page", agent_page_text),
        ("Settings page", settings_page_text),
        ("Agent API defaults", agent_api_settings_text),
        ("Local UI mock", UI_MOCK.read_text(encoding="utf-8")),
    ):
        for marker in ("Kimi K3", "kimi-k3"):
            if marker not in source_text:
                failures.append(f"{source_name} is missing K3 marker: {marker}")
    if "#define SI_AGENT_API_PROFILE_COUNT 5" not in agent_api_settings_h_text:
        failures.append("Agent API profile count does not include Kimi K3")
    custom_slot = agent_api_settings_text.find(
        '{"custom", "Custom", "custom", "", "gpt-4o"}'
    )
    kimi_slot = agent_api_settings_text.find(
        '{"kimi", "Kimi K3", "kimi", "https://api.moonshot.cn/v1", "kimi-k3"}'
    )
    if custom_slot < 0 or kimi_slot <= custom_slot:
        failures.append("Kimi K3 must be appended after existing NVS profile slots")
    for marker in (
        "si_agent_api_validate_endpoint(endpoint, false)",
        "strchr(value + 8, '?')",
        "strchr(value + 8, '#')",
        "port_value > (65535U - digit) / 10U",
    ):
        if marker not in agent_api_settings_text:
            failures.append(f"Agent endpoint fail-closed validation missing {marker}")
    endpoint_gate = agent_run_engine_text.find(
        "agent_api_validate_endpoint(profile.endpoint, false)"
    )
    secret_read = agent_run_engine_text.find(
        "agent_api_get_secret_for_profile(", endpoint_gate
    )
    if endpoint_gate < 0 or secret_read < 0 or endpoint_gate > secret_read:
        failures.append("Agent endpoint must be validated before reading its API key")
    for marker in (
        'text_equal_ci(provider, "kimi")',
        'text_starts_with_ci(model, "kimi-k3")',
        'capabilities->base64_image_input = true;',
        'capabilities->public_image_url',
        'capabilities->preserve_assistant_message = true;',
        'capabilities->fixed_sampling_parameters = true;',
        'capabilities->default_reasoning_effort = "low";',
    ):
        if marker not in model_provider_text:
            failures.append(f"K3 capability matrix guard missing: {marker}")
    for marker in (
        '"reasoning_effort"',
        "if (!capabilities.fixed_sampling_parameters)",
        "agent_provider_allows_screenshot",
        '"kimi-k3"',
    ):
        if marker not in agent_request_text:
            failures.append(f"K3 request adapter guard missing: {marker}")
    for marker in (
        "previous_assistant_message",
        "agent_run_preserve_assistant_message",
        "model_capabilities.preserve_assistant_message",
    ):
        if marker not in agent_run_engine_text:
            failures.append(f"K3 assistant-message continuity guard missing: {marker}")

    firmware_sources = list((PROJECT_DIR / "main").rglob("*.c"))
    firmware_sources += list((PROJECT_DIR / "main").rglob("*.inc"))
    firmware_text = "\n".join(path.read_text(encoding="utf-8") for path in firmware_sources)
    agent_routes_text = AGENT_HTTP_ROUTES.read_text(encoding="utf-8")
    registered_routes = {(uri, method) for uri, method in FIRMWARE_ROUTE_PATTERN.findall(firmware_text)}
    registered_routes.update(
        (uri, method) for uri, method in DIRECT_FIRMWARE_ROUTE_PATTERN.findall(firmware_text)
    )
    registered_routes.update(
        (uri, method) for uri, method in AGENT_ROUTE_ENTRY_PATTERN.findall(firmware_text)
    )
    expected_routes = expected_http_routes()
    if registered_routes != expected_routes:
        missing = sorted(expected_routes - registered_routes)
        extra = sorted(registered_routes - expected_routes)
        failures.append(f"HTTP route contract mismatch: missing={missing}, extra={extra}")

    http_api_text = HTTP_API.read_text(encoding="utf-8")
    auth_text = AUTH_SERVICE.read_text(encoding="utf-8")
    for marker in (
        "mbedtls_pkcs5_pbkdf2_hmac_ext(",
        "AUTH_PASSWORD_SALT_KEY",
        "AUTH_BOOTSTRAP_REQUIRED_KEY",
        'AUTH_FACTORY_PASSWORD "admin"',
        "generate_bootstrap_password(",
        "si_auth_reset_bootstrap(",
        "esp_fill_random(token, sizeof(token));",
        "AUTH_SESSION_ABSOLUTE_MS",
        "AUTH_FAILURE_LIMIT",
        "si_auth_revoke_all_sessions();",
    ):
        if marker not in auth_text:
            failures.append(f"authentication hardening guard missing {marker}")
    for marker in (
        'const char *name = "EA_SESSION=";',
        "HttpOnly; SameSite=Strict",
        "media-src 'self' blob:",
        "SI_HTTP_SESSION_COOKIE_MAX_AGE_SECONDS 86400U",
        'httpd_req_get_hdr_value_len(req, name)',
        "copy_next_session_cookie(",
        "si_http_sync_browser_session_cookie_from_bearer(",
        "session.principal != SI_PRINCIPAL_BROWSER",
        '"Web auth rejected: %s, cookie=%uB, candidates=%u"',
        "bootstrap_request_may_pass(",
        'strcmp(req->uri, "/api/settings/account") == 0',
        "send_ret == ESP_OK ? ESP_ERR_INVALID_STATE : send_ret",
        "successfully transmitted denial is still a failed authorization",
    ):
        if marker not in http_api_text:
            failures.append(f"HTTP session boundary missing {marker}")
    sdkconfig_defaults_text = SDKCONFIG_DEFAULTS.read_text(encoding="utf-8")
    if "CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y" not in sdkconfig_defaults_text:
        failures.append("H.264 WebSocket post-handshake support is disabled")
    for marker in (
        "cfg.ws_post_handshake_cb = si_h264_stream_ws_post_handshake",
        "si_h264_stream_ws_post_handshake(httpd_req_t *req)",
    ):
        if marker not in firmware_text:
            failures.append(f"H.264 WebSocket startup contract missing {marker}")
    h264_stream_text = (
        PROJECT_DIR / "main" / "services" / "video_h264_stream.c"
    ).read_text(encoding="utf-8")
    for marker in (
        "SI_H264_PIPELINE_IDLE_GRACE_MS",
        "H.264 client replaced; reusing open codec pipeline",
        "codec closed and MJPEG capture profile restored",
        "MJPEG restore deferred after H.264 teardown",
        "s_restore_pending",
        "SI_H264_RESTORE_RETRY_MAX_MS",
    ):
        if marker not in h264_stream_text:
            failures.append(f"H.264 reconnect lifecycle missing {marker}")
    base_settings_text = (
        PROJECT_DIR / "main" / "services" / "web" /
        "base_settings_http_module.inc"
    ).read_text(encoding="utf-8")
    ui_core_text = (
        WWW_DIR / "assets" / "ui-core.js"
    ).read_text(encoding="utf-8")
    if "mcp_client && auth_status.using_default" not in base_settings_text:
        failures.append("MCP can authenticate with the temporary bootstrap credential")
    for marker in (
        "首次使用请以默认账户 admin / admin 登录",
        "当前账户仍使用默认凭据，请设置至少六位的新密码。",
        "return await this.requireLogin(state);",
        'byId("authCurrentPassword").value = currentPassword;',
    ):
        if marker not in ui_core_text:
            failures.append(f"factory claim UI missing {marker}")
    mock_text = UI_MOCK.read_text(encoding="utf-8")
    for marker in (
        'os.environ.get("SI_UI_FACTORY_CLAIM", "")',
        '"min_length": 6',
        'password must be 6..64 printable ASCII characters',
    ):
        if marker not in mock_text:
            failures.append(f"factory claim browser mock missing {marker}")
    for marker in (
        "char refreshed_cookie[SI_HTTP_SESSION_COOKIE_MAX_LEN]",
        "si_http_sync_browser_session_cookie_from_bearer(",
        "si_secret_store_clear(refreshed_cookie",
    ):
        if marker not in base_settings_text:
            failures.append(
                f"browser image-stream cookie recovery missing {marker}"
            )
    if "char cookie[384]" in http_api_text:
        failures.append("HTTP authentication still truncates real-browser Cookie headers")
    if 'httpd_query_key_value(query, "auth"' in http_api_text:
        failures.append("HTTP authentication still accepts a bearer token in the URL query")
    absolute_guard = auth_text.find("if (age_ms > AUTH_SESSION_ABSOLUTE_MS)")
    expiry_guard = auth_text.find("if (!settings || !settings->auto_logout_enabled)", absolute_guard)
    if absolute_guard < 0 or expiry_guard < 0:
        failures.append("24-hour absolute session expiry is no longer enforced")

    tool_dispatch_text = AGENT_TOOL_DISPATCH.read_text(encoding="utf-8")
    ask_user_tool_text = AGENT_ASK_USER_TOOL.read_text(encoding="utf-8")
    action_event_runtime_text = AGENT_ACTION_EVENT_RUNTIME.read_text(encoding="utf-8")
    if "si_agent_tool_policy_allows(" not in tool_dispatch_text:
        failures.append("Agent tool dispatch does not enforce the saved tool policy")
    agent_tools_settings_text = (
        PROJECT_DIR / "main" / "application" / "agent_tools_settings.c"
    ).read_text(encoding="utf-8")
    device_http_text = (
        PROJECT_DIR / "main" / "services" / "device_http.c"
    ).read_text(encoding="utf-8")
    for marker in (
        "SI_AGENT_TOOL_AUDIENCE_AGENT",
        "SI_AGENT_TOOL_AUDIENCE_MCP",
        '"agent_enabled"',
        '"mcp_enabled"',
        "si_mcp_tool_policy_allows(",
    ):
        if marker not in agent_tools_settings_text:
            failures.append(f"dual Agent/MCP tool policy missing {marker}")
    for marker in (
        '"exoanchor.agent_tools.v2"',
        '"agent_callable"',
        '"mcp_callable"',
        '"agent_enabled"',
        '"mcp_enabled"',
    ):
        if marker not in device_http_text:
            failures.append(f"tool access capability contract missing {marker}")
    for marker in (
        "mcp_policy_tool_for_request(",
        "MCP endpoint is not mapped to a managed tool",
        "si_mcp_tool_policy_allows(",
    ):
        if marker not in http_api_text:
            failures.append(f"device-side MCP tool gate missing {marker}")

    ota_settings_text = OTA_SETTINGS.read_text(encoding="utf-8")
    ota_runtime_text = OTA_RUNTIME.read_text(encoding="utf-8")
    for marker in (
        'strncmp(url, "https://", 8) == 0',
        "ota_board_matches(board)",
        "ota_sha256_is_valid(sha)",
        ".disable_auto_redirect = true",
        "strcasecmp(sha_hex, manifest->sha256) != 0",
        "read_len == HTTPD_SOCK_ERR_TIMEOUT",
        "last_progress_us = esp_timer_get_time()",
        "OTA_WITH_SEQUENTIAL_WRITES",
        "handle_consumed = true",
    ):
        if marker not in ota_settings_text + ota_runtime_text:
            failures.append(f"OTA integrity/transport guard missing {marker}")
    if "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" not in SDKCONFIG_DEFAULTS.read_text(encoding="utf-8"):
        failures.append("bootloader OTA rollback must be enabled")
    app_main_text = APP_MAIN.read_text(encoding="utf-8")
    for marker in (
        "esp_ota_get_state_partition",
        "ESP_OTA_IMG_PENDING_VERIFY",
        "esp_ota_mark_app_valid_cancel_rollback",
        "confirm_running_ota_image();",
    ):
        if marker not in app_main_text:
            failures.append(f"OTA startup health confirmation missing {marker}")
    if "OTA_UPLOAD_IDLE_TIMEOUT_MS 30000" not in WEB_SERVER.read_text(encoding="utf-8"):
        failures.append("OTA upload idle-timeout recovery guard missing")
    partitions_text = PARTITIONS.read_text(encoding="utf-8")
    for marker in ("ota_0", "ota_1"):
        if marker not in partitions_text:
            failures.append(f"dual-slot OTA partition missing {marker}")

    target_uart_text = TARGET_UART.read_text(encoding="utf-8")
    for marker in (
        "s_http_backlog", "s_cli_backlog", "si_target_uart_read_cli",
        "s_journal_next_seq", "si_target_uart_journal_read",
        "si_target_uart_journal_read_from",
        "journal_history_lost",
    ):
        if marker not in target_uart_text:
            failures.append(f"UART consumer isolation guard missing {marker}")
    uart_module_text = UART_MODULE.read_text(encoding="utf-8")
    for marker in (
        "uart.replay", "uart_generation",
        "s_uart_ws_replaying", "HTTPD_WS_TYPE_BINARY",
        "uart_ws_has_manual_client", "uart_ws_pre_handshake",
        "uart_ws_post_handshake", "httpd_ws_send_frame(req, &frame)",
    ):
        if marker not in uart_module_text:
            failures.append(f"UART ordered replay contract missing {marker}")
    uart_handler_start = uart_module_text.find(
        "static esp_err_t uart_ws_handler(httpd_req_t *req)"
    )
    uart_handler_end = uart_module_text.find("\n#else", uart_handler_start)
    uart_handler_text = uart_module_text[uart_handler_start:uart_handler_end]
    if "req->method == HTTP_GET" in uart_handler_text:
        failures.append(
            "UART setup must run in pre/post-handshake callbacks on ESP-IDF 5.5"
        )
    web_server_text = WEB_SERVER.read_text(encoding="utf-8")
    uri_budget_match = re.search(
        r"#define SI_MAIN_HTTP_MAX_URI_HANDLERS (\d+)", web_server_text
    )
    uri_budget_required = len(expected_routes) + 8
    if not uri_budget_match:
        failures.append("main HTTP URI handler budget is not explicit")
    elif int(uri_budget_match.group(1)) < uri_budget_required:
        failures.append(
            "main HTTP URI handler budget has no route-growth headroom: "
            f"configured={uri_budget_match.group(1)} required>={uri_budget_required}"
        )
    for marker in (
        "cfg.ws_pre_handshake_cb = uart_ws_pre_handshake;",
        "cfg.ws_post_handshake_cb = uart_ws_post_handshake;",
    ):
        if marker not in web_server_text:
            failures.append(f"UART WebSocket callback registration missing {marker}")
    async_http_text = ASYNC_HTTP.read_text(encoding="utf-8")
    for marker in (
        "httpd_req_async_handler_begin", "httpd_req_async_handler_complete",
        "ssh_exec_async_handler", "storage_upload_async_handler",
        "storage_download_async_handler", "storage_list_async_handler",
        "storage_view_async_handler", "hid_actions_async_handler",
        "ota_install_async_handler", "SI_LONG_HTTP_MAX_WORKERS",
        "agent_sessions_get_async_handler",
        "agent_history_get_async_handler",
        "agent_memory_get_async_handler",
        "agent_data_clear_async_handler",
    ):
        if marker not in async_http_text:
            failures.append(f"long HTTP isolation contract missing {marker}")
    for marker in (
        ".sessions_get = agent_sessions_get_handler",
        ".history_get = agent_history_get_handler",
        ".memory_get = agent_memory_get_handler",
        '"/api/storage/list", HTTP_GET, storage_list_handler',
        '"/api/storage/view", HTTP_GET, storage_view_handler',
    ):
        if marker in WEB_SERVER.read_text(encoding="utf-8"):
            failures.append(f"blocking storage handler registered on main HTTP task: {marker}")
    power_control_text = POWER_CONTROL.read_text(encoding="utf-8")
    if "gpio_reserved_by_non_power_subsystem(" not in power_control_text:
        failures.append("power control does not reject GPIOs owned by another subsystem")
    device_http_text = DEVICE_HTTP.read_text(encoding="utf-8")
    for marker in (
        "USB_USJ_INT_PHY_DM_GPIO_NUM",
        "USB_USJ_INT_PHY_DP_GPIO_NUM",
        '"usb_host_dm", "USB HOST D-"',
        '"usb_host_dp", "USB HOST D+"',
    ):
        if marker not in device_http_text:
            failures.append(f"GPIO matrix USB host reservation missing {marker}")
    firmware_paths = {uri for uri, _ in registered_routes if uri.startswith("/api/")}
    agent_routes = set(AGENT_ROUTE_ENTRY_PATTERN.findall(agent_routes_text))
    if agent_routes != EXPECTED_AGENT_ROUTES:
        missing = sorted(EXPECTED_AGENT_ROUTES - agent_routes)
        extra = sorted(agent_routes - EXPECTED_AGENT_ROUTES)
        failures.append(
            f"Agent HTTP route contract mismatch: missing={missing}, extra={extra}"
        )
    mock_paths = normalized_api_paths(UI_MOCK.read_text(encoding="utf-8"))

    missing_firmware = sorted(frontend_paths - firmware_paths)
    missing_mock = sorted(frontend_paths - mock_paths)
    if missing_firmware:
        failures.append("frontend API routes missing from firmware: " + ", ".join(missing_firmware))
    if missing_mock:
        failures.append("frontend API routes missing from local mock: " + ", ".join(missing_mock))

    settings_text = (WWW_DIR / "settings.html").read_text(encoding="utf-8")
    overview_text = (WWW_DIR / "index.html").read_text(encoding="utf-8")
    for marker in (
        "工具能力授权",
        'class="tool-agent-enabled"',
        'class="tool-mcp-enabled"',
        "agent_enabled:",
        "mcp_enabled:",
    ):
        if marker not in settings_text:
            failures.append(f"Settings dual tool access UI missing {marker}")
    if 'addEventListener("scroll",updateSettingsMenu' in settings_text:
        failures.append("settings page still resets navigation state on every scroll")
    for section in ("target-info", "target", "kvm", "remote", "device", "agent", "storage", "diagnostics", "security", "system", "advanced", "gpio"):
        if f'id="{section}"' not in settings_text:
            failures.append(f"settings information architecture missing section {section}")
    for marker in (
        'href="#target-info" data-scope="target"><b>被控端信息</b><small>身份、配置、用途与备注',
        'href="#target" data-scope="target"><b>被控端访问</b><small>Console 与 SSH',
        'href="#device" data-scope="device"><b>设备与网络</b><small>设备、KVM、HID 与诊断',
        'href="#security"><b>账户与安全</b><small>本地账户和会话策略',
        'href="#agent"><b id="automationMenuTitle">集成与权限</b>',
        'href="#system"><b>系统与支持</b><small>远程更新与版本维护',
        'href="#advanced"><b>高级设置</b><small>硬件、存储、诊断与恢复',
        'id="remote" class="settings-subsection-title anchor-target" data-settings-section="target"><span>SSH 维护接入',
        'const SETTINGS_IDS=["target-info","target","device","security","agent","system","advanced"]',
        'id="advancedSettings" data-settings-section="advanced"',
        '.account-grid,.hardware-grid,.storage-grid,.kvm-grid,.control-grid,.agent-grid,.firmware-grid,.diagnostic-grid{grid-template-columns:minmax(0,1fr)}',
    ):
        if marker not in settings_text:
            failures.append(f"settings target/device boundary missing {marker}")
    for marker in (
        'id="targetProfileForm"',
        'id="targetDeviceType"',
        'id="targetOperatingSystem"',
        'id="targetEnvironment"',
        'id="targetConfiguration"',
        'id="targetPurpose"',
        'id="targetNotes"',
        'API.get("/api/settings/target-profile")',
        'API.post("/api/settings/target-profile",payload)',
        'id="agentDisplayName"',
        'id="agentIdentityForm"',
        'id="agentDisplayNameDefault"',
        'window.dispatchEvent(new CustomEvent("exoanchor:agent-name-changed"',
    ):
        if marker not in settings_text:
            failures.append(f"target profile / Agent identity UI missing {marker}")
    for marker in (
        "<h2>GPIO 映射</h2>",
        '<span>只读 GPIO 矩阵</span>',
        "function fixedGpioSet()",
        "<b>Locator LED 方向反转</b>",
        "<h3>Control and sensing</h3>",
        "gpioMatrixSummary.textContent=",
    ):
        if marker not in settings_text:
            failures.append(f"GPIO mapping/reservation UI missing {marker}")
    if 'id="gpioMatrix" class="section-title anchor-target" data-settings-section="device" data-settings-priority="90"' not in settings_text:
        failures.append("read-only GPIO matrix must be the final Device Settings group")
    for removed in (
        "Reserved GPIO",
        "Reserve GPIO 1",
        "refreshReserveGpioOptions",
        "gpioIsReserveRole",
        "用户保留",
        "定位灯",
    ):
        if removed in settings_text:
            failures.append(f"removed GPIO terminology still exposed in Settings: {removed}")
    if "定位灯" in overview_text or "定位灯" in settings_text:
        failures.append("Locator LED terminology is not consistently English")
    if ">Locator LED<" not in overview_text:
        failures.append("Overview Locator LED label is missing")
    for removed in ("POWER_ROLE_RESERVE1", 'role = "reserve1"', "reserve1_gpio"):
        if removed in power_control_text or removed in (
            PROJECT_DIR / "main" / "drivers" / "power_control.h"
        ).read_text(encoding="utf-8"):
            failures.append(f"removed reserve GPIO backend role still present: {removed}")
    if 'href="#kvm"><b>' in settings_text or 'href="#remote"><b>' in settings_text:
        failures.append("settings sidebar still splits target-host controls into separate entries")
    target_info_start = settings_text.find('id="target-info"')
    target_profile_form = settings_text.find('id="targetProfileForm"')
    target_settings_start = settings_text.find('id="target"')
    if not (
        0 <= target_info_start < target_profile_form < target_settings_start
    ):
        failures.append(
            "target profile is not isolated from the target control settings section"
        )
    for marker in (
        ".settings-menu a b{grid-column:1;grid-row:1",
        ".settings-menu a:after{content:\"\";grid-column:2;grid-row:1",
        'href="/terminal">打开 Terminal</a>',
    ):
        if marker not in settings_text:
            failures.append(f"settings navigation/terminal handoff missing {marker}")
    if 'id="general"' in settings_text:
        failures.append("settings still exposes the ambiguous general section")
    target_profile_header = (
        PROJECT_DIR / "main" / "application" / "target_profile_settings.h"
    ).read_text(encoding="utf-8")
    for marker in (
        "si_target_profile_settings_t",
        "si_target_profile_load(",
        "si_target_profile_save(",
    ):
        if marker not in target_profile_header:
            failures.append(
                f"machine-readable target/device boundary missing {marker}"
            )
    if 'id="overview"' in settings_text or "renderOverview(" in settings_text:
        failures.append("settings still duplicates the global device overview")
    if (WWW_DIR / "skills.html").exists():
        failures.append("obsolete standalone skills page still exists")
    for marker in ('id="deviceDiagnosticsSummary"', 'id="diagnosticDevice"', 'id="diagnosticNetwork"', 'id="diagnosticVideo"', 'id="diagnosticUsb"', 'id="diagnosticGpio"', 'id="diagnosticLogs"', "renderDiagnostics("):
        if marker not in settings_text:
            failures.append(f"settings diagnostics section missing {marker}")
    device_summary_start = settings_text.find('id="deviceDiagnosticsSummary"')
    target_start = settings_text.find('id="target-info"')
    detailed_logs = settings_text.find('id="diagnosticLogs"')
    if not (0 <= device_summary_start < target_start):
        failures.append("device diagnostic summary is not owned by Device Settings")
    if not (detailed_logs >= 0 and settings_text.rfind('data-settings-section="advanced"', 0, detailed_logs) >= 0):
        failures.append("detailed logs are not retained in Advanced Settings")

    overview_text = (WWW_DIR / "index.html").read_text(encoding="utf-8")
    for marker in ('>System DashBoard<', 'class="identity-action"', 'class="power-circle"', 'class="reset-circle"', 'id="holdPowerBtn"', 'class="hold-power-circle"', 'id="locatorToggle"', 'setLocatorEnabled(', 'powerAction("force_off")'):
        if marker not in overview_text:
            failures.append(f"overview dashboard layout missing {marker}")
    for marker in (
        '>累计登录次数<',
        'id="loginCountValue"',
        'Number(info.auth?.login_count)',
        '>成功认证计数<',
    ):
        if marker not in overview_text:
            failures.append(f"overview cumulative login count missing {marker}")
    for obsolete in ('id="nameValue"', 'id="nameMeta"', 'DEVICE_LABEL_LOADING'):
        if obsolete in overview_text:
            failures.append(f"overview still duplicates the shell device name {obsolete}")
    for marker in ('renderSettingsLocator(', 'id="settingsLocatorReverse"', 'id="settingsLocatorReverseMeta"', 'locator_reverse_on', 'locator_reverse_off', 'id="powerResetSwap"', '{swap_power_reset:true}'):
        if marker not in settings_text:
            failures.append(f"settings locator direction control missing {marker}")
    for obsolete in ('id="settingsLocatorToggle"', 'id="settingsLocatorMeta"', 'action:on?"locator_on":"locator_off"'):
        if obsolete in settings_text:
            failures.append(f"settings still duplicates Overview Locator on/off control {obsolete}")
    power_http_text = (PROJECT_DIR / "main" / "services" / "web" / "power_http_module.inc").read_text(encoding="utf-8")
    for marker in ('"return_gpio"', '"bidirectional"', '"reversed"', 'si_power_set_locator_reversed(true, true)', 'si_power_set_locator_reversed(false, true)', 'cJSON_IsTrue(swap_power_reset)', 'si_power_swap_button_gpios(true)'):
        if marker not in power_http_text:
            failures.append(f"power locator reverse API missing {marker}")
    for marker in ('id="previewFpsSelect"', 'preview_fps_x100:previewFpsX100', 'Overview 预览帧率'):
        if marker not in settings_text:
            failures.append(f"settings preview FPS control missing {marker}")
    for marker in ('>预览 FPS<', 'refreshSnapshot()', '独立于 KVM 采集模式'):
        if marker not in overview_text:
            failures.append(f"overview independent preview FPS missing {marker}")
    for marker in ('function fmtUsageBytes(region)', 'perfSramMeta.textContent=fmtUsageBytes(internal)', 'perfPsramMeta.textContent=fmtUsageBytes(psram)', 'renderExtraStorageDevices(storage.devices)', 'id="extraStorageMetrics"'):
        if marker not in overview_text:
            failures.append(f"overview concrete memory/storage expansion flow missing {marker}")
    for obsolete in ('perfSramMeta.textContent=`${fmtPercent(internal.usage_percent)}', 'perfPsramMeta.textContent=psram.total_bytes?`${fmtPercent(psram.usage_percent)}'):
        if obsolete in overview_text:
            failures.append(f"overview memory detail still duplicates ring percentage {obsolete}")
    device_http_text = (PROJECT_DIR / "main" / "services" / "device_http.c").read_text(encoding="utf-8")
    for marker in ('"exoanchor.storage.v1"', '"devices"', '"expansion_slots"', '"spi_data"'):
        if marker not in device_http_text:
            failures.append(f"firmware extensible storage contract missing {marker}")
    storage_mock_text = UI_MOCK.read_text(encoding="utf-8")
    for marker in ('"schema_version": "exoanchor.storage.v1"', '"id": "spi_data"', '"implemented": False'):
        if marker not in storage_mock_text:
            failures.append(f"local mock extensible storage contract missing {marker}")
    for obsolete in ('id="locatorToggleText"', 'id="locatorMeta"'):
        if obsolete in overview_text:
            failures.append(f"overview locator control still exposes redundant text {obsolete}")
    if 'id="settingsLocatorState"' in settings_text:
        failures.append("settings locator control still exposes redundant state text")
    for obsolete in ('class="hero-power"', 'id="forceOffBtn"', 'id="powerRows"', '>现在需要关注<', '>System performance · nominal<', '>完整诊断与设备信息<', 'id="network"', 'id="stream"', 'id="usbDetails"', 'id="gpioStatus"', 'id="logs"', 'id="downloadLogs"'):
        if obsolete in overview_text:
            failures.append(f"overview still contains obsolete block {obsolete}")
    if '/api/system/logs' in overview_text:
        failures.append("overview still polls diagnostic logs")
    for marker in ("previewPending", "previewRequestBusy", "renderPreviewControl(", 'aria-busy'):
        if marker not in overview_text:
            failures.append(f"overview preview optimistic state missing {marker}")

    net_driver_text = (PROJECT_DIR / "main" / "drivers" / "net_manager.c").read_text(encoding="utf-8")
    for marker in (
        "ETH_CMD_G_SPEED",
        "ETH_CMD_G_DUPLEX_MODE",
        "%s-duplex",
        "read Ethernet duplex mode failed",
        "install_eth_driver_with_retry",
        "ETH_DRIVER_INSTALL_ATTEMPTS 3",
        "Ethernet driver recovered on install attempt",
        "phy->del(phy)",
        "mac->del(mac)",
    ):
        if marker not in net_driver_text:
            failures.append(f"Ethernet link diagnostics missing {marker}")

    video_http_text = (PROJECT_DIR / "main" / "services" / "web" / "video_http_module.inc").read_text(encoding="utf-8")
    for marker in (
        "VIDEO_SNAPSHOT_FIRST_FRAME_TIMEOUT_MS",
        "Snapshot could not start demand-driven video capture",
        "si_video_control_keep_automation_alive(snapshot_actor);",
        "Snapshot first frame timeout",
    ):
        if marker not in video_http_text:
            failures.append(f"cold snapshot capture startup missing {marker}")

    diag_cli_text = (PROJECT_DIR / "main" / "services" / "diag_cli.c").read_text(encoding="utf-8")
    for marker in ("speed=%dMbps duplex=%s", "power_active=%d", "standby_active=%d"):
        if marker not in diag_cli_text:
            failures.append(f"serial hardware diagnostics missing {marker}")
    for marker in (
        "agent-data-clear CONFIRM",
        "si_diagnostics_agent_data_clear(",
        "refused: use exactly 'agent-data-clear CONFIRM'",
    ):
        if marker not in diag_cli_text:
            failures.append(f"serial Agent data deletion guard missing {marker}")
    device_http_text = (PROJECT_DIR / "main" / "services" / "device_http.c").read_text(encoding="utf-8")
    for marker in (
        '"connected", device_connected',
        '"device_connected", device_connected',
        '"frame_ready", video->frame_ready',
        '"state", state',
        '"device_connected",',
        'video.modes_count > 0',
    ):
        if marker not in device_http_text:
            failures.append(f"video physical/capture state split missing {marker}")
    video_input_text = (PROJECT_DIR / "main" / "drivers" / "video_input.c").read_text(encoding="utf-8")
    for marker in (
        "static void uvc_supervisor_task(void *arg)",
        "SI_UVC_MISSING_DEVICE_RECOVERY_DELAY_MS",
        "SI_UVC_MISSING_DEVICE_RECOVERY_INTERVAL_MS",
        "if (recover && !s_uvc_stream)",
        "ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))",
        "s_status.frame_interval_ms",
        '"UVC stream restarting"',
    ):
        if marker not in video_input_text:
            failures.append(f"UVC supervisor lifecycle missing {marker}")
    for marker in (
        '"runtime_owner"',
        '"embedded_esp32p4" : "external_mcp"',
        '"embedded_runtime_lifecycle"',
        '"active" : "excluded"',
        '"embedded_runtime_new_features"',
        '"build_profile", SI_BUILD_PROFILE',
        '"embedded_agent"',
        '"external_mcp"',
        '"board", SI_BOARD_ID',
        '"silicon_target", SI_SILICON_TARGET',
        '"human_kvm_preempts_agent", true',
        '"observations"',
        '"control_lease", "/api/control/lease"',
        '"runtime_surface"',
        '"runtime_callable"',
        '"uart_status"',
        '"runtime.uart_read"',
        '"runtime.console_login"',
        '"ensure_package"',
    ):
        if marker not in device_http_text:
            failures.append(f"Agent runtime ownership contract missing {marker}")
    agent_run_task_text = AGENT_RUN_TASK.read_text(encoding="utf-8")
    for marker in (
        "xTaskCreateWithCaps(",
        "AGENT_RUN_TASK_MEMORY_CAPS",
        "vTaskDeleteWithCaps(NULL);",
        "Agent run task internal stack high-water=%u",
        "Agent executor task allocation failed: stack=%u ",
        "uxTaskGetStackHighWaterMark(NULL)",
    ):
        if marker not in agent_run_task_text:
            failures.append(f"Agent cache-safe task lifecycle guard missing {marker}")
    if "xTaskCreate(agent_run_task" in agent_run_task_text:
        failures.append("Agent run task must retain explicit memory capabilities")
    agent_run_checkpoint_text = AGENT_RUN_CHECKPOINT.read_text(encoding="utf-8")
    agent_repository_text = AGENT_REPOSITORY.read_text(encoding="utf-8")
    for marker in (
        '"/RUN.CHECKPOINT"',
        '"/RUN.CHECKPOINT.TMP"',
        "si_agent_repository_run_checkpoint_read(",
        "si_agent_repository_run_checkpoint_clear(",
        "si_agent_repository_run_checkpoint_clear_if_job_id(",
    ):
        if marker not in agent_repository_text:
            failures.append(f"legacy Agent checkpoint migration input missing {marker}")
    for marker in (
        "RUN.CHECKPOINT belonged to the legacy executor",
        "agent_run_checkpoint_restore(",
        '"turn.outcome_unknown"',
        '"legacy_checkpoint_migrated"',
        '"blind_retry_forbidden", true',
        '"raw_goal_replayed", false',
        '"page_context_replayed", false',
        "si_agent_task_service_replay(",
        "si_agent_event_store_append(",
        "si_agent_repository_run_checkpoint_clear_if_job_id(",
        "AGENT_RUN_STATE_OUTCOME_UNKNOWN",
        '"重启后结果未知"',
        "resume is forbidden",
    ):
        if marker not in agent_run_checkpoint_text:
            failures.append(f"Agent fail-closed checkpoint migration missing {marker}")
    legacy_resumption_surface = agent_run_checkpoint_text + agent_run_task_text
    for marker in (
        "agent_run_checkpoint_write_locked(",
        "agent_run_resume_recovered(",
        "s_agent_run_job.recovered_after_reboot = true;",
    ):
        if marker in legacy_resumption_surface:
            failures.append(f"legacy Agent checkpoint remains resumable: {marker}")
    agent_task_service_text = AGENT_TASK_SERVICE.read_text(encoding="utf-8")
    agent_event_store_text = AGENT_EVENT_STORE.read_text(encoding="utf-8")
    for marker in (
        'SI_AGENT_EVENT_STORE_DIR "/TASKS.LOG"',
        '"previous_hash"',
        '"event_hash"',
        "si_agent_event_store_recover(",
        "si_agent_event_store_replay(",
        "fsync(descriptor)",
        "cursor is committed only after the VFS acknowledges media sync",
        "durable TASKS.LOG append failed",
    ):
        if marker not in agent_event_store_text:
            failures.append(f"canonical TASKS.LOG contract missing {marker}")
    for marker in (
        '"turn.submitted"',
        '"action.started"',
        '"action.result"',
        '"action.verified"',
        '"turn.outcome_unknown"',
        '"turn.interrupted"',
        '"blind_retry_forbidden"',
        "A tool return is not verification.",
        "unresolved_action_overflow",
        "SI_AGENT_TASK_ACTION_LEDGER_CAPACITY",
        "action_ledger_find(run_id, action_id)",
        "ledger->result_recorded",
        "ledger->plan_version != turn->plan_version",
        "scoped_idempotency_hash(",
        '"idempotency_key_hash"',
        "PASSED is intentionally unavailable through this legacy scalar API",
        "verification == SI_AGENT_VERIFICATION_PASSED",
        "return ESP_ERR_NOT_SUPPORTED;",
        "si_agent_task_service_record_readonly_completion_artifact(",
        "si_agent_task_service_complete_readonly(",
        "completion_artifact_matches(",
        "SI_AGENT_TASK_CRITERIA_DELIVERABLE",
        '"completion_criteria_kind"',
        '"artifact delivery cannot verify observation criteria"',
        "recover_open_turns(",
        "si_agent_task_runtime_pending_count(&s_runtime)",
    ):
        if marker not in agent_task_service_text:
            failures.append(f"Runtime V2 Task Service contract missing {marker}")
    if 'cJSON_AddStringToObject(payload, "idempotency_key",' in agent_task_service_text:
        failures.append("Runtime V2 Task Service persists a raw idempotency key")

    agent_verifier_text = AGENT_VERIFIER.read_text(encoding="utf-8")
    for marker in (
        "si_agent_evidence_kind_is_authoritative",
        "si_agent_evidence_issuer_is_authoritative",
        "item->kind != SI_AGENT_EVIDENCE_READBACK",
        "item->captured_ms <= spec->action_started_ms",
        "item->generation < spec->minimum_generation",
        "item->issuer != spec->required_issuer",
        "item->completion_criteria_hash",
        "sha256_hex_valid(item->artifact_hash)",
        '"tool/model success is not mutation evidence"',
    ):
        if marker not in agent_verifier_text:
            failures.append(f"independent Agent verification contract missing {marker}")
    finalizer_start = agent_run_task_text.find(
        "static bool agent_run_finalize_task_service("
    )
    finalizer_end = agent_run_task_text.find(
        "static void agent_run_task(", finalizer_start
    )
    agent_finalizer_text = agent_run_task_text[finalizer_start:finalizer_end]
    for marker in (
        "si_agent_result_failure_error(",
        "si_agent_result_nonempty_string(result, \"error\")",
        '"agent execution failed"',
    ):
        if marker not in agent_run_task_text:
            failures.append(
                f"Agent failed-result error projection missing {marker}"
            )
    worker_start = agent_run_task_text.find("static void agent_run_task(void *arg)")
    error_projection = agent_run_task_text.find(
        "si_agent_result_failure_error(", worker_start
    )
    shadow_finalize = agent_run_task_text.find(
        "agent_run_finalize_task_service(", error_projection
    )
    if not worker_start < error_projection < shadow_finalize:
        failures.append(
            "Agent failure error must be projected before shadow finalization"
        )
    worker_end = agent_run_task_text.find(
        "static esp_err_t agent_run_launch_execution(", worker_start
    )
    worker_text = agent_run_task_text[worker_start:worker_end]
    if worker_text.count(
        "ret != ESP_OK ? esp_err_to_name(ret)"
    ) < 2:
        failures.append(
            "Agent failed job/result sink may still render ESP_OK"
        )
    for marker in (
        "agent_result_requires_independent_verification(",
        "si_agent_task_service_record_readonly_completion_artifact(",
        "si_agent_task_service_complete_readonly(",
        '"response_delivered"',
        "observation criteria still requires independent evidence",
        '"blind_retry_forbidden"',
        '"runtime_shadow_affects_legacy_outcome"',
        '"mutation has no typed manager readback"',
        "si_agent_request_broker_terminalize_and_retire_run(",
    ):
        if marker not in agent_run_task_text:
            failures.append(f"Agent completion verification gate missing {marker}")
    for forbidden in (
        "si_agent_task_service_record_step_verification(\n"
        "                run_id, AGENT_RUNTIME_V2_STEP_ID,\n"
        "                SI_AGENT_VERIFICATION_PASSED",
        "si_agent_task_service_record_turn_verification(\n"
        "            run_id, SI_AGENT_VERIFICATION_PASSED",
        "si_agent_task_service_complete(run_id)",
        "si_agent_verify(",
    ):
        if forbidden in agent_finalizer_text:
            failures.append(
                "Agent finalizer bypasses typed completion ownership: "
                f"{forbidden}"
            )
    complete_readonly_start = agent_task_service_text.find(
        "esp_err_t si_agent_task_service_complete_readonly("
    )
    complete_readonly_end = agent_task_service_text.find(
        "esp_err_t si_agent_task_service_record_action_started(",
        complete_readonly_start,
    )
    complete_readonly_text = agent_task_service_text[
        complete_readonly_start:complete_readonly_end
    ]
    for marker in (
        "completion_artifact_matches(&record->completion_artifact, artifact)",
        "SI_AGENT_TASK_CRITERIA_DELIVERABLE",
        "*verification_out = si_agent_verify(&spec, &evidence, 1U);",
        "record_step_verification(",
        "record_turn_verification(",
        "si_agent_task_service_complete(run_id)",
    ):
        if marker not in complete_readonly_text:
            failures.append(
                f"Task Service typed completion gate missing {marker}"
            )
    for forbidden in (
        "si_agent_request_broker_cancel_run(run->job_id)",
        "si_agent_request_broker_retire_run(run->job_id)",
    ):
        if forbidden in agent_run_task_text:
            failures.append(
                f"Agent finalizer still uses split best-effort Broker cleanup: {forbidden}"
            )
    for forbidden in ("trusted_observation", "agent_result_sha256("):
        if forbidden in agent_run_task_text:
            failures.append(
                f"Agent model/tool result still self-issues completion evidence: {forbidden}"
            )

    agent_execution_guard_text = AGENT_EXECUTION_GUARD.read_text(encoding="utf-8")
    for marker in (
        "si_agent_execution_guard_evaluate(",
        "principal_capabilities & intent->origin_authority_ceiling",
        "intent->expected_policy_revision != intent->current_policy_revision",
        "intent->expected_lease_epoch != intent->current_lease_epoch",
        "intent->approval_required && !intent->grant_consumed",
        "human_kvm_active = intent->human_kvm_active",
    ):
        if marker not in agent_execution_guard_text:
            failures.append(f"Agent execution Guard contract missing {marker}")
    web_server_text = WEB_SERVER.read_text(encoding="utf-8")
    agent_run_engine_text = AGENT_RUN_ENGINE.read_text(encoding="utf-8")
    if (
        "#define AGENT_RUN_TASK_MEMORY_CAPS "
        "(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)"
        not in web_server_text
    ):
        failures.append("Agent run task cache-safe internal capability contract is missing")
    if (
        "#define AGENT_RUN_TASK_MEMORY_CAPS "
        "(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
        in web_server_text
    ):
        failures.append("Agent run task stack must not use cache-disabled PSRAM")
    if "#define AGENT_RUN_TASK_STACK 24576" not in web_server_text:
        failures.append(
            "Agent run task internal stack must fit the V2.4 live largest-block gate"
        )
    for forbidden in (
        "char latest_message[AGENT_HISTORY_CONTENT_STORE_MAX + 1]",
        "char report_message[AGENT_HISTORY_CONTENT_STORE_MAX + 1]",
        "char guarded_message[AGENT_HISTORY_CONTENT_STORE_MAX + 1]",
    ):
        if forbidden in agent_run_engine_text:
            failures.append(
                f"Agent large message buffer regressed onto task stack: {forbidden}"
            )
    for marker in (
        "char *latest_message = agent_alloc(AGENT_HISTORY_CONTENT_STORE_MAX + 1);",
        "guarded_message = agent_alloc(AGENT_HISTORY_CONTENT_STORE_MAX + 1);",
        'strlcpy(err, "agent message buffer alloc failed", err_size);',
    ):
        if marker not in agent_run_engine_text:
            failures.append(f"Agent heap-backed message buffer guard missing {marker}")
    if "si_ssh_exec_config_t config = {0};" in agent_ssh_tool_text:
        failures.append("Agent SSH config regressed onto the 24 KiB worker stack")
    for marker in (
        "si_ssh_exec_config_t *config = agent_ssh_exec_config_alloc();",
        "si_secret_store_clear(config, sizeof(*config));",
        "agent_ssh_exec_config_free(config);",
        'cJSON_AddStringToObject(item, "error", "ssh config alloc failed");',
    ):
        if marker not in agent_ssh_tool_text:
            failures.append(f"Agent heap-backed SSH config guard missing {marker}")
    loop_cap_block = (
        "cJSON_AddBoolToObject(ctx->resp, \"loop_max_reached\", true);"
    )
    if (
        loop_cap_block not in agent_run_task_text
        and loop_cap_block not in AGENT_RUN_ENGINE.read_text(encoding="utf-8")
    ):
        failures.append("Agent loop cap result marker is missing")
    loop_cap_start = agent_run_engine_text.find(loop_cap_block)
    loop_cap_end = agent_run_engine_text.find(
        "agent_json_set_number(ctx->resp, \"loop_iterations\"",
        loop_cap_start,
    )
    loop_cap_text = agent_run_engine_text[loop_cap_start:loop_cap_end]
    if (
        loop_cap_start < 0
        or loop_cap_end < 0
        or "loop_completed_at_limit" not in loop_cap_text
        or 'agent_json_set_bool(ctx->resp, "ok", false)' in loop_cap_text
        or "ctx->last_batch_ok = false" in loop_cap_text
    ):
        failures.append(
            "A successful final tool batch must not be converted to failure at the planning-loop cap"
        )
    agent_text = (WWW_DIR / "agent.html").read_text(encoding="utf-8")
    for marker in (
        'id="threadTargetProfile"',
        'function renderTargetProfile()',
        'API.get("/api/settings/target-profile")',
        'workspaceState.targetProfile=',
    ):
        if marker not in agent_text:
            failures.append(f"Agent target profile visibility missing {marker}")
    if '>读取中<' in agent_text:
        failures.append("agent model selector can remain in a permanent loading state")
    for marker in ("agentModelRequestSeq", "agentModelStorageKey", "MODEL FALLBACK", "refreshAgentConfig()", "bootAgentPage("):
        if marker not in agent_text:
            failures.append(f"agent model loading fallback missing {marker}")
    preview_init = agent_text.find('let previewEnabled=')
    status_subscribe = agent_text.find('UI.status.subscribe(renderAgentStatus)')
    if preview_init < 0 or status_subscribe < 0 or preview_init > status_subscribe:
        failures.append("agent subscribes to immediate status updates before preview state is initialized")
    for marker in (
        'class="workspace-shell"',
        'class="thread-layout"',
        'id="threadListResizer"',
        'id="threadContextResizer"',
        'bindColumnResizer(threadListResizer,"list")',
        'bindColumnResizer(threadContextResizer,"context")',
        'ea_agent_thread_list_width',
        'ea_agent_thread_context_width',
        'class="composer-settings"',
        'class="composer-models"',
        'id="agentKvmPermission"',
        'id="agentWebPermission"',
        'function updateAgentManualPermissions()',
        'const includeScreenshot=!agentKvmPermission.disabled&&agentKvmPermission.checked;',
        'const includeWebSearch=!agentWebPermission.disabled&&agentWebPermission.checked;',
        'if(!workspaceState.booted){',
        'await Promise.all([loadAgentToolSettings().catch(()=>null),loadWorkspaceData()]);',
        'class="composer-authority"',
        'class="ui-switch screen-preview-switch"',
        'togglePreview.onchange=',
        'localStorage.getItem("ea_agent_preview_enabled")==="1"',
        'previewEnabled&&activeLeftView==="screen"',
        '.screen-toolbar{top:10px;bottom:auto;justify-content:flex-end}',
        'data-workspace-view="threads"',
        'id="workspace-capabilities"',
        'id="workspace-runs"',
        ".session-entry:hover .session-delete",
        ".session-entry:focus-within .session-delete",
        'wrap.className="session-entry"',
        'del.setAttribute("aria-label"',
        'id="clearAgentData"',
        'API.post("/api/agent/data/clear",{confirm:"DELETE_ALL_AGENT_DATA"})',
        'document.dispatchEvent(new CustomEvent("exoanchor:agent-data-cleared"))',
    ):
        if marker not in agent_text:
            failures.append(f"agent session history hover-delete layout missing {marker}")
    for marker in (
        'let activeSessionId=optionalSessionId(localStorage.getItem("ea_agent_session"))',
        'function renderBlankThread()',
        'function materializeSession(text)',
        'if(!sessionId)sessionId=materializeSession(text);',
        'activeThreadMeta.textContent=activeSessionId?',
        'delete terminalLogs[id];',
    ):
        if marker not in agent_text:
            failures.append(f"agent blank/new conversation flow missing {marker}")
    for obsolete in (
        'if(!agentSessions.length)agentSessions=[{id:"default"',
        "if(agentSessions.length>1)",
        'activeSessionId="default"',
    ):
        if obsolete in agent_text:
            failures.append(f"agent still forces an undeletable default conversation {obsolete}")
    agent_sessions_text = (PROJECT_DIR / "main" / "services" / "web" / "agent_sessions_module.inc").read_text(encoding="utf-8")
    if 'cJSON_AddBoolToObject(root, "empty_allowed", true);' not in agent_sessions_text:
        failures.append("agent sessions API does not advertise an allowed empty workspace")
    if "agent_session_get_slot(sessions, count, AGENT_SESSION_DEFAULT_ID)" in agent_sessions_text:
        failures.append("agent sessions backend still synthesizes a default session")
    for marker in (
        'API.get("/api/capabilities")',
        'API.get("/api/settings/mcp")',
        'API.get("/api/agent/memory")',
        'window.addEventListener("hashchange"',
        'next==="threads"?location.pathname:`#${next}`',
        "item.runtime_callable===true",
        'registryGroup("Context Attachments"',
        'registryGroup("Manual Surfaces"',
    ):
        if marker not in agent_text:
            failures.append(f"agent workspace real-data boundary missing {marker}")
    if 'del.textContent="Delete"' in agent_text or "if(id===activeSessionId&&agentSessions.length>1)" in agent_text:
        failures.append("agent session delete still uses the legacy active-only full-width button")
    for marker in (
        "terminalProgressKey(",
        "terminalProgressIsRunning(",
        "settleTerminalProgress(",
        "line.progress_active===true",
        "settleTerminalProgress(sessionId);",
        "settleTerminalProgress(sessionId);\n    const marker=agentJobMarker(status)",
        "terminalFollowTail",
        "terminalNearBottom()",
        "const previousTop=agentTerminal.scrollTop",
        "renderTerminal(true)",
    ):
        if marker not in agent_text:
            failures.append(f"agent terminal progress lifecycle missing {marker}")
    if 'if((line.kind||"")==="progress")' in agent_text:
        failures.append("agent terminal still animates every historical progress event")
    for marker in ("agentPageLeaving", "stopAgentStream(true)", "agentStreamActive&&!agentPageLeaving"):
        if marker not in agent_text:
            failures.append(f"agent-to-kvm stream handoff guard missing {marker}")
    terminal_text = (WWW_DIR / "terminal.html").read_text(encoding="utf-8")
    for marker in (
        'id="sshSettings" class="button-link settings" href="/settings#target"',
        'API.get("/api/settings/ssh-target")',
        'id="sessionPasswordInput"',
        'autocomplete="off"',
        'needsSessionPassword()',
        'els.sshBadge.hidden=activeChannel==="uart"',
        'payload.password=sessionPassword',
        'if("password" in payload)payload.password=""',
        'socket.send(JSON.stringify(payload))',
        'sessionPassword=""',
        '一次性密码仅用于当前连接，不会保存',
        '当前 HTTP/WS 传输未加密，仅在可信局域网使用',
        'localStorage.getItem(TERMINAL_CHANNEL_STORAGE_KEY)==="uart"?"uart":"ssh"',
        'localStorage.setItem(TERMINAL_CHANNEL_STORAGE_KEY,activeChannel)',
        'switchChannel(activeChannel)',
        'AGENT ACTION REPLAY',
        '<span>TIME</span><span>TYPE</span><span>ACTION</span><span>RESULT</span>',
        'formatActionReplayTimestamp(',
        'describeAgentTerminalAction(',
        'className="agent-mirror-type"',
        'className="agent-mirror-action"',
        'className="agent-mirror-result"',
    ):
        if marker not in terminal_text:
            failures.append(f"terminal Settings-owned target flow missing {marker}")
    for obsolete in (
        "AGENT ACTION MIRROR",
        "tool[1]",
        "cursor=1",
        'status?.running?"LIVE · BACKGROUND":"REPLAY"',
    ):
        if obsolete in terminal_text:
            failures.append(f"terminal action replay exposes obsolete detail {obsolete}")
    for marker in (
        'id="gpioMatrixGrid"',
        'id="gpioMatrixSummary"',
        'function renderGpioMatrix(power)',
        'capabilities.gpio_matrix||lastGpioMatrix',
        '当前固件未分配',
        '<h2>GPIO 映射</h2>',
    ):
        if marker not in settings_text:
            failures.append(f"settings GPIO matrix component missing {marker}")
    for marker in (
        'add_gpio_matrix_json(root)',
        '"gpio_matrix"',
        '"target_uart_rx"',
        '"tf_sdmmc_d0"',
        '"eth_rmii_crs_dv"',
        '"debug_uart_tx"',
        '"ms2109_power_switch", "MS2109 3V3 POWER", "ms2109", "output"',
        '"ms2109_core_enable", "MS2109 1V2 ENABLE", "ms2109", "output"',
        '"ms2109_eeprom_wp", "MS2109 EEPROM WP", "ms2109", "output"',
        '"ms2109_eeprom_scl", "MS2109 EEPROM SCL", "ms2109", "inout"',
        '"ms2109_eeprom_sda", "MS2109 EEPROM SDA", "ms2109", "inout"',
    ):
        if marker not in device_http_text:
            failures.append(f"device GPIO matrix capability missing {marker}")
    for obsolete in ('id="connPanel"', 'id="connToggle"', 'id="host"', 'id="username"', 'id="password"'):
        if obsolete in terminal_text:
            failures.append(f"terminal still embeds SSH target editor {obsolete}")
    for forbidden in (
        'localStorage.setItem("sessionPassword',
        'sessionStorage.setItem("sessionPassword',
        'API.post("/api/settings/ssh-target",payload)',
    ):
        if forbidden in terminal_text:
            failures.append(f"terminal persists a one-time SSH password via {forbidden}")
    console_credentials_text = CONSOLE_CREDENTIALS_MODULE.read_text(encoding="utf-8")
    console_login_mcp_text = CONSOLE_LOGIN_MCP_MODULE.read_text(encoding="utf-8")
    console_service_text = CONSOLE_CREDENTIALS_SERVICE.read_text(encoding="utf-8")
    for marker in (
        'id="consoleCredentialsForm"',
        'id="consoleUsername"',
        'id="consolePassword" type="password"',
        'id="consoleClearPassword"',
        'API.get("/api/settings/console-credentials")',
        'API.post("/api/settings/console-credentials",payload)',
        'API.post("/api/settings/console-credentials",{clear:true})',
        'SettingsDirty.register("console_credentials",consoleCredentialsForm)',
    ):
        if marker not in settings_text:
            failures.append(f"target Console credential UI missing {marker}")
    for marker in (
        "si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL)",
        "si_console_credentials_get_status",
        "si_console_credentials_save",
        "si_console_credentials_clear",
        "clear_console_password_json(root)",
        "si_secret_store_clear(buf, CONSOLE_CREDENTIALS_MAX_BODY)",
        '"local_secret_not_returned_or_sent_to_agent_api"',
    ):
        if marker not in console_credentials_text:
            failures.append(f"Console credential HTTP boundary missing {marker}")
    for marker in (
        "si_secret_store_is_configured",
        "si_secret_store_set_string",
        "si_secret_store_get_string",
        "si_secret_store_clear",
        "si_hid_ascii_map",
    ):
        if marker not in console_service_text:
            failures.append(f"Console credential secret service missing {marker}")
    if re.search(r'cJSON_AddStringToObject\s*\([^,]+,\s*"password"\s*,',
                 console_credentials_text):
        failures.append("Console credential API can return a password value")
    if "si_console_credentials_get_password" in AGENT_REQUEST.read_text(encoding="utf-8"):
        failures.append("Agent request builder can read the target Console password")
    for marker in (
        'si_http_require_action(',
        'SI_CAPABILITY_HID, "console_login", SI_AUTHZ_RISK_HIGH',
        'session.principal != SI_PRINCIPAL_MCP',
        'si_control_lease_owner_session_matches(',
        '"mcp", context->session.session_id',
        'si_hid_execute_owned(',
        'si_hid_owner_token_equal(',
        'si_console_credentials_get_password(',
        'si_secret_store_clear(password, sizeof(password))',
        '"secret_redacted"',
        '"verification_required"',
    ):
        if marker not in console_login_mcp_text:
            failures.append(f"MCP local Console injection boundary missing {marker}")
    if re.search(r'cJSON_AddStringToObject\s*\([^,]+,\s*"(?:password|username|secret)"\s*,',
                 console_login_mcp_text):
        failures.append("MCP Console login endpoint can return credential material")
    for marker in (
        "ref=console://default",
        "secret_access=device-local-only",
        "username_configured=",
        "password_configured=",
    ):
        if marker not in agent_request_text:
            failures.append(f"Agent masked Console credential context missing {marker}")
    for marker in (
        "agent_tool_is_console_login",
        "agent_tool_is_runtime_call",
    ):
        if marker not in AGENT_TOOL_CORE.read_text(encoding="utf-8"):
            failures.append(f"Agent Console login classification missing {marker}")
    for marker in (
        "agent_execute_console_login_tool",
        'credential_ref = "console://default"',
        "si_console_credentials_get_password(password",
        "si_secret_store_clear(password, sizeof(password))",
        "secret_redacted",
        "agent_console_hid_type",
        "agent_console_wait_keyboard",
        'between = "enter"',
        '"verification_required"',
        '"observe_screenshot"',
    ):
        if marker not in AGENT_DEVICE_TOOLS.read_text(encoding="utf-8"):
            failures.append(f"Agent local Console injection boundary missing {marker}")
    if re.search(r'cJSON_AddStringToObject\s*\([^,]+,\s*"password"\s*,',
                 AGENT_DEVICE_TOOLS.read_text(encoding="utf-8")):
        failures.append("Agent Console login tool can return a password value")
    for marker in (
        'agent_tool_is_console_login(call) ? "console_login"',
        "agent_execute_console_login_tool(",
        'strcmp(name, "console_login") == 0',
        "*lease_required = true",
    ):
        if marker not in tool_dispatch_text:
            failures.append(f"Agent Console login dispatch/approval missing {marker}")
    for marker in (
        "loop_image_b64",
        "agent_capture_screenshot_b64(",
        "ctx->job_id, &loop_image_b64",
        "ctx->allow_screenshot && ctx->route->prefer_screenshot",
        "agent_capture_screenshot_b64(job_id, &image_b64",
        "screenshot_effective",
        "route.prefer_screenshot",
        "si_video_control_touch_agent(true)",
        "si_video_control_apply()",
    ):
        if marker not in agent_run_engine_text:
            failures.append(f"KVM login fresh-frame loop missing {marker}")
    run_http_text = AGENT_RUN_HTTP.read_text(encoding="utf-8")
    debug_status_start = run_http_text.find(
        "static void web_debug_agent_run_status("
    )
    debug_status_end = run_http_text.find(
        "static void web_debug_agent_run_events(", debug_status_start
    )
    if debug_status_start < 0 or debug_status_end <= debug_status_start:
        failures.append("Agent UART status diagnostic is missing")
    else:
        debug_status_body = run_http_text[debug_status_start:debug_status_end]
        for marker in (
            "cJSON_IsNumber(tool_executed_item) ?",
            "cJSON_IsNumber(hid_executed_item) ?",
            "cJSON_IsNumber(cloud_status_item) ?",
        ):
            if marker not in debug_status_body:
                failures.append(
                    f"Agent UART status missing safe numeric fallback {marker}"
                )
        if "cJSON_GetNumberValue(" in debug_status_body:
            failures.append(
                "Agent UART status may cast a missing JSON number from NaN"
            )
    authority_policy_block = re.search(
        r'if \(authority_mode && strcmp\(authority_mode, "policy"\) == 0\) \{(.*?)\n    \}',
        run_http_text,
        re.S,
    )
    if not authority_policy_block or "dry_run = false;" not in authority_policy_block.group(1):
        failures.append("Agent policy mode no longer selects execute mode")
    elif (
        "include_screenshot =" in authority_policy_block.group(1)
        or "allow_web_search =" in authority_policy_block.group(1)
    ):
        failures.append("Agent policy mode overrides manual KVM/Web permissions")
    for marker in (
        "AGENT_SCREENSHOT_FIRST_FRAME_TIMEOUT_MS",
        "AGENT_SCREENSHOT_POLL_MS",
        "si_video_control_keep_agent_alive()",
        "agent_run_control_point(job_id)",
        "vTaskDelay(pdMS_TO_TICKS(AGENT_SCREENSHOT_POLL_MS))",
        "video first frame timeout after",
    ):
        if marker not in agent_request_text:
            failures.append(f"Agent screenshot warm-up guard missing {marker}")
    if "video ? video->enabled && video->initialized : false" not in device_http_text:
        failures.append("Agent screenshot capability still depends on an already-running frame")
    agent_router_text = (
        PROJECT_DIR / "main/core/agent_router.c"
    ).read_text(encoding="utf-8")
    if "idle state, not proof that the capture card is physically disconnected" not in agent_router_text:
        failures.append("KVM prompt still conflates demand-driven capture idle with hardware disconnect")
    agent_control_text = (
        PROJECT_DIR / "main/services/web/agent_run_control_module.inc"
    ).read_text(encoding="utf-8")
    for module, name, lease_cleanup in (
        (AGENT_RUN_TASK.read_text(encoding="utf-8"), "task cleanup",
         "agent_run_release_hid_control(run->job_id)"),
        (agent_control_text, "abort cleanup",
         "agent_run_release_hid_control(exact_job)"),
    ):
        for marker in (
            lease_cleanup,
            "si_video_control_release_agent()",
            "si_video_control_apply()",
        ):
            if marker not in module:
                failures.append(
                    f"KVM Agent takeover {name} missing {marker}"
                )
    terminal_actions_start = terminal_text.find('<div class="session-actions">')
    terminal_actions_end = terminal_text.find("</div>", terminal_text.find('id="clearBtn"'))
    uart_baud_pos = terminal_text.find('id="uartNote"')
    connect_pos = terminal_text.find('id="connectBtn"')
    if not (
        terminal_actions_start >= 0
        and terminal_actions_start < uart_baud_pos < connect_pos < terminal_actions_end
    ):
        failures.append("UART baud selector is not immediately available to the left of Connect")
    if ".uart-mode-row{display:flex;align-items:center;gap:9px;margin:0;" not in terminal_text:
        failures.append("UART baud selector still carries feedback-row spacing inside session actions")
    for marker in (
        "term.attachCustomKeyEventHandler(event=>",
        "if(!event.repeat)startUartKeyRepeat(code,data)",
        "UART_KEY_REPEAT_DELAY_MS=400",
        "UART_KEY_REPEAT_INTERVAL_MS=45",
        'if(event.type==="keyup")',
        "stopUartKeyRepeat(code)",
        'window.addEventListener("blur",stopUartRepeatOnBlur)',
        "uartReplayBytesPending",
        "suppressTerminalResponses",
        "writeShellData(data,isReplay)",
        "if(suppressTerminalResponses)return;sendRaw(data)",
    ):
        if marker not in terminal_text:
            failures.append(f"UART immediate key/repeat lifecycle missing {marker}")
    ssh_module_text = SSH_MODULE.read_text(encoding="utf-8")
    ssh_client_text = SSH_CLIENT.read_text(encoding="utf-8")
    for marker in (
        'const char *password = json_string_any(root, "password", "pass", NULL);',
        'ssh_ws_delete_connect_json(root);',
        'ssh_secure_clear(buf, frame.len + 1);',
        'ssh_secure_clear(&config, sizeof(config));',
    ):
        if marker not in ssh_module_text:
            failures.append(f"SSH WebSocket one-time secret lifecycle missing {marker}")
    for marker in (
        'ssh_secure_clear(shell->config.password, sizeof(shell->config.password));',
        'ssh_secure_clear(&shell->config, sizeof(shell->config));',
    ):
        if marker not in ssh_client_text:
            failures.append(f"SSH shell credential cleanup missing {marker}")
    kvm_text = (WWW_DIR / "kvm.html").read_text(encoding="utf-8")
    for obsolete in ('id="conn"', 'id="hidStatus"', 'id="videoStatus"', 'class="toolbar"', 'conn.textContent=', 'hidStatus.textContent=', 'videoStatus.textContent='):
        if obsolete in kvm_text:
            failures.append(f"kvm still exposes redundant local status strip {obsolete}")
    for marker in ('id="agentTakeoverBanner" class="lease-banner hide"', '.stage{height:calc(100vh - 64px)', 'ws.onopen=()=>{if(this.ws!==ws||generation!==hidConnectGeneration||streamId!==videoStreamId||!hidMayConnect())', 'hidSocketStreamId=streamId;this.retry=1000;', 'scheduleHidReconnect(this.retry)'):
        if marker not in kvm_text:
            failures.append(f"kvm compact status removal broke layout or connection feedback {marker}")
    if 'nosignal.classList.add("hide")' not in kvm_text:
        failures.append("KVM Agent takeover hides the observable live video")
    for marker in (
        'if(!keyboardCapture.enabled)setKeyboardCaptureMsg("待机")',
        "if(!API.ws||API.ws.readyState>1)API.connect()",
    ):
        if marker not in kvm_text:
            failures.append(f"KVM human reclaim state recovery missing {marker}")
    if ".panel.hide{display:none}" not in kvm_text:
        failures.append("KVM empty contextual panels are not actually hidden")
    for marker in (
        "kvmLeaseStarting",
        "kvmLeaseInFlight",
        "kvmLeaseFailures",
        "scheduleLeaseRecovery(",
        'API.request("POST","/api/video/lease"',
        "await kvmDocumentReady;",
        "await claimVideo(",
        "scheduleVideoRecovery(",
        "videoStreamId=newVideoStreamId()",
        "videoFirstFrameTimer",
        "视频首帧超时，正在自动重连",
        "browserVideoReady()",
        'class="nosignal-logo" src="/assets/exoanchor-ui-mark.svg"',
        "kvmWindowBlurred",
        "function refreshKvmAfterWindowFocus()",
        "function finishKvmFocusRefresh()",
        "hidFocusBlocked=false;if(!kvmReadOnly",
        'localStorage.getItem("si_kvm_focus_refresh")!=="0"',
        'id="streamStats" class="stream-stats stale"',
        "v.stream_metrics||{}",
        "UI.status.lastDurationMs",
        "RTT -- ms",
        'id="bootKeyLoopKey"',
        'id="bootKeyLoopToggle" type="checkbox" role="switch"',
        'const BOOT_KEY_LOOP_INTERVAL_MS=250;',
        'function startBootKeyLoop()',
        'function stopBootKeyLoop(reason="已手动关闭")',
        'tapCode(bootKeyLoopKey.value,[],inputGeneration)',
        'bootKeyLoopToggle.onchange=()=>{if(bootKeyLoopToggle.checked)startBootKeyLoop();else stopBootKeyLoop("已手动关闭")}',
        '不会自动重启、识别 BIOS 或停止',
        "function restoreKeyboardCaptureAfterFocus()",
        'window.addEventListener("blur"',
        'window.addEventListener("focus",refreshKvmAfterWindowFocus)',
        "await touchKvm(rotate,false,true,previousStreamId)",
        "if(kvmLeaseInFlight&&!forceRequest&&kvmLeaseInFlight.epoch===requestEpoch",
        "backendHealthy&&browserHealthy",
        "采集后端正常，正在重连浏览器视频流",
        'label for="resolutionSelect">采集模式',
        'label for="outputModeSelect">输出模式',
        'option.textContent="H.264（实验，1080p25 / 720p30）"',
        'typeof VideoDecoder.isConfigSupported==="function"',
        'VideoDecoder.isConfigSupported(config)',
        'function switchH264ToMse(',
        'h264Mode=webCodecs?"webcodecs-probe":"mse"',
        'localStorage.getItem(outputModeStorageKey)==="h264"',
        'selectedOutputMode==="h264"',
        'function failH264(',
        '当前仍选择 H.264，请重连或手动切换为 MJPEG',
        'mime=`video/mp4; codecs="avc1.${suffix}"`',
        'const sample=avccAccessUnit(nalus,false)',
        'mse.media.removeSourceBuffer(mse.source)',
        'mse.media.endOfStream()',
        'ws.close(1000,"reconnect")',
        "h264LastFrameAt=performance.now()",
        "h264Socket.readyState===WebSocket.OPEN",
        'recoverH264(videoStreamId,"H.264 视频流停滞")',
        "function recoverH264(",
        "h264ReconnectAttempts",
        "自动重连仍未恢复",
        "kvmLeaseEpoch",
        "requestEpoch===kvmLeaseEpoch&&requestStreamId===videoStreamId",
        "kvmLeaseInFlight.streamId===requestStreamId",
    ):
        if marker not in kvm_text:
            failures.append(
                f"kvm video startup/lease lifecycle missing {marker}"
            )
    for obsolete in (
        'id="bootSequenceBios"',
        'id="bootSequenceMenu"',
        'id="bootSequenceConfirm"',
        "startBootSequence(",
        'API.get("/api/host/boot-key")',
        'trigger:"reset"',
        "max_attempts",
        "total_timeout_ms",
    ):
        if obsolete in kvm_text:
            failures.append(
                f"KVM still exposes the obsolete automatic BIOS sequence {obsolete}"
            )
    if "fallbackToMjpeg" in kvm_text or "已回退 MJPEG" in kvm_text:
        failures.append("KVM H.264 output still silently falls back to MJPEG")
    if 'selectedOutputMode==="h264"&&!backendVideoHealthy' in kvm_text:
        failures.append("KVM H.264 startup is still gated by transitional backend status")
    if 'h264Socket.onclose=()=>{if(generation===h264Generation&&videoClaimed&&!kvmLeaving&&!document.hidden)failH264' in kvm_text:
        failures.append("transient H.264 WebSocket close still becomes a permanent manual-retry failure")
    h264_status_start = kvm_text.find('if(selectedOutputMode==="h264"){', kvm_text.find("function renderKvmStatus"))
    h264_status_end = kvm_text.find("return;", h264_status_start)
    if h264_status_start >= 0 and "closeH264Transport()" in kvm_text[h264_status_start:h264_status_end]:
        failures.append("KVM H.264 status rendering still destroys transport on transient backend status")
    close_h264_start = kvm_text.find("function closeH264Transport()")
    close_h264_end = kvm_text.find("function closeVideoTransport()", close_h264_start)
    if close_h264_start < 0 or "videoStreamReady=false" not in kvm_text[close_h264_start:close_h264_end]:
        failures.append("closing H.264 transport preserves stale browser-ready state")
    if "MediaSource.isTypeSupported(avc3)" in kvm_text or 'state.sampleEntry==="avc3"' in kvm_text:
        failures.append("KVM Safari MSE fallback still prefers the rejected avc3 path")
    if 'fetch("/api/video/lease"' in kvm_text:
        failures.append("KVM lease still bypasses shared authentication recovery")
    if 'window.addEventListener("load",resolve,{once:true})' not in kvm_text:
        failures.append("KVM MJPEG stream can start before the document finishes loading")
    if "pollBootSequence" in kvm_text or "scheduleBootSequencePoll(" in kvm_text:
        failures.append("KVM still polls the removed automatic boot sequence")

    agent_tool_core_text = AGENT_TOOL_CORE.read_text(encoding="utf-8")
    agent_uart_tools_text = AGENT_UART_TOOLS.read_text(encoding="utf-8")
    agent_device_tools_text = AGENT_DEVICE_TOOLS.read_text(encoding="utf-8")
    agent_router_text = AGENT_ROUTER.read_text(encoding="utf-8")
    for marker in (
        "agent_tool_is_uart_status",
        "agent_tool_is_uart_read",
        "agent_tool_is_uart_write",
        "agent_tool_is_uart_baud",
        "agent_tool_is_runtime_call",
    ):
        if marker not in agent_tool_core_text:
            failures.append(f"Agent runtime tool classification missing {marker}")
    for marker in (
        "si_target_uart_journal_read_from",
        "terminal_control_begin(",
        "terminal_control_publish_input(",
        "AGENT_UART_MAX_WAIT_MS",
        "data_redacted",
    ):
        if marker not in agent_uart_tools_text:
            failures.append(f"Agent UART boundary missing {marker}")
    for marker in (
        "agent_execute_observe_status_tool",
        "agent_execute_observe_video_status_tool",
        "agent_execute_observe_hid_status_tool",
        "agent_execute_console_login_tool",
        "agent_execute_power_action_tool",
    ):
        if marker not in agent_device_tools_text:
            failures.append(f"Agent device runtime tool missing {marker}")
    for marker in (
        '"uart_ops"',
        '"uart_operator"',
        "Call uart_status before reporting availability",
    ):
        if marker not in agent_router_text:
            failures.append(f"Agent UART intent route missing {marker}")
    for marker in (
        "agent_execute_uart_status_tool",
        "agent_execute_uart_read_tool",
        "agent_execute_uart_write_tool",
        "agent_execute_uart_baud_tool",
    ):
        if marker not in tool_dispatch_text:
            failures.append(f"Agent UART dispatch missing {marker}")
    for marker in (
        "Before claiming UART is unavailable, call uart_status",
        "UART target: %s",
    ):
        if marker not in web_server_text + AGENT_REQUEST.read_text(encoding="utf-8"):
            failures.append(f"Agent UART prompt/context missing {marker}")
    broker_text = AGENT_REQUEST_BROKER.read_text(encoding="utf-8")
    for marker in (
        "resource_auto_approved(",
        '"uart_status"',
        '"uart_read"',
        '"check_service"',
        '"verify_port"',
        '"wait"',
    ):
        if marker not in broker_text:
            failures.append(f"low-risk observation is not auto-granted: {marker}")
    if "video.removeAttribute(\"src\");releaseKvmLease(false);" not in kvm_text:
        failures.append("a KVM tab displaced by another tab keeps renewing the video lease")
    video_http_text = (PROJECT_DIR / "main/services/web/video_http_module.inc").read_text(
        encoding="utf-8"
    )
    for marker in (
        "stream_id_present",
        "si_video_control_kvm_stream_is_current(stream_id)",
        "agent_stream_epoch",
        "video_stream_peer_closed(req)",
        "si_video_begin_mjpeg_send(1200)",
        "si_video_jpeg_view_t jpeg_view = {0};",
        "si_video_borrow_jpeg_if_new(last_frame_id, &jpeg_view)",
        "const size_t jpeg_len = jpeg_view.len",
        "const uint32_t frame_id = jpeg_view.frame_id",
        "(const char *)jpeg_view.data, jpeg_len",
        "si_video_release_jpeg(&jpeg_view)",
        "si_video_end_mjpeg_send()",
        "ret == ESP_ERR_NOT_FINISHED",
        "last_frame_id == 0",
        "video stream first frame timeout",
        "VIDEO_STREAM_STALL_TIMEOUT_MS",
        "last_frame_tick",
        "Observation is shared",
        "Only Agent input takeover is exclusive",
    ):
        if marker not in video_http_text:
            failures.append(f"KVM MJPEG stream handoff/backpressure guard missing {marker}")
    stream_start = video_http_text.find("static esp_err_t stream_handler(")
    stream_end = video_http_text.find("static int httpd_client_count(", stream_start)
    stream_text = video_http_text[stream_start:stream_end]
    if '"human KVM video is active"' in stream_text:
        failures.append(
            "read-only Agent video observation is still blocked by an active human KVM"
        )
    for forbidden in (
        "video_stream_grow_buffer(",
        "heap_caps_realloc(",
        "free(jpeg)",
    ):
        if forbidden in stream_text:
            failures.append(
                f"KVM MJPEG stream must not allocate a whole frame: {forbidden}"
            )
    h264_stream_text = (
        PROJECT_DIR / "main/services/video_h264_stream.c"
    ).read_text(encoding="utf-8")
    for obsolete in (
        "si_h264_stream_stage_mjpeg_frame",
        "si_h264_stream_release_mjpeg_frame",
        "si_h264_mjpeg_staged_frame_t",
        "s_preallocated_jpeg_input",
    ):
        if obsolete in h264_stream_text or obsolete in video_http_text:
            failures.append(f"obsolete H.264-owned MJPEG staging remains: {obsolete}")
    for marker in (
        "si_video_borrow_h264_jpeg_if_new(last_jpeg_frame, &jpeg_view)",
        "si_video_release_h264_jpeg(&jpeg_view)",
        "decode_job_release_source(",
        "return jpeg_decoder_process(",
        "never synthesize entropy bytes",
    ):
        if marker not in h264_stream_text:
            failures.append(f"H.264 immutable validated-input contract missing {marker}")
    video_input_text = (PROJECT_DIR / "main/drivers/video_input.c").read_text(
        encoding="utf-8"
    )
    for marker in (
        "s_transport_gate = xSemaphoreCreateBinary()",
        "si_video_begin_mjpeg_send(uint32_t timeout_ms)",
        "si_video_set_h264_transport_active(bool active)",
        "xSemaphoreTake(s_transport_gate,",
        "__atomic_store_n(&s_h264_transport_active, true",
        "__atomic_store_n(&s_h264_transport_active, false",
    ):
        if marker not in video_input_text:
            failures.append(f"MJPEG/H.264 transport gate missing {marker}")
    for marker in (
        "si_video_control_set_h264_transport(true)",
        "si_video_control_set_h264_transport(false)",
    ):
        if marker not in h264_stream_text:
            failures.append(f"H.264 output-mode gate lifecycle missing {marker}")
    for marker in (
        "agentDocumentReady",
        "agentStreamRequest",
        "await Promise.all([sendAgentVideoLease(true),agentDocumentReady])",
    ):
        if marker not in agent_text:
            failures.append(f"Agent MJPEG startup/load cancellation guard missing {marker}")
    video_input_text = (PROJECT_DIR / "main/drivers/video_input.c").read_text(
        encoding="utf-8"
    )
    for marker in (
        "__atomic_add_fetch(&s_uvc_callbacks",
        "uint32_t last_callbacks =",
        "if (callbacks != last_callbacks)",
        "SI_UVC_NO_FRAME_TIMEOUT_MS",
        "no_frame_timeout = true;",
    ):
        if marker not in video_input_text:
            failures.append(f"UVC supervisor liveness guard missing {marker}")
    if "last_stream_frames = s_status.frames_captured" in video_input_text:
        failures.append("UVC watchdog still treats status-lock contention as device loss")
    for marker in (
        "static bool stop_uvc_stream(void)",
        "static bool close_uvc_stream(bool stop_first)",
        "s_uvc_close_pending",
        "uvc_host_stream_open(",
        "uvc_host_stream_start(opened)",
        "if (!close_uvc_stream(false))",
        "if (transport_fault)",
        "closed = close_uvc_stream(true);",
        "s_uvc_close_pending = !closed;",
        "if (no_frame_timeout && stopped &&",
        "SI_UVC_HEALTHY_RESET_CALLBACKS",
        "SI_UVC_HEALTHY_RESET_MS",
        '"refusing USB power cycle with live stream handle"',
    ):
        if marker not in video_input_text:
            failures.append(f"UVC supervisor recovery contract missing {marker}")
    if "previous_stride != s_capture_stride_ms" in video_input_text:
        failures.append("KVM/preview stride changes still restart the USB UVC hardware stream")
    for marker in (
        ".logical_fps_x100 = best->public_mode.fps_x100,",
        ".format = best->format,",
        "s_status.target_fps_x100 = selected.logical_fps_x100;",
    ):
        if marker not in video_input_text:
            failures.append(f"native UVC frame-rate selection contract missing {marker}")
    for forbidden in (
        "SI_UVC_1080P30_TRANSPORT_FPS_X100",
        "transport_uplifted",
        "transport_pacing_should_skip_locked",
    ):
        if forbidden in video_input_text:
            failures.append(
                f"logical 25 fps is still uplifted to a higher USB mode: {forbidden}"
            )
    if "s_status.target_fps_x100 = fps_float_to_x100(actual_format.fps);" in video_input_text:
        failures.append("physical UVC transport fps still leaks into logical video status")
    video_control_text = (PROJECT_DIR / "main/application/video_control.c").read_text(
        encoding="utf-8"
    )
    if "request->fps_x100 = 3000U;" in video_control_text:
        failures.append("video control still rewrites logical 25 fps into a physical transport mode")
    render_sessions_start = agent_text.find("function renderSessions()")
    load_sessions_start = agent_text.find("async function loadAgentSessions()")
    if render_sessions_start < 0 or load_sessions_start < 0 or "renderTerminal(" in agent_text[render_sessions_start:load_sessions_start]:
        failures.append("agent session list refresh still redraws the terminal log")
    mock_text = UI_MOCK.read_text(encoding="utf-8")
    for marker in ("preview_enabled = True", "MockState.preview_enabled = active"):
        if marker not in mock_text:
            failures.append(f"local mock preview state is not mutable: {marker}")
    if 'if session_id and session_id != "default":' in mock_text:
        failures.append("local mock still prevents deleting the default Agent session")

    core_js = (WWW_DIR / "assets" / "ui-core.js").read_text(encoding="utf-8")
    core_css = (WWW_DIR / "assets" / "ui-core.css").read_text(encoding="utf-8")
    if '<div class="auth-brand"><img src="/assets/exoanchor-ui-mark.svg"' not in core_js:
        failures.append("shared login dialog does not use the ExoAnchor mark")
    for obsolete in (
        'id="authModal" class="ui-modal modal"',
        'id="authForm" class="ui-dialog dialog"',
    ):
        if obsolete in core_js:
            failures.append(
                f"shared login dialog still inherits page-specific styles: {obsolete}"
            )
    for marker in (
        ".ui-dialog label",
        ".ui-dialog input",
        ".ui-dialog input:focus",
        ".ui-dialog .msg",
        ".ui-dialog button.primary",
    ):
        if marker not in core_css:
            failures.append(f"shared login dialog styling missing {marker}")
    for marker in (
        'credentials: "same-origin"',
        'localStorage.getItem("ea_auth_token")',
        'localStorage.getItem("ea_auth_confirmed") === "1"',
        'sessionStorage.removeItem("ea_auth_token")',
        "for (const delay of [80, 180])",
        '"/api/agent/run/events?after_seq="',
    ):
        if marker not in core_js:
            failures.append(f"frontend navigation-auth recovery missing {marker}")
    if 'sessionStorage.setItem("ea_auth_token"' in core_js:
        failures.append("browser auth token still expires when the browser session closes")
    for marker in (
        "const lifecycle = {",
        "lifecycle.signal()",
        'window.addEventListener("pagehide"',
        'this.destroy("pagehide"',
        'this.emit("visible"',
        'this.emit("reconnect"',
        'this.controller.abort(reason)',
        "lifecycle.interval(() => this.active(), 10000)",
    ):
        if marker not in core_js:
            failures.append(f"shared page lifecycle contract missing {marker}")
    for marker in (
        "UI.lifecycle.mount(activePage)",
        "UI.lifecycle.navigate(destination)",
        "UI.lifecycle.logout()",
        'UI.lifecycle.destroy("logout")',
    ):
        if marker not in shell_js_text:
            failures.append(f"shared shell lifecycle handoff missing {marker}")
    if 'api.getSilent("/api/agent/run/status")' in core_js:
        failures.append("shared action mirror still serializes the full Agent run every 800ms")
    auth_header = (PROJECT_DIR / "main/application/auth_service.h").read_text(encoding="utf-8")
    settings_text = (WWW_DIR / "settings.html").read_text(encoding="utf-8")
    if "#define SI_AUTH_PASSWORD_MIN_LEN 6" not in auth_header:
        failures.append("firmware account password minimum is not six characters")
    if core_js.count('minlength="6"') < 2:
        failures.append("shared credential-change UI does not enforce the six-character minimum")
    if settings_text.count('minlength="6"') < 2:
        failures.append("Settings account form does not enforce the six-character minimum")
    project_text = (PROJECT_DIR / "CMakeLists.txt").read_text(encoding="utf-8")
    app_config = (PROJECT_DIR / "main/config/app_config.h").read_text(encoding="utf-8")
    version_match = re.search(r'set\(SI_VERSION_BASE "([^"]+)"\)', project_text)
    if not version_match:
        failures.append("firmware base version is missing")
    else:
        version_base = version_match.group(1)
        version = f"{version_base}-dev"
        if version_base != "0.87.6":
            failures.append(
                "0.87.6 IndigoShore integration checkpoint violated; keep SI_VERSION_BASE "
                "aligned with the released current development version"
            )
        if f'window.ExoAnchorBuild?.version || "{version}"' not in core_js:
            failures.append("shared UI core does not use the build version with the dev fallback")
        if f'#define SI_BMC_VERSION "{version}"' not in app_config:
            failures.append("firmware app version does not use the dev fallback")

    shell_css = SHELL_CSS_FILE.read_text(encoding="utf-8")
    shell_js = (WWW_DIR / "assets" / "ui-shell.js").read_text(encoding="utf-8")
    for marker in (
        'id="releaseMeta"',
        'const RELEASE_NAME = "IndigoShore";',
        'const CHIP_LABEL = "P4";',
        'const DISPLAY_VERSION = UI.VERSION.replace(/-dev$/, "dev");',
        "UI.VERSION",
    ):
        if marker not in shell_js:
            failures.append(f"shared navigation device/version label missing {marker}")
    if '<div class="title">ExoAnchor ESP32-P4</div>' in shell_js:
        failures.append("shared shell title still repeats the ESP32-P4 chip model")
    if 'id="deviceVersionText"' in shell_js:
        failures.append("shared shell still repeats the firmware version below the device title")
    for marker in (
        '<div class="title">ExoAnchor</div><div class="sub"><span id="deviceLabelText"',
        "function normalizeDeviceLabel(value)",
        "function renderDeviceLabel(info)",
        "normalizeDeviceLabel(info?.device_label || info?.label)",
        '/^(?:ESP32[- ]?P4|P4)$/i',
        "UI.info.subscribe(renderDeviceLabel)",
    ):
        if marker not in shell_js:
            failures.append(f"shared shell device-name subtitle missing {marker}")
    if shell_js.find('class="status-strip"') > shell_js.find('id="logoutNav"'):
        failures.append("Logout is not placed at the far-right end of the shared shell")
    if '<div class="nav">' + "' + nav + '" + '<span id="releaseMeta"' not in shell_js:
        failures.append("release codename/chip/version is not placed immediately after Settings")
    for marker in (
        "const hidMounted = !!(hid.connected || hid.mounted);",
        'if (hidMounted) setBadge(byId("badgeUsb"), "USB", "ONLINE", "ok");',
    ):
        if marker not in shell_js:
            failures.append(f"shared USB badge does not use stable mount state: {marker}")
    for marker in (
        "function videoModeState(video)",
        "const ms2109 = status.ms2109 || {};",
        "ms2109.initialized === true && ms2109.power_on === false;",
        'setBadge(byId("badgeVideo"), "VIDEO", "OFF", "neutral");',
        "if (video.frame_ready) {",
        'setBadge(byId("badgeVideo"), "VIDEO", videoModeState(video), "ok");',
        'return height + "P" + fpsLabel;',
    ):
        if marker not in shell_js:
            failures.append(f"shared video badge does not expose the negotiated video mode: {marker}")
    for marker in (
        'id="badgeVideo" class="data-state neutral" type="button" aria-haspopup="menu"',
        'id="videoPowerToggle" type="checkbox" role="switch"',
        'id="videoPowerState" class="ui-switch-state">不可用</span>',
        'action: desired ? "on" : "off"',
        'ms2109PowerAction(this.checked)',
        '.ea-shell .status-toggle-row',
        'id="badgePwr" class="data-state unknown" type="button" aria-haspopup="menu"',
        'toggleStatusMenu("powerMenu", "badgePwr")',
        '.ea-shell .status-menu > .data-state::after',
    ):
        if marker not in shell_js and marker not in shell_css:
            failures.append(f"shared status dropdown contract missing {marker}")
    if 'id="powerMenuButton"' in shell_js:
        failures.append("shared shell still renders a power-menu button separate from POWER status")
    if 'if (video.connected) setBadge(byId("badgeVideo"), "VIDEO", "ONLINE", "ok");' in shell_js:
        failures.append("shared video badge still renders redundant ONLINE text")
    if 'if (!(height > 0 && fps > 0)) return "ONLINE";' in shell_js:
        failures.append("shared video badge still falls back to redundant ONLINE text")
    if 'if (hid.hid_ready || hid.ready) setBadge(byId("badgeUsb"), "USB", "ONLINE", "ok");' in shell_js:
        failures.append("shared USB badge still treats transient endpoint readiness as link state")
    if ".ea-shell #logoutNav {" in shell_css:
        failures.append("Logout has a persistent danger style instead of hover-only feedback")
    if ".ea-shell #logoutNav:hover" not in shell_css:
        failures.append("Logout hover feedback is missing from the shared UI shell")
    logout_start = shell_css.find(".ea-shell > .shell-logout {")
    logout_end = shell_css.find("}", logout_start)
    logout_css = shell_css[logout_start:logout_end]
    for marker in (
        "min-height: 30px;",
        "padding: 6px 10px;",
        "border: 1px solid #3b4654;",
        "border-radius: 999px;",
        "background: #1b2028;",
        "font: 700 12px/1 ui-monospace",
    ):
        if marker not in logout_css:
            failures.append(
                f"Logout no longer matches the shared low-emphasis shell controls: {marker}"
            )
    for marker in (
        "grid-template-columns: minmax(0, 1fr) auto;",
        "grid-column: 2;",
        "align-self: center;",
    ):
        if marker not in shell_css:
            failures.append(
                f"responsive Logout is no longer grouped with the shell controls: {marker}"
            )
    data_state_start = shell_css.find(".ea-shell .data-state {")
    data_state_end = shell_css.find(".ea-shell .data-state::before", data_state_start)
    data_state_css = shell_css[data_state_start:data_state_end]
    for marker in (
        "min-height: 27px;",
        "padding: 5px 9px;",
        "border: 1px solid #3b4654;",
        "border-radius: 999px;",
        "background: rgba(27, 32, 40, .72);",
    ):
        if marker not in data_state_css:
            failures.append(
                f"shared VIDEO/USB/POWER status indicators lost readable framing: {marker}"
            )
    for state, marker in (
        ("ok", "background: rgba(16, 44, 41, .62);"),
        ("warn", "background: rgba(50, 38, 16, .62);"),
        ("bad", "background: rgba(53, 24, 28, .62);"),
        ("unknown", "background: rgba(51, 57, 66, .48);"),
    ):
        state_start = shell_css.find(f".ea-shell .data-state.{state} {{")
        state_end = shell_css.find("}", state_start)
        if state_start < 0 or marker not in shell_css[state_start:state_end]:
            failures.append(
                f"shared {state} status indicator lost state-specific contrast"
            )
    for marker in (
        'const DEFAULT_ASSISTANT_NAME = "Agent";',
        'function applyAssistantName(value)',
        'UI.api.get("/api/settings/device")',
        '"exoanchor:agent-name-changed"',
        'const BRAND_MARK_HTML = \'<img src="/assets/exoanchor-ui-mark.svg" alt="">\';',
        'id="eaAssistantLauncher"',
        'id="eaAssistantWindow"',
        "function assistantStartDrag(",
        "function assistantOpen(",
        'UI.api.durablePost("/api/agent/run"',
        "function assistantNewSessionId()",
        'launcher.classList.toggle("running"',
        'localStorage.getItem("ea_agent_session")',
        'title="关闭；后台任务会继续"',
        "UI.pageContext.capture(assistant.page)",
        'UI.api.getShared("/api/agent/requests"',
        'UI.api.post("/api/agent/run/steer"',
        'status.resource === "ask_user"',
        '{ response: answer }',
        '"exoanchor:agent-request-changed"',
        "function assistantEnsureStream(",
        "function assistantPullEvents(",
        '"/api/agent/run/events?job_id="',
        "function assistantStreamText(",
        "await assistantFinishStream(status)",
    ):
        if marker not in shell_js:
            failures.append(f"ExoAnchor Agent global assistant missing marker: {marker}")
    if 'durablePost(path, body)' not in core_js or 'timeoutMs: 8000' not in core_js:
        failures.append("lifecycle-independent durable POST lost its bounded timeout")
    if "<b>Indigo Shore</b>" in shell_js or ">IS</span><b>Shore</b>" in shell_js:
        failures.append("release codename is still being used as the assistant name")
    for marker in (
        ".ea-assistant-launcher",
        ".ea-assistant-window",
        ".ea-assistant-window.dragging",
        ".ea-assistant-context-row",
        ".ea-assistant-stream-answer",
        ".ea-assistant-message.agent",
    ):
        if marker not in shell_css:
            failures.append(f"ExoAnchor Agent global assistant style missing marker: {marker}")
    for marker in (
        'API.getShared("/api/agent/requests"',
        'API.post("/api/agent/run/steer"',
        'id="agentRequestResponse"',
        'id="threadAgentRequest"',
        'id="threadAgentRequestApprove"',
        'request.resource==="ask_user"',
        'items.find(item=>item?.status==="waiting_user")',
        '"exoanchor:agent-request-changed"',
        "threadAgentRequestApprove.onclick",
        '{response}:{})',
        'bootKeyRequestCard.hidden=state==="EMPTY"',
        'bootKeyDeveloperPanel.hidden=state!=="EMPTY"',
        '"Developer · 创建固件按键请求"',
        "agent-stream-turn",
        "function streamTextInto(",
        "await finishRunBubble(activeAgentPending",
        'API.durablePost("/api/agent/run"',
        "API.durablePost(endpoint,payload)",
        "function newSessionId()",
    ):
        if marker not in agent_text:
            failures.append(f"Agent Workspace shared request/steer UI missing {marker}")
    overview_text = (WWW_DIR / "index.html").read_text(encoding="utf-8")
    for marker in (
        'class="dashboard-grid"',
        'id="agentTask" class="agent-task" hidden',
        'dashboardGrid.classList.toggle("agent-idle",!visible)',
        'API.getShared("/api/agent/requests"',
        '["running","waiting_request","paused"]',
    ):
        if marker not in overview_text:
            failures.append(f"Overview conditional Agent summary missing {marker}")
    for marker in (
        "const SettingsDirty={",
        'SettingsDirty.register("device",deviceForm)',
        'SettingsDirty.register("agent_tools"',
        'window.addEventListener("beforeunload"',
        'SettingsDirty.captureAll();',
        'field.type==="checkbox"||field.type==="radio"',
        '!["password","file"].includes(field.type)',
    ):
        if marker not in settings_text:
            failures.append(f"Settings non-secret dirty-state model missing {marker}")
    page_context_markers = {
        "index.html": 'UI.pageContext.register("overview"',
        "kvm.html": 'UI.pageContext.register("kvm"',
        "agent.html": 'UI.pageContext.register("agent"',
        "terminal.html": 'UI.pageContext.register("terminal"',
        "settings.html": 'UI.pageContext.register("settings"',
    }
    for page_name, marker in page_context_markers.items():
        page_text = (WWW_DIR / page_name).read_text(encoding="utf-8")
        if marker not in page_text:
            failures.append(f"{page_name} does not provide bounded Agent page context")
    if "Explicit terminal selection" not in terminal_text or "term.hasSelection()" not in terminal_text:
        failures.append("terminal page context can capture output without explicit selection")
    for marker in (
        "providers: new Map()",
        "register(page, provider)",
        "capture(page)",
        "context_id:",
        "page_id:",
        "route:",
    ):
        if marker not in core_js:
            failures.append(f"stable page-context client contract missing marker: {marker}")
    agent_request_text = AGENT_REQUEST.read_text(encoding="utf-8")
    for marker in (
        'cJSON_AddStringToObject(normalized, "context_id"',
        'cJSON_AddStringToObject(normalized, "page_id", page)',
        'cJSON_AddStringToObject(normalized, "route", expected_route)',
        "agent_page_context_id_valid(",
    ):
        if marker not in agent_request_text:
            failures.append(f"stable page-context firmware contract missing marker: {marker}")
    broker_http_text = AGENT_BROKER_HTTP.read_text(encoding="utf-8")
    broker_text = AGENT_REQUEST_BROKER.read_text(encoding="utf-8")
    broker_header_text = AGENT_REQUEST_BROKER_H.read_text(encoding="utf-8")
    tool_core_text = AGENT_TOOL_CORE.read_text(encoding="utf-8")
    tool_dispatch_text = AGENT_TOOL_DISPATCH.read_text(encoding="utf-8")
    run_http_text = AGENT_RUN_HTTP.read_text(encoding="utf-8")
    action_event_header_text = ACTION_EVENT_H.read_text(encoding="utf-8")
    for marker in (
        '"/api/agent/requests"',
        '"/api/agent/requests/decision"',
        '"/api/agent/requests/cancel"',
        '"/api/agent/context-catalog"',
    ):
        if marker not in agent_routes_text:
            failures.append(f"Agent Request Broker route missing {marker}")
    for marker in (
        "agent_request_authorize_ex(",
        "AGENT_RUN_STATE_WAITING_REQUEST",
        '"精确请求已保存在设备端，关闭页面不会丢失"',
        "response, response_size",
    ):
        if marker not in broker_http_text:
            failures.append(f"Agent Request Broker runtime bridge missing {marker}")
    for marker in (
        "sha256_hex(input->binding.normalized_args_json, args_hash)",
        "copy_text(request->params_hash",
        "SI_AGENT_REQUEST_WAITING_USER",
        "SI_AGENT_REQUEST_GRANTED",
        "SI_AGENT_REQUEST_EXECUTING",
        "SI_AGENT_REQUEST_VERIFYING",
        "request->decided_by_session",
        "request->response",
        "si_agent_request_broker_consume(",
        "si_agent_request_broker_complete_verified(",
    ):
        if marker not in broker_text:
            failures.append(f"Agent Request Broker decision binding missing {marker}")
    if "#define SI_AGENT_REQUEST_RESPONSE_MAX_LEN 512" not in broker_header_text:
        failures.append("Agent Request Broker answer bound is missing")
    for marker in (
        'strcasecmp(name, "ask_user") == 0',
        'strcasecmp(name, "request_user_input") == 0',
    ):
        if marker not in tool_core_text:
            failures.append(f"Agent ask_user alias missing {marker}")
    for marker in (
        "agent_execute_ask_user_tool(",
        "SI_AGENT_REQUEST_KIND_CONTEXT",
        '"ask_user question is required and must be at most 160 bytes"',
        '"ask_user request completed without an answer"',
    ):
        if marker not in ask_user_tool_text:
            failures.append(f"Agent ask_user execution boundary missing {marker}")
    if "!agent_tool_is_ask_user(call)" not in tool_dispatch_text:
        failures.append("Agent ask_user dispatch guard missing")
    for marker in (
        "agent_compat_dispatch_allowed(",
        "agent_compat_degraded_readonly(",
        "agent_recovery_tool_allowed(",
        "risk != SI_AGENT_REQUEST_RISK_LOW || lease_required",
        'strcmp(name, "host_display") != 0',
        'strcmp(name, "boot_key_sequence") != 0',
        'strcmp(operation, "plan") == 0',
        "agent_run_task_service_is_bound(job_id)",
        "!task_service_bound &&",
        "agent_request_authorize(",
        '"mutation execution is disabled until the target-bound manager gateway and typed readback are available"',
        '"HID execution is disabled until target-bound readback is available"',
        '"ask_user is unavailable in the RAM-only recovery candidate"',
        '"tool is unavailable in the RAM-only recovery candidate"',
    ):
        if marker not in tool_dispatch_text:
            failures.append(
                f"Runtime V2 read-only cutover guard missing {marker}"
            )
    degraded_start = tool_dispatch_text.find(
        "static bool agent_compat_degraded_readonly("
    )
    healthy_start = tool_dispatch_text.find(
        "static bool agent_compat_dispatch_allowed(", degraded_start
    )
    degraded_allowlist_text = tool_dispatch_text[
        degraded_start:healthy_start
    ]
    recovery_allowlist_text = tool_dispatch_text[
        tool_dispatch_text.find("static bool agent_recovery_tool_allowed("):
        degraded_start
    ]
    if 'strcmp(operation, "plan") == 0' in degraded_allowlist_text:
        failures.append(
            "degraded Runtime compatibility path admits manager plan writes"
        )
    for forbidden in (
        'strcmp(name, "memory_search") == 0',
        'strcmp(name, "history_search") == 0',
        'strcmp(name, "memory_write") == 0',
        'strcmp(name, "ssh_exec") == 0',
        'strcmp(name, "check_service") == 0',
        'strcmp(name, "verify_port") == 0',
        'strcmp(name, "host_display") == 0',
        'strcmp(name, "boot_key_sequence") == 0',
    ):
        if forbidden in degraded_allowlist_text or forbidden in recovery_allowlist_text:
            failures.append(
                f"degraded Runtime compatibility path reaches storage/write {forbidden}"
            )
    recovery_gate_pos = tool_dispatch_text.find(
        "if (!agent_recovery_tool_allowed(policy_name, call))"
    )
    non_dry_gate_pos = tool_dispatch_text.find(
        "if (!dry_run && policy_name && !agent_tool_is_ask_user(call))"
    )
    first_effect_pos = tool_dispatch_text.find(
        "} else if (agent_tool_is_ssh_exec(call))"
    )
    if not (0 <= recovery_gate_pos < non_dry_gate_pos < first_effect_pos):
        failures.append(
            "RAM-only positive allowlist must guard dry-run and real effects"
        )
    for marker, source in (
        ("AGENT_RUNTIME_READONLY_TOOL_PROMPT", agent_request_text),
        ("agent_runtime_tool_prompt()", agent_request_text),
        ("AGENT_RUNTIME_MUTATION_ENABLED &&", agent_request_text),
        ("screenshot capture is unavailable in the RAM-only recovery candidate", agent_run_task_text),
        ('workspaceState.run?.admission_scope==="full_cap_browser_only"', agent_text),
    ):
        if marker not in source:
            failures.append(f"recovery prompt/lease boundary missing {marker}")
    for marker in (
        '"action_event_v1"',
        '"current_run_ram_ring_no_tf_scan"',
        'cJSON_AddBoolToObject(resp, "history_lost", history_lost);',
        'cJSON_AddBoolToObject(resp, "persisted", persisted);',
    ):
        if marker not in run_http_text and marker not in action_event_runtime_text:
            failures.append(f"persistent Agent event replay missing {marker}")
    for marker in (
        '"task_events"',
        '"task_event_schema_version"',
        "SI_AGENT_EVENT_SCHEMA",
        '"ram_cursor_only_no_tf_scan"',
        '"task_recovery_status"',
        'cJSON_AddBoolToObject(resp, "task_history_lost",',
    ):
        if marker not in run_http_text:
            failures.append(f"canonical Task event HTTP replay missing {marker}")
    events_start = run_http_text.find(
        "static esp_err_t agent_run_events_handler("
    )
    events_end = run_http_text.find(
        "static void web_debug_agent_run_start(", events_start
    )
    events_handler = run_http_text[events_start:events_end]
    for forbidden in (
        "agent_read_persisted_events(",
        "si_agent_task_service_replay(",
        "si_agent_event_store_replay(",
    ):
        if forbidden in events_handler:
            failures.append(
                f"Web event handler performs forbidden TF replay {forbidden}"
            )
    for marker in (
        ".expected_policy_revision = expected_policy_revision",
        ".current_policy_revision = current_policy_revision",
        ".expected_lease_epoch = lease_required ?",
        ".current_lease_epoch = lease_required ?",
        ".origin_authority_ceiling =",
        ".grant_consumed = status.grant_consumed",
        "si_agent_execution_guard_evaluate(&execution_intent)",
        "si_agent_task_service_record_action_started(",
    ):
        if marker not in broker_http_text:
            failures.append(f"Agent execution Guard bridge missing {marker}")
    guard_pos = broker_http_text.find(
        "si_agent_execution_guard_evaluate(&execution_intent)"
    )
    action_started_pos = broker_http_text.find(
        "si_agent_task_service_record_action_started(", guard_pos
    )
    if guard_pos < 0 or action_started_pos <= guard_pos:
        failures.append("Agent execution Guard must run before durable action.started")
    if "#define SI_ACTION_EVENT_CAPACITY 64U" not in action_event_header_text:
        failures.append("Agent Action Event RAM replay window is not 64 entries")
    base_settings_text = (PROJECT_DIR / "main" / "services" / "web" / "base_settings_http_module.inc").read_text(encoding="utf-8")
    for marker in (
        '"private, max-age=0, must-revalidate"',
        '"ETag"',
        '"If-None-Match"',
        '"304 Not Modified"',
    ):
        if marker not in base_settings_text:
            failures.append(f"embedded web asset revalidation missing {marker}")
    embedded_asset_start = base_settings_text.find(
        "static esp_err_t send_embedded_asset("
    )
    embedded_asset_end = base_settings_text.find(
        "static esp_err_t send_embedded_html(", embedded_asset_start
    )
    if embedded_asset_start < 0 or embedded_asset_end <= embedded_asset_start:
        failures.append("embedded asset handler is missing")
    elif '"Cache-Control", "no-store"' in base_settings_text[
        embedded_asset_start:embedded_asset_end
    ]:
        failures.append("embedded assets are still forced to redownload on every page")

    for marker in (
        "#define AUTH_BROWSER_SESSION_SLOTS 12U",
        "#define AUTH_MCP_SESSION_SLOTS 4U",
        "size_t slot_begin = is_mcp ? AUTH_BROWSER_SESSION_SLOTS : 0U;",
        "size_t slot_end = is_mcp ? AUTH_SESSION_SLOTS :",
    ):
        if marker not in AUTH_SERVICE.read_text(encoding="utf-8"):
            failures.append(f"browser/MCP session isolation missing {marker}")

    for marker in (
        "const hadWaitingCaller = !!this.pending;",
        "if (!hadWaitingCaller && this.onAuthenticated)",
        'submit.textContent = this.mode === "login" ? "正在登录…" : "正在保存…";',
        "async readAuthState()",
        "if (this.authProbe) return this.authProbe;",
        "if (document.hidden) return null;",
        "delay = Math.min(Math.round(delay * 1.7), 1000);",
        "let state = await this.readAuthState();",
    ):
        if marker not in core_js:
            failures.append(f"authentication startup deduplication missing {marker}")
    if (
        "async checkRequired()" in core_js
        and "catch (error) {\n        return await this.requireLogin();" in core_js
    ):
        failures.append("transient auth-status failure still forces a login prompt")

    access_mode_text = ACCESS_MODE_HTTP.read_text(encoding="utf-8")
    for marker in (
        '"manual", "手动授权"',
        '"assisted", "替我审批"',
        '"full", "完全访问"',
        "automation_request_stop_all(",
        "si_agent_access_mode_set(value)",
        "session.principal != SI_PRINCIPAL_BROWSER",
    ):
        if marker not in access_mode_text:
            failures.append(f"access-mode firmware boundary missing {marker}")
    for page_name in ("kvm.html", "terminal.html"):
        page_text = (WWW_DIR / page_name).read_text(encoding="utf-8")
        for marker in ('id="accessModeSelect"', "UI.accessMode.bind("):
            if marker not in page_text:
                failures.append(f"{page_name} access-mode UI missing {marker}")
    for marker in (
        '"/api/settings/access-mode"',
        'new BroadcastChannel("exoanchor-access-mode")',
        'title: "启用完全访问"',
    ):
        if marker not in core_js:
            failures.append(f"shared access-mode UI contract missing {marker}")

    mcp_start = base_settings_text.find(
        "static esp_err_t settings_mcp_handler(httpd_req_t *req)"
    )
    mcp_end = base_settings_text.find(
        "static bool agent_tools_mode_valid", mcp_start
    )
    if mcp_start < 0 or mcp_end <= mcp_start:
        failures.append("MCP settings handler is missing")
    else:
        mcp_body = base_settings_text[mcp_start:mcp_end]
        get_pos = mcp_body.find("if (req->method == HTTP_GET)")
        observe_pos = mcp_body.find("SI_CAPABILITY_OBSERVE")
        post_pos = mcp_body.find("if (req->method != HTTP_POST)")
        settings_pos = mcp_body.find("SI_CAPABILITY_SETTINGS")
        if not (0 <= get_pos < observe_pos < post_pos < settings_pos):
            failures.append(
                "MCP settings authorization must allow observe-only GET "
                "without granting MCP global settings capability"
            )

    if failures:
        for failure in failures:
            print(f"web contract: FAIL: {failure}", file=sys.stderr)
        return 1

    print(
        "web contract: PASS "
        f"({len(frontend_paths)} frontend APIs, "
        f"{len(firmware_paths)} firmware APIs, {len(mock_paths)} mock APIs)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
