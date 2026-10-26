// AnyMouse dongle protocol client. Transport-agnostic: it needs only an object shaped like a WebHID
// HIDDevice (open/close/sendReport/addEventListener('inputreport')/opened), so the same code runs on
// real WebHID, on the built-in demo device, and in the Node tests.
import { P, ST_NAMES } from './proto.js';

export const statusName = (s) => ST_NAMES[s] ?? `status ${s}`;

export class DongleError extends Error {
  constructor(message, status, state) {
    super(message);
    this.status = status;
    this.state = state;
  }
}

// ---------- frame encoding ----------
export function buildFrame(cmd, seq, payload = new Uint8Array(0)) {
  if (payload.length > P.PAYLOAD_MAX) throw new RangeError('payload too long');
  const f = new Uint8Array(P.FRAME_SIZE);
  f[0] = P.MAGIC0; f[1] = P.MAGIC1; f[2] = P.PROTO_VERSION;
  f[3] = cmd; f[4] = seq; f[5] = 0; f[6] = payload.length; f[7] = 0;
  f.set(payload, P.HEADER_SIZE);
  return f;
}

export function parseFrame(u8) {
  if (u8.length < P.HEADER_SIZE || u8[0] !== P.MAGIC0 || u8[1] !== P.MAGIC1) return null;
  const len = Math.min(u8[6], P.PAYLOAD_MAX, u8.length - P.HEADER_SIZE);
  return { ver: u8[2], cmd: u8[3], seq: u8[4], status: u8[5], payload: u8.slice(P.HEADER_SIZE, P.HEADER_SIZE + len) };
}

const view = (u8) => new DataView(u8.buffer, u8.byteOffset, u8.byteLength);

// state payload: u32 gen, f32 w[8], u8 bypass, u8 active_slot (38 bytes)
export function parseState(p) {
  const v = view(p);
  const w = [];
  for (let i = 0; i < 8; i++) w.push(v.getFloat32(4 + i * 4, true));
  return { gen: v.getUint32(0, true), w, bypass: p[36] !== 0, slot: p[37] };
}

// event payload: state + u8 source (39 bytes)
export function parseEvent(p) {
  return { ...parseState(p), source: p[38] };
}

export function encodeSetWeights(baseGen, w) {
  const p = new Uint8Array(36);
  const v = view(p);
  v.setUint32(0, baseGen >>> 0, true);
  for (let i = 0; i < 8; i++) v.setFloat32(4 + i * 4, w[i], true);
  return p;
}

export function parseInfo(p) {
  const v = view(p);
  const mac = Array.from(p.slice(8, 14), (b) => b.toString(16).padStart(2, '0')).join(':');
  return { proto: p[0], fw: `${p[1]}.${p[2]}.${p[3]}`, chipRev: v.getUint16(4, true), slots: p[6], hasMouse: p[7] !== 0, mac, uptimeMs: v.getUint32(14, true) };
}

export function parseStats(p) {
  const v = view(p);
  const k = ['setsOk', 'staleRejects', 'badValueRejects', 'saves', 'loads', 'eventsSent', 'eventsDropped', 'testScales'];
  return Object.fromEntries(k.map((n, i) => [n, v.getUint32(i * 4, true)]));
}

export function parseTestScale(p) {
  const v = view(p);
  return { dx: v.getInt32(0, true), dy: v.getInt32(4, true), remX: v.getFloat32(8, true), remY: v.getFloat32(12, true), octant: p[16] };
}

// ---------- client ----------
export class DongleClient extends EventTarget {
  constructor(hid, { timeoutMs = 1500 } = {}) {
    super();
    this.hid = hid;
    this.timeoutMs = timeoutMs;
    this.seq = 0;
    this.pending = new Map();
    this.lastRttMs = null;
    this._onReport = this._onReport.bind(this);
  }

  async open() {
    this.hid.addEventListener('inputreport', this._onReport);
    if (!this.hid.opened) await this.hid.open();
  }

  async close() {
    this.hid.removeEventListener('inputreport', this._onReport);
    for (const [, p] of this.pending) { clearTimeout(p.timer); p.reject(new DongleError('connection closed', -1)); }
    this.pending.clear();
    if (this.hid.opened) await this.hid.close();
  }

  _nextSeq() {
    for (let i = 0; i < 255; i++) {
      this.seq = (this.seq % 255) + 1;   // 1..255, never 0
      if (!this.pending.has(this.seq)) return this.seq;
    }
    throw new DongleError('too many requests in flight', -1);
  }

  _onReport(ev) {
    const f = parseFrame(new Uint8Array(ev.data.buffer, ev.data.byteOffset, ev.data.byteLength));
    if (!f) return;
    if (f.cmd === P.CMD.EVENT_STATE) {
      this.dispatchEvent(new CustomEvent('state', { detail: parseEvent(f.payload) }));
      return;
    }
    const p = this.pending.get(f.seq);
    if (!p || p.cmd !== f.cmd) return;
    this.pending.delete(f.seq);
    clearTimeout(p.timer);
    this.lastRttMs = performance.now() - p.t0;
    p.resolve(f);
  }

