import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const www = new URL("../../main/www/", import.meta.url);
const coreSource = fs.readFileSync(new URL("assets/ui-core.js", www), "utf8");
const shellSource = fs.readFileSync(new URL("assets/ui-shell.js", www), "utf8");
const settingsSource = fs.readFileSync(new URL("settings.html", www), "utf8");

// Execute the production registrations rather than reproducing their behavior.
const shellStart = shellSource.indexOf("  function bindShell() {");
const shellEnd = shellSource.indexOf("\n  const assistant =", shellStart);
assert.ok(shellStart >= 0 && shellEnd > shellStart, "shell binding must be present");
const dirtyStart = settingsSource.indexOf("const SettingsDirty={");
const dirtyEnd = settingsSource.indexOf("\nSettingsDirty.register(", dirtyStart);
const dirtyEventsStart = settingsSource.indexOf('document.addEventListener("input",event=>SettingsDirty');
const dirtyEventsEnd = settingsSource.indexOf("\n", dirtyEventsStart);
assert.ok(dirtyStart >= 0 && dirtyEnd > dirtyStart, "dirty tracking must be present");
assert.ok(dirtyEventsStart >= 0 && dirtyEventsEnd > dirtyEventsStart,
  "settings input and leave-warning registrations must be present");

class EventTarget {
  constructor() { this.listeners = new Map(); }
  addEventListener(type, callback, options = {}) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push({
      callback,
      capture: options === true || !!options.capture,
      once: !!options.once,
    });
  }
  dispatchEvent(event) {
    if (!event.preventDefault) event.preventDefault = () => { event.defaultPrevented = true; };
    const listeners = this.listeners.get(event.type) || [];
    for (const entry of [...listeners].sort((a, b) => Number(b.capture) - Number(a.capture))) {
      if (entry.once) listeners.splice(listeners.indexOf(entry), 1);
      entry.callback(event);
    }
    return !event.defaultPrevented;
  }
}

class Element {
  constructor() {
    this.attributes = new Map();
    const classes = new Set();
    this.classList = {
      contains: name => classes.has(name),
      add: name => classes.add(name),
      remove: name => classes.delete(name),
      toggle(name, force = !classes.has(name)) {
        if (force) classes.add(name);
        else classes.delete(name);
      },
    };
    this.dataset = {};
    this.type = "text";
    this.value = "original";
    this.disabled = false;
  }
  setAttribute(name, value) { this.attributes.set(name, String(value)); }
  getAttribute(name) { return this.attributes.get(name) ?? null; }
  hasAttribute(name) { return this.attributes.has(name); }
  focus() {}
}

