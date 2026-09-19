import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const source = fs.readFileSync(new URL("../../main/www/assets/ui-shell.js", import.meta.url), "utf8");
function extract(name) {
  let start = source.indexOf(`function ${name}(`);
  assert.ok(start >= 0);
  if (source.slice(start - 6, start) === "async ") start -= 6;
  const end = source.indexOf("\n  }", start) + 4;
  return source.slice(start, end);
}
function pending() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
const tick = () => new Promise(setImmediate);
function harness() {
  const statuses = [], updates = [], messages = [], renderedEvents = [], timers = new Map(), stored = new Map();
  let timerId = 0;
  const context = {
    assistant: { open: true, jobId: "old", sessionId: "old-session", polling: false, pollTimer: 0, lastEventSeq: 10 },
    document: { hidden: false },
    byId: () => null,
    localStorage: { setItem: (key, value) => stored.set(key, value) },
    clearTimeout: id => timers.delete(id), setTimeout: fn => { timers.set(++timerId, fn); return timerId; },
    UI: { lifecycle: { destroyed: false,
      clearTimer: id => timers.delete(id), timeout: fn => { timers.set(++timerId, fn); return timerId; },
    }, api: { getShared(path) {
      if (path.includes("/events?")) return context.getEvents();
      const request = pending(); statuses.push(request); return request.promise;
    } } },
    getEvents: async () => ({ events: [] }),
    assistantSyncRequests: async () => {},
    assistantSetSession: id => { context.assistant.sessionId = id; },
    assistantUpdateStream: status => updates.push(status.job_id),
    assistantStatus: text => messages.push(text), assistantTaskControls() {},
    assistantAddMessage: (_kind, text) => messages.push(text),
    assistantAppendEvent: event => renderedEvents.push(event),
    assistantFinishStream: async () => {},
    assistantRenderHistory() {}, assistantRenderContext() {},
  };
  const names = ["assistantReadRunStatus", "assistantPullEvents", "assistantPollRun", "assistantSyncRun", "assistantSchedulePoll", "assistantResetForDataClear"];
  if (source.includes("function assistantInvalidateRunPolling(")) names.push("assistantInvalidateRunPolling");
  if (source.includes("function assistantInvalidateSend(")) names.push("assistantInvalidateSend");
  vm.runInNewContext(names.map(extract).join("\n"), context);
  return { context, statuses, updates, messages, renderedEvents, timers, stored };
}
function replace(h) {
  h.context.assistantResetForDataClear();
  h.context.assistant.jobId = "new";
  h.context.assistant.sessionId = "new-session";
  h.updates.length = 0; h.messages.length = 0;
}
const active = job => ({ job_id: job, state: "running", busy: true, session_id: job + "-session" });
const stale = harness();
const old = stale.context.assistantPollRun();
replace(stale);
const fresh = stale.context.assistantPollRun();
stale.statuses[0].resolve(active("old"));
await old;
assert.equal(stale.context.assistant.jobId, "new", "reset must invalidate earlier status responses");
assert.equal(stale.context.assistant.polling, true, "old finally must not release a newer poll");
assert.deepEqual(stale.updates, []);
stale.statuses[1].resolve(active("new"));
await fresh;
assert.deepEqual(stale.updates, ["new"]);
assert.equal(stale.context.assistant.polling, false);
assert.equal(stale.timers.size, 1);
stale.context.assistantResetForDataClear();
assert.equal(stale.timers.size, 0, "reset must release the scheduled poll timer");

const replay = harness(), eventReply = pending();
replay.context.getEvents = () => eventReply.promise;
const eventPoll = replay.context.assistantPollRun();
replay.statuses[0].resolve(active("old"));
await tick();
replace(replay);
replay.context.assistant.jobId = "old"; // Returning to the same ID still has a different polling owner.
eventReply.resolve({ events: [{ text: "old event" }], next_after_seq: 99 });
await eventPoll;
assert.equal(replay.context.assistant.lastEventSeq, 0, "stale replay must not advance the new run's cursor");
assert.deepEqual(replay.renderedEvents, []);
assert.equal(replay.timers.size, 0);

const finishing = harness(), animation = pending();
finishing.context.assistantFinishStream = () => animation.promise;
const finalPoll = finishing.context.assistantPollRun();
finishing.statuses[0].resolve({ job_id: "old", state: "done", result: { ok: true } });
await tick();
replace(finishing);
animation.resolve();
await finalPoll;
assert.equal(finishing.context.assistant.jobId, "new");
assert.equal(finishing.stored.size, 0, "old completion must not record a replacement job as completed");
assert.equal(finishing.messages.length, 0);

const sync = harness();
const olderSync = sync.context.assistantSyncRun();
const newerSync = sync.context.assistantSyncRun();
sync.statuses[1].resolve(active("new"));
await newerSync;
sync.statuses[0].resolve(active("old"));
await olderSync;
assert.equal(sync.context.assistant.jobId, "new");
assert.deepEqual(sync.updates, ["new"]);

const destroyed = harness();
const cancelled = destroyed.context.assistantPollRun();
destroyed.context.UI.lifecycle.destroyed = true;
destroyed.statuses[0].reject(new Error("late offline"));
await cancelled;
assert.equal(destroyed.messages.length, 0);
destroyed.context.assistantSchedulePoll(100);
assert.equal(destroyed.timers.size, 0);

const done = harness();
const completed = done.context.assistantSyncRun();
done.statuses[0].resolve({ job_id: "old", state: "done", result: { ok: true } });
await completed;
assert.equal(done.context.assistant.jobId, "");
assert.equal(done.stored.get("ea_assistant_last_job"), "old");

const closed = harness();
closed.context.assistant.open = false;
await closed.context.assistantPollRun();
assert.equal(closed.statuses.length, 0);
const initialSync = closed.context.assistantSyncRun();
closed.statuses[0].resolve(active("old"));
await initialSync;
assert.deepEqual(closed.updates, ["old"], "initial background sync still updates the launcher's run state");
assert.equal(closed.timers.size, 0);

const closing = harness();
closing.context.agentRuntimeEnabled = true;
closing.context.byId = () => null;
vm.runInNewContext(extract("assistantOpen"), closing.context);
const beforeClose = closing.context.assistantPollRun();
closing.context.assistantOpen(false);
closing.statuses[0].resolve(active("old"));
await beforeClose;
assert.equal(closing.updates.length + closing.messages.length + closing.timers.size, 0);
console.log("Assistant polling ownership and timer cleanup tests: PASS");
