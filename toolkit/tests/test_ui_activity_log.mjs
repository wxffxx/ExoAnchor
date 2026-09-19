import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const html = fs.readFileSync(new URL('../exoanchor_toolkit/toolkit_ui.html', import.meta.url), 'utf8');
const start = html.indexOf('function log(');
const end = html.indexOf('\nfunction syncControlState(', start);
assert.ok(start >= 0 && end > start, 'activity log function must exist');
let heightReads = 0;
const rows = [];
const frames = [];
const activityLog = {
  append(row) { rows.push(row); },
  get scrollHeight() { heightReads += 1; return rows.length * 20; },
  scrollTop: 0,
};
const context = {
  activityScrollPending: false,
  $: id => { assert.equal(id, 'activityLog'); return activityLog; },
  document: {
    createElement(tag) {
      return { tag, textContent: '', className: '', children: [],
        append(...children) { this.children.push(...children); } };
    },
  },
  requestAnimationFrame(callback) { frames.push(callback); return frames.length; },
};
vm.runInNewContext(html.slice(start, end), context);

for (let index = 0; index < 1000; index += 1) {
  context.log(`<untrusted-${index}>`, index % 2 ? 'warn' : 'info');
}
assert.equal(rows.length, 1000, 'all log rows remain available');
assert.equal(rows[999].children[2].textContent, '<untrusted-999>', 'messages remain plain text');
assert.equal(rows[999].children[1].textContent, 'WARN');
assert.equal(heightReads, 0, 'appending a burst must not force per-row layout reads');
assert.equal(frames.length, 1, 'one event-loop burst schedules one scroll update');
frames.shift()();
assert.equal(heightReads, 1);
assert.equal(activityLog.scrollTop, 20000, 'frame scrolls to the final height');

context.log('next operation', 'ok');
assert.equal(frames.length, 1, 'later bursts can schedule another update');
frames.shift()();
assert.equal(heightReads, 2);
assert.equal(activityLog.scrollTop, 20020);
assert.equal(rows[1000].children[1].textContent, 'OK');
console.log('Toolkit activity log batching tests: PASS');
