#!/usr/bin/env python3
"""Validate the Stable KVM route, UI, and source boundaries."""

from pathlib import Path
import re
import sys


PROJECT = Path(__file__).resolve().parents[2]
MAIN = PROJECT / "main"
WWW = MAIN / "www"
WEB_SERVER = MAIN / "services" / "web_server.c"
EXPECTED = PROJECT / "tests" / "host" / "expected_http_routes.txt"
PARTITIONS = PROJECT / "partitions.csv"
README = PROJECT / "README.md"
KCONFIG = MAIN / "Kconfig.projbuild"
DEVICE_SETTINGS_HEADER = MAIN / "application" / "device_settings.h"
APP_CONFIG_HEADER = MAIN / "config" / "app_config.h"
SDKCONFIG_DEFAULTS = PROJECT / "sdkconfig.defaults"

API_PATTERN = re.compile(r"/api/[A-Za-z0-9_./?-]+")
LOCAL_RESOURCE_PATTERN = re.compile(r'(?:href|src)="(/[^"#?]*)')
LOCAL_WEB_INCLUDE_PATTERN = re.compile(r'#include "(web/[^"]+)"')
MARKDOWN_LINK_PATTERN = re.compile(r'\[[^\]]*\]\(([^)]+)\)')
ROUTE_PATTERN = re.compile(
    r'register_uri\([^,]+,\s*"([^"]+)",\s*(HTTP_[A-Z]+)'
)


def api_paths(text: str) -> set[str]:
    return {match.split("?", 1)[0] for match in API_PATTERN.findall(text)}


def expected_routes() -> set[tuple[str, str]]:
    routes = set()
    for line in EXPECTED.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            method, uri = line.split(maxsplit=1)
            routes.add((uri, method))
    return routes