function harness() {
  const window = new EventTarget();
  const document = new EventTarget();
  const timers = new Map();
  let nextTimer = 0;
  const setTimer = (callback, kind) => {
    const id = ++nextTimer;
    timers.set(id, { callback, kind });
    return id;
  };
  Object.assign(window, {
    setInterval: callback => setTimer(callback, "interval"),
    setTimeout: callback => setTimer(callback, "timeout"),
    clearInterval: id => timers.delete(id),
    clearTimeout: id => timers.delete(id),
  });
  const elements = new Map();
  for (const id of ["badgeVideo", "badgePwr", "videoPowerToggle", "powerActionPower",
    "powerActionReset", "logoutNav", "eaAutomationStop", "shellLogoutCancel",
    "shellLogoutConfirm", "shellLogoutModal"]) elements.set(id, new Element());
  document.getElementById = id => elements.get(id) || null;
  document.hidden = false;
  document.body = { insertAdjacentHTML() {
    for (const id of ["uiConfirmModal", "uiConfirmTitle", "uiConfirmMessage",
      "uiConfirmCancel", "uiConfirmSubmit"]) elements.set(id, new Element());
  } };
  const values = new Map();
  const storage = {
    getItem: key => values.get(key) ?? null,
    setItem: (key, value) => values.set(key, String(value)),
    removeItem: key => values.delete(key),
  };
  const requests = [];
  let reloads = 0;
  const context = vm.createContext({
    window, document, localStorage: storage, sessionStorage: storage,
    location: { href: "https://device.test/settings", pathname: "/settings",
      reload() { reloads += 1; } },
    AbortController, DOMException, performance, console,
    setTimeout: window.setTimeout, clearTimeout: window.clearTimeout,
    CustomEvent: class {
      constructor(type, options) { this.type = type; this.detail = options.detail; }
    },
    fetch: async (path, options) => {
      requests.push({ path, options });
      if (options.signal?.aborted) throw new DOMException("aborted", "AbortError");
      return { ok: true, status: 200, headers: { get: () => "application/json" },
        json: async () => ({ saved: true }) };
    },
    toggleStatusMenu() {}, ms2109PowerAction() {}, powerAction() {},
    showLogout() {}, stopPrimaryAutomation() {}, hideLogout() {},
    performLogout() {}, closeStatusMenus() {},
  });
  vm.runInContext(coreSource, context, { filename: "ui-core.js" });
  const UI = window.ExoAnchorUI;
  UI.lifecycle.mount("settings");
  UI.api.setSession("test-token", "tester");
  context.UI = UI;
  context.byId = document.getElementById;
  vm.runInContext(shellSource.slice(shellStart, shellEnd) + "\nbindShell();", context,
    { filename: "ui-shell.js:bindShell" });
  vm.runInContext(settingsSource.slice(dirtyStart, dirtyEnd) +
    "\nglobalThis.dirty = SettingsDirty;\n" +
    settingsSource.slice(dirtyEventsStart, dirtyEventsEnd), context,
    { filename: "settings.html:dirty-tracking" });
  const field = new Element();
  const group = new Element();
  group.querySelectorAll = () => [field];
  group.contains = candidate => candidate === field;
  context.dirty.register("device", group);
  context.dirty.capture("device");
  field.value = "unsaved edit";
  document.dispatchEvent({ type: "input", target: field });
  assert.equal(context.dirty.any(), true, "production dirty tracking must observe the edit");

  const link = new Element();
  link.href = "https://device.test/kvm";
  link.setAttribute("href", "/kvm");
  link.closest = selector => selector === ".ea-shell .nav a" ? link : null;
  return {
    UI, window, document, context, requests, timers, elements, link,
    reloads: () => reloads,
    click(options = {}) {
      const event = { type: "click", target: link, button: 0, ...options };
      document.dispatchEvent(event);
      return event;
    },
    leaveAttempt() {
      const event = { type: "beforeunload" };
      window.dispatchEvent(event);
      return event;
    },
    tickIntervals() {
      for (const timer of [...timers.values()]) {
        if (timer.kind === "interval") timer.callback();
      }
    },
  };
}

function pendingRequest(h) {
  let complete;
  let signal;
  const regularFetch = h.context.fetch;
  h.context.fetch = (path, options) => {
    if (path !== "/pending") return regularFetch(path, options);
    signal = options.signal;
    return new Promise((resolve, reject) => {
      complete = () => resolve({ ok: true, status: 200,
        headers: { get: () => "application/json" }, json: async () => ({ completed: true }) });
      if (signal.aborted) reject(new DOMException("aborted", "AbortError"));
      else signal.addEventListener("abort", () => reject(new DOMException("aborted", "AbortError")),
        { once: true });
    });
  };
  const promise = h.UI.api.getSilent("/pending");
  // Keep an expected old-code abort from becoming an unhandled rejection.
  promise.catch(() => {});
  return { promise, complete: () => complete(), signal: () => signal };
}

