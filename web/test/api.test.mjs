import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";

let srv, received, api;

before(async () => {
  srv = http.createServer((req, res) => {
    let body = "";
    req.on("data", (d) => (body += d));
    req.on("end", () => {
      if (req.url === "/api/info") res.end('{"id":"7F3A21","name":"Anna","fw":"0.1.0"}');
      else if (req.url === "/api/wifi" && req.method === "POST") { received = { ct: req.headers["content-type"], body }; res.end('{"ok":true}'); }
      else if (req.url === "/api/bad") { res.statusCode = 400; res.end("ssid"); }
      else { res.statusCode = 404; res.end(""); }
    });
  });
  await new Promise((r) => srv.listen(0, r));
  globalThis.window = globalThis;
  globalThis.location = { search: `?device=127.0.0.1:${srv.address().port}` };
  ({ api } = await import("../src/lib/api.ts"));
});

after(() => srv.close());

test("GET returns payload and ok flag", async () => {
  const r = await api.get("/api/info");
  assert.equal(r.ok, true);
  assert.equal(JSON.parse(r.payload).name, "Anna");
});

test("POST sends JSON as text/plain (no CORS preflight) and keeps special characters intact", async () => {
  const r = await api.post("/api/wifi", { ssid: 'Caf\u00e9 "X"', pass: 'p\\w"d', name: "Anna" });
  assert.equal(r.ok, true);
  assert.equal(received.ct, "text/plain");
  assert.deepEqual(JSON.parse(received.body), { ssid: 'Caf\u00e9 "X"', pass: 'p\\w"d', name: "Anna" });
});

test("error status is reported with the device's message", async () => {
  const r = await api.get("/api/bad");
  assert.equal(r.ok, false);
  assert.equal(r.code, "400");
  assert.equal(r.payload, "ssid");
});
