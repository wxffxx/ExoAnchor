import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";


class MemoryStorage {
  constructor() { this.values = new Map(); }
  getItem(key) { return this.values.has(key) ? this.values.get(key) : null; }
  setItem(key, value) { this.values.set(key, String(value)); }
  removeItem(key) { this.values.delete(key); }
}

class FakeEventTarget {
  constructor() { this.listeners = new Map(); }
  addEventListener(name, callback) {
    if (!this.listeners.has(name)) this.listeners.set(name, new Set());
    this.listeners.get(name).add(callback);
  }
  dispatchEvent(event) {
    for (const callback of this.listeners.get(event.type) || []) callback(event);
    return true;
  }
}

class FakeCustomEvent {
  constructor(type, options = {}) {
    this.type = type;
    this.detail = options.detail;
  }
}

const windowTarget = new FakeEventTarget();
windowTarget.setInterval = setInterval;
windowTarget.clearInterval = clearInterval;
windowTarget.setTimeout = setTimeout;
windowTarget.clearTimeout = clearTimeout;
windowTarget.crypto = globalThis.crypto;

const documentTarget = new FakeEventTarget();
documentTarget.hidden = false;
documentTarget.getElementById = () => null;

globalThis.window = windowTarget;
globalThis.document = documentTarget;
globalThis.CustomEvent = FakeCustomEvent;
globalThis.sessionStorage = new MemoryStorage();
globalThis.localStorage = new MemoryStorage();
globalThis.location = { pathname: "/", reload() {} };

const source = fs.readFileSync(
  new URL("../../main/www/assets/ui-core.js", import.meta.url),
  "utf8",
);
vm.runInThisContext(source, { filename: "ui-core.js" });

const UI = windowTarget.ExoAnchorUI;
assert.ok(UI?.lifecycle, "shared lifecycle must be exported");

const events = [];
for (const name of ["mount", "navigate", "destroy"]) {
  documentTarget.addEventListener("exoanchor:" + name, event => {
    events.push([name, event.detail]);
  });
}

UI.lifecycle.mount("overview");
assert.equal(events[0][0], "mount");
assert.equal(events[0][1].page, "overview");

let cleanupCount = 0;
UI.lifecycle.addCleanup(reason => {
  cleanupCount += 1;
  assert.equal(reason, "navigate");
});

let timerTicks = 0;

UI.api.token = "test-token";
let sharedFetchCalls = 0;
globalThis.fetch = (_path, options) => {
  sharedFetchCalls += 1;
  return new Promise((_resolve, reject) => {
    options.signal.addEventListener("abort", () => {
      reject(new DOMException("aborted", "AbortError"));
    }, { once: true });
  });
};
const sharedA = UI.api.getShared("/shared-slow", { timeoutMs: 20 });
const sharedB = UI.api.getShared("/shared-slow", { timeoutMs: 20 });
assert.equal(sharedA, sharedB, "identical GETs must share one in-flight request");
await assert.rejects(sharedA, error => error?.name === "TimeoutError");
assert.equal(sharedFetchCalls, 1, "shared GET must issue one network request");

globalThis.fetch = async () => ({
  ok: true,
  status: 200,
  headers: { get: () => "application/json" },
  json: async () => ({ recovered: true }),
});
const recovered = await UI.api.getShared("/shared-slow", { timeoutMs: 20 });
assert.equal(recovered.recovered, true, "timed-out shared GET must be retryable");

let bodyFetchCalls = 0;
globalThis.fetch = async (_path, options) => {
  bodyFetchCalls += 1;
  return {
    ok: true, status: 200,
    headers: { get: () => "application/json" },
    json: () => new Promise((_resolve, reject) => {
      options.signal.addEventListener("abort", () => {
        reject(new DOMException("body aborted", "AbortError"));
      }, { once: true });
    }),
  };
};
await assert.rejects(UI.api.getSilent("/stalled-body", { timeoutMs: 15 }),
  error => error?.name === "TimeoutError");
assert.equal(bodyFetchCalls, 1, "a stalled body must not trigger a replay");

