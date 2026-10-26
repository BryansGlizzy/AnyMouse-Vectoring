// A JavaScript twin of the dongle firmware's control plane. Used by the `?demo` mode of the web
// app and by the Node tests. tests/protocol.test.mjs checks it against frames recorded from the
// real board (tests/fixtures/frames.json), so it is not just a guess at the behavior.
import { P } from './proto.js';
import { buildFrame, parseFrame } from './dongle.js';

const f32 = Math.fround;

function octantScale(w, rem, dx, dy) {
  if (dx === 0 && dy === 0) return { ox: 0, oy: 0, oct: 0 };
  let th = Math.atan2(-dy, dx);
  if (th < 0) th += 2 * Math.PI;
  const pos = th * 4 / Math.PI;
  const k0 = Math.trunc(pos) & 7, k1 = (k0 + 1) & 7;
  const t = pos - Math.trunc(pos);
  const sm = (1 - Math.cos(t * Math.PI)) / 2;
  const scale = w[k0] * (1 - sm) + w[k1] * sm;
  const ex = f32(dx * scale + rem.x), ey = f32(dy * scale + rem.y);
  const ix = Math.trunc(ex), iy = Math.trunc(ey);
  rem.x = f32(ex - ix); rem.y = f32(ey - iy);
  return { ox: ix, oy: iy, oct: Math.trunc(pos + 0.5) & 7 };
}

// The Razer Basilisk Ultimate as the real dongle reports it (firmware 0.5.0, 2026-10-26), including
// the report descriptor it captured (tests/descriptors/razer_basilisk_ultimate_1532_0086_if0.hex).
const RAZER_DESC = Uint8Array.of(0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x05, 0x81, 0x02, 0x75, 0x01, 0x95, 0x03, 0x81, 0x03, 0x06, 0x00, 0xff, 0x09, 0x40, 0x75, 0x08, 0x95, 0x02, 0x15, 0x81, 0x25, 0x7f, 0x81, 0x02, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0x09, 0x30, 0x09, 0x31, 0x16, 0x00, 0x80, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06, 0x06, 0x00, 0xff, 0x09, 0xf1, 0x75, 0x08, 0x95, 0x05, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81, 0x00, 0xc0, 0x06, 0x00, 0xff, 0x09, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x08, 0x95, 0x5a, 0xb1, 0x01, 0xc0);
export const MOCK_RAZER = {
  tier: P.TIER.KNOWN, flags: P.MF.WHEEL | P.MF.VERIFIED | P.MF.CAN_SET_CPI | P.MF.CAN_SET_RATE,
  vid: 0x1532, pid: 0x0086, speed: 1, iface: 0, nominalHz: 1000, measuredHz: 0, deviceHz: 1000, cpi: 1600,
  cpiSource: P.CPI.DEVICE, battery: 100, buttons: 5, xBits: 16, driver: P.DRV.RAZER, reason: 0, name: 'Razer Basilisk Ultimate',
  caps: { rates: [125, 500, 1000], cpiMin: 100, cpiMax: 20000, cpiStep: 50, firmware: '1.7' }, desc: RAZER_DESC,
};
export const MOCK_GENERIC = {
  tier: P.TIER.GENERIC, flags: P.MF.WHEEL, vid: 0x046d, pid: 0xc077, speed: 1, iface: 0, nominalHz: 125, measuredHz: 0,
  deviceHz: 0, cpi: 0, cpiSource: P.CPI.NONE, battery: null, buttons: 3, xBits: 8, driver: P.DRV.NONE, reason: 0,
  name: 'USB Optical Mouse', caps: null, desc: Uint8Array.of(0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0xc0),
};
export const MOCK_KEYBOARD = {
  tier: P.TIER.IGNORED, flags: 0, vid: 0x05ac, pid: 0x024f, speed: 1, iface: 0, nominalHz: 0, measuredHz: 0, deviceHz: 0, cpi: 0,
  cpiSource: P.CPI.NONE, battery: null, buttons: 0, xBits: 0, driver: P.DRV.NONE, reason: P.IGN.NOT_MOUSE, name: 'USB Keyboard', caps: null, desc: null,
};

