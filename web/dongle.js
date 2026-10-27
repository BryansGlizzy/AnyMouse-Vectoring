// AnyMouse dongle protocol client. Transport-agnostic: it needs only an object shaped like a WebHID
// HIDDevice (open/close/sendReport/addEventListener('inputreport')/opened), so the same code runs on
// real WebHID, on the built-in demo device, and in the Node tests.
import { P, ST_NAMES } from './proto.js';

// a mouse write can wait for the mouse to confirm (Logitech rate changes: up to 6 s)
const WRITE_TIMEOUT_MS = 9000;
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

const cstr = (u8) => { let t = ''; for (const b of u8) { if (!b) break; t += String.fromCharCode(b); } return t; };

// GET_MOUSE reply (amv_mouse_info_t, 56 bytes)
export function parseMouse(p) {
  const v = view(p);
  return {
    index: p[0], count: p[1], tier: p[2], flags: p[3], vid: v.getUint16(4, true), pid: v.getUint16(6, true),
    speed: p[8], iface: p[9], nominalHz: v.getUint16(10, true), measuredHz: v.getUint16(12, true),
    deviceHz: v.getUint16(14, true), cpi: v.getUint16(16, true), cpiSource: p[18], battery: p[19] === 0xff ? null : p[19],
    buttons: p[20], xBits: p[21], driver: p[22], reason: p[23], uid: v.getUint16(24, true), name: cstr(p.slice(26, 56)),
  };
}

// GET_MOUSE_CAPS reply (amv_mouse_caps_t, 42 bytes)
export function parseMouseCaps(p) {
  const v = view(p), rates = [];
  for (let i = 0; i < Math.min(p[2], 8); i++) rates.push(v.getUint16(4 + i * 2, true));
  return { index: p[0], driver: p[1], rates, cpiMin: v.getUint16(20, true), cpiMax: v.getUint16(22, true), cpiStep: v.getUint16(24, true), firmware: cstr(p.slice(26, 42)) };
}

// GET_MOUSE_MODEL reply (amv_mouse_model_t, 56 bytes)
export function parseMouseModel(p) {
  return { index: p[0], state: p[1], brand: cstr(p.slice(4, 20)), model: cstr(p.slice(20, 56)) };
}

// GET_MOUSE_SETTINGS / SET_MOUSE_SETTING reply (amv_settings_rep_t: 4 + 4n bytes)
export function parseSettings(p) {
  const v = view(p), n = Math.min(p[1], P.SETTINGS_MAX, (p.length - 4) >> 2), settings = [];
  for (let i = 0; i < n; i++) {
    const f = p[5 + i * 4];
    settings.push({ id: p[4 + i * 4], flags: f, readable: (f & P.SETF.READABLE) !== 0, writable: (f & P.SETF.WRITABLE) !== 0, value: v.getUint16(6 + i * 4, true) });
  }
  return { index: p[0], link: p[2], settings };
}

// GET_MOUSE_OPTIONS reply (amv_options_rep_t: 6 + 2n bytes)
export function parseOptions(p) {
  const v = view(p), n = Math.min(p[4], P.OPTIONS_MAX, (p.length - 6) >> 1), values = [];
  for (let i = 0; i < n; i++) values.push(v.getUint16(6 + i * 2, true));
  const range = p[2] === P.OPT.RANGE && n >= 3 ? { min: values[0], max: values[1], step: values[2] } : null;
  return { index: p[0], setting: p[1], kind: p[2], unit: p[3], values, range };
}

