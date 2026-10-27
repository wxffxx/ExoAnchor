import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";


class MemoryStorage {
  constructor(initial = {}) { this.values = new Map(Object.entries(initial)); }
  getItem(key) { return this.values.has(key) ? this.values.get(key) : null; }
  setItem(key, value) { this.values.set(key, String(value)); }
}

class FakeEventTarget {
  constructor() { this.listeners = new Map(); }
  addEventListener(name, callback) {
    if (!this.listeners.has(name)) this.listeners.set(name, new Set());
    this.listeners.get(name).add(callback);
  }
  dispatch(name) {
    for (const callback of this.listeners.get(name) || []) callback({ type: name });
  }
}

const KVM_SOURCE_FILES = ["kvm.html", "assets/kvm-core.js", "assets/kvm-video.js", "assets/kvm-lease.js", "assets/kvm-modes.js", "assets/kvm-input.js", "assets/kvm-status.js"];
function readKvmSources() {
  return KVM_SOURCE_FILES.map(rel => fs.readFileSync(
    new URL(`../../main/www/${rel}`, import.meta.url), "utf8")).join("\n");
}
const kvmHtml = readKvmSources();
const focusStart = kvmHtml.indexOf('const focusRefresh={enabled:localStorage.getItem("si_kvm_focus_refresh")');
const focusEnd = kvmHtml.indexOf("function leaveKvmPage()", focusStart);
assert.ok(focusStart >= 0 && focusEnd > focusStart,
  "KVM focus-refresh implementation must remain extractable for behavior tests");
const focusSource = kvmHtml.slice(focusStart, focusEnd);
const directReleaseStart = kvmHtml.indexOf("function sendHidReleaseAllOnCurrentSocket(");
const directReleaseEnd = kvmHtml.indexOf("function releaseAll(", directReleaseStart);
assert.ok(directReleaseStart >= 0 && directReleaseEnd > directReleaseStart,
  "direct HID release implementation must remain extractable");
const directReleaseSource = kvmHtml.slice(directReleaseStart, directReleaseEnd);
const leaseRecoveryStart = kvmHtml.indexOf("let kvmLeaseTimer=");
const leaseRecoveryEnd = kvmHtml.indexOf("async function sendKvmLease(", leaseRecoveryStart);
assert.ok(leaseRecoveryStart >= 0 && leaseRecoveryEnd > leaseRecoveryStart,
  "KVM lease-recovery implementation must remain extractable");
const leaseRecoverySource = kvmHtml.slice(leaseRecoveryStart, leaseRecoveryEnd);

{
  const sent = [];
  let guardedSends = 0;
  const releaseContext = vm.createContext({
    API: {
      ws: { readyState: 1, send: payload => sent.push(JSON.parse(payload)) },
      send: () => { guardedSends += 1; return true; },
    },
    WebSocket: { OPEN: 1 },
    hidSocketStreamId: 41,
    videoStreamId: 41,
  });
  vm.runInContext(`${directReleaseSource}\n`
    + "globalThis.__sendHidReleaseAll=sendHidReleaseAllOnCurrentSocket;",
    releaseContext);
  assert.equal(releaseContext.__sendHidReleaseAll(), true);
  assert.equal(guardedSends, 0,
    "hidden release must bypass the inactive-page HID send guard");
  assert.deepEqual(sent, [{ type: "releaseall" }],
    "hidden transitions must write releaseall to the current open HID socket");
  releaseContext.hidSocketStreamId = 42;
  assert.equal(releaseContext.__sendHidReleaseAll(), false,
    "forced release must reject a stale stream socket");
}

function createHarness(initialStorage = {}) {
  const windowTarget = new FakeEventTarget();
  const documentTarget = new FakeEventTarget();
  documentTarget.hidden = false;
  documentTarget.hasFocus = () => true;
  const timers = new Map();
  let nextTimer = 1;
  const counts = {
    startKvmLease: 0,
    startOptions: [],
    releaseKvmLease: 0,
    recoveryMessages: [],
    stopVideoTimers: 0,
    cancelVideoTransition: 0,
    closeVideoTransport: 0,
    scheduleVideoRecovery: 0,
    releaseAll: 0,
    forcedReleaseAll: 0,
    bootKeyLoopStops: 0,
    disconnectHidWs: 0,
  };
  const focusRefreshToggle = { checked: false, onchange: null };
  const focusRefreshText = { textContent: "" };
  const context = vm.createContext({
    window: windowTarget,
    document: documentTarget,
    localStorage: new MemoryStorage(initialStorage),
    focusRefreshToggle,
    focusRefreshText,
    UI: { lifecycle: {
      timeout(callback) {
        const id = nextTimer++;
        timers.set(id, callback);
        return id;
      },
      clearTimer(id) { timers.delete(id); },
    } },
    kvmLeaving: false,
    kvmWindowBlurred: false,
    kvmBlurPaused: false,
    videoClaimed: true,
    kvmPageActive: true,
    videoModeSwitching: false,
    hidFocusBlocked: false,
    kvmReadOnly: false,
    videoLeaseHeld: true,
    videoStreamReady: true,
    msVideoPowerOff: false,
    startKvmLease: async options => {
      counts.startKvmLease += 1;
      counts.startOptions.push({ ...options });
      return true;
    },
    restoreKeyboardCaptureAfterFocus() {},
    hidSocketIsCurrent: () => true,
    API: { connect() {} },
    releaseAll() {
      counts.releaseAll += 1;
    },
    releaseAllForHidden() {
      counts.releaseAll += 1;
      counts.forcedReleaseAll += 1;
    },
    stopBootKeyLoop() { counts.bootKeyLoopStops += 1; },
    disconnectHidWs() { counts.disconnectHidWs += 1; },
    stopVideoTimers() { counts.stopVideoTimers += 1; },
    cancelVideoTransition() { counts.cancelVideoTransition += 1; },
    closeVideoTransport() { counts.closeVideoTransport += 1; },
    scheduleVideoRecovery() { counts.scheduleVideoRecovery += 1; },
    releaseKvmLease() { counts.releaseKvmLease += 1; },
    showVideoRecovery(message) { counts.recoveryMessages.push(message); },
  });
  vm.runInContext(`${focusSource}\nglobalThis.__focusRefresh=focusRefresh;globalThis.__blurPause=blurPause;`, context,
    { filename: "kvm-focus-refresh.js" });
  return { context, windowTarget, documentTarget, timers, counts,
    focusRefreshToggle, focusRefreshText };
}

