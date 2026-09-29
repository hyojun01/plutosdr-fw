/* Browser behavior without third-party DOM or network dependencies. */
"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const {createRteApp} = require("../www/app.js");
const html = fs.readFileSync(path.join(__dirname, "../www/index.html"), "utf8");

class Element {
  constructor(id) { this.id = id; this.value = ""; this.valid = true; this.checked = false;
    this.disabled = false; this.hidden = false; this.textContent = ""; this.dataset = {}; this.children = []; this.listeners = {}; }
  addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); }
  emit(type) { return Promise.all((this.listeners[type] || []).map(fn => fn({preventDefault() {}}))); }
  appendChild(node) { this.children.push(node); }
  replaceChildren() { this.children = []; }
  reportValidity() { return this.valid; }
}
function documentFixture() {
  const elements = new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(match => [match[1], new Element(match[1])]));
  return {getElementById: id => { assert(elements.has(id), `Missing DOM id: ${id}`); return elements.get(id); },
    createElement: tag => new Element(tag)};
}
const copy = value => JSON.parse(JSON.stringify(value));
const config = range => ({enabled: true, range_m: range, radial_velocity_mps: 10, gain_linear: 0.25, phase_offset_deg: 0});
function serverFixture() {
  const server = {requests: [], mutation: null, rfCapsError: false,
    snapshot: {api_version: "v1", hardware_profile: "multitarget-v2", revision: 1, hardware_build_id: 2609182026,
      degraded: false, hardware_state_known: false, last_hardware_check_ms: 10, warning: "",
      rf: {sample_rate_hz: 61440000, carrier_hz: 2450000000, rx_lo_hz: 2450000000, tx_lo_hz: 2450000000,
        bandwidth_hz: 30000000, rx_bandwidth_hz: 30000000, tx_bandwidth_hz: 30000000, tx_gain_db: -10, rx_gain_db: 50,
        gain_control_mode: "manual", compatible: "ad9364"},
      targets: [1, 2, 3, 4].map(id => ({id, has_config: false, hardware_state_known: false, needs_reapply: false,
        encoded_carrier_hz: 0, requested: null, saved: id === 2 ? config(202) : null, applied: null}))}};
  const rfCaps = {carrier_hz: {min: 70000000, max: 6000000000, step: 1, readback_tolerance_hz: 5}, bandwidth_hz: {min: 200000, max: 40000000, step: 1},
    tx_gain_db: {min: -89.75, max: 0, step: 0.25}, rx_gain_db: {min: -3, max: 71, step: 1}};
  const rteCaps = {target_count: 4, min_range_m: 20, max_range_m: 2520, max_abs_velocity_mps: 100000,
    range_resolution_m: 0.0381206, max_gain_linear: 31.999999, range_reference: "DUT digital input/output", rf_calibrated: false};
  const reply = (data, status = 200) => ({ok: status < 400, status, json: async () => copy(data), headers: {get: key => key === "ETag" ? `"${server.snapshot.revision}"` : null}});
  server.fetch = async (url, options) => {
    server.requests.push({url, ...options, payload: options.body ? JSON.parse(options.body) : null});
    if (options.method !== "GET") {
      if (server.mutation) return server.mutation(url, options, reply);
      assert.equal(options.headers["If-Match"], `"${server.snapshot.revision}"`);
      const body = JSON.parse(options.body);
      if (options.method === "POST") {
        const id = Number(url.match(/targets\/(\d)/)[1]);
        const item = server.snapshot.targets[id - 1];
        item.requested = body; item.has_config = true; item.hardware_state_known = true;
        item.needs_reapply = false; item.encoded_carrier_hz = server.snapshot.rf.carrier_hz;
        item.applied = {enabled: body.enabled, delay_range_m: body.range_m, delay_samples: 64, doppler_hz: -12,
          effective_radial_velocity_mps: body.radial_velocity_mps, linear_gain: body.gain_linear,
          registers: {SC: {offset: id === 3 ? "0x158" : "0x140", value: "0x00040000"}}};
      } else {
        Object.assign(server.snapshot.rf, body);
        server.snapshot.targets.forEach(item => { item.needs_reapply = item.has_config; });
      }
      server.snapshot.revision++;
      return reply(server.snapshot);
    }
    if (url.endsWith("/rf/capabilities")) return server.rfCapsError
      ? reply({error: {message: "RF capability read failed"}}, 503) : reply(rfCaps);
    if (url.endsWith("/rte/capabilities")) return reply(rteCaps);
    assert.equal(url, "/api/v1/system");
    return reply(server.snapshot);
  };
  return server;
}
function fixture() {
  const doc = documentFixture();
  const server = serverFixture();
  let number = 0;
  const timers = new Map();
  const clock = {setTimeout: (fn, ms) => { const id = ++number; timers.set(id, {fn, ms}); return id; },
    clearTimeout: id => timers.delete(id), setInterval: (fn, ms) => { assert.equal(ms, 5000); return ++number; }, clearInterval() {}};
  const app = createRteApp(doc, server.fetch, clock);
  const field = id => doc.getElementById(id);
  const input = (id, value) => { field(id).value = String(value); field(id).emit("input"); };
  const targetField = (id, name) => field(`target-${id}-${name}`);
  const targetInput = (id, name, value) => input(`target-${id}-${name}`, value);
  return {app, server, field, input, targetField, targetInput, timers};
}
const mutations = server => server.requests.filter(request => request.method !== "GET");

