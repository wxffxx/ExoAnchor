import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const source = fs.readFileSync(new URL("../../main/www/assets/ui-shell.js", import.meta.url), "utf8");
const start = source.indexOf("  function assistantRenderRequests(requests) {");
const end = source.indexOf("\n  async function assistantSyncRequests()", start);
assert.ok(start >= 0 && end > start);

let allocations = 0;
let mutations = 0;
const document = { activeElement: null };
class Element {
  constructor(tag) {
    this.tagName = tag;
    this.children = [];
    this.parentNode = null;
    this.value = "";
    this.classList = { add() {}, toggle() {} };
    allocations += 1;
  }
  get lastChild() { return this.children.at(-1); }
  set innerHTML(_value) {
    [...this.children].forEach(child => this.removeChild(child));
  }
  append(...children) { children.forEach(child => this.appendChild(child)); }
  appendChild(child) { return this.insertBefore(child, null); }
  insertBefore(child, before) {
    if (child.parentNode) child.parentNode.removeChild(child);
    const index = before ? this.children.indexOf(before) : this.children.length;
    this.children.splice(index, 0, child);
    child.parentNode = this;
    mutations += 1;
    return child;
  }
  removeChild(child) {
    this.children.splice(this.children.indexOf(child), 1);
    child.parentNode = null;
    if (child.contains(document.activeElement)) document.activeElement = null;
    mutations += 1;
  }
  contains(node) { return this === node || this.children.some(child => child.contains(node)); }
  focus() { document.activeElement = this; }
  setSelectionRange(start, end, direction) {
    [this.selectionStart, this.selectionEnd, this.selectionDirection] = [start, end, direction];
  }
}
document.createElement = tag => new Element(tag);
const host = new Element("div");
const decisions = [];
const context = {
  document, assistant: {}, byId: () => host,
  assistantResolveRequest: async (...args) => { decisions.push(args); },
  assistantAddMessage: () => assert.fail("unexpected request error"),
};
vm.runInNewContext(source.slice(start, end) + "\nglobalThis.render = assistantRenderRequests;", context);
const request = {
  kind: "agent",
  status: { request_id: "request_1", argument_hash: "hash_1", kind: "context",
    resource: "ask_user", risk: "low", reason: "Which report?", expires_ms: 1000 },
};
context.render([request]);
const card = host.children[0];
const input = card.children.find(child => child.tagName === "textarea");
input.value = "The weekly report";
input.focus();
const beforeAllocations = allocations;
const beforeMutations = mutations;
for (let index = 0; index < 100; index += 1) {
  context.render([{ ...request, status: { ...request.status, expires_ms: 1000 - index } }]);
}
assert.equal(host.children[0], card, "unchanged requests must keep their card");
assert.equal(input.value, "The weekly report");
assert.equal(document.activeElement, input, "polling must preserve typing focus");
assert.equal(allocations, beforeAllocations, "100 unchanged polls must allocate no DOM nodes");
assert.equal(mutations, beforeMutations, "100 unchanged polls must not mutate the card tree");
card.children.at(-1).children.at(-1).onclick();
assert.equal(decisions[0][1].expires_ms, 901, "actions must use the latest status object");
assert.equal(decisions[0][3], "The weekly report");

const second = { ...request, status: { ...request.status, request_id: "request_second" } };
context.render([second, request]);
input.setSelectionRange(4, 10, "forward");
context.render([request, second]);
assert.equal(document.activeElement, input, "reordering must restore focus to a retained input");
assert.deepEqual([input.selectionStart, input.selectionEnd, input.selectionDirection], [4, 10, "forward"]);
assert.equal(input.value, "The weekly report");

context.render([{ ...request, status: { ...request.status, argument_hash: "hash_2" } }]);
assert.notEqual(host.children[0], card, "changed arguments require a fresh card");
assert.equal(host.children[0].children.find(child => child.tagName === "textarea").value, "");
const changed = host.children[0];
context.render([{ ...request, status: { ...request.status, request_id: "request_2" } }]);
assert.notEqual(host.children[0], changed, "new request IDs must not inherit an answer");
const sameId = host.children[0];
context.render([{ ...request, status: { ...request.status, request_id: "request_2", reason: "A different question" } }]);
assert.notEqual(host.children[0], sameId, "changed questions must not inherit an answer");
context.render([]);
assert.equal(host.children.length, 0, "resolved requests must be removed");
assert.equal(context.assistant.requestCards.size, 0, "removed cards must leave the cache");
console.log("Assistant request reconciliation tests: PASS (100 unchanged polls: 0 allocations, 0 tree mutations)");