async function runTimers(harness) {
  while (harness.timers.size) {
    const callbacks = [...harness.timers.values()];
    harness.timers.clear();
    for (const callback of callbacks) callback();
    await Promise.resolve();
  }
  await Promise.resolve();
}

function createLeaseRecoveryHarness() {
  const timers = new Map();
  let nextTimer = 1;
  const startOptions = [];
  const recoveryMessages = [];
  const context = vm.createContext({
    UI: { lifecycle: {
      timeout(callback) {
        const id = nextTimer++;
        timers.set(id, callback);
        return id;
      },
      clearTimer(id) { timers.delete(id); },
    } },
    document: { hidden: false },
    kvmLeaving: false,
    kvmPageActive: true,
    msVideoPowerOff: false,
    videoLeaseHeld: true,
    disconnectHidWs() {},
    closeVideoTransport() {},
    showVideoRecovery(message) { recoveryMessages.push(message); },
    startKvmLease(options) { startOptions.push({ ...options }); },
  });
  vm.runInContext(`${leaseRecoverySource}\n`
    + "globalThis.__handleLeaseFailure=handleLeaseFailure;"
    + "globalThis.__scheduleLeaseRecovery=scheduleLeaseRecovery;",
  context, { filename: "kvm-lease-recovery.js" });
  return { context, timers, startOptions, recoveryMessages };
}

function runLeaseRecoveryTimer(harness) {
  assert.equal(harness.timers.size, 1,
    "lease recovery must keep exactly one bounded retry timer");
  const [callback] = harness.timers.values();
  harness.timers.clear();
  callback();
}

const staleLease = createLeaseRecoveryHarness();
staleLease.context.__handleLeaseFailure(new Error("stale KVM stream"));
runLeaseRecoveryTimer(staleLease);
assert.deepEqual(staleLease.startOptions,
  [{ rotate: true, resetStream: true }],
  "a stale stream must rotate its canceled ID before reclaiming the lease");
assert.deepEqual(staleLease.recoveryMessages,
  ["视频会话已过期，正在自动恢复"]);

const transientLease = createLeaseRecoveryHarness();
transientLease.context.__handleLeaseFailure(new Error("network timed out"));
runLeaseRecoveryTimer(transientLease);
assert.deepEqual(transientLease.startOptions, [{}],
  "a transient network failure should retry the current stream ID");

const escalatedLease = createLeaseRecoveryHarness();
escalatedLease.context.__scheduleLeaseRecovery(250, false);
escalatedLease.context.__handleLeaseFailure(new Error("stale KVM stream"));
runLeaseRecoveryTimer(escalatedLease);
assert.deepEqual(escalatedLease.startOptions,
  [{ rotate: true, resetStream: true }],
  "a pending normal retry must be upgraded when a stale response arrives");

const defaults = createHarness();
assert.equal(defaults.context.__focusRefresh.enabled, true,
  "focus refresh must default to enabled");
assert.equal(defaults.focusRefreshToggle.checked, true);
assert.equal(defaults.focusRefreshText.textContent, "开启");
defaults.focusRefreshToggle.checked = false;
defaults.focusRefreshToggle.onchange();
assert.equal(defaults.context.localStorage.getItem("si_kvm_focus_refresh"), "0",
  "disabled preference must persist in browser storage");

const persisted = createHarness({ si_kvm_focus_refresh: "0" });
assert.equal(persisted.context.__focusRefresh.enabled, false,
  "stored disabled preference must be restored");
assert.equal(persisted.focusRefreshToggle.checked, false);
persisted.windowTarget.dispatch("blur");
persisted.documentTarget.hidden = true;
persisted.documentTarget.dispatch("visibilitychange");
assert.equal(persisted.counts.closeVideoTransport, 0,
  "disabled focus refresh must not close the video transport when hidden");
