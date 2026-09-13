import assert from "node:assert/strict";
import fs from "node:fs";
import http from "node:http";
import vm from "node:vm";

const source = fs.readFileSync(new URL("../../main/www/assets/ui-core.js", import.meta.url), "utf8");
const settingsSource = fs.readFileSync(new URL("../../main/www/settings.html", import.meta.url), "utf8");
class Storage {
  constructor() { this.values = new Map(); }
  getItem(key) { return this.values.get(key) ?? null; }
  setItem(key, value) { this.values.set(key, String(value)); }
  removeItem(key) { this.values.delete(key); }
}
class Target {
  constructor() { this.listeners = new Map(); }
  addEventListener(name, callback) {
    if (!this.listeners.has(name)) this.listeners.set(name, new Set());
    this.listeners.get(name).add(callback);
  }
  removeEventListener(name, callback) { this.listeners.get(name)?.delete(callback); }
  dispatchEvent(event) {
    for (const callback of this.listeners.get(event.type) || []) callback(event);
    return true;
  }
}
function browser(overrides = {}) {
  const window = Object.assign(new Target(), { setTimeout, clearTimeout, setInterval, clearInterval });
  const document = Object.assign(new Target(), { hidden: false, getElementById: () => null });
  const context = {
    window, document, localStorage: new Storage(), sessionStorage: new Storage(),
    location: { pathname: "/", reload() {} },
    AbortController, DOMException, performance, console, setTimeout, clearTimeout,
    CustomEvent: class { constructor(type, options = {}) { this.type = type; this.detail = options.detail; } },
    fetch: async () => Response.json({ ok: true }),
    ...overrides,
  };
  vm.runInNewContext(source, context, { filename: "ui-core.js" });
  return { context, UI: window.ExoAnchorUI };
}