const firstController = new AbortController();
const secondController = new AbortController();
const firstPrivate = UI.api.getShared("/caller-owned", { signal: firstController.signal });
const secondPrivate = UI.api.getShared("/caller-owned", { signal: secondController.signal });
assert.notEqual(firstPrivate, secondPrivate);
await new Promise(resolve => setTimeout(resolve, 0));
firstController.abort();
await assert.rejects(firstPrivate, error => error?.name === "AbortError");
assert.equal(secondController.signal.aborted, false);
secondController.abort();
await assert.rejects(secondPrivate, error => error?.name === "AbortError");

globalThis.fetch = async () => ({
  ok: true, status: 200,
  headers: { get: () => "application/json" },
  json: async () => { throw new SyntaxError("invalid JSON"); },
});
await assert.rejects(UI.api.getSilent("/invalid-json"), SyntaxError);

globalThis.fetch = async () => ({
  ok: true,
  status: 200,
  headers: { get: () => "application/json" },
  json: async () => ({ video: { frame_ready: true } }),
});
await UI.status.refresh();
assert.ok(Number.isFinite(UI.status.lastDurationMs),
  "successful status refresh must expose its round-trip duration");
assert.ok(UI.status.lastDurationMs >= 0,
  "status round-trip duration must not be negative");

let notified = 0;
const brokenListener = UI.status.subscribe(() => { throw new Error("bad widget"); }, false);
const healthyListener = UI.status.subscribe(() => { notified += 1; }, false);
const originalConsoleError = console.error;
try {
  console.error = () => {};
  await UI.status.refresh();
} finally {
  console.error = originalConsoleError;
  brokenListener();
  healthyListener();
}
assert.equal(notified, 1, "one broken widget must not block the remaining listeners");
assert.equal(UI.status.error, null, "widget errors must not become transport failures");

let resolveEvents;
let eventFetchCalls = 0;
globalThis.fetch = async () => {
  eventFetchCalls += 1;
  return {
    ok: true, status: 200,
    headers: { get: () => "application/json" },
    json: () => new Promise(resolve => { resolveEvents = resolve; }),
  };
};
const unsubscribeFirst = UI.actionMirror.subscribe(() => {});
await new Promise(resolve => setTimeout(resolve, 0));
unsubscribeFirst();
const unsubscribeSecond = UI.actionMirror.subscribe(() => {});
assert.equal(eventFetchCalls, 1, "resubscribe must not overlap an active event poll");
resolveEvents({ events: [] });
await new Promise(resolve => setTimeout(resolve, 0));
unsubscribeSecond();

globalThis.fetch = async () => ({
  ok: false,
  status: 503,
  headers: { get: () => "text/plain" },
  text: async () => "temporarily unavailable",
});
await assert.rejects(UI.status.refresh());
assert.equal(UI.status.lastDurationMs, null,
  "failed status refresh must clear stale round-trip duration");

let requestSignal = null;
globalThis.fetch = (_path, options) => {
  requestSignal = options.signal;
  return new Promise((_resolve, reject) => {
    options.signal.addEventListener("abort", () => {
      reject(new DOMException("aborted", "AbortError"));
    }, { once: true });
  });
};

const pending = UI.api.getSilent("/slow");
UI.lifecycle.interval(() => { timerTicks += 1; }, 50);
UI.lifecycle.navigate("/kvm");
await assert.rejects(pending, error => error?.name === "AbortError");
await new Promise(resolve => setTimeout(resolve, 70));

assert.equal(requestSignal.aborted, true, "navigation must abort in-flight requests");
assert.equal(cleanupCount, 1, "cleanup must run exactly once");
assert.equal(timerTicks, 0, "owned intervals must stop on destroy");
assert.deepEqual(events.map(item => item[0]), ["mount", "navigate", "destroy"]);
assert.equal(events[1][1].href, "/kvm");
assert.equal(events[2][1].reason, "navigate");

UI.lifecycle.destroy("duplicate");
assert.equal(cleanupCount, 1, "destroy must be idempotent");

const agentSource = fs.readFileSync(
  new URL("../../main/www/agent.html", import.meta.url),
  "utf8",
);

