import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { P } from '../proto.js';
import { buildFrame, parseFrame, parseState, parseInfo, parseStats, parseMouse, parseMouseCaps, DongleClient, DongleError } from '../dongle.js';
import { MockHid, MOCK_RAZER, MOCK_GENERIC, MOCK_KEYBOARD } from '../mock_device.js';
import { importProfile, exportProfile, weightsValid, PRESETS } from '../presets.js';

const here = dirname(fileURLToPath(import.meta.url));
const fx = JSON.parse(readFileSync(join(here, 'fixtures', 'frames.json'), 'utf8'));
const hex = (s) => Uint8Array.from(Buffer.from(s, 'hex'));
const toHex = (u8) => Buffer.from(u8).toString('hex');

test('proto.js is up to date with common/amv_dongle_proto.h', () => {
  const gen = join(here, '..', '..', 'firmware', 'esp32p4-dongle', 'tools', 'gen_proto_js.py');
  assert.doesNotThrow(() => execFileSync('python3', ['-I', gen, '--check']), 'run gen_proto_js.py to regenerate web/proto.js');
});

test('buildFrame reproduces every request recorded from the real firmware', () => {
  for (const s of fx.steps) {
    const req = hex(s.request);
    const mine = buildFrame(req[3], req[4], req.slice(P.HEADER_SIZE, P.HEADER_SIZE + req[6]));
    // the recording of a deliberately malformed request ("bad_command", short SET) still has a valid header
    assert.equal(toHex(mine), s.request, s.name);
  }
});

test('the demo device answers exactly like the real firmware (replayed from the recording)', async () => {
  const mock = new MockHid(fx.pre);
  const seen = [];
  mock.addEventListener('inputreport', (e) => seen.push(new Uint8Array(e.data.buffer.slice(0))));
  for (const s of fx.steps) {
    seen.length = 0;
    await mock.sendReport(0, hex(s.request));
    await new Promise((r) => setTimeout(r, 0));
    const [reply, ...events] = seen;
    if (s.volatile) {
      assert.equal(reply[5], parseFrame(hex(s.reply)).status, `${s.name} status`);
      assert.equal(reply[6], hex(s.reply)[6], `${s.name} length`);
      continue;
    }
    assert.equal(toHex(reply), s.reply, `${s.name}: reply`);
    assert.deepEqual(events.map(toHex), s.events, `${s.name}: events`);
  }
});

test('client decoders parse the real replies', () => {
  const byName = Object.fromEntries(fx.steps.map((s) => [s.name, parseFrame(hex(s.reply))]));
  const st = parseState(byName.set_weights_ok.payload);
  assert.deepEqual(st.w, [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5]);
  assert.equal(st.bypass, false);
  assert.equal(byName.set_weights_stale.status, P.ST.STALE);
  assert.equal(byName.set_weights_bad_value.status, P.ST.BAD_VALUE);
  assert.equal(byName.set_weights_bad_len.status, P.ST.BAD_LEN);
  assert.equal(byName.load_empty_slot.status, P.ST.EMPTY);
  assert.equal(byName.bad_slot.status, P.ST.BAD_SLOT);
  assert.equal(byName.bad_command.status, P.ST.BAD_CMD);
  assert.equal(byName.get_slots_empty.payload[0], 0);
  assert.equal(byName.get_slots_one.payload[0], 0b100);
  const info = parseInfo(byName.info.payload);
  assert.equal(info.proto, P.PROTO_VERSION);
  assert.match(info.mac, /^([0-9a-f]{2}:){5}[0-9a-f]{2}$/);
  assert.equal(info.slots, 4);
  const stats = parseStats(byName.stats.payload);
  assert.ok(stats.setsOk >= 1 && stats.staleRejects >= 1 && stats.badValueRejects >= 1);
});