for (const mode of ["reload", "sidebar"]) {
  const h = harness();
  const pending = pendingRequest(h);
  let cleanups = 0;
  let polls = 0;
  h.UI.lifecycle.addCleanup(() => { cleanups += 1; });
  h.UI.status.subscribe(() => { polls += 1; }, false);
  h.UI.status.start();
  await h.UI.status.inFlight;
  if (mode === "sidebar") {
    assert.ok(!h.click().defaultPrevented, "enabled links retain native browser navigation");
  }
  const attempt = h.leaveAttempt();
  assert.equal(attempt.defaultPrevented, true, "unsaved edits must request the browser leave warning");
  assert.equal(attempt.returnValue, "");
  // Choosing Stay causes no pagehide event. Repeated attempts must stay usable.
  h.leaveAttempt();
  assert.equal(h.UI.lifecycle.destroyed, false, `${mode}: canceled navigation must preserve the page`);
  assert.equal(pending.signal().aborted, false, `${mode}: pending requests must survive Stay`);
  assert.equal(cleanups, 0, `${mode}: Stay must not release active resources`);
  assert.equal(h.link.getAttribute("aria-busy"), null, "canceled links must not remain busy");
  h.tickIntervals();
  await h.UI.status.inFlight;
  assert.equal(polls, 2, `${mode}: polling must continue after Stay`);
  const saved = await h.UI.api.post("/api/settings/device", { name: "edited name" });
  assert.equal(saved.saved, true, `${mode}: saving must remain possible after Stay`);
  assert.equal(h.context.dirty.any(), true, "a canceled leave must preserve dirty state");
  const confirmation = h.UI.confirmAction({ message: "Still available after Stay" });
  assert.equal(h.elements.get("uiConfirmModal")?.classList.contains("show"), true);
  h.elements.get("uiConfirmSubmit").onclick();
  assert.equal(await confirmation, true, `${mode}: confirmation UI must remain usable`);
  pending.complete();
  assert.equal((await pending.promise).completed, true);
  h.context.dirty.capture("device");
  assert.ok(!h.leaveAttempt().defaultPrevented, "saved fields should no longer request a leave warning");
  h.window.dispatchEvent({ type: "pagehide", persisted: false });
  assert.equal(cleanups, 1, "later accepted navigation must still release resources");
}

for (const persisted of [false, true]) {
  const h = harness();
  const pending = pendingRequest(h);
  let cleanups = 0;
  let destroyDetail;
  h.UI.lifecycle.addCleanup(() => { cleanups += 1; });
  h.UI.lifecycle.interval(() => assert.fail("departed page must not poll"), 1000);
  h.document.addEventListener("exoanchor:destroy", event => { destroyDetail = event.detail; });
  h.click();
  h.leaveAttempt();
  // Choosing Leave proceeds to pagehide, including entry into BFCache.
  h.window.dispatchEvent({ type: "pagehide", persisted });
  await assert.rejects(pending.promise, error => error.name === "AbortError");
  assert.equal(h.UI.lifecycle.destroyed, true);
  assert.equal(destroyDetail.reason, "pagehide");
  assert.equal(destroyDetail.persisted, persisted);
  assert.equal(cleanups, 1);
  assert.equal(h.timers.size, 0, "actual departure must clear all owned timers");
  h.window.dispatchEvent({ type: "pagehide", persisted });
  assert.equal(cleanups, 1, "departure cleanup must be idempotent");
  h.tickIntervals();
  h.window.dispatchEvent({ type: "pageshow", persisted: false });
  assert.equal(h.reloads(), 0);
  h.window.dispatchEvent({ type: "pageshow", persisted: true });
  assert.equal(h.reloads(), 1, "BFCache restore must reload the one-shot resource owners");
}

const disabled = harness();
disabled.link.setAttribute("aria-disabled", "true");
assert.equal(disabled.click().defaultPrevented, true, "disabled navigation must remain blocked");
assert.equal(disabled.UI.lifecycle.destroyed, false);
disabled.window.dispatchEvent({ type: "pagehide", persisted: false });

console.log("ui canceled navigation tests: PASS");