function agentNamedFunction(name) {
  let start = agentSource.indexOf(`function ${name}(`);
  assert.notEqual(start, -1, `missing Agent function ${name}`);
  if (agentSource.slice(Math.max(0, start - 6), start) === "async ") start -= 6;
  const parameterStart = agentSource.indexOf("(", start);
  let parameterDepth = 0;
  let parameterEnd = -1;
  for (let index = parameterStart; index < agentSource.length; index += 1) {
    if (agentSource[index] === "(") parameterDepth += 1;
    else if (agentSource[index] === ")") {
      parameterDepth -= 1;
      if (parameterDepth === 0) { parameterEnd = index; break; }
    }
  }
  assert.notEqual(parameterEnd, -1, `unterminated Agent parameters ${name}`);
  const brace = agentSource.indexOf("{", parameterEnd);
  let depth = 0;
  let quote = "";
  let escaped = false;
  for (let index = brace; index < agentSource.length; index += 1) {
    const char = agentSource[index];
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
      if (depth === 0) return agentSource.slice(start, index + 1);
    }
  }
  assert.fail(`unterminated Agent function ${name}`);
}

let sessionsGet = async () => ({ sessions: [], supported: true });
const agentContext = {
  API: { get: path => {
    assert.equal(path, "/api/agent/sessions");
    return sessionsGet();
  } },
  chatLog: { dataset: {} },
  renderCount: 0,
  blankCount: 0,
  noticeCount: 0,
};
vm.runInNewContext(`
  let agentSessions=[];
  let agentSessionsLoadError="";
  const provisionalAgentSessions=new Map();
  let activeSessionId="";
  const localStorage={setItem(){},removeItem(){}};
  function renderSessions(){globalThis.renderCount++}
  function renderBlankThread(){globalThis.blankCount++}
  function showAgentHistoryNotice(){globalThis.noticeCount++}
  ${agentNamedFunction("safeSessionId")}
  ${agentNamedFunction("optionalSessionId")}
  ${agentNamedFunction("setActiveSession")}
  ${agentNamedFunction("agentSessionId")}
  ${agentNamedFunction("agentHistoryPersistenceState")}
  ${agentNamedFunction("agentHistoryStateLabel")}
  ${agentNamedFunction("ensureProvisionalAgentSession")}
  ${agentNamedFunction("rememberProvisionalAgentTurn")}
  ${agentNamedFunction("setProvisionalAgentHistoryState")}
  ${agentNamedFunction("projectAgentSessions")}
  ${agentNamedFunction("agentSessionListEmptyState")}
  ${agentNamedFunction("agentHistoryNoticeText")}
  ${agentNamedFunction("agentHistoryRecordRole")}
  ${agentNamedFunction("agentHistoryRecordContent")}
  ${agentNamedFunction("projectAgentHistory")}
  ${agentNamedFunction("loadAgentSessions")}
  globalThis.agentThreadContract={
    reset(active="",sessions=[]){activeSessionId=active;agentSessions=sessions.map(item=>({...item}));agentSessionsLoadError="";provisionalAgentSessions.clear();globalThis.chatLog.dataset={sessionId:active}},
    ensure:ensureProvisionalAgentSession,
    remember:rememberProvisionalAgentTurn,
    setHistory:setProvisionalAgentHistoryState,
    persistence:agentHistoryPersistenceState,
    label:agentHistoryStateLabel,
    notice:agentHistoryNoticeText,
    projectHistory:projectAgentHistory,
    load:loadAgentSessions,
    active:()=>activeSessionId,
    sessions:()=>agentSessions,
    error:()=>agentSessionsLoadError,
    draft:id=>provisionalAgentSessions.get(id),
    emptyState:agentSessionListEmptyState,
  };
`, agentContext);

const threads = agentContext.agentThreadContract;
threads.reset("s_unsaved");
threads.ensure("s_unsaved", "本地对话");
threads.remember("s_unsaved", "user", "你好");
threads.remember("s_unsaved", "assistant", "你好，我在");
threads.setHistory("s_unsaved", "unsaved", "TF queue unavailable");
sessionsGet = async () => ({ sessions: [], supported: true });
await threads.load();
assert.equal(threads.active(), "s_unsaved",
  "an empty sessions snapshot must not clear the active provisional Thread");
