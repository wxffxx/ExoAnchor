#!/usr/bin/env python3
"""Static host contract for build-scoped product settings and discovery."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FEATURES = (
    ROOT / "main/services/web/product_features_http_module.inc"
).read_text(encoding="utf-8")
DEVICE_SETTINGS = (ROOT / "main/application/device_settings.c").read_text(
    encoding="utf-8"
)
DISCOVERY = (ROOT / "main/services/network_discovery.c").read_text(
    encoding="utf-8"
)
APP_MAIN = (ROOT / "main/app/app_main.c").read_text(encoding="utf-8")
AGENT_TASK = (
    ROOT / "main/services/web/agent_run_task_module.inc"
).read_text(encoding="utf-8")
AGENT_HTTP = (
    ROOT / "main/services/web/agent_run_http_module.inc"
).read_text(encoding="utf-8")


def require(text: str, fragment: str, label: str) -> None:
    if fragment not in text:
        raise AssertionError(f"{label}: missing {fragment!r}")


def forbid(text: str, fragment: str, label: str) -> None:
    if fragment in text:
        raise AssertionError(f"{label}: unexpected {fragment!r}")


# LAN discovery is mutable in every build. Agent-only fields are mutable only
# when the embedded runtime is compiled, so Stable POSTs cannot overwrite
# invisible NVS preferences with the false values of hidden form controls.
require(
    FEATURES,
    'READ_FEATURE("lan_discovery_enabled", lan_discovery_enabled);\n'
    "#if SI_CFG_EMBEDDED_AGENT_ENABLED",
    "build-independent LAN discovery update",
)
for field in (
    "embedded_agent_enabled",
    "page_context_enabled",
    "conversation_history_enabled",
    "long_term_memory_enabled",
):
    require(
        FEATURES,
        f'READ_FEATURE("{field}", {field});',
        f"conditional {field} update",
    )
forbid(
    FEATURES,
    "settings.embedded_agent_enabled = false;",
    "Stable hidden preference preservation",
)
forbid(
    FEATURES,
    "settings.page_context_enabled = false;",
    "Stable hidden page-context preservation",
)
require(
    FEATURES,
    'cJSON_AddStringToObject(root, "page_context_scope", "device");',
    "page context device scope",
)

# Disabling the embedded runtime is a central admission gate. HTTP currently
# checks early for a clear 403, but diagnostics and future adapters must not be
# able to bypass the setting by calling the shared submit owner directly.
submit_body = AGENT_TASK.split(
    "static esp_err_t agent_run_submit(", 1
)[1].split("static void agent_run_status_json(", 1)[0]
for marker in (
    "si_product_feature_settings_get(&product_features);",
    "if (!product_features.embedded_agent_enabled)",
    'strlcpy(err, "embedded Agent disabled in Settings", err_size);',
    "return ESP_ERR_NOT_ALLOWED;",
):
    require(submit_body, marker, "central embedded Agent admission gate")
if submit_body.index("si_product_feature_settings_get(&product_features);") > \
        submit_body.index("agent_run_start();"):
    raise AssertionError("disabled Agent must be rejected before runtime start")
require(
    AGENT_HTTP,
    "if (submit_ret == ESP_ERR_NOT_ALLOWED)",
    "runtime-disable HTTP race mapping",
)
require(
    AGENT_HTTP,
    'req, "403 Forbidden",',
    "runtime-disable HTTP 403 mapping",
)

# Turning LAN discovery off must remove both externally visible mechanisms:
# close the UDP listener and withdraw the _exoanchor mDNS service.  The mDNS
# responder itself remains initialized so hostname.local resolution, which is
# separate from service discovery, is not disrupted.
disabled_branch = DISCOVERY.split(
    "if (!si_lan_discovery_is_enabled_cached()) {", 1
)[1].split("continue;", 1)[0]
require(disabled_branch, "close(socket_fd);", "disabled UDP listener")
require(
    disabled_branch,
    "unpublish_mdns_service();",
    "disabled mDNS service publication",
)
require(
    DISCOVERY,
    'mdns_service_remove("_exoanchor", "_tcp")',
    "mDNS service withdrawal",
)
require(DISCOVERY, "open_discovery_socket();", "dynamic UDP enable")
require(DISCOVERY, "publish_mdns_service();", "dynamic mDNS enable")

start_body = DISCOVERY.split(
    "esp_err_t si_network_discovery_start(void)", 1
)[1]
discovery_task_body = DISCOVERY.split(
    "static void discovery_task(void *arg)", 1
)[1].split("esp_err_t si_network_discovery_start(void)", 1)[0]
forbid(
    discovery_task_body,
    "si_product_feature_settings_get(",
    "PSRAM discovery task NVS-backed settings access",
)
forbid(
    discovery_task_body,
    "si_product_feature_settings_preload(",
    "PSRAM discovery task settings preload",
)
for flash_api in (
    "si_settings_store_",
    "nvs_",
    "esp_flash_",
    "esp_partition_",
    "esp_ota_",
):
    forbid(
        discovery_task_body,
        flash_api,
        f"PSRAM discovery task cache-disabling call {flash_api}",
    )
require(
    start_body,
    "si_product_feature_settings_preload()",
    "internal-stack feature preload",
)
if start_body.index("si_product_feature_settings_preload()") > start_body.index(
    "xTaskCreateWithCaps("
):
    raise AssertionError("feature preload must precede PSRAM task creation")
require(
    DEVICE_SETTINGS,
    "static atomic_bool s_lan_discovery_enabled_cached",
    "atomic LAN discovery cache",
)
require(
    DEVICE_SETTINGS,
    "atomic_store_explicit(&s_lan_discovery_enabled_cached",
    "atomic LAN discovery cache update",
)
setter_success_body = DEVICE_SETTINGS.rsplit(
    "if (ret == ESP_OK) {", 1
)[1].split("return ret;", 1)[0]
require(
    setter_success_body,
    "atomic_store_explicit(&s_lan_discovery_enabled_cached",
    "commit-success LAN discovery cache update",
)
require(
    DEVICE_SETTINGS,
    "atomic_load_explicit(&s_lan_discovery_enabled_cached",
    "atomic LAN discovery cache read",
)

# app_main must establish the feature snapshot unconditionally, before any
# diagnostics or Agent-capable adapter can start. A real read/open failure
# keeps the zero-initialized published snapshot disabled and does not latch the
# product defaults as loaded; a missing namespace remains a valid first boot.
app_body = APP_MAIN.split("void app_main(void)", 1)[1]
require(
    app_body,
    "si_product_feature_settings_preload()",
    "unconditional app_main feature preload",
)
if app_body.index("si_product_feature_settings_preload()") > app_body.index(
    "si_diag_cli_start()"
):
    raise AssertionError("feature preload must precede diagnostics startup")
for marker in (
    "static si_product_feature_settings_t s_product_features;",
    "static esp_err_t load_product_features(void)",
    "if (ret == ESP_ERR_NOT_FOUND)",
    "if (ret != ESP_OK) {\n        return ret;\n    }",
    "s_product_features = candidate;",
    "s_product_features_loaded = true;",
    "esp_err_t si_product_feature_settings_preload(void)",
):
    require(DEVICE_SETTINGS, marker, "fail-closed feature preload")
load_body = DEVICE_SETTINGS.split(
    "static esp_err_t load_product_features(void)", 1
)[1].split("esp_err_t si_product_feature_settings_preload(void)", 1)[0]
if load_body.index("if (ret != ESP_OK)") > load_body.index(
    "s_product_features_loaded = true;"
):
    raise AssertionError("failed feature preload can latch defaults as loaded")
forbid(
    start_body,
    'mdns_service_add(NULL, "_exoanchor", "_tcp"',
    "unconditional startup publication",
)

print("product feature contracts: OK")
