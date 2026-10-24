import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";


const source = fs.readFileSync(
  new URL("../../main/www/settings.html", import.meta.url), "utf8");

function declaration(prefix) {
  const start = source.indexOf(prefix);
  assert.notEqual(start, -1, `missing ${prefix}`);
  const end = source.indexOf(";", start);
  assert.notEqual(end, -1, `unterminated ${prefix}`);
  return source.slice(start, end + 1);
}

function namedFunction(name) {
  const start = source.indexOf(`function ${name}(`);
  assert.notEqual(start, -1, `missing function ${name}`);
  const brace = source.indexOf("{", start);
  let depth = 0;
  let quote = "";
  let escaped = false;
  for (let index = brace; index < source.length; index += 1) {
    const char = source[index];
    if (quote) {
      if (escaped) escaped = false;
      else if (char === "\\") escaped = true;
      else if (char === quote) quote = "";
      continue;
    }
    if (char === "\"" || char === "'" || char === "`") {
      quote = char;
      continue;
    }
    if (char === "{") depth += 1;
    else if (char === "}") {
      depth -= 1;
      if (depth === 0) return source.slice(start, index + 1);
    }
  }
  assert.fail(`unterminated function ${name}`);
}

const routingContext = {};
vm.runInNewContext(`
  ${declaration("const SETTINGS_IDS=")}
  ${declaration("const SETTINGS_ALIASES=")}
  ${namedFunction("settingsId")}
  globalThis.contract = { SETTINGS_IDS, SETTINGS_ALIASES, settingsId };
`, routingContext);

const router = routingContext.contract;
assert.deepEqual(
  Array.from(router.SETTINGS_IDS),
  ["target-info", "target", "device", "security", "agent", "system", "status", "advanced"],
  "Settings must expose exactly the eight product pages",
);
for (const section of router.SETTINGS_IDS) {
  assert.equal(router.settingsId(section), section, `route ${section} must be stable`);
}
const legacyRoutes = {
  system: ["storage", "storage-management", "firmware", "firmware-maintenance", "ota", "reset"],
  status: ["diagnostic", "diagnostics", "device-diagnostics", "deviceDiagnostics",
           "logs", "gpio-matrix", "gpioMatrix"],
  advanced: ["gpio", "advanced-settings", "developer", "developer-settings"],
  device: ["hardware", "kvm", "kvm-video", "ms", "ms2109", "network", "networkSettings",
           "general", "overview"],
  target: ["console", "remote", "target-host", "control-access"],
  agent: ["mcp", "skills", "embeddedAgent"],
};
for (const [page, routes] of Object.entries(legacyRoutes)) {
  for (const route of routes) {
    assert.equal(router.settingsId(route), page, `deep link #${route} must land on ${page}`);
  }
}
assert.equal(router.settingsId("target-profile"), "target-info");
assert.equal(router.settingsId("unknown-route"), "device");

class MemoryStorage {
  constructor() { this.values = new Map(); }
  getItem(key) { return this.values.has(key) ? this.values.get(key) : null; }
  setItem(key, value) { this.values.set(key, String(value)); }
}
class FakeCustomEvent {
  constructor(type, options = {}) { this.type = type; this.detail = options.detail; }
}
const preferenceContext = {
  localStorage: new MemoryStorage(),
  CustomEvent: FakeCustomEvent,
  setTimeout() { return 1; },
  window: {
    events: [],
    dispatchEvent(event) { this.events.push(event); return true; },
  },
};
vm.runInNewContext(`${namedFunction("bindBrowserPreference")}
  globalThis.bind = bindBrowserPreference;`, preferenceContext);

const message = { textContent: "" };
const first = { checked: false, onchange: null };
preferenceContext.bind(first, "si_kvm_focus_refresh", message);
assert.equal(first.checked, true, "browser preference defaults to enabled");
assert.equal(typeof first.onchange, "function");
first.checked = false;
first.onchange();
assert.equal(preferenceContext.localStorage.getItem("si_kvm_focus_refresh"), "0",
  "change must persist immediately without a Save button");