assert.equal(threads.sessions().length, 1);
assert.equal(threads.sessions()[0].history_state, "unsaved");
assert.match(threads.notice(threads.draft("s_unsaved")), /未保存/);
assert.equal(threads.projectHistory([], threads.draft("s_unsaved")).length, 2,
  "RAM projection must retain the user and assistant turns after completion");
threads.setHistory("s_unsaved", "saved");
assert.equal(threads.draft("s_unsaved").history_state, "unsaved",
  "a later saved turn must not erase an older unsaved RAM overlay");

for (const failure of [
  "HTTP 401 Unauthorized",
  "HTTP 500 Internal Server Error",
  "NetworkError when attempting to fetch resource",
]) {
  const before = threads.sessions().map(item => item.session_id);
  sessionsGet = async () => { throw new Error(failure); };
  await threads.load();
  assert.deepEqual(Array.from(threads.sessions(), item => item.session_id), before,
    `${failure} must preserve the current sessions projection`);
  assert.equal(threads.active(), "s_unsaved",
    `${failure} must preserve the active Thread`);
  assert.match(threads.error(), new RegExp(failure.split(" ")[0]));
  assert.equal(threads.emptyState().title, "会话列表读取失败",
    `${failure} must render an error state instead of an empty-list state`);
}

threads.reset("s_pending");
threads.ensure("s_pending", "后台保存");
threads.remember("s_pending", "user", "问题");
threads.remember("s_pending", "assistant", "回答");
threads.setHistory("s_pending", "pending");
threads.setHistory("s_pending", "saved");
assert.equal(threads.draft("s_pending").history_state, "pending",
  "a pending overlay must wait for the canonical message count before release");
sessionsGet = async () => ({
  sessions: [{ session_id: "s_pending", title: "后台保存", message_count: 1 }],
  supported: true,
});
await threads.load();
assert.ok(threads.draft("s_pending"),
  "a partially persisted pending Thread must keep its RAM projection");
assert.equal(threads.sessions()[0].history_state, "pending");
assert.match(threads.notice(threads.draft("s_pending")), /保存中/);
sessionsGet = async () => ({
  sessions: [{ session_id: "s_pending", title: "后台保存", message_count: 2 }],
  supported: true,
});
await threads.load();
assert.equal(threads.draft("s_pending"), undefined,
  "a pending Thread may drop its RAM overlay only after the canonical count catches up");
assert.equal(threads.sessions()[0].provisional, undefined);

threads.reset("s_saved");
threads.ensure("s_saved", "已保存对话");
threads.remember("s_saved", "user", "问题");
threads.remember("s_saved", "assistant", "回答");
threads.setHistory("s_saved", "saved");
sessionsGet = async () => ({ sessions: [], supported: true });
await threads.load();
assert.ok(threads.draft("s_saved"),
  "history_saved=true must still tolerate a briefly lagging sessions projection");
assert.equal(threads.active(), "s_saved");
assert.match(threads.notice(threads.draft("s_saved")), /已保存/);
sessionsGet = async () => ({
  sessions: [{ session_id: "s_saved", title: "已保存对话", message_count: 2 }],
  supported: true,
});
await threads.load();
assert.equal(threads.draft("s_saved"), undefined,
  "the RAM overlay must be removed once the saved Thread appears canonically");
assert.equal(threads.sessions()[0].provisional, undefined);

assert.equal(threads.persistence({ history_saved: false, history_pending: true }), "pending",
  "history_pending must take precedence over a timeout's false saved flag");
assert.equal(threads.persistence({ history_saved: false }), "unsaved");
assert.equal(threads.persistence({ history_saved: true }), "saved");

threads.reset("");
sessionsGet = async () => ({
  sessions: [], supported: false, error: "TF card not mounted",
});
await threads.load();
assert.equal(threads.emptyState().title, "会话列表读取失败",
  "a storage-level sessions failure must not look like a valid empty list");
assert.match(threads.error(), /TF card not mounted/);

threads.reset("");
sessionsGet = async () => { throw new Error("HTTP 403 Forbidden"); };
await threads.load();
assert.equal(threads.emptyState().title, "会话列表读取失败",
  "an unauthenticated initial load must not claim that there are no conversations");

console.log("ui lifecycle tests: PASS");
