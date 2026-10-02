import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const root = new URL("../../main/www/", import.meta.url);
const core = fs.readFileSync(new URL("assets/ui-core.js", root), "utf8");
const shell = fs.readFileSync(new URL("assets/ui-shell.js", root), "utf8");
const agent = fs.readFileSync(new URL("agent.html", root), "utf8");
function extract(source, name, indent) {
  const start = source.indexOf(`function ${name}(`);
  assert.ok(start >= 0);
  const lineEnd = source.indexOf("\n", start);
  if (source[lineEnd - 1] === "}") return source.slice(start, lineEnd);
  const end = source.indexOf("\n" + indent + "}", start);
  assert.ok(end > start);
  return source.slice(start, end + indent.length + 2);
}
function harness(source, name, indent) {
  let now = 0, next = 0, updates = 0;
  const frames = new Map(), cleanups = new Set(), listeners = new Map();
  const document = {
    hidden: false,
    addEventListener(type, fn) {
      if (!listeners.has(type)) listeners.set(type, new Set());
      listeners.get(type).add(fn);
    },
    removeEventListener(type, fn) { listeners.get(type)?.delete(fn); },
  };
  const lifecycle = {
    destroyed: false,
    addCleanup(fn) { cleanups.add(fn); return () => cleanups.delete(fn); },
  };
  const motion = { matches: false };
  const context = {
    document, lifecycle, motion, textAnimations: new WeakMap(), console,
    performance: { now: () => now },
    matchMedia: () => motion,
    requestAnimationFrame: fn => { frames.set(++next, fn); return next; },
    cancelAnimationFrame: id => frames.delete(id),
    assistantScrollToTail: () => { updates += 1; }, chatLog: {}, UI: {},
  };
  if (core.includes("function streamText(")) {
    vm.runInNewContext(extract(core, "streamText", "  ") + "\nUI.streamText = streamText;", context);
  }
  vm.runInNewContext(extract(source, name, indent) + `\nglobalThis.stream = ${name};`, context);
  return {
    ...context, frames, cleanups,
    frame(time) {
      now = time;
      const callbacks = [...frames.values()]; frames.clear();
      callbacks.forEach(fn => fn(time));
    },
    hide() { document.hidden = true; [...(listeners.get("visibilitychange") || [])].forEach(fn => fn()); },
    destroy() { lifecycle.destroyed = true; [...cleanups].forEach(fn => fn()); },
    listenerCount: () => [...listeners.values()].reduce((n, set) => n + set.size, 0),
    updates: () => updates,
  };
}
function node() {
  const classes = new Set();
  return { isConnected: true, textContent: "", classList: {
    add: value => classes.add(value), remove: value => classes.delete(value),
    contains: value => classes.has(value),
  } };
}
for (const [source, name, indent] of [[shell, "assistantStreamText", "  "], [agent, "streamTextInto", ""]]) {
  const hidden = harness(source, name, indent);
  const reply = node(); let settled = false;
  const completion = hidden.stream(reply, "A long reply ".repeat(1000)).then(() => { settled = true; });
  hidden.frame(16);
  hidden.hide();
  await Promise.resolve();
  assert.equal(settled, true, `${name}: backgrounding must settle without another animation frame`);
  await completion;
  assert.equal(reply.textContent, "A long reply ".repeat(1000));
  assert.equal(hidden.frames.size + hidden.cleanups.size + hidden.listenerCount(), 0);

  const long = harness(source, name, indent), target = node();
  const text = "A😀汉字".repeat(2000);
  const done = long.stream(target, text);
  let frameCount = 0;
  for (let time = 0; time <= 1600 && long.frames.size; time += 16) {
    long.frame(time); frameCount += 1;
    assert.ok(!/[\uD800-\uDBFF]$/.test(target.textContent), "do not show half a surrogate pair");
  }
  assert.equal(long.frames.size, 0, "animation must be bounded independently of reply length");
  await done;
  assert.equal(target.textContent, text);
  assert.ok(frameCount <= 96);
  console.log(`${name}: ${text.length} UTF-16 units completed in ${frameCount} simulated 16ms frames`);
  assert.equal(long.cleanups.size + long.listenerCount(), 0);

  const leaving = harness(source, name, indent), detached = node();
  const cancelled = leaving.stream(detached, "complete after leaving");
  leaving.destroy();
  await cancelled;
  assert.equal(detached.textContent, "complete after leaving");
  assert.equal(leaving.frames.size + leaving.cleanups.size + leaving.listenerCount(), 0);
  const afterDestroy = leaving.stream(node(), "no new frames");
  await afterDestroy;
  assert.equal(leaving.frames.size, 0);

  const replaced = harness(source, name, indent), same = node();
  const old = replaced.stream(same, "old response");
  replaced.frame(16);
  const latest = replaced.stream(same, "new response");
  await old;
  replaced.hide();
  await latest;
  assert.equal(same.textContent, "new response");
  assert.equal(replaced.frames.size + replaced.cleanups.size + replaced.listenerCount(), 0);

  const removed = harness(source, name, indent), gone = node();
  const moved = removed.stream(gone, "finish before reattachment");
  gone.isConnected = false;
  removed.frame(16);
  await moved;
  assert.equal(gone.textContent, "finish before reattachment");
  assert.equal(removed.frames.size + removed.cleanups.size + removed.listenerCount(), 0);

  const instant = harness(source, name, indent), short = node();
  instant.document.hidden = true;
  await instant.stream(short, "already hidden");
  assert.equal(short.textContent, "already hidden");
  assert.equal(instant.frames.size + instant.cleanups.size + instant.listenerCount(), 0);
  assert.equal(short.classList.contains("streaming"), false);
  instant.document.hidden = false;
  instant.motion.matches = true;
  await instant.stream(short, "reduced motion");
  assert.equal(short.textContent, "reduced motion");
  assert.equal(instant.frames.size + instant.cleanups.size + instant.listenerCount(), 0);
}
console.log("Bounded assistant text animation and cleanup tests: PASS");
