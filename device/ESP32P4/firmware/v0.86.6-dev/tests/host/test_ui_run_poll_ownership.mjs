import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const source = fs.readFileSync(new URL("../../main/www/agent.html", import.meta.url), "utf8");
function extract(name) {
  let start = source.indexOf(`function ${name}(`);
  assert.ok(start >= 0);
  if (source.slice(start - 6, start) === "async ") start -= 6;
  const lineEnd = source.indexOf("\n", start);
  const end = source[lineEnd - 1] === "}" ? lineEnd : source.indexOf("\n}", start) + 2;
  return source.slice(start, end);
}
function pending() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
function harness() {
  const statuses = [], events = [], warnings = [], rendered = [], controls = [], badges = [], turns = [];
  const context = {
    activeAgentJob: "old", activeAgentPoll: 1, activeAgentPollBusy: false,
    activeAgentPollGeneration: 0, activeAgentLastEventSeq: 0,
    activeAgentPending: { isConnected: true }, activeAgentContext: { sessionId: "session-old" },
    activeSessionId: "session-old", agentSessions: [], activeAgentHeartbeats: new Set(),
    UI: { lifecycle: { destroyed: false, clearTimer() {} } },
    getAgentRunStatus() { const request = pending(); statuses.push(request); return request.promise; },
    getAgentRunEvents() { const request = pending(); events.push(request); return request.promise; },
    terminalLine: (...args) => warnings.push(args), settleTerminalProgress() {},
    setAgentButtonRunning: value => controls.push(value), renderAgentRunStatus: value => rendered.push(value),
    refreshAgentRequestQueue: async () => {}, loadHostDisplayPlan: async () => {}, loadBootKeyPlan: async () => {},
    agentStatusFinished: value => value.state === "done",
    safeSessionId: value => value, runFromStatus: status => status.result,
    summarizeRun: run => run.message, logRunResult() {},
    finishRunBubble: async () => {}, addBubble() {},
    rememberProvisionalAgentTurn: (...args) => turns.push(args), agentHistoryPersistenceState: () => "saved",
    setProvisionalAgentHistoryState() {}, projectAgentSessions: value => value, renderSessions() {},
    loadAgentSessions: async () => {}, showAgentHistoryNotice() {}, badgeAgent: {},
    setBadge: (...args) => badges.push(args), refreshStatus: async () => {},
  };
  vm.runInNewContext(["clearAgentPoll", "pollAgentRun", "finishAgentJob"].map(extract).join("\n"), context);
  return { context, statuses, events, warnings, rendered, controls, badges, turns };
}
function replaceRun(context, job = "new") {
  context.clearAgentPoll();
  context.activeAgentJob = job;
  context.activeAgentPending = { isConnected: true };
  context.activeAgentContext = { sessionId: "session-new" };
  context.activeAgentPollBusy = true;
}
const stale = harness();
const oldPoll = stale.context.pollAgentRun();
replaceRun(stale.context);
stale.statuses[0].resolve({ job_id: "old", state: "running" });
await oldPoll;
assert.equal(stale.context.activeAgentJob, "new", "old status must not clear the replacement run");
assert.equal(stale.context.activeAgentPollBusy, true, "old finally must not release the new poll's busy flag");
assert.equal(stale.warnings.length + stale.controls.length + stale.rendered.length, 0);

const replay = harness();
const replayPoll = replay.context.pollAgentRun();
replay.statuses[0].resolve({ job_id: "old", state: "running" });
await new Promise(setImmediate);
assert.equal(replay.events.length, 1);
replaceRun(replay.context, "old"); // Same ID can be reattached with a new polling owner.
replay.events[0].resolve({ events: [{ text: "old replay" }], history_lost: true });
await replayPoll;
assert.equal(replay.context.activeAgentPollBusy, true);
assert.equal(replay.rendered.length + replay.warnings.length, 0);

const failure = harness();
const failedPoll = failure.context.pollAgentRun();
replaceRun(failure.context);
failure.statuses[0].reject(new Error("previous request failed"));
await failedPoll;
assert.equal(failure.warnings.length, 0, "old errors must not enter the replacement run's log");

const finish = harness();
const animation = pending();
finish.context.finishRunBubble = () => animation.promise;
const finalStatus = { job_id: "old", state: "done", session_id: "session-old", result: { ok: true, message: "old result" } };
const finishing = finish.context.finishAgentJob(finalStatus);
replaceRun(finish.context);
animation.resolve();
await finishing;
assert.equal(finish.context.activeAgentJob, "new", "old animation completion must not reset a new run");
assert.equal(finish.context.activeAgentContext.sessionId, "session-new");
assert.deepEqual(finish.turns, [["session-old", "assistant", "old result"]]);
assert.equal(finish.controls.length + finish.badges.length, 0);

const sessions = harness();
const loading = pending();
sessions.context.loadAgentSessions = () => loading.promise;
const finishingSession = sessions.context.finishAgentJob(finalStatus);
await new Promise(setImmediate);
replaceRun(sessions.context);
loading.resolve();
await finishingSession;
assert.equal(sessions.badges.length, 0, "old session refresh must not replace a new run's badge");

const changed = harness();
const current = changed.context.pollAgentRun();
changed.statuses[0].resolve({ job_id: "different-device-run", state: "running" });
await current;
assert.equal(changed.context.activeAgentJob, null, "a current poll must still detect an actual run change");
assert.equal(changed.warnings.length, 1);

const disposed = harness();
const late = disposed.context.pollAgentRun();
disposed.context.UI.lifecycle.destroyed = true;
disposed.statuses[0].resolve({ job_id: "old", state: "running" });
await late;
assert.equal(disposed.events.length + disposed.rendered.length, 0);
const completed = harness();
await completed.context.finishAgentJob(finalStatus);
assert.equal(completed.context.activeAgentJob, null);
assert.equal(completed.context.activeAgentPending, null);
assert.equal(completed.context.activeAgentPollBusy, false);
assert.deepEqual(completed.controls, [false]);
assert.equal(completed.badges.length, 1);
assert.equal(completed.turns.length, 1);

const attach = harness(), freshBubble = { isConnected: true };
Object.assign(attach.context, {
  agentProfile: { value: "default" }, agentModel: { value: "model" },
  addRunBubble: () => freshBubble, pollAgentRun() {},
});
attach.context.UI.lifecycle.interval = () => 1;
vm.runInNewContext(extract("attachAgentJob"), attach.context);
const existingBubble = attach.context.activeAgentPending;
await attach.context.attachAgentJob({ job_id: "old", state: "running" }, { sessionId: "session-old" });
assert.equal(attach.context.activeAgentPending, existingBubble, "reattaching the same run keeps its bubble");
await attach.context.attachAgentJob({ job_id: "new", state: "running" }, { sessionId: "session-new" });
assert.equal(attach.context.activeAgentPending, freshBubble, "another run must receive its own bubble");
console.log("Workspace run polling ownership tests: PASS");
