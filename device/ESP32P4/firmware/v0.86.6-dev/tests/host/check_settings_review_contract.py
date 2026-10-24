#!/usr/bin/env python3
"""Product-facing Settings information-architecture contract.

Settings is organised as page -> subsection -> card.  Every device-persisted
setting card is a registered dirty-tracking group that is written only by the
page save bar; actions that take effect immediately stay outside those groups
and are labelled as such.  Read-only diagnostics live on their own Status page.
The checks below keep that structure, the Stable/Dev gating and the audited
control ownership from drifting.
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
    ("#device", "设备与视频"),
    ("#security", "账户与安全"),
    ("#agent", "AI 与集成"),
    ("#system", "系统维护"),
    ("#status", "状态与诊断"),
    ("#advanced", "高级"),
)
EXPECTED_SECTIONS = [item[0][1:] for item in EXPECTED_MENU]
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
    "agentSkillsJson",
}
OWNERSHIP = {
    "target-info": ("targetProfileForm",),
    "target": ("console", "remote", "consoleCredentialsForm", "sshTargetForm",
               "sshKeyForm"),
    "device": ("deviceForm", "kvm", "kvmDeviceSettings", "networkSettings",
               "lanDiscoveryEnabled", "deviceHardwareCard", "powerResetSwap",
               "settingsLocatorReverse", "ms2109PowerControl",
               "browserFocusRefreshEnabled"),
    "security": ("accountForm", "autoLogoutEnabled", "passkeyCard", "twoStepCard"),
    "agent": ("mcpEnabled", "agentToolPolicyRows", "embeddedAgentEnabled",
              "agentApiForm", "webSearchForm", "agentSkillsJson"),
    "system": ("otaSettingsForm", "otaUploadForm", "storage", "storageFileList",
               "resetEverything"),
    "status": ("deviceDiagnostics", "deviceDiagnosticsSummary", "diagnosticDevice",
               "diagnosticNetwork", "diagnosticVideo", "diagnosticUsb",
               "diagnosticGpio", "gpioMatrix", "gpioMatrixCard", "diagnostics",
               "diagnosticLogs", "refreshDeviceDiagnostics"),
    "advanced": ("gpio", "gpioMapForm", "advancedSettings"),
}
# Controls that act immediately or only on this browser must never be
# swept into a page save.
IMMEDIATE_CONTROLS = {
    "browserFocusRefreshEnabled", "browserAgentActionsEnabled",
    "ms2109PowerToggle", "settingsLocatorReverse",
}
ACTION_FORMS = {"accountForm", "passkeyForm", "twoStepForm", "sshKeyForm",
                "otaUploadForm", "storageUploadForm"}
REMOVED_CARD_BADGE_IDS = {
    "productFeaturesBadge", "ms2109PowerBadge", "ms2109EepromBadge",
    "storageBadge", "storageFileBadge", "storageUploadBadge",
    "diagnosticBadge", "networkBadge", "diagnosticLogsBadge", "authBadge",
    "videoRuntimeBadge", "consoleCredentialsBadge", "sshTargetBadge",
    "sshKeyBadge", "mcpBadge", "agentApiBadge", "agentPromptBadge",
    "agentToolsBadge", "webSearchBadge", "skillsBadge", "otaBadge",
    "systemBadge",
}
REMOVED_PER_CARD_SAVES = {
    "saveVideoRuntime", "saveAutoLogout", "saveMcpSettings", "saveAgentPrivacy",
    "saveAgentTools", "saveAgentSkills", "targetProfileLoad", "agentApiLoad",
    "agentPromptLoad", "consoleCredentialsLoad", "sshTargetLoad", "sshKeyLoad",
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


def _inside(node: Node, ancestor: Node) -> bool:
    parent = node.parent
    while parent is not None:
        if parent is ancestor:
            return True
        parent = parent.parent
    return False


def fail(message: str) -> None:
    FAILURES.append(message)


SOURCE = SETTINGS_PATH.read_text(encoding="utf-8")
PARSER = SettingsParser()
PARSER.feed(SOURCE)
FAILURES: list[str] = []

def controls_inside(node_id: str) -> set[str]:
    return {
        node.attrs["id"] for node in PARSER.nodes
        if node.tag in {"button", "input", "select", "textarea"}
        and node.attrs.get("id") and node.ancestor(node_id=node_id) is not None
    }


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
        while parent is not None and parent is not menu:
            parent = parent.parent
        if parent is None:
            continue
        hidden = ("hidden" in node.attrs or
                  node.attrs.get("aria-hidden") == "true" or
                  node.attrs.get("tabindex") == "-1")
        if not hidden:
            visible_links.append((node.attrs.get("href", ""), text_content(node)))
    if len(visible_links) != len(EXPECTED_MENU):
        fail(f"primary navigation exposes {len(visible_links)} entries, "
             f"expected {len(EXPECTED_MENU)}")
    for (actual_href, actual_text), (expected_href, expected_label) in zip(
            visible_links, EXPECTED_MENU):
        if actual_href != expected_href or expected_label not in actual_text:
            fail(f"primary navigation mismatch: got {actual_href!r} {actual_text!r}, "
                 f"expected {expected_href!r} containing {expected_label!r}")
    groups = [text_content(node) for node in PARSER.nodes
              if "settings-menu-group" in node.classes()]
    if groups != ["被控端", "本设备", "维护"]:
        fail(f"navigation groups must be 被控端 / 本设备 / 维护, got {groups}")

target_menu_ids = {
    node.attrs.get("href", "")[1:] for node in PARSER.nodes
    if node.tag == "a" and node.attrs.get("data-scope") == "target"
}
if target_menu_ids != TARGET_SCOPE_IDS:
    fail(f"target blue scope must cover exactly the target menu entries, got {sorted(target_menu_ids)}")
target_heads = {
    node.attrs.get("id", "") for node in PARSER.nodes
    if node.tag == "h2" and node.attrs.get("data-scope") == "target"
}
if target_heads != TARGET_SCOPE_IDS:
    fail(f"target blue scope must cover exactly the target page titles, got {sorted(target_heads)}")

pages = [node for node in PARSER.nodes
         if node.tag == "section" and "settings-page" in node.classes()]
page_ids = [node.attrs.get("data-settings-section", "") for node in pages]
if page_ids != EXPECTED_SECTIONS:
    fail(f"settings pages must follow the menu order {EXPECTED_SECTIONS}, got {page_ids}")
for page in pages:
    if page.parent is None or "settings-content" not in page.parent.classes():
        fail(f"page {page.attrs.get('data-settings-section')} must be a direct child of .settings-content")
    heads = [node for node in PARSER.nodes if node.tag == "header" and node.parent is page
             and "settings-page-head" in node.classes()]
    if len(heads) != 1:
        fail(f"page {page.attrs.get('data-settings-section')} needs exactly one page header")
declared = {node.attrs["data-settings-section"] for node in PARSER.nodes
            if node.attrs.get("data-settings-section")}
if declared != set(EXPECTED_SECTIONS):
    fail(f"unexpected data-settings-section owners: {sorted(declared ^ set(EXPECTED_SECTIONS))}")

for owner, node_ids in OWNERSHIP.items():
    for node_id in node_ids:
        node = PARSER.ids.get(node_id)
        if node is None:
            fail(f"missing Settings surface #{node_id}")
        elif node.owning_section() != owner:
            fail(f"#{node_id} belongs on the {owner} page, found {node.owning_section()}")

mcp = PARSER.ids.get("mcpEnabled")
if mcp is not None and mcp.has_ancestor_class("agent-only"):
    fail("MCP access control is incorrectly hidden with the embedded Agent")
for node_id in AGENT_ONLY_IDS:
    node = PARSER.ids.get(node_id)
    if node is None:
        fail(f"missing Dev Agent setting #{node_id}")
    elif node.owning_section() != "agent" or not node.has_ancestor_class("agent-only"):
        fail(f"#{node_id} must stay on the AI page behind the Stable/Dev agent-only gate")

ota_upload = PARSER.ids.get("otaUploadForm")
if ota_upload is not None and not ota_upload.has_ancestor_class("developer-details"):
    fail("local firmware upload is not gated to Dev firmware")
factory_reset = PARSER.ids.get("resetEverything")
if factory_reset is not None and factory_reset.has_ancestor_class("developer-details"):
    fail("factory reset is incorrectly hidden in Stable firmware")

if controls_inside("kvmDeviceSettings") != {
        "previewFpsSelect", "videoAlwaysOnline", "browserFocusRefreshEnabled",
        "ms2109PowerToggle"}:
    fail(f"video settings moved controls without audit: {sorted(controls_inside('kvmDeviceSettings'))}")
if controls_inside("deviceHardwareCard") != {"powerResetSwap", "settingsLocatorReverse"}:
    fail(f"device hardware settings moved controls without audit: {sorted(controls_inside('deviceHardwareCard'))}")
for row_id, control_id in (("settingsLocatorReverseRow", "settingsLocatorReverse"),
                           ("powerResetSwapRow", "powerResetSwap")):
    row = PARSER.ids.get(row_id)
    control = PARSER.ids.get(control_id)
    if row is None or "hidden" not in row.attrs:
        fail(f"#{row_id} must stay hidden until board capabilities load")
    elif control is None or control.ancestor(node_id=row_id) is None:
        fail(f"#{control_id} is outside its capability-gated row")
locator_message = PARSER.ids.get("settingsLocatorMsg")
if locator_message is None or "hidden" not in locator_message.attrs:
    fail("Locator LED reverse feedback is visible before board capabilities load")

# Save model: setting cards are registered groups without their own save
# button; one save bar per page writes them.
group_nodes = [node for node in PARSER.nodes if node.attrs.get("data-settings-group")]
group_names = [node.attrs["data-settings-group"] for node in group_nodes]
if len(group_names) != len(set(group_names)):
    fail("each data-settings-group may appear only once")
for node in group_nodes:
    name = node.attrs["data-settings-group"]
    if f'SettingsDirty.register("{name}",' not in SOURCE:
        fail(f"setting group {name} is not registered for dirty tracking")
    if not re.search(rf"\n  {re.escape(name)}:(async)?\(\)=>", SOURCE):
        fail(f"setting group {name} has no SETTINGS_SAVERS entry")
    if node.owning_section() == "status":
        fail(f"setting group {name} leaked onto the read-only Status page")
    for child in PARSER.nodes:
        if child.tag == "button" and "primary" in child.classes() and _inside(child, node):
            fail(f"setting group {name} still has its own primary save button")
for node_id in REMOVED_PER_CARD_SAVES:
    if node_id in PARSER.ids:
        fail(f"per-card save/reload control #{node_id} came back")
for node_id in IMMEDIATE_CONTROLS:
    node = PARSER.ids.get(node_id)
    if node is None or "data-dirty-ignore" not in node.attrs:
        fail(f"immediate control #{node_id} must be excluded from the page save")
for node_id in ACTION_FORMS:
    node = PARSER.ids.get(node_id)
    if node is None or "data-dirty-ignore" not in node.attrs:
        fail(f"action form #{node_id} must be excluded from the page save")
for node_id in ("settingsSaveBar", "settingsSave", "settingsDiscard"):
    if node_id not in PARSER.ids:
        fail(f"missing page save bar control #{node_id}")
if PARSER.ids.get("settingsSaveBar") is not None and "hidden" not in PARSER.ids["settingsSaveBar"].attrs:
    fail("page save bar must start hidden")
for node in PARSER.nodes:
    if node.tag == "h2" and node.has_ancestor_class("card"):
        fail("cards use h3 titles; h2 is reserved for page headers")
        break

for node_id in REMOVED_CARD_BADGE_IDS:
    if node_id in PARSER.ids or re.search(rf"\b{re.escape(node_id)}\b", SOURCE):
        fail(f"removed card-header status badge #{node_id} is still present")

visible_text = " ".join(text_content(node) for node in PARSER.nodes if node.tag in {"h2", "h3"})
if "产品功能" in visible_text or "保存产品功能" in SOURCE:
    fail("generic 产品功能 card leaked back into the product Settings UI")

required_source_fragments = (
    'const SETTINGS_IDS=["target-info","target","device","security","agent","system","status","advanced"]',
    "async function requestSettingsSection(",
    "if(ticket!==settingsNavTicket)return;",
    'title:"放弃未保存的更改？"',
    "--target-accent:var(--blue)",
    '.settings-menu a[data-scope="target"]:after{background:var(--target-accent)!important}',
    '.settings-menu a.has-dirty:after{background:var(--yellow)!important',
    '.msg.ok{color:var(--green)}',
    '.msg.warn{color:var(--yellow)}',
    '.msg.bad{color:var(--red)}',
    'button.primary{border-color:#1f766d;background:#17302f;color:var(--green)}',
    'button.danger{border-color:#7f2d2d;color:#fecaca}',
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
    'id="ms2109PowerControl" class="ui-setting-row" data-ms2109-control hidden',
    'if(SettingsDirty.isDirty("video_runtime"))return;',
    "视频采集常在线",
    "返回 KVM 自动刷新",
    "MS 视频电源",
)
for fragment in required_source_fragments:
    if fragment not in SOURCE:
        fail(f"missing Settings review contract marker: {fragment}")

if SOURCE.count("var(--target-accent)") != 2:
    fail("target accent must remain limited to the navigation dot and scoped save buttons")
for removed_ms_debug in ('id="ms2109PowerRows"', 'id="ms2109PowerCycle"',
                         'id="ms2109Refresh"', "3.3V Switch", "1.2V Enable",
                         "电源反馈", "操作次数", "Last result"):
    if removed_ms_debug in SOURCE:
        fail(f"MS2109 debug-only detail leaked into Settings: {removed_ms_debug}")
for removed_ms2109_eeprom_marker in ("ms2109Eeprom", "/api/ms2109/eeprom/",
                                     "PROGRAM MS2109 EEPROM", "AT24C16"):
    if removed_ms2109_eeprom_marker in SOURCE:
        fail(f"Settings exposes removed MS2109 EEPROM control: {removed_ms2109_eeprom_marker!r}")
if "settingsLocatorToggle" in PARSER.ids or 'action:on?"locator_on":"locator_off"' in SOURCE:
    fail("Settings still duplicates the Overview Locator LED on/off control")
if "margin:-5px" in SOURCE:
    fail("Settings card copy must not use negative spacing")
if "for(const el of [...content.children])" in SOURCE or "data-settings-priority" in SOURCE:
    fail("Settings pages must be declared in markup, not reordered at runtime")
if re.search(r"UI\.lifecycle\.interval\([^;]{0,1200}\brefresh\(\)", SOURCE):
    fail("Settings still performs an unconditional whole-page refresh loop")

if FAILURES:
    print("Settings review contract: FAIL", file=sys.stderr)
    for failure in FAILURES:
        print(f"- {failure}", file=sys.stderr)
    raise SystemExit(1)

print("Settings review contract: PASS")