// A real HTTP response flushes its headers and never finishes its body. Fetch
// itself has resolved: only retaining the abort scope through parsing can stop it.
let bodyStarted;
const server = http.createServer((request, response) => {
  response.writeHead(request.url === "/error" ? 503 : 200, {
    "Content-Type": request.url === "/json" ? "application/json" : "text/plain",
  });
  response.write(request.url === "/json" ? '{"pending":' : "pending");
  bodyStarted?.();
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
try {
  for (const path of ["/json", "/text", "/error"]) {
    const { UI } = browser({ fetch: (path, options) => fetch(origin + path, options) });
    const responseBodyStarted = new Promise(resolve => { bodyStarted = resolve; });
    const pending = UI.api.getShared(path, { timeoutMs: 250 });
    const failed = assert.rejects(pending, error => error.name === "TimeoutError");
    await responseBodyStarted;
    await failed;
    assert.equal(UI.api.sharedGets.size, 0, `${path}: timed-out body must release shared request`);
  }
  const { UI } = browser({ fetch: (path, options) => fetch(origin + path, options) });
  const responseBodyStarted = new Promise(resolve => { bodyStarted = resolve; });
  const pending = UI.api.getSilent("/json", { timeoutMs: 5000 });
  const failed = assert.rejects(pending, error => error.name === "AbortError");
  await responseBodyStarted;
  // Allow the response headers to reach fetch before aborting the page.
  await new Promise(resolve => setTimeout(resolve, 20));
  UI.lifecycle.destroy("navigate");
  await failed;
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
}

// Idle expiry must revoke the server session using the old bearer plus Cookie,
// lock ordinary and silent calls immediately, and remain usable after navigation.
{
  let resolveLogout;
  const requests = [];
  const { UI, context } = browser({ fetch: (path, options) => {
    requests.push({ path, options });
    return new Promise(resolve => { resolveLogout = resolve; });
  } });
  UI.api.setSession("original-token", "admin");
  UI.session.apply({ auto_logout_enabled: true, auto_logout_minutes: 1 });
  context.localStorage.setItem("si_auth_last_active", Date.now() - 61000);
  const events = [];
  context.document.addEventListener("exoanchor:logout", () => events.push("logout"));
  context.document.addEventListener("exoanchor:session-expired", () => events.push("expired"));
  UI.lifecycle.destroy("navigate");
  UI.session.touch();
  assert.equal(UI.session.active(), false, "first click after expiry must not renew the session");
  assert.equal(UI.api.token, "");
  assert.equal(context.localStorage.getItem("ea_auth_logged_out"), "1");
  assert.deepEqual(events, ["logout", "expired"]);
  assert.equal(requests.length, 1);
  assert.equal(requests[0].path, "/api/auth/logout");
  assert.equal(requests[0].options.headers.Authorization, "Bearer original-token");
  assert.equal(requests[0].options.credentials, "same-origin");
  assert.equal(requests[0].options.keepalive, true);
  assert.equal(requests[0].options.signal.aborted, false, "logout must outlive page destruction");
  await assert.rejects(UI.api.getSilent("/api/status"), /login required/);
  await assert.rejects(UI.api.post("/api/power", {}), /login required/);
  resolveLogout(Response.json({ ok: true }));
  await UI.session.logoutPending;
  assert.equal(UI.session.active(), false, "successful revoke still requires explicit login");
}

// A Cookie-only session has no JS token, but must obey the same idle timer.
{
  const { UI, context } = browser();
  context.localStorage.setItem("ea_auth_confirmed", "1");
  context.localStorage.setItem("si_auth_last_active", Date.now() - 61000);
  UI.session.apply({ auto_logout_enabled: true, auto_logout_minutes: 1 });
  assert.equal(UI.session.active(), false);
  await UI.session.logoutPending;
}

// Polls cannot count as user activity; cross-tab logout also closes local streams.
{
  const { UI, context } = browser();
  UI.api.setSession("poll-token", "admin");
  UI.session.apply({ auto_logout_enabled: true, auto_logout_minutes: 1 });
  const before = String(Date.now() - 10000);
  context.localStorage.setItem("si_auth_last_active", before);
  await UI.status.refresh();
  assert.equal(context.localStorage.getItem("si_auth_last_active"), before);
  UI.session.start();
  let closed = 0;
  context.document.addEventListener("exoanchor:logout", () => closed++);
  context.localStorage.setItem("ea_auth_logout_generation", "peer-logout");
  context.localStorage.setItem("ea_auth_logged_out", "1");
  context.window.dispatchEvent({ type: "storage", key: "ea_auth_logged_out", newValue: "1" });
  await UI.session.logoutPending;
  assert.equal(closed, 1);
  assert.equal(UI.api.token, "");
  UI.lifecycle.destroy("test");
}

// If the device is offline, refreshing or resolving an old auth probe may not
// restore access from its still-valid Cookie. A credential login clears the lock.
{
  const { UI, context } = browser({ fetch: async () => { throw new TypeError("offline"); } });
  UI.api.setSession("offline-token", "admin");
  await assert.rejects(UI.session.logout(), /offline/);
  const { UI: refreshed } = browser({ localStorage: context.localStorage });
  let loginPrompts = 0;
  refreshed.auth.readAuthState = async () => ({ enabled: true, token_valid: true });
  refreshed.auth.requireLogin = async () => { loginPrompts++; return false; };
  assert.equal(await refreshed.auth.checkRequired(), false);
  assert.equal(loginPrompts, 1);
  await refreshed.session.logoutPending;
  assert.equal(refreshed.session.active(), false);
  refreshed.api.setSession("new-token", "admin");
  assert.equal(refreshed.session.active(), true);
  assert.equal(context.localStorage.getItem("ea_auth_logged_out"), null);
}
{
  const { UI } = browser();
  let resolveProbe;
  UI.auth.readAuthState = () => new Promise(resolve => { resolveProbe = resolve; });
  UI.auth.requireLogin = async () => false;
  const checking = UI.auth.checkRequired();
  await UI.session.logout();
  resolveProbe({ enabled: true, token_valid: true });
  assert.equal(await checking, false);
  assert.equal(UI.session.active(), false);
  await UI.session.logoutPending;
}

// Logout can also arrive inside navigation-auth's anonymous-response retry.
{
  const { UI, context } = browser();
  UI.api.setSession("retry-token", "admin");
  let calls = 0;
  UI.auth.readAuthState = async () => {
    if (++calls === 1) return { enabled: true, token_valid: false };
    await UI.session.logout();
    return { enabled: true, token_valid: true };
  };
  UI.auth.requireLogin = async () => false;
  assert.equal(await UI.auth.checkRequired(), false);
  assert.equal(context.localStorage.getItem("ea_auth_confirmed"), null);
}

// The shared token can change while an older tab remains open.
{
  let revokedToken;
  const { UI, context } = browser({ fetch: async (_path, options) => {
    revokedToken = options.headers.Authorization;
    return Response.json({ ok: true });
  } });
  UI.api.setSession("old-tab-token", "admin");
  context.localStorage.setItem("ea_auth_token", "current-cookie-token");
  await UI.session.logout();
  assert.equal(revokedToken, "Bearer current-cookie-token");
}

// A delayed logout notification cannot revoke a later successful login.
{
  let networkCalls = 0;
  const { UI, context } = browser({ fetch: async () => {
    networkCalls++;
    return Response.json({ ok: true });
  } });
  UI.session.start();
  await UI.session.logout();
  UI.api.setSession("new-session", "admin");
  context.window.dispatchEvent({ type: "storage", key: "ea_auth_logged_out", newValue: "1" });
  assert.equal(UI.api.token, "new-session");
  assert.equal(UI.session.active(), true);
  assert.equal(networkCalls, 1);
  UI.lifecycle.destroy("test");
}

// Run the real modal submission plus Agent preview start/stop and its actual
// authentication callback, rather than only testing ApiClient.setSession().
const agentSource = fs.readFileSync(new URL("../../main/www/agent.html", import.meta.url), "utf8");
for (const trigger of ["idle", "peer"]) {
  const requests = [];
  const { UI, context } = browser({ fetch: async (path, options) => {
    requests.push({ path, options });
    if (path === "/api/auth/login") return Response.json({ token: "relogin-token", username: "admin" });
    if (path === "/api/settings/session") return Response.json({ auto_logout_enabled: true, auto_logout_minutes: 1 });
    return Response.json({ ok: true });
  } });
  const elements = new Map();
  context.document.getElementById = id => {
    if (!elements.has(id)) {
      const classes = new Set();
      const attributes = new Map();
      elements.set(id, {
        value: "", textContent: "", style: {}, focus() {},
        classList: { add: name => classes.add(name), remove: name => classes.delete(name), contains: name => classes.has(name) },
        hasAttribute: name => attributes.has(name),
        removeAttribute(name) { attributes.delete(name); if (name === "src") this.src = ""; },
      });
    }
    return elements.get(id);
  };
  context.document.body = { insertAdjacentHTML() {} };
  UI.api.setSession("live-token", "admin");
  UI.session.apply({ auto_logout_enabled: true, auto_logout_minutes: 1 });
  UI.session.start();
  const preview = context.document.getElementById("preview");
  preview.src = "/previous-stream";
  const page = {
    UI, API: UI.api, AuthUI: UI.auth, Session: UI.session,
    document: context.document, fetch: context.fetch, setTimeout, clearTimeout,
    workspaceState: { ready: true, booted: true },
    activeLeftView: "screen", previewEnabled: true, preview,
    agentPageLeaving: false, agentStreamActive: true, streamTimer: 0,
    agentStreamRequest: 0, agentDocumentReady: Promise.resolve(),
    videoText: { textContent: "streaming" }, streamUrl: () => "/restarted-stream",
    loadAgentToolSettings: async () => {}, loadWorkspaceData: async () => {},
  };
  const callbackStart = agentSource.indexOf("AuthUI.bindForm(async()=>{");
  const callbackEnd = agentSource.indexOf("\n});", callbackStart) + 4;
  const declarations = ["sendAgentVideoLease", "startAgentStream", "stopAgentStream"].map(name =>
    agentSource.split("\n").find(line => line.startsWith(`function ${name}(`) || line.startsWith(`async function ${name}(`)));
  const logoutListener = agentSource.split("\n").find(line => line.startsWith('document.addEventListener("exoanchor:logout"'));
  vm.runInNewContext([...declarations, agentSource.slice(callbackStart, callbackEnd), logoutListener].join("\n"), page);
  if (trigger === "idle") {
    context.localStorage.setItem("si_auth_last_active", Date.now() - 61000);
    UI.session.active();
    await UI.session.logoutPending;
  } else {
    context.localStorage.setItem("ea_auth_logout_generation", "live-peer-logout");
    context.localStorage.setItem("ea_auth_logged_out", "1");
    context.window.dispatchEvent({ type: "storage", key: "ea_auth_logged_out", newValue: "1" });
  }
  assert.equal(preview.src, "", `${trigger}: logout closes preview`);
  assert.equal(page.agentPageLeaving, false, `${trigger}: logout is not page destruction`);
  assert.equal(UI.auth.pending, null, `${trigger}: unsolicited modal has no fake waiting caller`);
  assert.ok(requests.some(({ path, options }) => path === "/api/video/lease" && JSON.parse(options.body).active === false),
    `${trigger}: Agent stream cleanup still sends its lease release`);
  // KVM uses the shared request client for its lifecycle-independent release.
  await UI.api.request("POST", "/api/video/lease", { owner: "kvm", active: false }, false,
    { timeoutMs: 3000, signal: null, keepalive: true });
  await assert.rejects(UI.api.request("POST", "/api/video/lease", { owner: "kvm", active: true }, false), /login required/);
  context.document.getElementById("authCurrentUsername").value = "admin";
  context.document.getElementById("authCurrentPassword").value = "password";
  await context.document.getElementById("authForm").onsubmit({ preventDefault() {} });
  await new Promise(resolve => setTimeout(resolve, 20));
  assert.equal(UI.api.token, "relogin-token");
  assert.equal(UI.auth.pending, null);
  assert.equal(preview.src, "/restarted-stream", `${trigger}: modal success invokes actual Agent page reconnect`);
  assert.equal(UI.lifecycle.destroyed, false);
  UI.lifecycle.destroy("test");
}

// A new logout wins over an earlier login whose body has not completed yet.
for (const trigger of ["local", "undelivered-peer-event"]) {
  let completeBody;
  let bodyStarted;
  const started = new Promise(resolve => { bodyStarted = resolve; });
  const { UI, context } = browser({ fetch: async path => {
    if (path === "/api/auth/logout") return Response.json({ ok: true });
    return { ok: true, status: 200, json: () => new Promise(resolve => {
      completeBody = resolve;
      bodyStarted();
    }) };
  } });
  const login = UI.api.login("admin", "password");
  const rejected = assert.rejects(login, error => error.name === "AbortError");
  await started;
  if (trigger === "local") await UI.session.logout();
  else {
    context.localStorage.setItem("ea_auth_logout_generation", "newer-peer-logout");
    context.localStorage.setItem("ea_auth_logged_out", "1");
  }
  completeBody({ token: "obsolete-login", username: "admin" });
  await rejected;
  assert.equal(UI.api.token, "");
  assert.equal(UI.session.active(), false);
}

// Advance a virtual wall clock without waiting 30+ seconds in the host suite.
// The polling response becomes ready only beyond the old 30-second cutoff.
{
  let now = 100000;
  let polls = 0;
  let deadlineCallback;
  let deadlineCleared = false;
  const { UI } = browser({
    Date: class extends Date { static now() { return now; } },
    setTimeout(callback, delay) {
      if (delay >= 30000) { deadlineCallback = callback; return 1; }
      now += delay;
      queueMicrotask(callback);
      return 2;
    },
    clearTimeout(id) { if (id === 1) deadlineCleared = true; },
    fetch: async path => {
      if (path.includes("/status?")) polls++;
      return Response.json(now < 134000 ? { job_id: "job", poll_after_ms: 1000 } :
        { token: "slow-login-token", username: "admin" }, { status: now < 134000 ? 202 : 200 });
    },
  });
  assert.equal((await UI.api.login("admin", "password")).token, "slow-login-token");
  assert.ok(polls > 30, "a slow successful login must survive the old deadline");
  assert.equal(typeof deadlineCallback, "function");
  assert.equal(deadlineCleared, true);
}
{
  let fireDeadline;
  const { UI } = browser({
    setTimeout(callback) { fireDeadline = callback; return 1; },
    clearTimeout() {},
    fetch: async (_path, options) => ({
      ok: true, status: 200,
      json: () => new Promise((_resolve, reject) => {
        options.signal.addEventListener("abort", () => reject(new DOMException("aborted", "AbortError")));
        queueMicrotask(() => fireDeadline());
      }),
    }),
  });
  await assert.rejects(UI.api.login("admin", "password"), error => error.name === "TimeoutError");
  assert.equal(UI.api.token, "", "timed-out login body must not create a local session");
}

// Settings labels, usernames and diagnostic values are text, while the static
// row structure and diagnostic state indicator remain rendered markup.
{
  const { UI } = browser();
  const context = { UI };
  const functions = ["row", "diagnosticRow", "esc"].map(name => {
    const line = settingsSource.split("\n").find(line => line.startsWith(`function ${name}(`));
    assert.ok(line, `missing settings ${name}`);
    return line;
  });
  vm.runInNewContext(functions.join("\n"), context);
  for (const render of [UI.row, context.row, context.diagnosticRow]) {
    const output = render('<img src=x onerror="alert(1)">', '<script>alert("x")</script>&\'');
    assert.ok(!output.includes("<img"));
    assert.ok(!output.includes("<script"));
    assert.match(output, /&lt;img/);
    assert.match(output, /&lt;script&gt;/);
    assert.match(output, /&amp;&#39;/);
  }
  assert.match(context.diagnosticRow("状态", "可用", true), /diagnostic-dot ok/);
  assert.match(UI.row("计数", 0), /<span>0<\/span>/);
  assert.match(UI.row("空", null), /<span><\/span>/);
}
console.log("UI auth, timeout and settings text runtime tests: PASS");