function encodeMouse(m, index, count) {
  const p = new Uint8Array(56), v = new DataView(p.buffer);
  p[0] = index; p[1] = count; p[2] = m.tier; p[3] = m.flags;
  v.setUint16(4, m.vid, true); v.setUint16(6, m.pid, true); p[8] = m.speed; p[9] = m.iface;
  v.setUint16(10, m.nominalHz, true); v.setUint16(12, m.measuredHz, true); v.setUint16(14, m.deviceHz, true);
  v.setUint16(16, m.cpi, true); p[18] = m.cpiSource; p[19] = m.battery ?? 0xff; p[20] = m.buttons; p[21] = m.xBits;
  p[22] = m.driver; p[23] = m.reason; v.setUint16(24, m.uid, true);
  for (let i = 0; i < Math.min(m.name.length, 29); i++) p[26 + i] = m.name.charCodeAt(i);
  return p;
}

function encodeCaps(m, index) {
  const p = new Uint8Array(42), v = new DataView(p.buffer), c = m.caps;
  p[0] = index; p[1] = m.driver; p[2] = c.rates.length;
  c.rates.forEach((r, i) => v.setUint16(4 + i * 2, r, true));
  v.setUint16(20, c.cpiMin, true); v.setUint16(22, c.cpiMax, true); v.setUint16(24, c.cpiStep, true);
  for (let i = 0; i < Math.min(c.firmware.length, 15); i++) p[26 + i] = c.firmware.charCodeAt(i);
  return p;
}

export class MockHid extends EventTarget {
  /** init: {gen, w, bypass, slot, filledSlots:[...]} to start from a recorded device state. */
  constructor(init = {}, { latencyMs = 0, autosaveMs = 2000 } = {}) {
    super();
    this.opened = false;
    this.latencyMs = latencyMs;
    this.autosaveMs = autosaveMs;
    this._saveTimer = null;
    this.gen = init.gen ?? 1;
    this.w = (init.w ?? [1, 1, 1, 1, 1, 1, 1, 1]).map(f32);
    this.bypass = !!init.bypass;
    this.slot = init.slot ?? 0;
    this.slots = Array.from({ length: P.NUM_SLOTS }, (_, i) => ((init.filledSlots ?? []).includes(i) ? this.w.slice() : null));
    this.rem = { x: 0, y: 0 };
    this.stats = { setsOk: 0, staleRejects: 0, badValueRejects: 0, saves: 0, loads: 0, eventsSent: 0, eventsDropped: 0, testScales: 0 };
    this.bootMs = Date.now();
    this.sent = [];   // every host->device frame, for assertions
    // Attached mice in list order (claimed first, ignored last), like the firmware
    this.mice = (init.mice ?? [MOCK_RAZER]).map((m, i) => ({ ...m, caps: m.caps && { ...m.caps, rates: [...m.caps.rates] }, uid: i + 1 }))
      .sort((a, b) => (a.tier === P.TIER.IGNORED) - (b.tier === P.TIER.IGNORED));
  }

  async open() { this.opened = true; }
  async close() { this.opened = false; }

  async sendReport(reportId, data) {
    const frame = new Uint8Array(data);
    this.sent.push(frame);
    const { reply, events } = this._handle(frame);
    const out = (f) => { const b = new Uint8Array(f); const ev = new Event('inputreport'); ev.data = new DataView(b.buffer); ev.reportId = 0; this.dispatchEvent(ev); };
    const go = () => { out(reply); for (const e of events) { this.stats.eventsSent++; out(e); } };
    if (this.latencyMs > 0) setTimeout(go, this.latencyMs); else queueMicrotask(go);
  }

