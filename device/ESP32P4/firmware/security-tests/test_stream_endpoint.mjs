import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const stable = new URL("../v0.86-stable-kvm/", import.meta.url);
const config = fs.readFileSync(new URL("main/config/app_config.h", stable), "utf8");
const port = Number(config.match(/^#define SI_STREAM_HTTP_PORT (\d+)$/m)?.[1]);
assert.ok(port > 0, "firmware must declare the streaming listener port");
const page = fs.readFileSync(new URL("main/www/kvm.html", stable), "utf8");
const fn = page.match(/^function streamUrl\(\)\{.*\}$/m)?.[0];
assert.ok(fn, "test must execute the production stream URL builder");

for (const href of ["https://192.0.2.8/kvm", "https://example.test:8443/kvm?old=1#tab", "https://[::1]/kvm"]) {
  const location = new URL(href);
  const context = vm.createContext({ location, URL, Date, authUrl: value => value });
  const stream = new URL(vm.runInContext(fn + ";streamUrl()", context));
  assert.equal(stream.protocol, "https:");
  assert.equal(stream.hostname, location.hostname);
  assert.equal(Number(stream.port), port, "browser must use the active TLS stream listener");
  assert.equal(stream.pathname, "/api/stream");
  assert.equal(stream.hash, "");
  assert.ok(stream.searchParams.has("t"));
  assert.equal(stream.searchParams.has("old"), false);
}
console.log("Stable browser video endpoint matches TLS listener: PASS");