test('HELLO: the demo device answers the recorded real reply, and the client verifies it', async () => {
  const s = fx.steps.find((x) => x.name === 'hello');
  const nonce = hex(s.request).slice(P.HEADER_SIZE, P.HEADER_SIZE + P.HELLO_NONCE_LEN);
  const real = parseFrame(hex(s.reply));
  assert.equal(real.status, P.ST.OK);
  assert.equal(new DataView(real.payload.buffer, real.payload.byteOffset).getUint32(0, true), P.HELLO_IDENT);
  const mock = new MockHid(fx.pre);
  const c = new DongleClient(mock, { timeoutMs: 300 });
  await c.open();
  const h = await c.hello(nonce);
  assert.equal(h.legacy, false); assert.equal(h.vectoring, true); assert.equal(h.fw, '0.5.0');
  assert.match(h.mac, /^([0-9a-f]{2}:){5}[0-9a-f]{2}$/);
});

test('HELLO rejects a device that does not echo the nonce, and accepts legacy firmware via GET_INFO', async () => {
  const bad = new MockHid({});
  const orig = bad._handle.bind(bad);
  bad._handle = (f) => { const r = orig(f); if (f[3] === P.CMD.HELLO) r.reply[12] ^= 0xff; return r; };
  const c1 = new DongleClient(bad, { timeoutMs: 300 }); await c1.open();
  await assert.rejects(() => c1.hello(), (e) => e instanceof DongleError && e.status === -4);

  const old = new MockHid({});
  const orig2 = old._handle.bind(old);
  old._handle = (f) => { if (f[3] === P.CMD.HELLO) { const r = orig2(f); r.reply[3] = P.CMD.HELLO; r.reply[5] = P.ST.BAD_CMD; r.reply[6] = 0; return r; } return orig2(f); };
  const c2 = new DongleClient(old, { timeoutMs: 300 }); await c2.open();
  const h = await c2.hello();
  assert.equal(h.legacy, true);

  const silent = new MockHid({});
  silent.sendReport = async () => {};
  const c3 = new DongleClient(silent, { timeoutMs: 80 });
  await c3.open();
  await assert.rejects(() => c3.hello(), (e) => e.status === -4);
});

test('mouse info: the real Razer replies decode, and the demo device matches them', () => {
  const byName = Object.fromEntries(fx.steps.map((s) => [s.name, parseFrame(hex(s.reply))]));
  const m = parseMouse(byName.mouse_0.payload);
  assert.equal(m.tier, P.TIER.KNOWN); assert.equal(m.driver, P.DRV.RAZER);
  assert.equal(m.vid, 0x1532); assert.equal(m.pid, 0x0086); assert.equal(m.name, 'Razer Basilisk Ultimate');
  assert.equal(m.nominalHz, 1000); assert.equal(m.deviceHz, 1000); assert.equal(m.cpiSource, P.CPI.DEVICE);
  assert.ok(m.flags & P.MF.VERIFIED && m.flags & P.MF.CAN_SET_CPI);
  const c = parseMouseCaps(byName.mouse_caps_0.payload);
  assert.deepEqual(c.rates, MOCK_RAZER.caps.rates); assert.equal(c.cpiMax, MOCK_RAZER.caps.cpiMax); assert.equal(c.firmware, MOCK_RAZER.caps.firmware);
  assert.equal(byName.mouse_out_of_range.status, P.ST.NO_MOUSE);
  assert.equal(byName.mouse_set_rate_bad.status, P.ST.BAD_VALUE);
});

