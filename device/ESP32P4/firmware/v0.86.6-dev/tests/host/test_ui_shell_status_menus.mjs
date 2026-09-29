import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";


const source = fs.readFileSync(
  new URL("../../main/www/assets/ui-shell.js", import.meta.url),
  "utf8",
);

function namedFunction(name) {
  let start = source.indexOf(`function ${name}(`);
  assert.notEqual(start, -1, `missing function ${name}`);
  if (source.slice(Math.max(0, start - 6), start) === "async ") start -= 6;
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

class FakeClassList {
  constructor(...values) { this.values = new Set(values); }
  add(...names) { names.forEach(name => this.values.add(name)); }
  remove(...names) { names.forEach(name => this.values.delete(name)); }
  contains(name) { return this.values.has(name); }
}

class FakeElement {
  constructor(...classes) {
    this.attributes = new Map();
    this.classList = new FakeClassList(...classes);
    this.disabled = false;
    this.checked = false;
    this.textContent = "";
    this.title = "";
  }
  setAttribute(name, value) { this.attributes.set(name, String(value)); }
  removeAttribute(name) { this.attributes.delete(name); }
  getAttribute(name) { return this.attributes.get(name) ?? null; }
}

const elements = Object.fromEntries([
  "badgeVideo", "badgeUsb", "badgePwr", "videoPowerToggle", "videoPowerState",
  "powerActionPower", "powerActionReset", "videoMenu", "powerMenu",
].map(id => [id, new FakeElement(...(id.endsWith("Menu") ? ["hide"] : []))]));

const posts = [];
let refreshCount = 0;
let nextPostError = null;
const context = {
  elements,
  UI: {
    setStatus(element, text, kind) {
      element.textContent = text;
      element.className = "data-state " + (kind || "neutral");
    },
    api: {
      async post(path, body) {
        posts.push([path, body]);
        if (nextPostError) {
          const error = nextPostError;
          nextPostError = null;
          throw error;
        }
        return {};
      },
    },
    status: {
      async refresh() { refreshCount += 1; return {}; },
    },
  },
};

vm.runInNewContext(`
  const UI = globalThis.UI;
  const elements = globalThis.elements;
  let automationState = { operations: [] };
  let lastDeviceStatus = null;
  let msPowerMutationPending = false;
  function byId(id) { return elements[id] || null; }
  ${namedFunction("setBadge")}
  ${namedFunction("modeCount")}
  ${namedFunction("videoModeState")}
  ${namedFunction("automationOperations")}
  ${namedFunction("automationBlocksPower")}
  ${namedFunction("automationBlocksVideoPower")}
  ${namedFunction("renderStatus")}
  ${namedFunction("closeVideoMenu")}
  ${namedFunction("closePowerMenu")}
  ${namedFunction("closeStatusMenus")}
  ${namedFunction("toggleStatusMenu")}
  ${namedFunction("powerAction")}
  ${namedFunction("ms2109PowerAction")}
  globalThis.contract = {
    renderStatus,
    toggleStatusMenu,
    powerAction,
    ms2109PowerAction,
    setOperations(operations) { automationState = { operations }; },
    failNextPost(error) { globalThis.nextPostError = error; },
  };
`, context);

const status = {
  video: {
    frame_ready: true,
    height: 1080,
    target_fps_x100: 2500,
    control: { agent_active: false },
  },
  ms2109: { supported: true, initialized: true, power_on: true },
  hid: { connected: true },
  power: {
    available: true,
    busy: false,
    buttons: { power: { available: true }, reset: { available: true } },
    detect: {
      power: { supported: true, active: true },
      standby: { supported: true, active: true },
    },
  },
};

context.contract.renderStatus(status);
assert.equal(elements.badgeVideo.textContent, "VIDEO 1080P25");
assert.equal(elements.badgeVideo.disabled, false);
assert.equal(elements.videoPowerToggle.checked, true,
  "MS power switch must reflect the commanded on state");
assert.equal(elements.videoPowerToggle.disabled, false);
assert.equal(elements.videoPowerState.textContent, "开启");
assert.equal(elements.badgePwr.textContent, "POWER ON");
assert.equal(elements.badgePwr.disabled, false);

context.contract.toggleStatusMenu("videoMenu", "badgeVideo");
assert.equal(elements.videoMenu.classList.contains("hide"), false);
assert.equal(elements.badgeVideo.getAttribute("aria-expanded"), "true");
context.contract.toggleStatusMenu("powerMenu", "badgePwr");
assert.equal(elements.videoMenu.classList.contains("hide"), true,
  "opening POWER must close the VIDEO menu");
assert.equal(elements.powerMenu.classList.contains("hide"), false);

context.contract.setOperations([{
  resource: "video-hardware", blocks_manual: true,
}]);
context.contract.renderStatus(status);
assert.equal(elements.videoPowerToggle.disabled, true,
  "manual MS power must pause while Agent/MCP owns video hardware");
assert.match(elements.badgeVideo.title, /Agent\/MCP/);

context.contract.setOperations([{ resource: "power", blocks_manual: true }]);
context.contract.renderStatus(status);
assert.equal(elements.badgePwr.disabled, true,
  "manual host power menu must pause while Agent/MCP owns power");

context.contract.setOperations([]);
context.contract.renderStatus(status);
await context.contract.ms2109PowerAction(false);
assert.equal(elements.videoPowerToggle.checked, false);
assert.equal(elements.videoPowerState.textContent, "关闭");

const offStatus = structuredClone(status);
offStatus.ms2109.power_on = false;
context.contract.renderStatus(offStatus);
nextPostError = new Error("simulated power failure");
await assert.rejects(context.contract.ms2109PowerAction(true),
  /simulated power failure/);
assert.equal(elements.videoPowerToggle.checked, false,
  "a rejected MS power mutation must roll the switch back");
assert.equal(elements.videoPowerState.textContent, "关闭");
await context.contract.powerAction("reset");
assert.equal(JSON.stringify(posts), JSON.stringify([
  ["/api/ms2109/power", { action: "off" }],
  ["/api/ms2109/power", { action: "on" }],
  ["/api/power/action", { action: "reset" }],
]));
assert.equal(refreshCount, 3,
  "each status-menu mutation must immediately refresh shared status");

console.log("UI shell status-menu tests: PASS");
