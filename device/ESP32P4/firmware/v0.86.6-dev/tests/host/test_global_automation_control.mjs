import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const source = fs.readFileSync(
  new URL("../../main/www/assets/ui-shell.js", import.meta.url), "utf8");

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
    else if (char === "}" && --depth === 0) return source.slice(start, index + 1);
  }
  assert.fail(`unterminated function ${name}`);
}

class FakeClassList {
  constructor() { this.values = new Set(); }
  toggle(name, enabled) {
    if (enabled) this.values.add(name);
    else this.values.delete(name);
  }
  contains(name) { return this.values.has(name); }
}

class FakeElement {
  constructor() {
    this.classList = new FakeClassList();
    this.dataset = {};
    this.hidden = false;
    this.disabled = false;
    this.textContent = "";
  }
}

const elements = Object.fromEntries([
  "eaAutomationBanner", "eaAutomationTitle", "eaAutomationDetail",
  "eaAutomationStop",
].map(id => [id, new FakeElement()]));
const events = [];
const posts = [];
const context = {
  UI: {
    session: { active: () => true },
    api: {
      getShared: async () => ({ operations: [] }),
      post: async (path, body) => {
        posts.push({ path, body });
        return { accepted: true, operations: [] };
      },
    },
  },
  document: { dispatchEvent: event => events.push(event) },
  CustomEvent: class CustomEvent {
    constructor(type, init) { this.type = type; this.detail = init.detail; }
  },
};

vm.runInNewContext(`
  let automationState = { operations: [] };
  let automationSignature = "";
  let lastDeviceStatus = null;
  let renderStatusCalls = 0;
  const AUTOMATION_RESOURCE_LABELS = Object.freeze({
    power: "主机电源", hid: "键盘与鼠标", ssh: "SSH 终端",
    uart: "UART 终端", "video-observe": "KVM 视频",
    maintenance: "系统维护",
  });
  function byId(id) { return globalThis.elements[id] || null; }
  function renderStatus() { renderStatusCalls += 1; }
  ${namedFunction("automationOperations")}
  ${namedFunction("automationBlocksPower")}
  ${namedFunction("automationActorLabel")}
  ${namedFunction("automationPrimaryOperation")}
  ${namedFunction("renderAutomation")}
  ${namedFunction("refreshAutomation")}
  ${namedFunction("stopPrimaryAutomation")}
  globalThis.contract = {
    renderAutomation, automationBlocksPower, stopPrimaryAutomation,
    renderStatusCalls: () => renderStatusCalls,
  };
`, Object.assign(context, { elements }));

const blocking = {
  resource: "power",
  actor: "mcp",
  generation: 7,
  label: "MCP 正在操作主机电源",
  cancellable: true,
  cancel_requested: false,
  blocks_manual: true,
};
context.contract.renderAutomation({ operations: [blocking] });
assert.equal(elements.eaAutomationBanner.hidden, false);
assert.equal(elements.eaAutomationTitle.textContent, "MCP 正在操作 主机电源");
assert.match(elements.eaAutomationDetail.textContent, /冲突的手动操作已暂停/);
assert.equal(elements.eaAutomationStop.dataset.resource, "power");
assert.equal(elements.eaAutomationStop.dataset.generation, "7");
assert.equal(context.contract.automationBlocksPower(), true);
assert.equal(events.length, 1);

context.contract.renderAutomation({ operations: [blocking] });
assert.equal(events.length, 1, "identical snapshots must not emit duplicate events");

context.contract.renderAutomation({ operations: [{
  resource: "video-observe",
  actor: "agent",
  generation: 8,
  label: "Agent 正在查看视频",
  cancellable: true,
  blocks_manual: false,
}] });
assert.equal(elements.eaAutomationBanner.classList.contains("observing"), true);
assert.match(elements.eaAutomationDetail.textContent, /手动查看仍可用/);
assert.equal(context.contract.automationBlocksPower(), false);

context.contract.renderAutomation({ operations: [blocking] });
await context.contract.stopPrimaryAutomation();
assert.equal(JSON.stringify(posts), JSON.stringify([{
  path: "/api/automation/stop",
  body: { resource: "power", generation: 7 },
}]));
assert.equal(elements.eaAutomationBanner.hidden, true);

context.contract.renderAutomation({ operations: [{
  resource: "maintenance",
  actor: "system",
  generation: 9,
  label: "正在安装固件",
  cancellable: false,
  blocks_manual: true,
}] });
assert.equal(elements.eaAutomationStop.hidden, true);
assert.match(elements.eaAutomationTitle.textContent, /SYSTEM/);

console.log("Global automation control UI tests: PASS");