assert.equal(persisted.counts.stopVideoTimers, 0,
  "disabled focus refresh must not stop video timers when hidden");
assert.equal(persisted.counts.cancelVideoTransition, 0,
  "disabled focus refresh must not cancel the active video transition");
assert.equal(persisted.counts.releaseAll, 2,
  "blur and hidden events must still release held human input");
assert.equal(persisted.counts.forcedReleaseAll, 1,
  "a hidden-only transition must release the current HID socket directly");
assert.equal(persisted.counts.bootKeyLoopStops, 2,
  "blur and hidden events must stop a running boot-key loop");
assert.equal(persisted.counts.disconnectHidWs, 2,
  "blur and hidden events must still disconnect the HID WebSocket");
persisted.documentTarget.hidden = false;
persisted.documentTarget.dispatch("visibilitychange");
persisted.windowTarget.dispatch("focus");
await runTimers(persisted);
assert.equal(persisted.counts.startKvmLease, 0,
  "disabled focus refresh must not rotate or reconnect video on return");
assert.equal(persisted.counts.scheduleVideoRecovery, 0,
  "disabled focus refresh must not schedule video recovery on return");

const enabled = createHarness();
enabled.windowTarget.dispatch("blur");
enabled.documentTarget.hidden = true;
enabled.documentTarget.dispatch("visibilitychange");
assert.equal(enabled.counts.closeVideoTransport, 1,
  "enabled focus refresh must suspend the old transport while hidden");
assert.equal(enabled.counts.stopVideoTimers, 1);
assert.equal(enabled.counts.cancelVideoTransition, 1);
enabled.documentTarget.hidden = false;
enabled.documentTarget.dispatch("visibilitychange");
enabled.windowTarget.dispatch("focus");
assert.equal(enabled.timers.size, 1,
  "focus and visibility return events must coalesce into one pending refresh");
await runTimers(enabled);
assert.equal(enabled.counts.startKvmLease, 1,
  "coalesced return events must rotate the video stream exactly once");
assert.deepEqual(enabled.counts.startOptions,
  [{ rotate: true, resetStream: true, delay: 0 }],
  "return refresh must reuse the bounded rotate/reset lease path");

const poweredOff = createHarness();
poweredOff.context.msVideoPowerOff = true;
poweredOff.windowTarget.dispatch("blur");
poweredOff.documentTarget.hidden = true;
poweredOff.documentTarget.dispatch("visibilitychange");
poweredOff.documentTarget.hidden = false;
poweredOff.documentTarget.dispatch("visibilitychange");
poweredOff.windowTarget.dispatch("focus");
await runTimers(poweredOff);
assert.equal(poweredOff.counts.startKvmLease, 0,
  "focus recovery must not request a video lease while MS2109 power is off");

console.log("kvm focus refresh tests: PASS");

// Blur-pause capture: a browser preference that defaults to OFF and, when
// enabled, releases the video lease on window blur and reclaims it on focus.
{
  const off = createHarness();
  assert.equal(off.context.__blurPause.enabled, false,
    "blur pause must default to disabled");
  off.windowTarget.dispatch("blur");
  assert.equal(off.counts.releaseKvmLease, 0,
    "default blur must keep the video lease and capture alive");
  assert.equal(off.counts.closeVideoTransport, 0);
  assert.equal(off.context.kvmBlurPaused, false);

  const on = createHarness({ si_kvm_blur_pause: "1", si_kvm_focus_refresh: "0" });
  assert.equal(on.context.__blurPause.enabled, true,
    "stored preference must enable blur pause");
  on.windowTarget.dispatch("blur");
  assert.equal(on.counts.releaseKvmLease, 1,
    "blur with the preference on must release the video lease once");
  assert.equal(on.counts.closeVideoTransport, 1);
  assert.equal(on.counts.stopVideoTimers, 1);
  assert.equal(on.counts.cancelVideoTransition, 1);
  assert.equal(on.context.kvmBlurPaused, true);
  assert.deepEqual(on.counts.recoveryMessages, ["窗口失焦，已中断采集；回到窗口后自动恢复"]);
  on.windowTarget.dispatch("blur");
  assert.equal(on.counts.releaseKvmLease, 1,
    "a repeated blur must not release the lease again");
  on.windowTarget.dispatch("focus");
  await runTimers(on);
  assert.equal(on.context.kvmBlurPaused, false,
    "focus must clear the paused state");
  assert.equal(on.counts.startKvmLease, 1,
    "focus must reclaim the lease even when focus refresh is disabled");
  assert.deepEqual(on.counts.startOptions, [{ rotate: true, resetStream: true, delay: 0 }]);

  const storage = createHarness();
  storage.windowTarget.dispatch = undefined;
  for (const callback of storage.windowTarget.listeners.get("storage") || []) {
    callback({ key: "si_kvm_blur_pause", newValue: "1" });
  }
  assert.equal(storage.context.__blurPause.enabled, true,
    "a change saved from the Settings page must apply to an open KVM tab");
}
