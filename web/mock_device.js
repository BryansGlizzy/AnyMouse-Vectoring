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
        p.set([P.PROTO_VERSION, 0, 4, 0], 0); v.setUint16(4, 103, true); p[6] = P.NUM_SLOTS; p[7] = 0;
        p.set([0x30, 0xed, 0xa0, 0xea, 0x6c, 0xab], 8); v.setUint32(14, (Date.now() - this.bootMs) >>> 0, true);
        return { reply: reply(cmd, seq, P.ST.OK, p), events };
      }
      case P.CMD.HELLO: {
        if (len !== P.HELLO_NONCE_LEN) return { reply: bad(P.ST.BAD_LEN), events };
        const p = new Uint8Array(24), v = new DataView(p.buffer);
        v.setUint32(0, P.HELLO_IDENT, true); p.set(pl.slice(0, P.HELLO_NONCE_LEN), 4);
        p.set([P.PROTO_VERSION, 0, 4, 0], 12); v.setUint16(16, P.CAP_VECTORED_MOUSE | P.CAP_MOUSE_HOST, true);
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
      default: return { reply: bad(P.ST.BAD_CMD), events };
    }
  }
}