test('mouse list, descriptor download and setters over the demo device', async () => {
  const mock = new MockHid({ mice: [MOCK_KEYBOARD, MOCK_GENERIC, MOCK_RAZER] });
  const c = new DongleClient(mock, { timeoutMs: 300 });
  await c.open();
  const list = await c.listMice();
  assert.deepEqual(list.map((m) => m.tier), [P.TIER.GENERIC, P.TIER.KNOWN, P.TIER.IGNORED]);   // ignored devices last
  assert.equal(list[2].reason, P.IGN.NOT_MOUSE);
  const razer = list.find((m) => m.driver === P.DRV.RAZER);
  const desc = await c.getMouseDescriptor(razer.index);
  assert.deepEqual(Array.from(desc), Array.from(MOCK_RAZER.desc));   // three 48-byte pieces put back together
  const events = [];
  c.addEventListener('mouse', (e) => events.push(e.detail));
  let r = await c.setMouseCpi(razer.index, 3200);
  assert.ok(r.ok); assert.equal(r.mouse.cpi, 3200); assert.equal(r.mouse.cpiSource, P.CPI.DEVICE);
  r = await c.setMousePolling(razer.index, 500);
  assert.ok(r.ok); assert.equal(r.mouse.deviceHz, 500);
  assert.equal((await c.setMousePolling(razer.index, 777)).status, P.ST.BAD_VALUE);
  r = await c.setMouseCpi(0, 800);                        // generic: remembered as a label
  assert.ok(r.ok); assert.equal(r.mouse.cpiSource, P.CPI.USER);
  assert.equal((await c.setMousePolling(0, 500)).status, P.ST.UNSUPPORTED);
  assert.equal(await c.getMouseCaps(0), null);
  await new Promise((res) => setTimeout(res, 5));
  assert.equal(events.length, 3); assert.equal(events[0].change, P.MEV.UPDATED);
});

test('listMice() on firmware without mouse commands returns null', async () => {
  const old = new MockHid({});
  const orig = old._handle.bind(old);
  old._handle = (f) => { if (f[3] === P.CMD.GET_MOUSE) { const r = orig(f); r.reply[5] = P.ST.BAD_CMD; r.reply[6] = 0; return r; } return orig(f); };
  const c = new DongleClient(old, { timeoutMs: 300 }); await c.open();
  assert.equal(await c.listMice(), null);
});

// ---------- client behavior over the demo device ----------
async function connected(init, opts) {
  const mock = new MockHid(init, opts);
  const c = new DongleClient(mock, { timeoutMs: 300 });
  await c.open();
  return { mock, c };
}

test('set / stale / invalid flow and event delivery', async () => {
  const { c } = await connected({ gen: 5 });
  const events = [];
  c.addEventListener('state', (e) => events.push(e.detail));
  const w = [0.6, 0.7, 0.8, 0.9, 1.1, 1.2, 1.3, 1.4].map(Math.fround);

  const ok = await c.setWeights(w, 5);
  assert.ok(ok.ok); assert.equal(ok.state.gen, 6); assert.deepEqual(ok.state.w, w);

  const stale = await c.setWeights(w, 5);
  assert.equal(stale.status, P.ST.STALE); assert.equal(stale.state.gen, 6);   // carries the current state

  const bad = await c.setWeights([...w.slice(0, 7), NaN], 6);
  assert.equal(bad.status, P.ST.BAD_VALUE);

  await new Promise((r) => setTimeout(r, 10));
  assert.equal(events.length, 1);
  assert.equal(events[0].source, P.SRC.HOST); assert.equal(events[0].gen, 6);
});

test('device-originated change arrives as an event', async () => {
  const { c } = await connected({});
  const got = new Promise((res) => c.addEventListener('state', (e) => res(e.detail)));
  const p = new Uint8Array(7); const v = new DataView(p.buffer); p[0] = 5; v.setFloat32(1, 2.5, true); v.setUint16(5, 5, true);
  assert.equal((await c.request(P.CMD.TEST_SELF_CHANGE, p)).status, P.ST.OK);
  const ev = await got;
  assert.equal(ev.source, P.SRC.DEVICE); assert.equal(ev.w[5], 2.5);
});

test('slots, bypass, stats and the scaling probe', async () => {
  const { c } = await connected({});
  assert.equal(await c.getSlotsMask(), 0);
  await c.setWeights([0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5]);
  assert.ok((await c.saveSlot(1)).ok);
  assert.equal(await c.getSlotsMask(), 0b10);
  assert.equal((await c.loadSlot(3)).status, P.ST.EMPTY);
  assert.equal((await c.testScale(100, 0, true)).dx, 50);
  await c.setBypass(true);
  assert.equal((await c.testScale(100, 0, true)).dx, 100);
  assert.equal((await c.getStats()).saves, 1);
  const info = await c.getInfo();
  assert.equal(info.proto, P.PROTO_VERSION);
});