const mouseIndex = (i) => Uint8Array.of(i);
const mouseSet = (i, value) => { const p = new Uint8Array(4); p[0] = i; view(p).setUint16(2, value, true); return p; };

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
  constructor(hid, { timeoutMs = 3000 } = {}) {   // a mouse write can take ~1 s while the mouse answers
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
    if (f.cmd === P.CMD.EVENT_MOUSE) {
      const v = view(f.payload);
      this.dispatchEvent(new CustomEvent('mouse', { detail: { count: f.payload[0], change: f.payload[1], uid: v.getUint16(2, true) } }));
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
  async request(cmd, payload, timeoutMs = this.timeoutMs) {
    const seq = this._nextSeq();
    const frame = buildFrame(cmd, seq, payload);
    const reply = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(seq);
        reject(new DongleError(`no reply to command 0x${cmd.toString(16)}`, -2));
      }, timeoutMs);
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
  // ---- attached mice (firmware 0.5.0+). listMice() returns null when the firmware predates them. ----
  async listMice() {
    const out = [];
    for (let i = 0; i < 16; i++) {
      const r = await this.request(P.CMD.GET_MOUSE, mouseIndex(i));
      if (r.status === P.ST.BAD_CMD) return null;
      if (r.status === P.ST.NO_MOUSE) break;
      if (r.status !== P.ST.OK) throw new DongleError(statusName(r.status), r.status);
      out.push(parseMouse(r.payload));
    }
    return out;
  }
  async getMouseCaps(i) {
    const r = await this.request(P.CMD.GET_MOUSE_CAPS, mouseIndex(i));
    return r.status === P.ST.OK ? parseMouseCaps(r.payload) : null;
  }
  /** The raw HID report descriptor of mouse i, fetched in 48-byte pieces. */
  async getMouseDescriptor(i) {
    const parts = [];
    let off = 0, total = 1;
    while (off < total) {
      const q = new Uint8Array(4); q[0] = i; view(q).setUint16(2, off, true);
      const r = await this.request(P.CMD.GET_MOUSE_DESC, q);
      if (r.status !== P.ST.OK) throw new DongleError(statusName(r.status), r.status);
      const v = view(r.payload), n = r.payload[6];
      total = v.getUint16(2, true);
      parts.push(r.payload.slice(8, 8 + n));
      off += n;
      if (!n) break;
    }
    const out = new Uint8Array(off);
    let at = 0;
    for (const part of parts) { out.set(part, at); at += part.length; }
    return out;
  }
  /** Known mice: written to the mouse. Others: stored on the dongle as a label (0 forgets). */
  async setMouseCpi(i, cpi) {
    const r = await this.request(P.CMD.SET_MOUSE_CPI, mouseSet(i, cpi), WRITE_TIMEOUT_MS);
    return { status: r.status, ok: r.status === P.ST.OK, mouse: r.payload.length >= 56 ? parseMouse(r.payload) : null };
  }
  // ---- brand-neutral settings (firmware 0.6.0+). Each returns null when the firmware predates them. ----
  async getMouseModel(i) {
    const r = await this.request(P.CMD.GET_MOUSE_MODEL, mouseIndex(i));
    return r.status === P.ST.OK ? parseMouseModel(r.payload) : null;
  }
  async getMouseSettings(i) {
    const r = await this.request(P.CMD.GET_MOUSE_SETTINGS, mouseIndex(i));
    return r.status === P.ST.OK ? parseSettings(r.payload) : null;
  }
  async getMouseOptions(i, setting) {
    const r = await this.request(P.CMD.GET_MOUSE_OPTIONS, Uint8Array.of(i, setting));
    return r.status === P.ST.OK ? parseOptions(r.payload) : null;
  }
  async setMouseSetting(i, setting, value) {
    const p = new Uint8Array(4); p[0] = i; p[1] = setting; view(p).setUint16(2, value, true);
    const r = await this.request(P.CMD.SET_MOUSE_SETTING, p, WRITE_TIMEOUT_MS);
    return { status: r.status, ok: r.status === P.ST.OK, settings: r.payload.length >= 4 && r.status === P.ST.OK ? parseSettings(r.payload) : null };
  }
  async setMousePolling(i, hz) {
    const r = await this.request(P.CMD.SET_MOUSE_POLLING, mouseSet(i, hz), WRITE_TIMEOUT_MS);
    return { status: r.status, ok: r.status === P.ST.OK, mouse: r.payload.length >= 56 ? parseMouse(r.payload) : null };
  }

  async reboot() {
    const r = await this.request(P.CMD.REBOOT);
    if (r.status !== P.ST.OK) throw new DongleError(statusName(r.status), r.status);
  }
}
