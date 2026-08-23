import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";


const source = fs.readFileSync(
  new URL("../../main/www/terminal.html", import.meta.url),
  "utf8",
);

function namedFunction(name) {
  let start = source.indexOf(`function ${name}(`);
  assert.notEqual(start, -1, `missing function ${name}`);
  if (source.slice(Math.max(0, start - 6), start) === "async ") start -= 6;
  const brace = source.indexOf("{", start);
  let depth = 0;
  let quote = "";
  let escaped = false;
  for (let index = brace; index < source.length; index += 1) {
    const char = source[index];
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
      if (depth === 0) return source.slice(start, index + 1);
    }
  }
  assert.fail(`unterminated function ${name}`);
}

class FakeWebSocket {
  static CONNECTING = 0;
  static OPEN = 1;
  static CLOSING = 2;
  static CLOSED = 3;
  static instances = [];

  constructor(url) {
    this.url = url;
    this.readyState = FakeWebSocket.CONNECTING;
    this.sent = [];
    FakeWebSocket.instances.push(this);
  }

  open() {
    this.readyState = FakeWebSocket.OPEN;
    this.onopen?.();
  }

  message(data) { this.onmessage?.({ data }); }
  send(data) { this.sent.push(data); }
  close(code, reason) {
    this.closeCode = code;
    this.closeReason = reason;
    this.readyState = FakeWebSocket.CLOSING;
  }

  finishClose() {
    this.readyState = FakeWebSocket.CLOSED;
    this.onclose?.();
  }
}

function createHarness() {
  FakeWebSocket.instances = [];
  const timers = new Map();
  const writes = [];
  const state = { badge: "", badgeKind: "", message: "", messageKind: "" };
  const sshBadge = { className: "", hidden: false, textContent: "" };
  let nextTimer = 1;
  const context = vm.createContext({
    ArrayBuffer,
    WebSocket: FakeWebSocket,
    location: { protocol: "http:", host: "192.0.2.112" },
    UI: { lifecycle: {
      timeout(callback) {
        const id = nextTimer++;
        timers.set(id, callback);
        return id;
      },
      clearTimer(id) { timers.delete(id); },
    } },
    API: { ensure: async () => true },
    els: { sessionPasswordInput: { value: "" }, sshBadge },
    term: { cols: 120, rows: 32, focus() {}, reset() {}, write() {} },
    fitTerm() {},
    setState(text, kind) { state.badge = text; state.badgeKind = kind; },
    setMsg(text, kind) { state.message = text; state.messageKind = kind; },
    switchChannel() {},
    requestSessionPassword: async () => null,
    stopUartKeyRepeat() {},
    closePasswordPrompt() {},
    writeShellData(data) { writes.push(data); },
    renderTerminalControl() {},
    applyTerminalControl() {},
    mirrorTerminalInput() {},
  });
  vm.runInContext(`
    const UART_TERMINAL_ENABLED=true;
    const UART_WS_READY_TIMEOUT_MS=4000;
    const SSH_WS_READY_GRACE_MS=3000;
    let ws=null,activeChannel="ssh",uartReplay=null,
        uartReplayBytesPending=0,suppressTerminalResponses=0;
    let uartHandshakeTimer=0,uartHandshakeReady=false,uartCloseReason="";
    let sshHandshakeTimer=0,sshHandshakeReady=false,sshCloseReason="";
    let uartStatus={initialized:true};
    let sshTarget={configured:true,host:"198.51.100.62",port:22,
        username:"test-user",auth_method:"auto",timeout_ms:30000,
        password_configured:true,private_key_configured:false,pty:true};
    let passwordPromptResolve=null;
    ${namedFunction("needsSessionPassword")}
    ${namedFunction("connected")}
    ${namedFunction("clearUartHandshakeTimer")}
    ${namedFunction("clearSshHandshakeTimer")}
    ${namedFunction("sshReadyTimeoutMs")}
    ${namedFunction("sshSessionFailureReason")}
    ${namedFunction("wsUrl")}
    ${namedFunction("renderChannelBadge")}
    ${namedFunction("handleShellFrame")}
    ${namedFunction("connectShell")}
    ${namedFunction("disconnectShell")}
    globalThis.contract={connectShell,disconnectShell,connected,
      renderChannelBadge,needsSessionPassword,socket:()=>ws,
      ready:()=>sshHandshakeReady};
  `, context, { filename: "ssh-terminal-handshake.js" });
  return { context, timers, writes, state, sshBadge };
}

