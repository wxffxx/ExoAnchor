import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";


const kvmHtml = fs.readFileSync(
  new URL("../../main/www/kvm.html", import.meta.url),
  "utf8",
);
const stateStart = kvmHtml.indexOf("function suspendKvmForMsPowerOff()");
const stateEnd = kvmHtml.indexOf("const focusRefresh=", stateStart);
assert.ok(stateStart >= 0 && stateEnd > stateStart,
  "KVM MS2109 power-state lifecycle must remain extractable");
const stateSource = kvmHtml.slice(stateStart, stateEnd);

function createHarness() {
  const counts = {
    suspend: 0,
    releaseLease: 0,
    resumeLease: 0,
    hidRelease: 0,
  };
  const streamStats = {
    textContent: "",
    classList: { add() {} },
  };
  const context = vm.createContext({
    msVideoPowerKnown: false,
    msVideoPowerOff: false,
    videoClaimed: true,
    videoLeaseHeld: true,
    videoStreamReady: true,
    kvmLeaseLastError: "previous failure",
    outputFailureMessage: "previous output failure",
    videoUnhealthyPolls: 3,
    kvmReadOnly: false,
    videoPeerBlocked: false,
    kvmLeaving: false,
    kvmPageActive: true,
    document: { hidden: false },
    streamStats,
    stopBootKeyLoop() { counts.suspend += 1; },
    sendHidReleaseAllOnCurrentSocket() { counts.hidRelease += 1; },
    releaseAll() {},
    stopVideoTimers() {},
    clearFocusRefreshPending() {},
    cancelVideoTransition() {},
    disconnectHidWs() {},
    closeVideoTransport() {},
    releaseKvmLease() { counts.releaseLease += 1; },
    setKvmControlsDisabled() {},
    showVideoRecovery() {},
    startKvmLease: async options => {
      counts.resumeLease += 1;
      return options;
    },
  });
  vm.runInContext(`${stateSource}\n`
    + "globalThis.__syncMsVideoPowerState=syncMsVideoPowerState;", context,
  { filename: "kvm-ms-power-state.js" });
  return { context, counts, streamStats };
}

const harness = createHarness();
const off = { ms2109: { supported: true, initialized: true, power_on: false } };
assert.equal(harness.context.__syncMsVideoPowerState(off), false);
assert.equal(harness.context.msVideoPowerKnown, true);
assert.equal(harness.context.msVideoPowerOff, true);
assert.equal(harness.context.videoClaimed, false);
assert.equal(harness.context.videoLeaseHeld, false);
assert.equal(harness.counts.suspend, 1,
  "first explicit MS2109-off state must suspend KVM exactly once");
assert.equal(harness.counts.hidRelease, 1,
  "MS2109-off transition must release held HID input before disconnecting");
assert.equal(harness.counts.releaseLease, 1,
  "MS2109-off transition must release the active video lease");
assert.equal(harness.streamStats.textContent, "STREAM OFF · MS2109 POWER OFF");

assert.equal(harness.context.__syncMsVideoPowerState(off), false);
assert.equal(harness.counts.suspend, 1,
  "repeated off snapshots must not repeat disconnect side effects");
assert.equal(harness.counts.resumeLease, 0,
  "off snapshots must never start a video lease");

const on = { ms2109: { supported: true, initialized: true, power_on: true } };
assert.equal(harness.context.__syncMsVideoPowerState(on), true);
await Promise.resolve();
assert.equal(harness.context.msVideoPowerOff, false);
assert.equal(harness.context.videoClaimed, true);
assert.equal(harness.counts.resumeLease, 1,
  "MS2109 on transition must resume through the bounded lease path once");

const unsupported = createHarness();
assert.equal(unsupported.context.__syncMsVideoPowerState(
  { ms2109: { supported: false, initialized: false, power_on: false } }), true);
await Promise.resolve();
assert.equal(unsupported.context.msVideoPowerOff, false,
  "unsupported profiles must not be mistaken for powered-off MS2109 hardware");
assert.equal(unsupported.counts.resumeLease, 1);

console.log("kvm MS2109 power-state tests: PASS");
