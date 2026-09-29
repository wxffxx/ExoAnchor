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

  close() { this.readyState = FakeWebSocket.CLOSING; }

  finishClose() {
    this.readyState = FakeWebSocket.CLOSED;
    this.onclose?.();
  }
}

function createHarness() {
  FakeWebSocket.instances = [];
  const timers = new Map();
  const state = { badge: "", badgeKind: "", message: "", messageKind: "" };
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
    els: { sessionPasswordInput: { value: "" } },
    term: { cols: 120, rows: 32, focus() {}, reset() {}, write() {} },
    fitTerm() {},
    setState(text, kind) { state.badge = text; state.badgeKind = kind; },
    setMsg(text, kind) { state.message = text; state.messageKind = kind; },
    switchChannel() {},
    requestSessionPassword: async () => null,
    needsSessionPassword: () => false,
    stopUartKeyRepeat() {},
    closePasswordPrompt() {},
    writeShellData() {},
    renderTerminalControl() {},
    applyTerminalControl() {},
    mirrorTerminalInput() {},
  });
  vm.runInContext(`
    const UART_TERMINAL_ENABLED=true;
    const UART_WS_READY_TIMEOUT_MS=4000;
    const SSH_WS_READY_GRACE_MS=3000;
    let ws=null,activeChannel="uart",uartReplay=null,
        uartReplayBytesPending=0,suppressTerminalResponses=0;
    let uartHandshakeTimer=0,uartHandshakeReady=false,uartCloseReason="";
    let sshHandshakeTimer=0,sshHandshakeReady=false,sshCloseReason="";
    let uartStatus={initialized:true};
    let sshTarget={configured:false,timeout_ms:30000};
    let passwordPromptResolve=null;
    ${namedFunction("connected")}
    ${namedFunction("clearUartHandshakeTimer")}
    ${namedFunction("clearSshHandshakeTimer")}
    ${namedFunction("sshReadyTimeoutMs")}
    ${namedFunction("sshSessionFailureReason")}
    ${namedFunction("wsUrl")}
    ${namedFunction("handleShellFrame")}
    ${namedFunction("connectShell")}
    ${namedFunction("disconnectShell")}
    globalThis.contract={connectShell,disconnectShell,connected,
      socket:()=>ws,ready:()=>uartHandshakeReady};
  `, context, { filename: "uart-terminal-handshake.js" });
  return { context, timers, state };
}

const success = createHarness();
await success.context.contract.connectShell();
const socket = FakeWebSocket.instances[0];
assert.equal(success.state.badge, "CONNECTING");
assert.equal(success.context.contract.connected(), false);
socket.open();
assert.equal(success.state.badge, "SYNCING",
  "transport onopen must not claim that the UART application session is live");
assert.match(success.state.message, /replay/);
assert.equal(success.context.contract.connected(), false,
  "terminal writes must remain blocked before replay acknowledgement");
socket.message(JSON.stringify({
  type: "uart.replay", generation: 5, bytes: 17, history_lost: false,
}));
assert.equal(success.state.badge, "LIVE");
assert.equal(success.context.contract.connected(), true);
assert.equal(success.timers.size, 0,
  "replay acknowledgement must cancel the bounded ready timeout");

const timeout = createHarness();
await timeout.context.contract.connectShell();
const timeoutSocket = FakeWebSocket.instances[0];
timeoutSocket.open();
assert.equal(timeout.timers.size, 1);
const [timeoutCallback] = timeout.timers.values();
timeout.timers.clear();
timeoutCallback();
assert.equal(timeout.state.badge, "ERROR");
assert.match(timeout.state.message, /replay/);
timeoutSocket.finishClose();
assert.equal(timeout.state.badge, "ERROR",
  "socket close must preserve the actionable replay-timeout reason");

const stale = createHarness();
await stale.context.contract.connectShell();
const oldSocket = FakeWebSocket.instances[0];
oldSocket.open();
await stale.context.contract.connectShell();
const newSocket = FakeWebSocket.instances[1];
oldSocket.finishClose();
assert.equal(stale.context.contract.socket(), newSocket,
  "a delayed close from an older socket must not clear the replacement");
assert.equal(stale.state.badge, "CONNECTING");

stale.context.contract.disconnectShell();
assert.equal(stale.context.contract.socket(), null);
assert.equal(stale.state.badge, "DISCONNECTED");
assert.equal(stale.timers.size, 0);

console.log("UART terminal handshake tests: PASS");