def main() -> int:
    failures: list[str] = []
    web_text = WEB_SERVER.read_text(encoding="utf-8")
    actual = {(uri, method) for uri, method in ROUTE_PATTERN.findall(web_text)}
    expected = expected_routes()
    if actual != expected:
        failures.append(
            f"route mismatch: missing={sorted(expected-actual)}, extra={sorted(actual-expected)}"
        )

    firmware_api = {uri for uri, _ in actual if uri.startswith("/api/")}
    get_routes = {uri for uri, method in actual if method == "HTTP_GET"}
    frontend_api: set[str] = set()
    for page in sorted(WWW.glob("*.html")):
        text = page.read_text(encoding="utf-8")
        frontend_api.update(api_paths(text))
        for resource in LOCAL_RESOURCE_PATTERN.findall(text):
            if resource not in get_routes:
                failures.append(f"{page.name} references missing GET route: {resource}")
        for asset in ("/assets/ui-core.css", "/assets/ui-core.js",
                      "/assets/ui-shell.css", "/assets/ui-shell.js"):
            if asset not in text:
                failures.append(f"{page.name} does not load {asset}")
        if 'id="appShell"' not in text or "ExoAnchorShell.mount(" not in text:
            failures.append(f"{page.name} does not mount the shared shell")
    for script in sorted((WWW / "assets").glob("*.js")):
        frontend_api.update(api_paths(script.read_text(encoding="utf-8")))
    missing_api = sorted(frontend_api - firmware_api)
    if missing_api:
        failures.append("frontend API routes missing from firmware: " + ", ".join(missing_api))

    kvm_text = (WWW / "kvm.html").read_text(encoding="utf-8")
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

    settings_text = (WWW / "settings.html").read_text(encoding="utf-8")
    for marker in (
        "color-scheme:dark",
        "--bg:#0b0d10",
        "--panel:#15191f",
        "--text:#f4f7fa",
        ".field input,.field select",
        "button.primary",
        'class="ui-switch"',
    ):
        if marker not in settings_text:
            failures.append(f"Stable Settings visual token missing: {marker}")

    for marker in (
        "function renderGpioRows()",
        "function collectGpioUpdates()",
        'data-gpio-index="${index}"',
        'data-level-index="${index}"',
        "GPIO ${gpio} 已被",
        "GPIO 映射已保存并立即生效",
        "AuthUI.bindForm(async()=>{await Session.load();await loadAll()})",
    ):
        if marker not in settings_text:
            failures.append(f"editable GPIO Settings contract missing: {marker}")
    for marker in ("UI.escapeHtml", "item.configurable===false", "gpioMap.filter("):
        if marker in settings_text:
            failures.append(f"GPIO Settings still contains read-only/broken path: {marker}")
    github_placeholder = re.search(
        r'<span id="gpioGithubGuidePlaceholder"[^>]*>', settings_text
    )
    if not github_placeholder:
        failures.append("future GitHub GPIO guide placeholder is missing")
    else:
        placeholder_tag = github_placeholder.group(0)
        for marker in ('role="link"', 'aria-disabled="true"', 'data-future-url=""'):
            if marker not in placeholder_tag:
                failures.append(f"GitHub GPIO guide placeholder missing: {marker}")
        if "href=" in placeholder_tag or "github.com" in placeholder_tag:
            failures.append("unpublished GitHub GPIO guide must not have a live URL")
    for marker in ("/api/ota", "OTA", "otaManifest", "otaChannel", "loadOta"):
        if marker in settings_text:
            failures.append(f"removed firmware-update UI remains: {marker}")

    shell_text = (WWW / "assets" / "ui-shell.js").read_text(encoding="utf-8")
    if '<div class="title">ESP32P4</div>' not in shell_text:
        failures.append("shared shell title is not ESP32P4")
    if "ExoAnchor Stable KVM" in shell_text:
        failures.append("legacy Stable KVM shell title remains")

    for page_name, title in (
        ("index.html", "ESP32P4"),
        ("kvm.html", "ESP32P4 KVM"),
        ("settings.html", "ESP32P4 Settings"),
    ):
        page_text = (WWW / page_name).read_text(encoding="utf-8")
        if f"<title>{title}</title>" not in page_text:
            failures.append(f"{page_name} title is not {title}")

    for removed in (WWW / "agent.html", WWW / "terminal.html", WWW / "vendor" / "xterm"):
        if removed.exists():
            failures.append(f"removed UI still exists: {removed.relative_to(PROJECT)}")

    ui_boundary = "\n".join(
        path.read_text(encoding="utf-8")
        for path in (WWW / "assets" / "ui-shell.js", WWW / "settings.html")
    ).lower()
    for marker in ("/agent", "/terminal", "/api/ssh", "/api/storage",
                   "/api/settings/mcp", "xterm"):
        if marker in ui_boundary:
            failures.append(f"removed UI surface remains: {marker}")

    cmake_text = (MAIN / "CMakeLists.txt").read_text(encoding="utf-8")
    for marker in ("agent_", "ssh_client", "storage_manager", "libssh",
                   "www/agent.html", "www/terminal.html", "xterm",
                   "app_update", "esp_http_client", "esp-tls"):
        if marker.lower() in cmake_text.lower():
            failures.append(f"removed compile dependency remains: {marker}")

    capabilities = (MAIN / "services" / "device_http.c").read_text(encoding="utf-8")
    for marker in ('"edition", "stable-kvm"', '"embedded_agent", false',
                   '"ssh_client", false', '"tf_file_manager", false',
                   '"external_control_api", true', '"ota", false'):
        if marker not in capabilities:
            failures.append(f"Stable KVM capability marker missing: {marker}")

    for removed in (
        MAIN / "services" / "web" / "ota_settings_module.inc",
        MAIN / "services" / "web" / "ota_runtime_module.inc",
        MAIN / "core" / "version_utils.c",
        MAIN / "core" / "version_utils.h",
    ):
        if removed.exists():
            failures.append(f"removed firmware-update module still exists: {removed.name}")

    source_boundary = "\n".join(
        path.read_text(encoding="utf-8")
        for path in MAIN.rglob("*")
        if path.is_file() and path.suffix in {".c", ".h", ".inc", ".html", ".js"}
    )
    for pattern in (r"/api/ota(?:/|\b)", r"\besp_ota_", r"\bOTA_[A-Z0-9_]", r"\bs_ota_"):
        if re.search(pattern, source_boundary):
            failures.append(f"removed firmware-update source reference remains: {pattern}")
    if "next_update_partition" in source_boundary:
        failures.append("removed update-partition observation remains")

    partition_text = PARTITIONS.read_text(encoding="utf-8")
    partition_labels = [
        line.split(",", 1)[0].strip()
        for line in partition_text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if partition_labels != ["nvs", "phy_init", "factory"]:
        failures.append(f"factory-only partition contract changed: {partition_labels}")

    for doc in (README, KCONFIG):
        if re.search(r"\bOTA\b", doc.read_text(encoding="utf-8"), re.IGNORECASE):
            failures.append(f"removed firmware-update documentation remains: {doc.name}")

    if "default SI_BOARD_EXOANCHOR_PROTOTYPE0" not in KCONFIG.read_text(encoding="utf-8"):
        failures.append("Stable KVM default board must remain ExoAnchor PrototypeV0")

    kconfig_text = KCONFIG.read_text(encoding="utf-8")
    if re.search(
        r"(?ms)^\s*config SI_AUTH_PASSWORD\b.*?^\s*default \"\"\s*$",
        kconfig_text,
    ) is None:
        failures.append("Stable KVM Kconfig must not ship a bootstrap password")
    if 'CONFIG_SI_AUTH_PASSWORD=""' not in SDKCONFIG_DEFAULTS.read_text(encoding="utf-8"):
        failures.append("Stable KVM sdkconfig defaults must not ship a password")
    app_config_text = APP_CONFIG_HEADER.read_text(encoding="utf-8")
    for marker in (
        "_Static_assert(sizeof(SI_CFG_AUTH_PASSWORD) >= 7",
        "Set a unique CONFIG_SI_AUTH_PASSWORD",
    ):
        if marker not in app_config_text:
            failures.append(f"Stable KVM build credential guard missing: {marker}")

    if "#define SI_SESSION_LOGOUT_DEFAULT_ENABLED true" not in DEVICE_SETTINGS_HEADER.read_text(encoding="utf-8"):
        failures.append("Stable KVM automatic logout must default to enabled")

    for include in LOCAL_WEB_INCLUDE_PATTERN.findall(web_text):
        include_path = MAIN / "services" / include
        if not include_path.is_file():
            failures.append(f"web server includes missing module: {include}")

    for markdown in PROJECT.rglob("*.md"):
        relative_markdown = markdown.relative_to(PROJECT)
        top_level = relative_markdown.parts[0]
        # IDF materializes third-party components and build trees locally. Their
        # upstream docs may link to files that are intentionally not packaged;
        # keep this audit scoped to documentation owned by this firmware tree.
        if top_level == "managed_components" or top_level == "build" or top_level.startswith("build-"):
            continue
        for target in MARKDOWN_LINK_PATTERN.findall(markdown.read_text(encoding="utf-8")):
            target = target.strip().strip("<>").split("#", 1)[0]
            if not target or "://" in target or target.startswith(("mailto:", "/")):
                continue
            if not (markdown.parent / target).resolve().exists():
                failures.append(
                    f"broken Markdown link: {relative_markdown} -> {target}"
                )

    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1
    print(f"stable KVM web contract: PASS ({len(actual)} routes, {len(frontend_api)} UI APIs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
