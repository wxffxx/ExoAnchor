#!/usr/bin/env python3
"""Product-facing Settings information-architecture contract.

The 0.87.5 product Settings keeps the existing back-end endpoints while
presenting a capability-driven product interface.  The checks below prevent
internal modules from leaking back into the primary navigation and keep
Stable/Dev capability gating explicit.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from html.parser import HTMLParser
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
SETTINGS_PATH = ROOT / "main" / "www" / "settings.html"
EXPECTED_MENU = (
    ("#target-info", "被控端信息"),
    ("#target", "被控端访问"),
    ("#device", "设备与网络"),
    ("#security", "账户与安全"),
    ("#agent", "集成与权限"),
    ("#system", "系统与支持"),
    ("#advanced", "高级设置"),
)
EXPECTED_SECTIONS = {item[0][1:] for item in EXPECTED_MENU}
TARGET_SCOPE_IDS = {"target-info", "target"}
AGENT_ONLY_IDS = {
    "embeddedAgentEnabled",
    "pageContextEnabled",
    "conversationHistoryEnabled",
    "longTermMemoryEnabled",
    "browserAgentActionsEnabled",
    "agentIdentityForm",
    "agentApiForm",
    "agentPromptForm",
    "webSearchForm",
}
ADVANCED_IDS = {"storage", "diagnostics", "gpio"}
REMOVED_CARD_BADGE_IDS = {
    "productFeaturesBadge", "ms2109PowerBadge", "ms2109EepromBadge",
    "storageBadge", "storageFileBadge", "storageUploadBadge",
    "diagnosticBadge", "networkBadge", "diagnosticLogsBadge", "authBadge",
    "videoRuntimeBadge", "consoleCredentialsBadge", "sshTargetBadge",
    "sshKeyBadge", "mcpBadge", "agentApiBadge", "agentPromptBadge",
    "agentToolsBadge", "webSearchBadge", "skillsBadge", "otaBadge",
    "systemBadge",
}
VOID_TAGS = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
    "meta", "param", "source", "track", "wbr",
}


@dataclass
class Node:
    tag: str
    attrs: dict[str, str]
    parent: Node | None
    text: list[str] = field(default_factory=list)

    def classes(self) -> set[str]:
        return set(self.attrs.get("class", "").split())

    def ancestor(self, *, node_id: str | None = None) -> Node | None:
        node: Node | None = self
        while node is not None:
            if node_id is not None and node.attrs.get("id") == node_id:
                return node
            node = node.parent
        return None

    def owning_section(self) -> str | None:
        node: Node | None = self
        while node is not None:
            if node.attrs.get("data-settings-section"):
                return node.attrs["data-settings-section"]
            node = node.parent
        return None

    def has_ancestor_class(self, class_name: str) -> bool:
        node: Node | None = self
        while node is not None:
            if class_name in node.classes():
                return True
            node = node.parent
        return False


class SettingsParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.stack: list[Node] = []
        self.nodes: list[Node] = []
        self.ids: dict[str, Node] = {}

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        node = Node(tag, {key: value or "" for key, value in attrs},
                    self.stack[-1] if self.stack else None)
        self.nodes.append(node)
        if node.attrs.get("id"):
            self.ids[node.attrs["id"]] = node
        if tag not in VOID_TAGS:
            self.stack.append(node)

    def handle_endtag(self, tag: str) -> None:
        for index in range(len(self.stack) - 1, -1, -1):
            if self.stack[index].tag == tag:
                del self.stack[index:]
                return

    def handle_data(self, data: str) -> None:
        if self.stack and self.stack[-1].tag not in {"script", "style"}:
            self.stack[-1].text.append(data)


def text_content(node: Node) -> str:
    parts: list[str] = []
    for candidate in PARSER.nodes:
        parent = candidate
        while parent is not None and parent is not node:
            parent = parent.parent
        if parent is node:
            parts.extend(candidate.text)
    return " ".join(" ".join(parts).split())


def fail(message: str) -> None:
    FAILURES.append(message)


SOURCE = SETTINGS_PATH.read_text(encoding="utf-8")
PARSER = SettingsParser()
PARSER.feed(SOURCE)
FAILURES: list[str] = []

menu = next((node for node in PARSER.nodes
             if node.tag == "nav" and "settings-menu" in node.classes()), None)
if menu is None:
    fail("missing Settings primary navigation")
else:
    visible_links: list[tuple[str, str]] = []
    for node in PARSER.nodes:
        if node.tag != "a":
            continue
        parent = node.parent
        in_menu = False
        while parent is not None:
            if parent is menu:
                in_menu = True
                break
            parent = parent.parent
        if not in_menu:
            continue
        hidden = ("hidden" in node.attrs or
                  node.attrs.get("aria-hidden") == "true" or
                  node.attrs.get("tabindex") == "-1")
        if not hidden:
            visible_links.append((node.attrs.get("href", ""), text_content(node)))
    if len(visible_links) != len(EXPECTED_MENU):
        fail(
            "primary navigation exposes "
            f"{len(visible_links)} entries, expected {len(EXPECTED_MENU)}"
        )
    for (actual_href, actual_text), (expected_href, expected_label) in zip(
            visible_links, EXPECTED_MENU):
        if actual_href != expected_href or expected_label not in actual_text:
            fail(
                "primary navigation mismatch: "
                f"got {actual_href!r} {actual_text!r}, expected "
                f"{expected_href!r} containing {expected_label!r}"
            )

target_menu_ids = {
    node.attrs.get("href", "")[1:]
    for node in PARSER.nodes
    if node.tag == "a" and node.attrs.get("data-scope") == "target"
    and node.attrs.get("href", "").startswith("#")
}
if target_menu_ids != TARGET_SCOPE_IDS:
    fail(
        "target blue scope must cover exactly the target-info and target menu "
        f"entries, got {sorted(target_menu_ids)}"
    )

target_section_ids = {
    node.attrs.get("id", "")
    for node in PARSER.nodes
    if node.tag == "div" and node.attrs.get("data-scope") == "target"
}
if target_section_ids != TARGET_SCOPE_IDS:
    fail(
        "target blue scope must cover exactly the target-info and target "
        f"section titles, got {sorted(target_section_ids)}"
    )

declared_sections = {
    node.attrs["data-settings-section"]
    for node in PARSER.nodes if node.attrs.get("data-settings-section")
}
unexpected_sections = declared_sections - EXPECTED_SECTIONS
missing_sections = EXPECTED_SECTIONS - declared_sections
if missing_sections:
    fail(f"missing explicit data-settings-section owners: {sorted(missing_sections)}")
if unexpected_sections:
    fail(f"legacy modules still act as primary sections: {sorted(unexpected_sections)}")

visible_text = " ".join(text_content(node) for node in PARSER.nodes if node.tag == "h2")
if "产品功能" in visible_text:
    fail("generic 产品功能 card leaked back into the product Settings UI")
if "保存产品功能" in SOURCE:
    fail("generic 保存产品功能 action leaked back into the Settings UI")

for required_id in (
    "mcpEnabled", "browserFocusRefreshEnabled", "advancedSettings",
    "otaUploadForm", "resetEverything", "settingsLocatorReverseRow",
    "settingsLocatorReverse", "settingsLocatorMsg", "deviceHardwareCard",
    "powerResetSwapRow", "powerResetSwap", "powerResetSwapMeta",
    "deviceHardwareMsg", "gpioMatrixCard", "deviceDiagnostics",
    "deviceDiagnosticsSummary", "refreshDeviceDiagnostics",
    "networkSettings", "gpioMatrix", "ms2109PowerControl",
):
    if required_id not in PARSER.ids:
        fail(f"missing required Settings control #{required_id}")

mcp = PARSER.ids.get("mcpEnabled")
if mcp is not None:
    if mcp.owning_section() != "agent":
        fail("MCP access control is not owned by 集成与权限")
    if mcp.has_ancestor_class("agent-only"):
        fail("MCP access control is incorrectly hidden with the embedded Agent")

for node_id in AGENT_ONLY_IDS:
    node = PARSER.ids.get(node_id)
    if node is None:
        fail(f"missing Dev Agent setting #{node_id}")
        continue
    if node.owning_section() != "agent":
        fail(f"#{node_id} is outside 集成与权限")
    if not node.has_ancestor_class("agent-only"):
        fail(f"#{node_id} lacks the Stable/Dev agent-only gate")

focus = PARSER.ids.get("browserFocusRefreshEnabled")
if focus is not None and focus.owning_section() != "device":
    fail("KVM focus-return preference is not owned by 设备与网络")
ms2109 = PARSER.ids.get("ms2109PowerControl")
if ms2109 is None:
    fail("missing capability-gated MS2109 power control")
elif ms2109.owning_section() != "device":
    fail("MS2109 power control is not owned by 设备与网络")
elif ms2109.ancestor(node_id="advancedSettings") is not None:
    fail("MS2109 power control leaked back into 高级设置")

for node_id in ("kvm", "kvmDeviceSettings", "gpioMatrixCard",
                "deviceDiagnostics", "deviceDiagnosticsSummary"):
    node = PARSER.ids.get(node_id)
    if node is not None and node.owning_section() != "device":
        fail(f"audited device surface #{node_id} is outside 设备与网络")
for node_id in ("console", "remote"):
    node = PARSER.ids.get(node_id)
    if node is not None and node.owning_section() != "target":
        fail(f"target access surface #{node_id} is outside 被控端访问")
for node_id in ("diagnosticDevice", "diagnosticNetwork", "diagnosticVideo",
                "diagnosticUsb", "diagnosticGpio"):
    node = PARSER.ids.get(node_id)
    if node is not None and node.owning_section() != "device":
        fail(f"diagnostic summary #{node_id} is outside 设备与网络")
diagnostic_logs = PARSER.ids.get("diagnosticLogs")
if diagnostic_logs is not None and diagnostic_logs.owning_section() != "advanced":
    fail("detailed diagnostic logs moved outside 高级设置")

kvm_device = PARSER.ids.get("kvmDeviceSettings")
if kvm_device is not None:
    allowed_kvm_controls = {
        "previewFpsSelect", "videoAlwaysOnline", "saveVideoRuntime",
        "browserFocusRefreshEnabled", "ms2109PowerToggle",
    }
    actual_kvm_controls = {
        node.attrs["id"] for node in PARSER.nodes
        if node.tag in {"button", "input", "select", "textarea"}
        and node.attrs.get("id")
        and node.ancestor(node_id="kvmDeviceSettings") is not None
    }
    if actual_kvm_controls != allowed_kvm_controls:
        fail(
            "KVM/HID device settings moved controls without audit: "
            f"{sorted(actual_kvm_controls)}"
        )

advanced = PARSER.ids.get("advancedSettings")
if advanced is not None:
    if advanced.tag != "section":
        fail("advanced Settings must be a first-class section")
    if advanced.owning_section() != "advanced":
        fail("advanced Settings is not owned by the top-level 高级设置 route")
    if advanced.has_ancestor_class("developer-details"):
        fail("top-level advanced Settings is incorrectly hidden in Stable")
for node_id in ADVANCED_IDS:
    node = PARSER.ids.get(node_id)
    if node is None:
        fail(f"missing preserved advanced deep-link #{node_id}")
    else:
        if node.ancestor(node_id="advancedSettings") is None:
            fail(f"advanced surface #{node_id} is exposed outside #advancedSettings")
        if node.owning_section() != "advanced":
            fail(f"advanced surface #{node_id} is not owned by 高级设置")

ota_upload = PARSER.ids.get("otaUploadForm")
if ota_upload is not None:
    if ota_upload.owning_section() != "advanced":
        fail("local firmware upload is outside 高级设置")
    if not ota_upload.has_ancestor_class("developer-details"):
        fail("local firmware upload is not gated to Dev firmware")

factory_reset = PARSER.ids.get("resetEverything")
if factory_reset is not None:
    if factory_reset.owning_section() != "advanced":
        fail("factory reset is outside 高级设置")
    if factory_reset.has_ancestor_class("developer-details"):
        fail("factory reset is incorrectly hidden in Stable firmware")

if "settingsLocatorToggle" in PARSER.ids or "settingsLocatorMeta" in PARSER.ids:
    fail("Settings still duplicates the Overview Locator LED on/off control")
if 'action:on?"locator_on":"locator_off"' in SOURCE:
    fail("Settings still binds the duplicate Locator LED on/off action")
locator_reverse = PARSER.ids.get("settingsLocatorReverse")
if locator_reverse is not None and locator_reverse.owning_section() != "device":
    fail("Locator LED reverse configuration is outside 设备与网络")
locator_reverse_row = PARSER.ids.get("settingsLocatorReverseRow")
if locator_reverse_row is not None:
    if "hidden" not in locator_reverse_row.attrs:
        fail("Locator LED reverse row is visible before board capabilities load")
    if locator_reverse is not None and locator_reverse.ancestor(
            node_id="settingsLocatorReverseRow") is None:
        fail("Locator LED reverse control is outside its capability-gated row")
locator_message = PARSER.ids.get("settingsLocatorMsg")
if locator_message is not None and "hidden" not in locator_message.attrs:
    fail("Locator LED reverse feedback is visible before board capabilities load")

power_reset_swap = PARSER.ids.get("powerResetSwap")
if power_reset_swap is not None and power_reset_swap.owning_section() != "device":
    fail("PWR/RST GPIO swap is outside 设备与网络")
power_reset_row = PARSER.ids.get("powerResetSwapRow")
if power_reset_row is not None:
    if "hidden" not in power_reset_row.attrs:
        fail("PWR/RST GPIO swap is visible before runtime mapping loads")
    if power_reset_swap is not None and power_reset_swap.ancestor(
            node_id="powerResetSwapRow") is None:
        fail("PWR/RST GPIO swap is outside its capability-gated row")
device_hardware = PARSER.ids.get("deviceHardwareCard")
if device_hardware is not None:
    allowed_device_hardware_controls = {
        "powerResetSwap", "settingsLocatorReverse",
    }
    actual_device_hardware_controls = {
        node.attrs["id"] for node in PARSER.nodes
        if node.tag in {"button", "input", "select", "textarea"}
        and node.attrs.get("id")
        and node.ancestor(node_id="deviceHardwareCard") is not None
    }
    if actual_device_hardware_controls != allowed_device_hardware_controls:
        fail(
            "device hardware settings moved controls without audit: "
            f"{sorted(actual_device_hardware_controls)}"
        )

for node_id in REMOVED_CARD_BADGE_IDS:
    if node_id in PARSER.ids:
        fail(f"removed card-header status badge #{node_id} is still present")
    if re.search(rf"\b{re.escape(node_id)}\b", SOURCE):
        fail(f"removed card-header status badge #{node_id} is still referenced")

required_source_fragments = (
    'const SETTINGS_IDS=["target-info","target","device","security","agent","system","advanced"]',
    'class="editor developer-details" data-settings-section="advanced"',
    'href="#advanced"><b>高级设置</b><small>硬件、存储、诊断与恢复</small>',
    "--target-accent:var(--blue)",
    '.settings-menu a[data-scope="target"]:after{background:var(--target-accent)!important}',
    '.settings-menu a[data-scope="device"]:after{background:var(--green)!important}',
    '.settings-menu a:after{content:"";grid-column:2;grid-row:1;align-self:center;justify-self:end;width:9px;height:9px;border-radius:50%;background:var(--green)}',
    '.settings-page>.section-title:not(:first-child),#advancedSettings>.section-title:not(:first-child){margin-top:32px;padding-top:28px;border-top:1px solid var(--line)}',
    '.section-lead{margin:0 0 18px;line-height:1.5}',
    '.card-note{color:var(--muted);font-size:12px;line-height:1.5;margin:0 0 16px}.card>h2+.card-note{margin-top:6px}.card>.ui-setting-row+.ui-setting-row{margin-top:8px}',
    '.settings-content [data-settings-section="target-info"] button.primary,.settings-content [data-settings-section="target"] button.primary{border-color:#547db8;background:#17253a;color:var(--target-accent)}',
    '.data-state.ok{color:var(--green)',
    '.data-state.warn{color:var(--yellow)',
    '.data-state.bad{color:var(--red)',
    '.msg.ok{color:var(--green)}',
    '.msg.warn{color:var(--yellow)}',
    '.msg.bad{color:var(--red)}',
    'button.primary{border-color:#1f766d;background:#17302f;color:var(--green)}',
    'button.danger{border-color:#7f2d2d;color:#fecaca}',
    '.inline-check input{width:16px;height:16px;accent-color:var(--green)}',
    '.checkline input{width:16px;height:16px;accent-color:var(--green)}',
    '关闭会同时停止 UDP Discovery 与 _exoanchor._tcp mDNS',
    '<summary>开发者 · 本地固件上传</summary>',
    'document.documentElement.classList.toggle("stable-profile",!EMBEDDED_AGENT)',
    '.stable-profile .agent-only{display:none!important}',
    "function bindBrowserPreference(",
    'bindBrowserPreference(browserFocusRefreshEnabled,"si_kvm_focus_refresh"',
    'bindBrowserPreference(browserAgentActionsEnabled,"si_agent_action_visualization"',
    "function loadSettingsSection(",
    "function refreshActiveSection(",
    "settingsLocatorReverseRow.hidden=!reported",
    "settingsLocatorMsg.hidden=!reported||!configurable",
    "function renderDeviceHardware(power)",
    'API.post("/api/settings/gpio-map",{swap_power_reset:true})',
    "capabilities.ms2109_power??capabilities.ms2109_test",
    "if(ms2109PowerAvailable)await loadMs2109Status(true)",
    'id="networkSettings" class="section-title anchor-target" data-settings-section="device" data-settings-priority="5"',
    'id="gpioMatrix" class="section-title anchor-target" data-settings-section="device" data-settings-priority="90"',
    'id="ms2109PowerControl" class="ui-setting-row" data-ms2109-control hidden',
    "视频采集常在线",
    "返回 KVM 自动刷新",
    "MS 视频电源",
)
for fragment in required_source_fragments:
    if fragment not in SOURCE:
        fail(f"missing Settings review contract marker: {fragment}")

if SOURCE.count("var(--target-accent)") != 2:
    fail("target accent must remain limited to the navigation dot and scoped save buttons")
for forbidden_selector in (
    ".settings-subsection-title:before{",
    ".section-title>span:before{",
    '.settings-menu a[data-scope="target"] b{',
    '.settings-menu a[data-scope="target"]:hover',
    '.section-title[data-scope="target"]>span:before{',
    '.settings-subsection-title[data-settings-section="target"]:before{',
):
    if forbidden_selector in SOURCE:
        fail(f"unscoped target accent selector is forbidden: {forbidden_selector}")

for removed_ms_debug in (
    'id="ms2109PowerRows"',
    'id="ms2109PowerCycle"',
    'id="ms2109Refresh"',
    "3.3V Switch",
    "1.2V Enable",
    "电源反馈",
    "操作次数",
    "Last result",
):
    if removed_ms_debug in SOURCE:
        fail(f"MS2109 debug-only detail leaked into Settings: {removed_ms_debug}")

if "margin:-5px" in SOURCE:
    fail("Settings card copy must not use negative spacing")

if "for(const el of [...content.children])" in SOURCE:
    fail("Settings routing still infers ownership from sibling DOM order")
if re.search(r"UI\.lifecycle\.interval\([^;]{0,1200}\brefresh\(\)", SOURCE):
    fail("Settings still performs an unconditional whole-page refresh loop")

for removed_ms2109_eeprom_marker in (
    "ms2109Eeprom",
    "/api/ms2109/eeprom/",
    "PROGRAM MS2109 EEPROM",
    "AT24C16",
):
    if removed_ms2109_eeprom_marker in SOURCE:
        fail(
            "0.87.5 Settings exposes removed MS2109 EEPROM control: "
            f"{removed_ms2109_eeprom_marker!r}"
        )

if FAILURES:
    print("Settings review contract: FAIL", file=sys.stderr)
    for failure in FAILURES:
        print(f"- {failure}", file=sys.stderr)
    raise SystemExit(1)

print("Settings review contract: PASS")
