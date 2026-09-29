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

for (const profile of ["v0.86.6-dev", "v0.86-stable-kvm"]) {
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
      if (path === "/api/auth/setup" && options.method !== "POST") return response(200, { nonce: "test-ticket" });
      if (path === "/api/auth/setup") return response(202, { job_id: "test-job", poll_after_ms: 1 });
      if (path.startsWith("/api/auth/login/status")) return response(200, { token: "test-session", username: "owner" });
      throw new Error("unexpected request: " + path);
    },
  });
  const source = fs.readFileSync(new URL(`../${profile}/main/www/assets/ui-core.js`, import.meta.url), "utf8");
  vm.runInContext(source, context);
  const ui = window.ExoAnchorUI;
  ui.lifecycle?.mount("overview");
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
}