assert.equal(preferenceContext.window.events.length, 1);
assert.equal(preferenceContext.window.events[0].detail.enabled, false);

const second = { checked: true, onchange: null };
preferenceContext.bind(second, "si_kvm_focus_refresh", message);
assert.equal(second.checked, false, "a new binding must restore the saved preference");
second.checked = true;
second.onchange();
assert.equal(preferenceContext.localStorage.getItem("si_kvm_focus_refresh"), "1");

const reverseControl = {
  checked: false,
  disabled: true,
  title: "",
  attributes: {},
  setAttribute(name, value) { this.attributes[name] = value; },
};
const reverseRow = { hidden: true };
const reverseMessage = { hidden: true, textContent: "" };
const locatorContext = {
  lastSettingsLocator: {
    supported: false,
    gpio: -1,
    return_gpio: -1,
    bidirectional: false,
    reversed: false,
  },
  settingsLocatorReversePending: null,
  settingsLocatorReverseBusy: false,
  settingsLocatorReverse: reverseControl,
  settingsLocatorReverseRow: reverseRow,
  settingsLocatorReverseMeta: { textContent: "" },
  settingsLocatorMsg: reverseMessage,
};
vm.runInNewContext(`${namedFunction("renderSettingsLocator")}
  globalThis.renderLocator = renderSettingsLocator;`, locatorContext);
locatorContext.renderLocator({
  available: true,
  busy: false,
  locator: {
    supported: true,
    gpio: 32,
    return_gpio: 33,
    bidirectional: true,
    reversed: true,
  },
});
assert.equal(reverseControl.checked, true,
  "Device Settings must render the saved Locator direction");
assert.equal(reverseControl.disabled, false,
  "available bidirectional Locator direction must remain configurable");
assert.equal(reverseRow.hidden, false,
  "bidirectional Locator direction must be visible");
assert.equal(reverseMessage.hidden, false,
  "bidirectional Locator save feedback must be visible");
assert.equal(reverseControl.attributes["aria-busy"], "false");
assert.match(locatorContext.settingsLocatorReverseMeta.textContent, /反向/);

locatorContext.settingsLocatorReversePending = false;
locatorContext.settingsLocatorReverseBusy = true;
locatorContext.renderLocator({ available: true, busy: false,
  locator: locatorContext.lastSettingsLocator });
assert.equal(reverseControl.checked, false,
  "pending Locator direction must be rendered optimistically");
assert.equal(reverseControl.disabled, true,
  "Locator direction must remain disabled while its save is pending");

locatorContext.settingsLocatorReversePending = null;
locatorContext.settingsLocatorReverseBusy = false;
reverseMessage.textContent = "stale feedback";
locatorContext.renderLocator({
  available: true,
  busy: false,
  locator: {
    supported: true,
    gpio: 17,
    return_gpio: -1,
    bidirectional: false,
    reversed: false,
  },
});
assert.equal(reverseRow.hidden, false,
  "reported single-GPIO Locator remains visible in Device Settings");
assert.equal(reverseMessage.hidden, true,
  "single-GPIO product boards must not expose reverse-control feedback");
assert.equal(reverseMessage.textContent, "",
  "hiding an unsupported reverse control must clear stale feedback");
assert.equal(reverseControl.disabled, true,
  "an unsupported reverse control must remain fail-closed");
assert.match(locatorContext.settingsLocatorReverseMeta.textContent, /不支持双向反转/);

const swapControl = {
  disabled: true,
  attributes: {},
  setAttribute(name, value) { this.attributes[name] = value; },
};
const deviceHardwareContext = {
  powerResetSwapBusy: false,
  powerResetSwapRow: { hidden: true },
  powerResetSwap: swapControl,
  powerResetSwapMeta: { textContent: "" },
  renderSettingsLocator() {},
};
vm.runInNewContext(`${namedFunction("renderDeviceHardware")}
  globalThis.renderDeviceHardwareForTest = renderDeviceHardware;`, deviceHardwareContext);