async function run() {
  const {app, server, field, input, targetField, targetInput} = fixture();
  await app.start();
  assert.deepEqual(server.requests.map(item => item.url),
    ["/api/v1/rf/capabilities", "/api/v1/rte/capabilities", "/api/v1/system"],
    "RF observation must precede target limits and snapshot retrieval");
  assert.equal(mutations(server).length, 0, "startup cannot apply saved target values");
  for (const id of [1, 2, 3, 4]) {
    assert.equal(targetField(id, "apply").disabled, false);
    assert.equal(targetField(id, "range-m").min, "20");
    assert.equal(targetField(id, "velocity-mps").max, "100000");
    assert.equal(targetField(id, "state").textContent, "미확인");
  }
  assert.equal(targetField(2, "range-m").value, "202");
  assert.match(targetField(2, "draft-status").textContent, /저장된 편집값/);
  for (const id of [1, 2, 3, 4]) targetInput(id, "range-m", 111 * id);
  targetField(3, "enabled").checked = true; await targetField(3, "enabled").emit("input");
  await app.refresh();
  for (const id of [1, 2, 3, 4]) {
    assert.equal(targetField(id, "range-m").value, String(111 * id));
    assert.equal(targetField(id, "panel").dataset.dirty, "true");
  }
  assert.equal(mutations(server).length, 0, "editing any of the four panels cannot write hardware");
  await targetField(3, "form").emit("submit");
  assert.equal(mutations(server).length, 1);
  assert.equal(mutations(server)[0].url, "/api/v1/rte/targets/3/load-param");
  assert.equal(mutations(server)[0].method, "POST");
  assert.deepEqual(mutations(server)[0].payload, {enabled: true, range_m: 333, radial_velocity_mps: 0, gain_linear: 0, phase_offset_deg: 0});
  assert.equal(targetField(3, "panel").dataset.dirty, "false");
  assert.equal(targetField(3, "state").textContent, "활성");
  for (const id of [1, 2, 4]) {
    assert.equal(server.snapshot.targets[id - 1].has_config, false);
    assert.equal(targetField(id, "range-m").value, String(111 * id));
    assert.equal(targetField(id, "panel").dataset.dirty, "true");
    assert.equal(targetField(id, "status").textContent, "");
  }
  // Reset affects only its own editor, and must never send loadParam.
  await targetField(2, "revert").emit("click");
  assert.equal(targetField(2, "range-m").value, "202");
  assert.equal(targetField(2, "panel").dataset.dirty, "false");
  assert.equal(targetField(1, "range-m").value, "111");
  assert.equal(targetField(4, "range-m").value, "444");
  assert.equal(mutations(server).length, 1);
  targetInput(2, "range-m", 222);

  input("rf-carrier", 2400); input("rf-bandwidth", 18); input("rf-tx-gain", -0.25); input("rf-rx-gain", 0);
  await app.mutate("rf");
  assert.equal(mutations(server).length, 2, "RF apply must not send loadParam");
  assert.deepEqual(mutations(server)[1].payload, {carrier_hz: 2400000000, bandwidth_hz: 18000000, tx_gain_db: -0.25, rx_gain_db: 0});
  assert.equal(mutations(server)[1].url, "/api/v1/rf/config");
  assert.equal(mutations(server)[1].headers["If-Match"], '"2"');
  assert.match(targetField(3, "draft-status").textContent, /재적용/);
  assert.equal(targetField(3, "state").textContent, "재적용 필요");
  assert.equal(field("targets-body").children.length, 4);
  assert.equal(field("registers-body").children[0].children[2].textContent, "0x158");
  assert.equal(targetField(4, "range-m").value, "444");

  // One pending request locks all editors and prevents a second LP or RF write.
  let complete;
  server.mutation = (url, options, reply) => new Promise(resolve => { complete = () => resolve(reply(server.snapshot)); });
  const pending = targetField(3, "form").emit("submit");
  for (const id of [1, 2, 3, 4]) {
    assert(targetField(id, "apply").disabled && targetField(id, "revert").disabled && targetField(id, "range-m").disabled);
  }
  assert(field("rf-apply").disabled);
  const count = mutations(server).length;
  await app.mutate("rf");
  for (const id of [1, 2, 3, 4]) await targetField(id, "form").emit("submit");
  complete(); await pending;
  assert.equal(mutations(server).length, count);

  // Conflicts preserve every dirty editor and retrieve the current ETag.
  targetInput(3, "range-m", 555);
  server.mutation = (url, options, reply) => { server.snapshot.revision++; return reply({error: {message: "revision conflict"}}, 409); };
  await targetField(3, "form").emit("submit");
  assert.equal(targetField(3, "range-m").value, "555");
  assert.equal(targetField(2, "range-m").value, "222");
  assert.match(field("error-message").textContent, /다른 요청/);
  server.mutation = null;
  await targetField(3, "form").emit("submit");
  assert.equal(mutations(server).at(-1).headers["If-Match"], '"4"');

  // Exercise actual submit handlers for every panel, with distinct payloads.
  const all = fixture(); await all.app.start();
  for (const id of [1, 2, 3, 4]) {
    all.targetInput(id, "range-m", 100 + id);
    all.targetInput(id, "velocity-mps", -10 * id);
    all.targetInput(id, "gain-linear", id / 64);
    all.targetInput(id, "phase-deg", 15 * id);
    all.targetField(id, "enabled").checked = id % 2 === 1;
    await all.targetField(id, "enabled").emit("input");
  }
  // An invalid draft in target 2 cannot block target 1's independent form.
  all.targetField(2, "form").valid = false;
  await all.targetField(2, "form").emit("submit");
  assert.equal(mutations(all.server).length, 0);
  for (const id of [1, 2, 3, 4]) {
    if (id === 2) all.targetField(2, "form").valid = true;
    await all.targetField(id, "form").emit("submit");
    const sent = mutations(all.server).at(-1);
    assert.equal(sent.url, `/api/v1/rte/targets/${id}/load-param`);
    assert.deepEqual(sent.payload, {enabled: id % 2 === 1, range_m: 100 + id,
      radial_velocity_mps: -10 * id, gain_linear: id / 64, phase_offset_deg: 15 * id});
    assert.equal(mutations(all.server).length, id);
    for (const other of [1, 2, 3, 4]) assert.equal(all.targetField(other, "range-m").value, String(100 + other));
  }
  await all.app.mutate("target"); await all.app.mutate("target", 5);
  assert.equal(mutations(all.server).length, 4, "target identity is mandatory");

  // Abort after server commit: GET reconciliation without a duplicate POST.
  const timeout = fixture(); await timeout.app.start();
  timeout.targetInput(4, "range-m", 150);
  timeout.targetInput(1, "range-m", 175);
  timeout.server.mutation = (url, options) => new Promise((resolve, reject) => {
    timeout.server.snapshot.revision++;
    options.signal.addEventListener("abort", () => { const error = new Error("timeout"); error.name = "AbortError"; reject(error); });
  });
  const aborted = timeout.targetField(4, "form").emit("submit");
  const timer = [...timeout.timers.values()].find(item => item.ms === 8000);
  assert(timer); timer.fn(); await aborted;
  assert.equal(mutations(timeout.server).length, 1);
  assert.equal(timeout.targetField(4, "range-m").value, "150");
  assert.equal(timeout.targetField(1, "range-m").value, "175");
  assert.match(timeout.field("error-message").textContent, /자동 재전송하지 않습니다/);
  timeout.server.snapshot.degraded = true;
  timeout.server.rfCapsError = true;
  await timeout.app.refresh();
  assert.equal(timeout.field("degraded-banner").hidden, false);
  assert.match(timeout.field("error-message").textContent, /RF capability read failed/);
  for (const id of [1, 2, 3, 4]) {
    assert.equal(timeout.targetField(id, "apply").disabled, true);
    await timeout.app.mutate("target", id);
  }
  assert.equal(timeout.field("rf-apply").disabled, true);
  assert.equal(mutations(timeout.server).length, 1);
  timeout.server.snapshot.degraded = false;
  await timeout.app.refresh();
  assert.equal(timeout.targetField(4, "apply").disabled, true, "capability failure blocks writes");
  timeout.server.rfCapsError = false;
  await timeout.app.refresh();
  for (const id of [1, 2, 3, 4]) assert.equal(timeout.targetField(id, "apply").disabled, false);

  // Nominal RF input and full actual PLL readback remain separate.
  const pll = fixture();
  pll.server.snapshot.requested_rf = {carrier_hz: 2450000000};
  Object.assign(pll.server.snapshot.rf, {carrier_hz: 2449999998, rx_lo_hz: 2449999998, tx_lo_hz: 2449999998});
  await pll.app.start();
  assert.equal(pll.field("rf-carrier").value, "2450");
  assert.match(pll.field("actual-lo").textContent, /2449\.999998 MHz/);
  pll.input("rf-tx-gain", -10.25); await pll.app.mutate("rf");
  assert.equal(mutations(pll.server)[0].payload.carrier_hz, 2450000000);
  assert.equal(mutations(pll.server).length, 1);
  Object.assign(pll.server.snapshot.rf, {carrier_hz: 2499999998, rx_lo_hz: 2499999998, tx_lo_hz: 2499999998});
  await pll.app.refresh();
  assert.equal(pll.field("rf-carrier").value, "2499.999998");
  app.stop(); all.app.stop(); timeout.app.stop(); pll.app.stop();
  console.log("GUI behavioral tests passed: four independent forms, draft isolation, all four LP routes, serialized requests, RF-only updates, conflicts, timeout resync, degraded state");
}
if (require.main === module) run().catch(error => { console.error(error); process.exitCode = 1; });
else module.exports = {serverFixture};
