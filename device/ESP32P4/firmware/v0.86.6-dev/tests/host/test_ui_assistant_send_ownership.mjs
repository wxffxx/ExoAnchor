import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const source = fs.readFileSync(new URL("../../main/www/assets/ui-shell.js", import.meta.url), "utf8");
function extract(name) {
  let start = source.indexOf(`function ${name}(`);
  assert.ok(start >= 0);
  if (source.slice(start - 6, start) === "async ") start -= 6;
  return source.slice(start, source.indexOf("\n  }", start) + 4);
}
function pending() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
const tick = () => new Promise(setImmediate);
function harness(jobId = "") {
  const calls = [], messages = [], statuses = [], streams = [], timers = [], store = new Map([["ea_agent_session", "session-old"]]);
  let focus = 0;
  const input = { value: "original message", disabled: false, focus: () => { focus += 1; } };
  const button = { disabled: false };
  const context = {
    assistant: { jobId, sessionId: "session-old", open: true, context: null },
    byId: id => id === "eaAssistantInput" ? input : id === "eaAssistantSend" ? button : null,
    UI: { lifecycle: { destroyed: false }, api: {
      ensure: async () => true,
      durablePost: (path, body) => post(path, body, true), post: (path, body) => post(path, body, false),
    } },
    localStorage: { getItem: key => store.get(key) },
    clearTimeout() {}, assistantInvalidateRunPolling() {},
    assistantSetSession(id) { context.assistant.sessionId = id || ""; store.set("ea_agent_session", id || ""); },
    assistantMaterializeSession() { if (!context.assistant.sessionId) context.assistantSetSession("session-new"); return context.assistant.sessionId; },
    assistantAddMessage: (kind, text) => messages.push([kind, text]),
    assistantStatus: value => statuses.push(value), assistantEnsureStream: id => streams.push(id),
    assistantSchedulePoll: delay => timers.push(delay), assistantRenderContext() {},
    assistantRenderHistory() {}, assistantTaskControls() {},
  };
  function post(path, body, durable) {
    const response = pending(); calls.push({ path, body, durable, ...response }); return response.promise;
  }
  const names = ["assistantSend", "assistantResetForDataClear"];
  if (source.includes("function assistantInvalidateSend(")) names.push("assistantInvalidateSend");
  vm.runInNewContext(names.map(extract).join("\n"), context);
  return { context, input, button, calls, messages, statuses, streams, timers, focus: () => focus };
}
function reset(h) {
  h.context.assistantResetForDataClear();
  h.messages.length = 0; h.statuses.length = 0;
  h.input.value = "new draft";
}
const late = harness();
const sending = late.context.assistantSend();
await tick();
assert.equal(late.calls.length, 1);
assert.equal(late.calls[0].durable, true);
reset(late);
late.calls[0].resolve({ accepted: true, job_id: "old-run", session_id: "session-old" });
await sending;
assert.equal(late.context.assistant.jobId, "", "old submission must not restore a cleared conversation");
assert.equal(late.context.assistant.sessionId, "");
assert.equal(late.input.value, "new draft");
assert.equal(late.streams.length + late.statuses.length + late.timers.length + late.focus(), 0);
assert.equal(late.input.disabled, false);

const duplicate = harness(), auth = pending();
duplicate.context.UI.api.ensure = () => auth.promise;
const first = duplicate.context.assistantSend();
const second = duplicate.context.assistantSend();
auth.resolve(true);
await tick();
assert.equal(duplicate.calls.length, 1, "duplicate submit during auth must not issue two POSTs");
duplicate.calls[0].resolve({ accepted: true, job_id: "run", session_id: "session-old" });
await Promise.all([first, second]);
assert.equal(duplicate.context.assistant.jobId, "run");
assert.equal(duplicate.input.disabled || duplicate.button.disabled, false);

const beforeAuth = harness(), delayedAuth = pending();
beforeAuth.context.UI.api.ensure = () => delayedAuth.promise;
const waiting = beforeAuth.context.assistantSend();
reset(beforeAuth);
delayedAuth.resolve(true);
await waiting;
assert.equal(beforeAuth.calls.length, 0, "a reset during auth must invalidate the unsent draft");

const overlap = harness();
const old = overlap.context.assistantSend();
await tick();
reset(overlap);
const replacement = overlap.context.assistantSend();
await tick();
assert.equal(overlap.calls.length, 2);
overlap.calls[0].reject(new Error("old network failure"));
await old;
assert.equal(overlap.input.disabled && overlap.button.disabled, true, "old finally must not release newer send controls");
assert.ok(!overlap.messages.some(([, text]) => text.includes("old network failure")));
overlap.calls[1].resolve({ accepted: true, job_id: "new-run", session_id: "session-new" });
await replacement;
assert.equal(overlap.context.assistant.jobId, "new-run");
assert.equal(overlap.input.disabled || overlap.button.disabled, false);

const steering = harness("active-run");
const steer = steering.context.assistantSend();
await tick();
assert.equal(steering.calls[0].path, "/api/agent/run/steer");
assert.equal(steering.calls[0].body.run_id, "active-run");
reset(steering);
steering.calls[0].resolve({ accepted: true, intent_revision: 2 });
await steer;
assert.equal(steering.messages.length + steering.statuses.length, 0);
assert.equal(steering.input.value, "new draft");

const closed = harness();
const closing = closed.context.assistantSend();
await tick();
closed.context.assistant.open = false;
closed.calls[0].resolve({ accepted: true, job_id: "accepted", session_id: "session-old" });
await closing;
assert.equal(closed.context.assistant.jobId, "accepted", "closing a panel does not cancel an accepted run");
assert.equal(closed.focus(), 0);

const disposed = harness();
const leaving = disposed.context.assistantSend();
await tick();
disposed.context.UI.lifecycle.destroyed = true;
disposed.calls[0].resolve({ accepted: true, job_id: "accepted", session_id: "session-old" });
await leaving;
assert.equal(disposed.context.assistant.jobId, "");
assert.equal(disposed.streams.length + disposed.focus(), 0);

const noAuth = harness();
noAuth.context.UI.api.ensure = async () => false;
await noAuth.context.assistantSend();
assert.equal(noAuth.calls.length, 0);
assert.equal(noAuth.input.disabled || noAuth.button.disabled, false);
assert.equal(noAuth.context.assistant.sendOwner, null);

const currentSteer = harness("current-run");
const acceptedSteer = currentSteer.context.assistantSend();
await tick();
currentSteer.calls[0].resolve({ accepted: true, intent_revision: 3 });
await acceptedSteer;
assert.deepEqual(currentSteer.messages, [["user", "original message"]]);
assert.equal(currentSteer.input.value, "");
assert.equal(currentSteer.context.assistant.jobId, "current-run");
assert.equal(currentSteer.input.disabled || currentSteer.button.disabled, false);
console.log("Assistant submission ownership and duplicate-send tests: PASS");