deviceHardwareContext.renderDeviceHardwareForTest({
  available: true,
  busy: false,
  gpio_map: [
    { role: "power_button", gpio: 4 },
    { role: "reset_button", gpio: 5 },
  ],
});
assert.equal(deviceHardwareContext.powerResetSwapRow.hidden, false,
  "valid PWR/RST mappings must expose the device-level swap control");
assert.equal(swapControl.disabled, false,
  "idle valid PWR/RST mappings must be swappable");
assert.match(deviceHardwareContext.powerResetSwapMeta.textContent, /开机 GPIO4/);
assert.match(deviceHardwareContext.powerResetSwapMeta.textContent, /重启 GPIO5/);

deviceHardwareContext.powerResetSwapBusy = true;
deviceHardwareContext.renderDeviceHardwareForTest({
  available: true,
  busy: false,
  gpio_map: [
    { role: "power_button", gpio: 5 },
    { role: "reset_button", gpio: 4 },
  ],
});
assert.equal(swapControl.disabled, true,
  "PWR/RST swap must remain disabled while a save is pending");
assert.equal(swapControl.attributes["aria-busy"], "true");

deviceHardwareContext.powerResetSwapBusy = false;
deviceHardwareContext.renderDeviceHardwareForTest({
  available: false,
  gpio_map: [],
});
assert.equal(deviceHardwareContext.powerResetSwapRow.hidden, true,
  "boards without two valid button mappings must fail closed");

console.log("Settings review runtime contract: PASS");

// Page save model: secrets never enter the baseline, restore puts the last
// saved values back, and sync only adopts server values while unedited.
{
  const start = source.indexOf("const SettingsDirty={");
  const end = source.indexOf("\nSettingsDirty.register(", start);
  assert.ok(start >= 0 && end > start, "missing SettingsDirty");
  const context = {};
  vm.runInNewContext(`${source.slice(start, end)}\nglobalThis.dirty = SettingsDirty;`, context);
  const dirty = context.dirty;
  const field = (type, value) => ({
    type, value, checked: false, disabled: false, dataset: {},
    classList: { set: new Set(), toggle(name, on) { on ? this.set.add(name) : this.set.delete(name); },
      remove(name) { this.set.delete(name); }, contains(name) { return this.set.has(name); } },
  });
  const name = field("text", "ExoAnchor");
  const secret = field("password", "");
  const toggle = field("checkbox", "");
  const root = {
    classList: { set: new Set(), toggle(c, on) { on ? this.set.add(c) : this.set.delete(c); },
      remove(c) { this.set.delete(c); }, contains(c) { return this.set.has(c); } },
    querySelectorAll: () => [name, secret, toggle],
    contains: candidate => [name, secret, toggle].includes(candidate),
  };
  let changes = 0;
  dirty.onChange = () => { changes += 1; };
  dirty.register("ssh_target", root);
  dirty.capture("ssh_target");
  assert.equal(dirty.isDirty("ssh_target"), false);

  secret.value = "hunter22";
  dirty.updateField(secret);
  assert.equal(dirty.isDirty("ssh_target"), true, "typing a secret is an unsaved edit");
  assert.ok(![...dirty.baselines.get("ssh_target").values()].includes("hunter22"),
    "secrets must never be stored in the dirty baseline");

  name.value = "renamed";
  toggle.checked = true;
  dirty.updateField(name);
  assert.deepEqual(Array.from(dirty.dirtyNames()), ["ssh_target"]);
  dirty.restore("ssh_target");
  assert.equal(name.value, "ExoAnchor", "discard restores the saved text");
  assert.equal(secret.value, "", "discard clears typed secrets");
  assert.equal(toggle.checked, false, "discard restores switches");
  assert.equal(dirty.isDirty("ssh_target"), false);
  assert.ok(changes > 0, "dirty changes notify the save bar");

  name.value = "server value";
  dirty.sync("ssh_target");
  assert.equal(dirty.isDirty("ssh_target"), false, "sync adopts server values while clean");
  name.value = "user edit";
  dirty.updateField(name);
  dirty.sync("ssh_target");
  assert.equal(dirty.isDirty("ssh_target"), true, "sync must not swallow a pending edit");
}
console.log("Settings save model runtime: PASS");
