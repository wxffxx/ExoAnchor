import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";
import { webcrypto } from "node:crypto";

class Storage {
  values = new Map();
  getItem(key) { return this.values.get(key) ?? null; }
  setItem(key, value) { this.values.set(key, String(value)); }
  removeItem(key) { this.values.delete(key); }
}

function browser(profile, { loggedOut = false, ticket = { nonce: "test-ticket" }, delayLogout = false } = {}) {
  const elements = new Map();
  const element = id => {
    if (!elements.has(id)) {
      const classes = new Set();
      elements.set(id, {
        value: "", style: {}, textContent: "", focus() {},
        classList: { add: x => classes.add(x), remove: x => classes.delete(x), contains: x => classes.has(x) },
      });
    }
    return elements.get(id);
  };
  const events = new EventTarget();
  const document = {
    body: { insertAdjacentHTML() {} }, hidden: false,
    getElementById: element,
    addEventListener: events.addEventListener.bind(events),
    removeEventListener: events.removeEventListener.bind(events),
    dispatchEvent: events.dispatchEvent.bind(events),
  };
  const window = new EventTarget();
  Object.assign(window, { setTimeout, clearTimeout, setInterval, clearInterval });
  const calls = [];
  const response = (status, value) => ({
    status, ok: status < 300, headers: { get: () => "application/json" },
    json: async () => value, text: async () => JSON.stringify(value),
  });
  const context = vm.createContext({
    window, document, localStorage: new Storage(), sessionStorage: new Storage(),
    location: { pathname: "/", reload() {} }, crypto: webcrypto,
    AbortController, DOMException, CustomEvent, setTimeout, clearTimeout, setInterval, clearInterval,
    fetch: async (path, options = {}) => {
      calls.push({ path, options });
      if (path === "/api/auth/setup" && options.method !== "POST") return response(200, ticket);
      if (path === "/api/auth/logout") {
        if (delayLogout) await new Promise(resolve => { finishLogout = resolve; });
        return response(200, { ok: true });
      }
      if (path === "/api/auth/setup") return response(202, { job_id: "test-job", poll_after_ms: 1 });
      if (path.startsWith("/api/auth/login/status")) return response(200, { token: "test-session", username: "owner" });
      throw new Error("unexpected request: " + path);
    },
  });
  let finishLogout;
  if (loggedOut) context.localStorage.setItem("ea_auth_logged_out", "1");
  const source = fs.readFileSync(new URL(`../${profile}/main/www/assets/ui-core.js`, import.meta.url), "utf8");
  vm.runInContext(source, context);
  const ui = window.ExoAnchorUI;
  ui.lifecycle?.mount("overview");
  return { ui, context, calls, element, finishLogout: () => finishLogout?.() };
}

for (const profile of ["v0.86.6-dev", "v0.86-stable-kvm"]) {
  const { ui, calls, element } = browser(profile);
  const result = await ui.api.setup("owner", "new-browser-password");
  assert.equal(result.token, "test-session");
  assert.equal(calls.length, 3);
  assert.equal(calls[1].path, "/api/auth/setup");
  assert.equal(calls[1].options.headers["X-ExoAnchor-Setup"], "test-ticket");
  assert.deepEqual(JSON.parse(calls[1].options.body).password, "new-browser-password");
  assert.ok(calls.every(call => call.path !== "/api/auth/login"), "setup must not require an initial password login");

  ui.auth.bindForm();
  ui.auth.show("setup", {});
  assert.equal(element("authCurrentFields").style.display, "none");
  assert.equal(element("authNewFields").style.display, "block");
  assert.equal(element("authTitle").textContent, "创建管理员账号");
  let created;
  ui.api.setup = async (username, password) => { created = { username, password }; return result; };
  ui.api.login = async () => { throw new Error("setup asked for an old password"); };
  ui.auth.finishAuthentication = () => {};
  element("authNewUsername").value = "owner";
  element("authNewPassword").value = "new-browser-password";
  element("authConfirmPassword").value = "new-browser-password";
  await element("authForm").onsubmit({ preventDefault() {} });
  assert.deepEqual(created, { username: "owner", password: "new-browser-password" });
  ui.lifecycle?.destroy("test-finished");
  console.log(profile + " browser first-account creation: PASS");

  // After logout and a factory reset, the same browser still holds its local
  // logout marker. Public setup must work without reopening protected APIs.
  const wasLoggedOut = browser(profile, { loggedOut: true, delayLogout: profile === "v0.86.6-dev" });
  const pending = wasLoggedOut.ui.api.setup("owner", "new-browser-password");
  if (profile === "v0.86.6-dev") {
    // Allow the ticket GET to settle and the old Cookie revocation to begin.
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(wasLoggedOut.calls.map(call => call.path), ["/api/auth/setup", "/api/auth/logout"]);
    assert.equal(wasLoggedOut.calls[0].options.method, "GET");
    assert.equal(wasLoggedOut.context.localStorage.getItem("ea_auth_logged_out"), "1",
      "obtaining a public setup ticket must not unlock the logged-out browser");
    wasLoggedOut.finishLogout();
  }
  assert.equal((await pending).token, "test-session");
  const setupPost = wasLoggedOut.calls.find(call => call.path === "/api/auth/setup" && call.options.method === "POST");
  assert.equal(setupPost.options.headers["X-ExoAnchor-Setup"], "test-ticket");
  if (profile === "v0.86.6-dev") {
    assert.equal(wasLoggedOut.context.localStorage.getItem("ea_auth_logged_out"), null,
      "only successful account creation may unlock the browser");
  }
  wasLoggedOut.ui.lifecycle?.destroy("test-finished");
  console.log(profile + " first-account creation after previous logout: PASS");
}

{
  const { ui, context, calls } = browser("v0.86.6-dev", { loggedOut: true, ticket: {} });
  await assert.rejects(ui.api.setup("owner", "new-browser-password"), /无法开始首次配置/);
  assert.equal(context.localStorage.getItem("ea_auth_logged_out"), "1",
    "a missing setup ticket must preserve the logout lock");
  assert.equal(calls.length, 1, "ticket failure must not revoke or submit credentials");
  await assert.rejects(ui.api.getSilent("/api/settings/session"), /login required/);
  await assert.rejects(ui.api.request("POST", "/api/settings/account", {}, false), /login required/);
  await assert.rejects(ui.api.request("POST", "/api/auth/setup", {}, false), /login required/);
  assert.equal(calls.length, 1,
    "the anonymous exception must cover only the setup-ticket GET");
  ui.lifecycle.destroy("test-finished");
  console.log("v0.86.6-dev failed setup preserves protected request lock: PASS");
}
