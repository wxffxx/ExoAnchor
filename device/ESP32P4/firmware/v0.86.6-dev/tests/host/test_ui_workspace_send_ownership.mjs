import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const source = fs.readFileSync(new URL("../../main/www/agent.html", import.meta.url), "utf8");
function extract(name) {
  let start = source.indexOf(`function ${name}(`);
  assert.ok(start >= 0);
  if (source.slice(start - 6, start) === "async ") start -= 6;
  const lineEnd = source.indexOf("\n", start);
  return source.slice(start, source[lineEnd - 1] === "}" ? lineEnd : source.indexOf("\n}", start) + 2);
}
function pending() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
const tick = () => new Promise(setImmediate);
function harness(jobId = null) {
  const requests = [], bubbles = [], terminal = [], attached = [], badges = [], history = [];
  const context = {
    agentSendOwner: null, activeSessionGeneration: 0, activeSessionId: "A",
    activeAgentJob: jobId, activeAgentContext: { sessionId: "A" }, agentSessions: [],
    promptBox: { value: "original draft" }, sendPrompt: {}, stopPrompt: {},
    agentProfile: { value: "profile" }, agentModel: { value: "model" },
    agentKvmPermission: { disabled: true }, agentWebPermission: { disabled: true },
    UI: { lifecycle: { destroyed: false } }, validateOutgoingPrompt: () => true,
    optionalSessionId: id => id || "", localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
    materializeSession: () => context.setActiveSession("new-session"),
    terminalLine: (...args) => terminal.push(args), compactTerminalText: value => value,
    postAgentRun: body => post("run", body), API: { post: (path, body) => post(path, body) },
    currentPageContext: () => ({ page: "agent" }), chatLog: { querySelectorAll: () => [] },
    addBubble: (...args) => bubbles.push([context.activeSessionId, ...args]),
    addRunBubble: () => ({ isConnected: true }), renderWorkspaceRun() {},
    rememberProvisionalAgentTurn: (...args) => history.push(args),
    projectAgentSessions: value => value, renderSessions() {}, loadAgentSessions: async () => {},
    badgeAgent: {}, setBadge: (...args) => badges.push(args),
    attachAgentJob: async (status, details) => {
      attached.push([status, details]); context.activeAgentJob = status.job_id;
      context.setAgentButtonRunning(true);
    },
  };
  function post(path, body) { const response = pending(); requests.push({ path, body, ...response }); return response.promise; }
  vm.runInNewContext(["setActiveSession", "setAgentButtonRunning", "sendAgentMessage"].map(extract).join("\n"), context);
  return { context, requests, bubbles, terminal, attached, badges, history };
}
const accepted = { accepted: true, job_id: "run", session_id: "A" };
const duplicate = harness();
const first = duplicate.context.sendAgentMessage();
const second = duplicate.context.sendAgentMessage();
assert.equal(duplicate.requests.length, 1, "repeated keyboard submit must send only one request");
duplicate.context.setAgentButtonRunning(true);
assert.equal(duplicate.context.sendPrompt.disabled, true, "a poll must not unlock in-flight send controls");
duplicate.requests[0].resolve(accepted);
await Promise.all([first, second]);
assert.equal(duplicate.context.promptBox.value, "");
assert.equal(duplicate.context.sendPrompt.disabled, false);
assert.equal(duplicate.context.sendPrompt.textContent, "Send update");
assert.equal(duplicate.attached.length, 1);

const switched = harness();
const beforeSwitch = switched.context.sendAgentMessage();
switched.context.setActiveSession("B");
switched.context.setActiveSession("A");
switched.context.promptBox.value = "draft after switching back";
switched.requests[0].resolve(accepted);
await beforeSwitch;
assert.equal(switched.bubbles.length + switched.attached.length, 0, "returning to the same ID must not revive old UI work");
assert.equal(switched.context.promptBox.value, "draft after switching back");

for (const jobId of [null, "active-run"]) {
  const edited = harness(jobId);
  const editing = edited.context.sendAgentMessage();
  edited.context.promptBox.value = "a newly typed draft";
  edited.requests[0].resolve(jobId ? { accepted: true, intent_revision: 4 } : accepted);
  await editing;
  assert.equal(edited.context.promptBox.value, "a newly typed draft", "accepted request must retain edits made while waiting");
  assert.deepEqual(edited.bubbles, [["A", "user", "original draft"]]);
  assert.equal(edited.context.sendPrompt.disabled, false);
}

const failure = harness();
const failed = failure.context.sendAgentMessage();
failure.context.setActiveSession("B");
failure.terminal.length = 0;
failure.requests[0].reject(new Error("old session failed"));
await failed;
assert.equal(failure.bubbles.length + failure.badges.length + failure.terminal.length, 0);

const errorRefresh = harness(), sessions = pending();
errorRefresh.context.loadAgentSessions = () => sessions.promise;
const refreshing = errorRefresh.context.sendAgentMessage();
errorRefresh.requests[0].reject(new Error("failed to start"));
await tick();
errorRefresh.context.setActiveSession("B");
sessions.resolve();
await refreshing;
assert.equal(errorRefresh.badges.length, 0, "old error refresh must not overwrite another session's badge");

const offline = harness();
offline.context.loadAgentSessions = async () => { throw new Error("session refresh also failed"); };
const failedOffline = offline.context.sendAgentMessage();
offline.requests[0].reject(new Error("original submission failure"));
await failedOffline;
assert.equal(offline.context.sendPrompt.disabled, false);
assert.ok(offline.bubbles.some(([, kind, text]) => kind === "system" && text.includes("original submission failure")));
assert.equal(offline.badges[0][2], "ERROR");

const newThread = harness();
newThread.context.setActiveSession("");
const materialized = newThread.context.sendAgentMessage();
assert.equal(newThread.requests[0].body.session_id, "new-session");
newThread.requests[0].resolve({ ...accepted, session_id: "new-session" });
await materialized;
assert.equal(newThread.attached.length, 1, "creating the submission's own session must not invalidate it");

const busy = harness();
const refused = busy.context.sendAgentMessage();
busy.requests[0].resolve({ accepted: false, busy: true, job_id: "existing" });
await refused;
assert.equal(busy.context.promptBox.value, "original draft");
assert.equal(busy.context.activeAgentJob, "existing");
assert.equal(busy.context.sendPrompt.disabled, false);

const disposed = harness();
const leaving = disposed.context.sendAgentMessage();
disposed.context.UI.lifecycle.destroyed = true;
disposed.requests[0].resolve(accepted);
await leaving;
assert.equal(disposed.bubbles.length + disposed.attached.length, 0);
assert.equal(disposed.context.agentSendOwner, null);
for (const method of ["getItem", "setItem"]) {
  const storage = harness();
  if (method === "getItem") storage.context.agentModel.value = "";
  storage.context.localStorage[method] = () => { throw new Error("local storage unavailable"); };
  await storage.context.sendAgentMessage();
  assert.equal(storage.context.agentSendOwner, null, "local storage errors must release send ownership");
  assert.equal(storage.context.sendPrompt.disabled, false);
  assert.equal(storage.requests.length, 0);
  assert.ok(storage.bubbles.some(([, kind, text]) => kind === "system" && text.includes("local storage unavailable")));
}
console.log("Workspace submission ownership and draft preservation tests: PASS");
