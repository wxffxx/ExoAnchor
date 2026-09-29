import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const source = fs.readFileSync(
  new URL("../../main/www/terminal.html", import.meta.url), "utf8");

function namedFunction(name) {
  let start = source.indexOf(`function ${name}(`);
  assert.notEqual(start, -1, `missing function ${name}`);
  if (source.slice(Math.max(0, start - 6), start) === "async ") start -= 6;
  const brace = source.indexOf("{", start);
  let depth = 0, quote = "", escaped = false;
  for (let index = brace; index < source.length; index += 1) {
    const char = source[index];
    if (quote) {
      if (escaped) escaped = false;
      else if (char === "\\") escaped = true;
      else if (char === quote) quote = "";
      continue;
    }
    if (char === '"' || char === "'" || char === "`") quote = char;
    else if (char === "{") depth += 1;
    else if (char === "}" && --depth === 0)
      return source.slice(start, index + 1);
  }
  assert.fail(`unterminated function ${name}`);
}

function classes() {
  const values = new Set();
  return {
    toggle(name, enabled) { enabled ? values.add(name) : values.delete(name); },
    contains(name) { return values.has(name); },
  };
}

const calls = [], writes = [], messages = [];
const els = {
  automationBanner: { hidden: true, classList: classes() },
  automationTitle: { textContent: "" },
  automationDetail: { textContent: "" },
  stopAutomation: { disabled: false },
  terminal: { classList: classes() },
  quickBtn: { disabled: false },
};
const context = vm.createContext({
  els,
  term: { options: { disableStdin: false } },
  stopUartKeyRepeat() {},
  renderUartMode() {},
  connected: () => true,
  setState(text, kind) { calls.push({ kind: "state", text, stateKind: kind }); },
  writeShellData(text, suppress) { writes.push({ text, suppress }); },
  setMsg(text, kind) { messages.push({ text, kind }); },
  sshTarget: { configured: true },
  API: {
    async post(path, body) { calls.push({ kind: "post", path, body }); return {}; },
  },
});

vm.runInContext(`
  let activeChannel="ssh";
  const terminalControls={
    ssh:{active:false,generation:0,actor:"none"},
    uart:{active:false,generation:0,actor:"none"},
  };
  let ws={sent:[],send(value){this.sent.push(value)}};
  ${namedFunction("manualInputLocked")}
  ${namedFunction("renderTerminalControl")}
  ${namedFunction("applyTerminalControl")}
  ${namedFunction("safeTerminalInputText")}
  ${namedFunction("mirrorTerminalInput")}
  ${namedFunction("stopTerminalAutomation")}
  ${namedFunction("sendRaw")}
  globalThis.contract={manualInputLocked,renderTerminalControl,
    applyTerminalControl,mirrorTerminalInput,stopTerminalAutomation,sendRaw,
    controls:terminalControls,socket:ws};
`, context, { filename: "terminal-automation-control.js" });

const active = {
  type: "terminal.control", channel: "ssh", active: true,
  generation: 7, actor: "agent", run_id: "run-7",
  label: "Agent 执行 SSH 命令", cancel_requested: false,
};
assert.equal(context.contract.applyTerminalControl(active), true);
assert.equal(context.contract.manualInputLocked(), true);
assert.equal(els.automationBanner.hidden, false);
assert.equal(els.terminal.classList.contains("manual-locked"), true);
assert.equal(context.term.options.disableStdin, true);
assert.match(els.automationTitle.textContent, /AGENT 正在操作 SSH/);
assert.match(els.automationDetail.textContent, /输入\/输出实时同步/);
assert.equal(context.contract.sendRaw("whoami\r"), false);
assert.equal(context.contract.socket.sent.length, 0,
  "manual input must not reach the WebSocket during automation");

context.contract.mirrorTerminalInput({
  channel: "ssh", text: "printf hi\u001b[31m", append_enter: true,
  redacted: false,
});
assert.match(writes.at(-1).text, /\[AGENT SSH INPUT\]/);
assert.match(writes.at(-1).text, /printf hi\\x1b\[31m/,
  "mirrored input must display escape bytes without executing terminal CSI");
context.contract.mirrorTerminalInput({
  channel: "ssh", text: "password=never-show", append_enter: true,
  redacted: true,
});
assert.match(writes.at(-1).text, /受保护输入/);
assert.doesNotMatch(writes.at(-1).text, /never-show/);

assert.equal(context.contract.applyTerminalControl({
  channel: "ssh", active: false, generation: 6,
}), false, "stale release must not unlock a newer operation");
assert.equal(context.contract.manualInputLocked(), true);

await context.contract.stopTerminalAutomation();
const posts = calls.filter(call => call.kind === "post");
assert.deepEqual(JSON.parse(JSON.stringify(posts[0])), {
  kind: "post", path: "/api/terminal/control/stop",
  body: { channel: "ssh", generation: 7 },
});
assert.deepEqual(JSON.parse(JSON.stringify(posts[1])), {
  kind: "post", path: "/api/agent/run/abort",
  body: { run_id: "run-7" },
});

assert.equal(context.contract.applyTerminalControl({
  channel: "ssh", active: false, generation: 7, actor: "none",
}), true);
assert.equal(context.contract.manualInputLocked(), false);
assert.equal(els.automationBanner.hidden, true);
assert.equal(context.term.options.disableStdin, false);

console.log("Terminal Agent/MCP control UI tests: PASS");