  /** Low-level: resolves with {status, payload} for any status. Rejects on timeout or transport failure. */
  async request(cmd, payload) {
    const seq = this._nextSeq();
    const frame = buildFrame(cmd, seq, payload);
    const reply = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(seq);
        reject(new DongleError(`no reply to command 0x${cmd.toString(16)}`, -2));
      }, this.timeoutMs);
      this.pending.set(seq, { cmd, resolve, reject, timer, t0: performance.now() });
    });
    try {
      await this.hid.sendReport(0, frame);
    } catch (e) {
      const p = this.pending.get(seq);
      if (p) { clearTimeout(p.timer); this.pending.delete(seq); }
      reply.catch(() => {});
      throw new DongleError(`send failed: ${e.message}`, -1);
    }
    return reply;
  }

  // ---- typed API: state-bearing calls return {status, ok, state}; the state is the result, or the
  // ---- device's current state when the call was refused (STALE / BAD_VALUE / ...).
  async _stateCall(cmd, payload) {
    const r = await this.request(cmd, payload);
    const state = r.payload.length >= 38 ? parseState(r.payload) : null;
    return { status: r.status, ok: r.status === P.ST.OK, state };
  }

  async getInfo() {
    const r = await this.request(P.CMD.GET_INFO);
    if (r.status !== P.ST.OK) throw new DongleError(statusName(r.status), r.status);
    const info = parseInfo(r.payload);
    if (info.proto !== P.PROTO_VERSION) throw new DongleError(`unsupported protocol version ${info.proto} (this page speaks ${P.PROTO_VERSION})`, -3);
    return info;
  }
  /**
   * Prove the device is an AnyMouse dongle: send a random nonce and require it back with the
   * identity word. Identity never depends on USB VID:PID. Firmware older than HELLO answers
   * BAD_CMD; a well-formed GET_INFO reply then counts (flagged legacy).
   */
  async hello(nonce = crypto.getRandomValues(new Uint8Array(P.HELLO_NONCE_LEN))) {
    const notDongle = (why) => new DongleError(`not an AnyMouse dongle (${why})`, -4);
    let r;
    try { r = await this.request(P.CMD.HELLO, nonce); }
    catch (e) { if (e.status === -2) throw notDongle('no reply'); throw e; }
    if (r.status === P.ST.BAD_CMD) {
      const info = await this.getInfo().catch(() => { throw notDongle('no valid reply to GET_INFO'); });
      return { legacy: true, vectoring: true, caps: P.CAP_VECTORED_MOUSE, fw: info.fw, mac: info.mac, proto: info.proto };
    }
    if (r.status !== P.ST.OK || r.payload.length < 24) throw notDongle('bad handshake reply');
    const v = view(r.payload);
    if (v.getUint32(0, true) !== P.HELLO_IDENT) throw notDongle('wrong identity word');
    for (let i = 0; i < P.HELLO_NONCE_LEN; i++) if (r.payload[4 + i] !== nonce[i]) throw notDongle('nonce not echoed');
    const proto = r.payload[12];
    if (proto !== P.PROTO_VERSION) throw new DongleError(`unsupported protocol version ${proto} (this page speaks ${P.PROTO_VERSION})`, -3);
    const caps = v.getUint16(16, true);
    return {
      legacy: false, proto, caps,
      vectoring: (caps & P.CAP_VECTORED_MOUSE) !== 0, mouseHost: (caps & P.CAP_MOUSE_HOST) !== 0,
      fw: `${r.payload[13]}.${r.payload[14]}.${r.payload[15]}`,
      mac: Array.from(r.payload.slice(18, 24), (b) => b.toString(16).padStart(2, '0')).join(':'),
    };
  }
  async getState() {
    const r = await this._stateCall(P.CMD.GET_WEIGHTS);
    if (!r.ok) throw new DongleError(statusName(r.status), r.status);
    return r.state;
  }
  setWeights(w, baseGen = P.GEN_ANY) { return this._stateCall(P.CMD.SET_WEIGHTS, encodeSetWeights(baseGen, w)); }
  setBypass(on) { return this._stateCall(P.CMD.SET_BYPASS, Uint8Array.of(on ? 1 : 0)); }
  saveSlot(slot) { return this._stateCall(P.CMD.SAVE_PROFILE, Uint8Array.of(slot)); }
  loadSlot(slot) { return this._stateCall(P.CMD.LOAD_PROFILE, Uint8Array.of(slot)); }
  factoryReset() { return this._stateCall(P.CMD.FACTORY_RESET); }
  async getSlotsMask() {
    const r = await this.request(P.CMD.GET_SLOTS);
    if (r.status !== P.ST.OK) throw new DongleError(statusName(r.status), r.status);
    return r.payload[0];
  }
  async getStats() {
    const r = await this.request(P.CMD.GET_STATS);
    if (r.status !== P.ST.OK) throw new DongleError(statusName(r.status), r.status);
    return parseStats(r.payload);
  }
  async testScale(dx, dy, reset = false) {
    const p = new Uint8Array(5);
    const v = view(p);
    v.setInt16(0, dx, true); v.setInt16(2, dy, true); p[4] = reset ? P.TEST_RESET_REM : 0;
    const r = await this.request(P.CMD.TEST_SCALE, p);
    if (r.status !== P.ST.OK) throw new DongleError(statusName(r.status), r.status);
    return parseTestScale(r.payload);
  }
  async reboot() {
    const r = await this.request(P.CMD.REBOOT);
    if (r.status !== P.ST.OK) throw new DongleError(statusName(r.status), r.status);
  }
}