test('concurrent requests are matched to the right replies', async () => {
  const { c } = await connected({}, { latencyMs: 5 });
  const rs = await Promise.all([c.getState(), c.getSlotsMask(), c.getInfo(), c.getStats(), c.getState()]);
  assert.equal(rs[0].gen, rs[4].gen); assert.equal(rs[1], 0); assert.equal(rs[2].proto, P.PROTO_VERSION);
});

test('a silent device times out instead of hanging, and close() rejects what is pending', async () => {
  const silent = Object.assign(new EventTarget(), { opened: true, async open() {}, async close() { this.opened = false; }, async sendReport() {} });
  const c = new DongleClient(silent, { timeoutMs: 40 });
  await c.open();
  await assert.rejects(c.getState(), (e) => e instanceof DongleError && e.status === -2);
  const pending = c.getState();
  const closed = c.close();
  await assert.rejects(pending, (e) => e.status === -1);
  await closed;
});

test('a transport failure surfaces as a DongleError', async () => {
  const broken = Object.assign(new EventTarget(), { opened: true, async open() {}, async close() {}, async sendReport() { throw new Error('device unplugged'); } });
  const c = new DongleClient(broken);
  await c.open();
  await assert.rejects(c.getState(), (e) => e instanceof DongleError && /unplugged/.test(e.message));
});

test('wrong protocol version is refused', async () => {
  const mock = new MockHid({});
  const orig = mock._handle.bind(mock);
  mock._handle = (f) => { const r = orig(f); if (f[3] === P.CMD.GET_INFO) r.reply[8] = 99; return r; };
  const c = new DongleClient(mock);
  await c.open();
  await assert.rejects(c.getInfo(), /unsupported protocol version 99/);
});

// ---------- profiles ----------
test('profile export/import round trip', () => {
  const w = [0.5, 1, 1.5, 2, 2.5, 0.75, 1.25, 1.75];
  const back = importProfile(exportProfile(w, 3200));
  assert.deepEqual(back.weights, w); assert.equal(back.cpi, 3200); assert.equal(back.note, '');
});

test('Linux-engine profile keeps its shape and averages to 1.0', () => {
  const linux = JSON.stringify({ hardware_cpi: 3200, octant_weights: [0.25, 0.35, 0.45, 0.35, 0.25, 0.25, 0.25, 0.25] });
  const { weights, note } = importProfile(linux);
  assert.ok(weightsValid(weights)); assert.match(note, /Linux-engine/);
  assert.ok(Math.abs(weights.reduce((a, b) => a + b) / 8 - 1) < 0.02);
  assert.ok(weights[2] > weights[0]);   // the vertical boost is still there
});

test('bad profiles are rejected with a reason', () => {
  assert.throws(() => importProfile('nope'), /JSON/);
  assert.throws(() => importProfile('{}'), /octant_weights/);
  assert.throws(() => importProfile(JSON.stringify({ octant_weights: [1, 2, 3] })), /8 numbers/);
  assert.throws(() => importProfile(JSON.stringify({ format: 'anymouse-dongle-profile', octant_weights: [9, 1, 1, 1, 1, 1, 1, 1] })), /between/);
  assert.throws(() => importProfile(JSON.stringify({ octant_weights: [0, 0, 0, 0, 0, 0, 0, 0] })), /positive/);
});

test('every preset is valid for the firmware', () => {
  for (const p of PRESETS) assert.ok(weightsValid(p.w), p.id);
});

test('auto-save writes the active slot after the debounce, like the firmware', async () => {
  const { mock, c } = await connected({}, { autosaveMs: 30 });
  assert.equal(await c.getSlotsMask(), 0);
  await c.setWeights([0.7, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7]);
  assert.equal(await c.getSlotsMask(), 0);                    // not yet
  await new Promise((r) => setTimeout(r, 80));
  assert.equal(await c.getSlotsMask(), 0b1);                  // slot 0 is the active slot
  assert.equal(Math.fround(0.7), mock.slots[0][0]);
});
