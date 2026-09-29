#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const root = path.resolve(__dirname, "../..");
const source = fs.readFileSync(
  path.join(root, "main/www/assets/ui-core.js"), "utf8");

function storage() {
  const values = new Map();
  return {
    getItem(key) { return values.has(key) ? values.get(key) : null; },
    setItem(key, value) { values.set(key, String(value)); },
    removeItem(key) { values.delete(key); },
  };
}

let intervalCount = 0;
let fetchCount = 0;
global.window = {
  ExoAnchorBuild: {
    profile: "stable",
    version: "0.87.4-Stable",
    embeddedAgent: false,
    uartTerminal: false,
    h264Video: false,
    externalMcp: true,
  },
  setInterval() { intervalCount += 1; return intervalCount; },
  clearInterval() {},
  setTimeout,
  clearTimeout,
};
global.document = {
  hidden: false,
  getElementById() { return null; },
};
global.localStorage = storage();
global.sessionStorage = storage();
global.fetch = async () => {
  fetchCount += 1;
  throw new Error("Stable UI must not poll an embedded-Agent endpoint");
};

vm.runInThisContext(source, { filename: "ui-core.js" });

const ui = window.ExoAnchorUI;
if (!ui || ui.features.embeddedAgent !== false) {
  throw new Error("Stable build feature manifest did not disable embedded Agent");
}
if (ui.actionMirror.available !== false) {
  throw new Error("Stable action mirror is still advertised as available");
}
const unsubscribe = ui.actionMirror.subscribe(() => {});
if (typeof unsubscribe !== "function") {
  throw new Error("disabled action mirror did not return a cleanup callback");
}
unsubscribe();
if (ui.actionMirror.subscribers.size !== 0 || ui.actionMirror.timer !== 0) {
  throw new Error("Stable action mirror retained a subscriber or timer");
}
if (intervalCount !== 0 || fetchCount !== 0) {
  throw new Error(
    `Stable action mirror leaked runtime work: intervals=${intervalCount} fetches=${fetchCount}`
  );
}

console.log("Stable UI feature gating: ok");
