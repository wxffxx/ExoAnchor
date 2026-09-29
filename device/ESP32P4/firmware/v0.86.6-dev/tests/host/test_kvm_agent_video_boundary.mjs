import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("../..", import.meta.url));
const kvm = fs.readFileSync(`${root}/main/www/kvm.html`, "utf8");
const videoHttp = fs.readFileSync(
  `${root}/main/services/web/video_http_module.inc`, "utf8",
);

const renderStart = kvm.indexOf("function renderAgentPresence(");
const renderEnd = kvm.indexOf("function videoUnavailableReason(", renderStart);
assert.ok(renderStart >= 0 && renderEnd > renderStart,
  "KVM Agent presence renderer must remain behavior-testable");
assert.match(kvm, /typeof lease\.input_control_active===\"boolean\"/,
  "KVM must prefer the explicit input-control state");
assert.doesNotMatch(kvm, /inputControlActive[^;]*vc\.agent_takeover/,
  "video takeover must never be inferred as HID input ownership");
const reclaimStart = kvm.indexOf("async function reclaimKvmControl(");
const reclaimEnd = kvm.indexOf("reclaimKvm.onclick=", reclaimStart);
assert.ok(reclaimStart >= 0 && reclaimEnd > reclaimStart,
  "manual automated-control termination must remain behavior-testable");

const streamStart = videoHttp.indexOf("static esp_err_t stream_handler(");
const streamEnd = videoHttp.indexOf("static int httpd_client_count(", streamStart);
const stream = videoHttp.slice(streamStart, streamEnd);
assert.ok(stream.includes("si_video_control_keep_automation_alive(stream_actor);"),
  "an Agent video observer must publish bounded presence");
assert.ok(!stream.includes("human KVM video is active"),
  "read-only Agent observation must not be rejected by a human KVM viewer");

function classList(initial = []) {
  const values = new Set(initial);
  return {
    toggle(name, active) { active ? values.add(name) : values.delete(name); },
    contains(name) { return values.has(name); },
  };
}

const agentTakeoverBanner = { classList: classList(["hide"]) };
const agentPresenceTitle = { textContent: "" };
const agentTakeoverDetail = { textContent: "" };
const reclaimKvm = { hidden: true };
const context = vm.createContext({
  agentTakeoverBanner,
  agentPresenceTitle,
  agentTakeoverDetail,
  reclaimKvm,
});
vm.runInContext(
  `${kvm.slice(renderStart, renderEnd)}\n` +
  "globalThis.__renderAgentPresence=renderAgentPresence;",
  context,
  { filename: "kvm-agent-video-boundary.js" },
);

context.__renderAgentPresence(false, false, "Agent", true);
assert.equal(agentTakeoverBanner.classList.contains("hide"), true,
  "no Agent observer must keep the disclosure hidden");
assert.equal(reclaimKvm.hidden, true,
  "human reclaim is irrelevant without Agent input takeover");

context.__renderAgentPresence(true, false, "Agent", true);
assert.equal(agentTakeoverBanner.classList.contains("hide"), false,
  "Agent observation must be visible on the KVM page");
assert.equal(agentTakeoverBanner.classList.contains("takeover"), false,
  "read-only observation must not look like input takeover");
assert.equal(agentPresenceTitle.textContent, "Agent 正在查看画面");
assert.match(agentTakeoverDetail.textContent, /只读观察 · 视频共享/);
assert.match(agentTakeoverDetail.textContent, /人工输入不受影响/);
assert.equal(reclaimKvm.hidden, true,
  "read-only observation must leave human input ownership unchanged");

context.__renderAgentPresence(false, true, "MCP", true);
assert.equal(agentTakeoverBanner.classList.contains("takeover"), true,
  "automated input ownership must have a distinct state");
assert.equal(agentPresenceTitle.textContent, "MCP 正在控制输入");
assert.match(agentTakeoverDetail.textContent, /KVM 视频继续共享/);
assert.match(agentTakeoverDetail.textContent, /画面已同步/);
assert.match(agentTakeoverDetail.textContent, /可随时终止自动控制/);
assert.equal(reclaimKvm.hidden, false,
  "input ownership must expose the human termination action");

{
  const calls = [];
  const reclaimButton = { disabled: false };
  const reclaimContext = vm.createContext({
    reclaimKvm: reclaimButton,
    API: {
      async request(method, path, body, auth, options) {
        calls.push({ kind: "control", method, path, body, auth, options });
        return { active: false };
      },
    },
    async sendKvmLease(active, force, claim, previousStreamId) {
      calls.push({ kind: "video", active, force, claim, previousStreamId });
      return { kvm_active: true };
    },
    videoLeaseHeld: false,
    setKvmReadOnly(value) { calls.push({ kind: "readonly", value }); },
    kvmLeaseTimer: 1,
    UI: { lifecycle: { interval() { throw new Error("unexpected timer"); } } },
    touchKvm() {},
    browserVideoReady() { return true; },
    startVideo() { throw new Error("unexpected video restart"); },
    setTimeout(callback) { calls.push({ kind: "status_timer" }); callback(); },
    status() { calls.push({ kind: "status" }); },
    videoText: { textContent: "" },
    nosignal: { classList: classList(["hide"]) },
  });
  vm.runInContext(
    `${kvm.slice(reclaimStart, reclaimEnd)}\n` +
    "globalThis.__reclaimKvmControl=reclaimKvmControl;",
    reclaimContext,
    { filename: "kvm-manual-termination.js" },
  );
  await reclaimContext.__reclaimKvmControl();
  assert.equal(reclaimButton.disabled, false,
    "manual termination button must be re-enabled after completion");
  assert.deepEqual(JSON.parse(JSON.stringify(calls[0])), {
    kind: "control",
    method: "POST",
    path: "/api/control/lease",
    body: {
      active: false,
      force: true,
      reason: "manual KVM terminated automated input",
    },
    auth: true,
    options: { timeoutMs: 3000 },
  }, "manual termination must revoke Agent/MCP input before reclaiming KVM");
  assert.deepEqual(JSON.parse(JSON.stringify(calls[1])), {
    kind: "video", active: true, force: true, claim: true,
    previousStreamId: 0,
  }, "manual termination must then reclaim the visible KVM lease");
  assert.deepEqual(calls[2], { kind: "readonly", value: false },
    "manual termination must restore browser input only after both transitions");
}

console.log("KVM Agent video boundary tests: PASS");