  _statePayload() {
    const p = new Uint8Array(38), v = new DataView(p.buffer);
    v.setUint32(0, this.gen, true);
    this.w.forEach((x, i) => v.setFloat32(4 + i * 4, x, true));
    p[36] = this.bypass ? 1 : 0; p[37] = this.slot;
    return p;
  }

  _event(source) {
    const p = new Uint8Array(39);
    p.set(this._statePayload()); p[38] = source;
    return buildFrame(P.CMD.EVENT_STATE, 0, p);
  }

  _valid(w) { return w.every((x) => Number.isFinite(x) && x >= f32(P.WEIGHT_MIN) && x <= P.WEIGHT_MAX); }

  _changed(source, events) { this.gen++; events.push(this._event(source)); }

  // Like the firmware: a weight change is written to the active slot 2 s after the last change.
  _armAutosave() {
    clearTimeout(this._saveTimer);
    this._saveTimer = setTimeout(() => { this.slots[this.slot] = this.w.slice(); this.stats.saves++; }, this.autosaveMs);
    this._saveTimer.unref?.();
  }

  _handle(frame) {
    const f = parseFrame(frame);
    const reply = (cmd, seq, status, payload) => { const r = buildFrame(cmd, seq, payload); r[5] = status; return r; };
    const cmd = frame[3], seq = frame[4];
    const events = [];
    if (!f) return { reply: reply(cmd, seq, P.ST.BAD_MAGIC), events };
    const len = frame[6];
    if (len > P.PAYLOAD_MAX) return { reply: reply(cmd, seq, P.ST.BAD_LEN), events };
    const pl = f.payload;
    const dv = new DataView(pl.buffer, pl.byteOffset, pl.byteLength);
    const state = (status) => reply(cmd, seq, status, this._statePayload());
    const bad = (s) => reply(cmd, seq, s);

    switch (cmd) {
      case P.CMD.GET_INFO: {
        const p = new Uint8Array(18), v = new DataView(p.buffer);
        p.set([P.PROTO_VERSION, 0, 5, 0], 0); v.setUint16(4, 103, true); p[6] = P.NUM_SLOTS; p[7] = this.mice.filter((m) => m.tier !== P.TIER.IGNORED).length;
        p.set([0x30, 0xed, 0xa0, 0xea, 0x6c, 0xab], 8); v.setUint32(14, (Date.now() - this.bootMs) >>> 0, true);
        return { reply: reply(cmd, seq, P.ST.OK, p), events };
      }
      case P.CMD.HELLO: {
        if (len !== P.HELLO_NONCE_LEN) return { reply: bad(P.ST.BAD_LEN), events };
        const p = new Uint8Array(24), v = new DataView(p.buffer);
        v.setUint32(0, P.HELLO_IDENT, true); p.set(pl.slice(0, P.HELLO_NONCE_LEN), 4);
        p.set([P.PROTO_VERSION, 0, 5, 0], 12); v.setUint16(16, P.CAP_VECTORED_MOUSE | P.CAP_MOUSE_HOST, true);
        p.set([0x30, 0xed, 0xa0, 0xea, 0x6c, 0xab], 18);
        return { reply: reply(cmd, seq, P.ST.OK, p), events };
      }
      case P.CMD.GET_WEIGHTS: return { reply: state(P.ST.OK), events };
      case P.CMD.SET_WEIGHTS: {
        if (len !== 36) return { reply: bad(P.ST.BAD_LEN), events };
        const base = dv.getUint32(0, true);
        const w = Array.from({ length: 8 }, (_, i) => dv.getFloat32(4 + i * 4, true));
        if (!this._valid(w)) { this.stats.badValueRejects++; return { reply: state(P.ST.BAD_VALUE), events }; }
        if (base !== P.GEN_ANY && base !== this.gen) { this.stats.staleRejects++; return { reply: state(P.ST.STALE), events }; }
        this.w = w; this.stats.setsOk++; this._changed(P.SRC.HOST, events); this._armAutosave();
        return { reply: state(P.ST.OK), events };
      }
      case P.CMD.SAVE_PROFILE: {
        if (len !== 1) return { reply: bad(P.ST.BAD_LEN), events };
        if (pl[0] >= P.NUM_SLOTS) return { reply: state(P.ST.BAD_SLOT), events };
        this.slots[pl[0]] = this.w.slice(); this.slot = pl[0]; this.stats.saves++;
        return { reply: state(P.ST.OK), events };
      }
      case P.CMD.LOAD_PROFILE: {
        if (len !== 1) return { reply: bad(P.ST.BAD_LEN), events };
        if (pl[0] >= P.NUM_SLOTS) return { reply: state(P.ST.BAD_SLOT), events };
        if (!this.slots[pl[0]]) return { reply: state(P.ST.EMPTY), events };
        this.w = this.slots[pl[0]].slice(); this.slot = pl[0]; this.stats.loads++; this._changed(P.SRC.LOAD, events);
        return { reply: state(P.ST.OK), events };
      }
      case P.CMD.SET_BYPASS:
        if (len !== 1) return { reply: bad(P.ST.BAD_LEN), events };
        this.bypass = pl[0] !== 0; this._changed(P.SRC.HOST, events);
        return { reply: state(P.ST.OK), events };
      case P.CMD.GET_SLOTS: return { reply: reply(cmd, seq, P.ST.OK, Uint8Array.of(this.slots.reduce((m, s, i) => m | (s ? 1 << i : 0), 0))), events };
      case P.CMD.GET_STATS: {
        const p = new Uint8Array(32), v = new DataView(p.buffer);
        Object.values(this.stats).forEach((x, i) => v.setUint32(i * 4, x, true));
        return { reply: reply(cmd, seq, P.ST.OK, p), events };
      }
      case P.CMD.TEST_SCALE: {
        if (len !== 5) return { reply: bad(P.ST.BAD_LEN), events };
        if (pl[4] & P.TEST_RESET_REM) this.rem = { x: 0, y: 0 };
        const w = this.bypass ? Array(8).fill(1) : this.w;
        const { ox, oy, oct } = octantScale(w, this.rem, dv.getInt16(0, true), dv.getInt16(2, true));
        const p = new Uint8Array(17), v = new DataView(p.buffer);
        v.setInt32(0, ox, true); v.setInt32(4, oy, true); v.setFloat32(8, this.rem.x, true); v.setFloat32(12, this.rem.y, true); p[16] = oct;
        this.stats.testScales++;
        return { reply: reply(cmd, seq, P.ST.OK, p), events };
      }
      case P.CMD.TEST_SELF_CHANGE: {
        if (len !== 7) return { reply: bad(P.ST.BAD_LEN), events };
        const oct = pl[0], value = dv.getFloat32(1, true), delay = dv.getUint16(5, true);
        if (oct > 7) return { reply: bad(P.ST.BAD_VALUE), events };
        setTimeout(() => {
          const w = this.w.slice(); w[oct] = f32(value);
          if (!this._valid(w)) return;
          this.w = w; this.stats.setsOk++; const ev = []; this._changed(P.SRC.DEVICE, ev); this._armAutosave();
          for (const e of ev) { this.stats.eventsSent++; const b = new Uint8Array(e); const x = new Event('inputreport'); x.data = new DataView(b.buffer); this.dispatchEvent(x); }
        }, delay || 1);
        return { reply: reply(cmd, seq, P.ST.OK), events };
      }
      case P.CMD.FACTORY_RESET:
        clearTimeout(this._saveTimer);
        this.w = Array(8).fill(1); this.bypass = false; this.slot = 0; this.slots.fill(null); this._changed(P.SRC.HOST, events);
        return { reply: state(P.ST.OK), events };
      case P.CMD.REBOOT: return { reply: reply(cmd, seq, P.ST.OK), events };
      case P.CMD.GET_MOUSE: {
        if (len < 1) return { reply: bad(P.ST.BAD_LEN), events };
        const m = this.mice[pl[0]];
        if (!m) return { reply: reply(cmd, seq, P.ST.NO_MOUSE, Uint8Array.of(pl[0], this.mice.length)), events };
        return { reply: reply(cmd, seq, P.ST.OK, encodeMouse(m, pl[0], this.mice.length)), events };
      }
      case P.CMD.GET_MOUSE_CAPS: {
        if (len < 1) return { reply: bad(P.ST.BAD_LEN), events };
        const m = this.mice[pl[0]];
        if (!m) return { reply: bad(P.ST.NO_MOUSE), events };
        if (m.tier !== P.TIER.KNOWN) return { reply: bad(P.ST.UNSUPPORTED), events };
        return { reply: reply(cmd, seq, P.ST.OK, encodeCaps(m, pl[0])), events };
      }
      case P.CMD.GET_MOUSE_DESC: {
        if (len < 4) return { reply: bad(P.ST.BAD_LEN), events };
        const m = this.mice[pl[0]], off = dv.getUint16(2, true);
        if (!m) return { reply: bad(P.ST.NO_MOUSE), events };
        if (!m.desc) return { reply: bad(P.ST.UNSUPPORTED), events };
        if (off > m.desc.length) return { reply: bad(P.ST.BAD_VALUE), events };
        const p = new Uint8Array(56), v = new DataView(p.buffer), n = Math.min(48, m.desc.length - off);
        p[0] = pl[0]; v.setUint16(2, m.desc.length, true); v.setUint16(4, off, true); p[6] = n;
        p.set(m.desc.slice(off, off + n), 8);
        return { reply: reply(cmd, seq, P.ST.OK, p), events };
      }
      case P.CMD.SET_MOUSE_CPI:
      case P.CMD.SET_MOUSE_POLLING: {
        if (len < 4) return { reply: bad(P.ST.BAD_LEN), events };
        const i = pl[0], m = this.mice[i], value = dv.getUint16(2, true), isCpi = cmd === P.CMD.SET_MOUSE_CPI;
        if (!m) return { reply: reply(cmd, seq, P.ST.NO_MOUSE, Uint8Array.of(i, this.mice.length)), events };
        const answer = (st) => reply(cmd, seq, st, encodeMouse(m, i, this.mice.length));
        const mouseEvent = () => { const e = new Uint8Array(4); e[0] = this.mice.length; e[1] = P.MEV.UPDATED; new DataView(e.buffer).setUint16(2, m.uid, true); events.push(buildFrame(P.CMD.EVENT_MOUSE, 0, e)); };
        if (m.tier === P.TIER.IGNORED) return { reply: answer(P.ST.UNSUPPORTED), events };
        if (m.tier !== P.TIER.KNOWN) {
          if (!isCpi) return { reply: answer(P.ST.UNSUPPORTED), events };
          if (value && (value < 50 || value > 64000)) return { reply: answer(P.ST.BAD_VALUE), events };
          m.cpi = value; m.cpiSource = value ? P.CPI.USER : P.CPI.NONE; mouseEvent();
          return { reply: answer(P.ST.OK), events };
        }
        if (!(m.flags & (isCpi ? P.MF.CAN_SET_CPI : P.MF.CAN_SET_RATE))) return { reply: answer(P.ST.UNSUPPORTED), events };
        if (isCpi ? value < m.caps.cpiMin || value > m.caps.cpiMax : !m.caps.rates.includes(value)) return { reply: answer(P.ST.BAD_VALUE), events };
        if (isCpi) m.cpi = value; else m.deviceHz = value;
        mouseEvent();
        return { reply: answer(P.ST.OK), events };
      }
      default: return { reply: bad(P.ST.BAD_CMD), events };
    }
  }
}
