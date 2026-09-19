import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const shell = fs.readFileSync(new URL("../../main/www/assets/ui-shell.js", import.meta.url), "utf8");
const agent = fs.readFileSync(new URL("../../main/www/agent.html", import.meta.url), "utf8");
function extract(source, name, indent) {
  let start = source.indexOf(`function ${name}(`);
  assert.ok(start >= 0);
  if (source.slice(start - 6, start) === "async ") start -= 6;
  const end = source.indexOf("\n" + indent + "}", start);
  assert.ok(end > start);
  return source.slice(start, end + indent.length + 2);
}
function pending() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
const store = new Map([["ea_agent_session", "s_old"]]);
const rendered = [];
const messages = [];
const requests = [];
const log = {
  children: [], querySelector: () => null, scrollHeight: 0,
  appendChild(node) { messages.push(node.textContent); this.children.push(node); },
};
const context = {
  assistant: { sessionId: "", historyGeneration: 0, historyLoaded: false },
  localStorage: {
    getItem: key => store.get(key), setItem: (key, value) => store.set(key, value),
    removeItem: key => store.delete(key),
  },
  byId: id => id === "eaAssistantLog" ? log : null,
  document: { createElement: () => ({}) },
  UI: { lifecycle: { destroyed: false }, api: { get() {
    const request = pending(); requests.push(request); return request.promise;
  } } },
  assistantRenderHistory: records => rendered.push(records),
};
vm.runInNewContext([
  "assistantSessionId", "assistantSetSession", "assistantAddMessage", "assistantLoadHistory",
].map(name => extract(shell, name, "  ")).join("\n") + `
  globalThis.load = assistantLoadHistory;
  globalThis.select = assistantSetSession;
  globalThis.add = assistantAddMessage;
`, context);

// Session changes, including returning to the same ID, invalidate old loads.
const old = context.load();
context.select("s_new");
context.select("s_old");
requests[0].resolve({ records: ["old session snapshot"] });
await old;
assert.equal(rendered.length, 0);

const older = context.load();
const newer = context.load();
requests[2].resolve({ records: ["newest"] });
await newer;
requests[1].resolve({ records: ["stale"] });
await older;
assert.equal(rendered.length, 1);
assert.equal(rendered[0][0], "newest");

const beforeMessage = context.load();
context.add("user", "A new message");
requests[3].resolve({ records: ["history without the new message"] });
await beforeMessage;
assert.equal(rendered.length, 1, "history must not erase a newly appended message");
assert.deepEqual(messages, ["A new message"]);

const staleFailure = context.load();
context.select("s_new");
requests[4].reject(new Error("old session offline"));
await staleFailure;
assert.deepEqual(messages, ["A new message"], "old errors must not enter a new conversation");
const destroyed = context.load();
context.UI.lifecycle.destroyed = true;
requests[5].resolve({ records: ["late page"] });
await destroyed;
assert.equal(rendered.length, 1);

// Main workspace already guards session IDs; newest load must also win within an ID.
const mainRequests = [];
const records = [];
const main = {
  activeSessionId: "same", agentHistoryLoadGeneration: 0,
  API: { get() { const request = pending(); mainRequests.push(request); return request.promise; } },
  provisionalAgentSessions: new Map(), projectAgentHistory: value => value,
  chatLog: { dataset: {} }, renderHistoryRecord: value => records.push(value),
  showAgentHistoryNotice() {}, setBadge() {}, badgeAgent: {},
  renderBlankThread() {}, addBubble() { assert.fail("unexpected history error"); }, terminalLine() {},
};
vm.runInNewContext(extract(agent, "loadAgentHistory", "") + "\nglobalThis.load = loadAgentHistory;", main);
const first = main.load();
const second = main.load();
mainRequests[1].resolve({ records: ["current"], supported: true });
await second;
mainRequests[0].resolve({ records: ["outdated"], supported: true });
await first;
assert.deepEqual(records, ["current"]);
const failedOlder = main.load();
const succeededNewer = main.load();
mainRequests[3].resolve({ records: ["latest after retry"], supported: true });
await succeededNewer;
mainRequests[2].reject(new Error("outdated network error"));
await failedOlder;
assert.deepEqual(records, ["current", "latest after retry"]);
console.log("Assistant and workspace history race tests: PASS");