const success = createHarness();
success.context.contract.renderChannelBadge();
assert.equal(success.sshBadge.textContent, "SSH PTY · password configured");
assert.equal(success.context.contract.needsSessionPassword(), false,
  "auto must prefer the configured password when the private key is empty");
await success.context.contract.connectShell();
const socket = FakeWebSocket.instances[0];
assert.equal(success.state.badge, "CONNECTING");
assert.equal(success.context.contract.connected(), false);
socket.open();
assert.equal(success.state.badge, "CONNECTING",
  "WebSocket open must not claim the SSH application session is ready");
assert.equal(success.context.contract.connected(), false);
assert.equal(JSON.parse(socket.sent[0]).password, undefined,
  "stored password must remain device-local");
socket.message("\r\n[ssh: connected]\r\n");
assert.equal(success.state.badge, "SHELL");
assert.equal(success.context.contract.connected(), true);
assert.equal(success.timers.size, 0,
  "SSH ready acknowledgement must cancel the bounded timeout");

const failed = createHarness();
await failed.context.contract.connectShell();
const failedSocket = FakeWebSocket.instances[0];
failedSocket.open();
failedSocket.message("\r\n[ssh: host key verification failed: ESP_ERR_TIMEOUT]\r\n");
assert.equal(failed.state.badge, "ERROR");
assert.match(failed.state.message, /主机身份校验失败/);
assert.match(failed.state.message, /尚未进入密码认证/);
failedSocket.message("\r\n[ssh: session closed]\r\n");
assert.equal(failedSocket.readyState, FakeWebSocket.CLOSING,
  "a failed SSH worker must close the otherwise-idle WebSocket");
failedSocket.finishClose();
assert.equal(failed.state.badge, "ERROR");
assert.match(failed.state.message, /ESP_ERR_TIMEOUT/,
  "socket close must preserve the actionable SSH failure");

const timeout = createHarness();
await timeout.context.contract.connectShell();
const timeoutSocket = FakeWebSocket.instances[0];
timeoutSocket.open();
assert.equal(timeout.timers.size, 1);
const [timeoutCallback] = timeout.timers.values();
timeout.timers.clear();
timeoutCallback();
assert.equal(timeout.state.badge, "ERROR");
assert.match(timeout.state.message, /超时/);
assert.equal(timeoutSocket.readyState, FakeWebSocket.CLOSING);

const switched = createHarness();
await switched.context.contract.connectShell();
const switchedSocket = FakeWebSocket.instances[0];
assert.equal(switched.context.contract.disconnectShell("switch-to-uart"), true);
assert.equal(switched.context.contract.socket(), null,
  "channel switching must immediately detach the SSH socket from terminal input");
assert.equal(switchedSocket.readyState, FakeWebSocket.CLOSING);
assert.equal(switchedSocket.closeCode, 1000);
assert.equal(switchedSocket.closeReason, "switch-to-uart",
  "SSH to UART switching must actively close the SSH WebSocket");
assert.equal(switched.state.badge, "DISCONNECTED");

assert.match(source,
  /disconnectShell\(switchingFromSsh\?"switch-to-uart":"channel-switch"\)/,
  "switchChannel must identify SSH to UART disconnects explicitly");

console.log("SSH terminal handshake tests: PASS");
