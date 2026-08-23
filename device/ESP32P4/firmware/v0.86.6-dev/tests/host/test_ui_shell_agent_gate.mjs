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
  constructor() { this.values = new Set(); }
  add(...names) { names.forEach(name => this.values.add(name)); }
  remove(...names) { names.forEach(name => this.values.delete(name)); }
  contains(name) { return this.values.has(name); }
  toggle(name, enabled) {
    if (enabled === undefined) enabled = !this.values.has(name);
    if (enabled) this.values.add(name);
    else this.values.delete(name);
    return enabled;
  }
}

class FakeElement {
  constructor() {
    this.attributes = new Map();
    this.classList = new FakeClassList();
    this.dataset = {};
    this.title = "";
  }
  setAttribute(name, value) { this.attributes.set(name, String(value)); }
  removeAttribute(name) { this.attributes.delete(name); }
  getAttribute(name) { return this.attributes.get(name) ?? null; }
  hasAttribute(name) { return this.attributes.has(name); }
}

const agentNav = new FakeElement();
agentNav.dataset.agentHref = "/agent";
let getProductFeatures = async () => ({ embedded_agent_enabled: false });
const context = {
  clearTimeout() {},
  UI: {
    api: {
      get(path) {
        assert.equal(path, "/api/settings/product-features");
        return getProductFeatures();
      },
      ensure: async () => true,
    },
  },
};

vm.runInNewContext(`
  const embeddedAgent = true;
  let agentRuntimeEnabled = false;
  let agentAvailabilityGeneration = 0;
  let assistantRuntimeInitialized = false;
  const assistant = { pollTimer: 0, open: false };
  const agentNav = globalThis.agentNav;
  function byId(id) { return id === "agentNav" ? agentNav : null; }
  function loadAssistantName() {}
  function assistantSyncRun() {}
  ${namedFunction("applyAgentAvailability")}
  ${namedFunction("updateProductFeatures")}
  ${namedFunction("handleAgentFeatureBroadcast")}
  ${namedFunction("refreshAgentAvailability")}
  globalThis.contract = {
    applyAgentAvailability,
    updateProductFeatures,
    handleAgentFeatureBroadcast,
    refreshAgentAvailability,
    runtimeEnabled: () => agentRuntimeEnabled,
  };
`, Object.assign(context, { agentNav }));

const gate = context.contract;

gate.applyAgentAvailability(null, false);
assert.equal(agentNav.hasAttribute("href"), false,
  "pending Agent availability must fail closed without an href");
assert.equal(agentNav.getAttribute("aria-disabled"), "true");
assert.equal(agentNav.getAttribute("tabindex"), "-1");
assert.equal(agentNav.classList.contains("agent-disabled"), true);
assert.match(agentNav.title, /正在读取/);

gate.updateProductFeatures({ embedded_agent_enabled: false });
assert.equal(gate.runtimeEnabled(), false);
assert.equal(agentNav.hasAttribute("href"), false,
  "runtime-disabled Agent navigation must not remain clickable");
assert.equal(agentNav.getAttribute("aria-disabled"), "true");
assert.equal(agentNav.getAttribute("tabindex"), "-1");
assert.match(agentNav.title, /已在 Settings 中关闭/);

gate.updateProductFeatures({ embedded_agent_enabled: true });
assert.equal(gate.runtimeEnabled(), true);
assert.equal(agentNav.getAttribute("href"), "/agent",
  "runtime-enabled Agent navigation must restore its destination");
assert.equal(agentNav.hasAttribute("aria-disabled"), false);
assert.equal(agentNav.hasAttribute("tabindex"), false,
  "enabled Agent navigation must return to native anchor focus behavior");
assert.equal(agentNav.classList.contains("agent-disabled"), false);

let resolveOldGet;
getProductFeatures = () => new Promise(resolve => { resolveOldGet = resolve; });
const staleEnable = gate.refreshAgentAvailability();
gate.updateProductFeatures({ embedded_agent_enabled: false });
resolveOldGet({ embedded_agent_enabled: true });
await staleEnable;
assert.equal(gate.runtimeEnabled(), false,
  "an older GET response must not override a newer Settings event");
assert.equal(agentNav.hasAttribute("href"), false);

let rejectOldGet;
getProductFeatures = () => new Promise((_resolve, reject) => { rejectOldGet = reject; });
const staleFailure = gate.refreshAgentAvailability();
gate.updateProductFeatures({ embedded_agent_enabled: true });
rejectOldGet(new Error("stale request failed"));
await staleFailure;
assert.equal(gate.runtimeEnabled(), true,
  "an older GET failure must not fail-close a newer enabled event");
assert.equal(agentNav.getAttribute("href"), "/agent");

gate.handleAgentFeatureBroadcast({
  data: { type: "embedded-agent-updated", embedded_agent_enabled: false },
});
assert.equal(gate.runtimeEnabled(), false,
  "another Settings tab must be able to disable Agent navigation immediately");
assert.equal(agentNav.hasAttribute("href"), false);

gate.handleAgentFeatureBroadcast({
  data: { type: "untrusted-message", embedded_agent_enabled: true },
});
assert.equal(gate.runtimeEnabled(), false,
  "unrecognized cross-tab messages must not change Agent availability");

gate.handleAgentFeatureBroadcast({
  data: { type: "embedded-agent-updated", embedded_agent_enabled: true },
});
assert.equal(gate.runtimeEnabled(), true,
  "another Settings tab must be able to restore Agent navigation immediately");
assert.equal(agentNav.getAttribute("href"), "/agent");

console.log("UI shell Agent runtime gate tests: PASS");
