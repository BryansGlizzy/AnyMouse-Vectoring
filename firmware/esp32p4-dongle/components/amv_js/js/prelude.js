/*
 * Browser globals OpenMouse's drivers expect, provided on the dongle. Imported first by
 * bridge.js so it runs before any driver module. `__amv` is the native object from amv_js.c.
 */
const N = globalThis.__amv;

globalThis.window = globalThis;

/* Timers: a sorted list the C event loop drains through amv.pump() (bridge.js). */
const timers = [];
let nextTimerId = 1;

function setTimeout(fn, ms = 0, ...args) {
  const id = nextTimerId++;
  const due = N.now() + Math.max(0, Number(ms) || 0);
  let i = timers.length;
  while (i > 0 && timers[i - 1].due > due) i--;
  timers.splice(i, 0, { id, due, fn, args });
  return id;
}

function clearTimeout(id) {
  const i = timers.findIndex((t) => t.id === id);
  if (i >= 0) timers.splice(i, 1);
}

/* Run due timers; returns ms until the next one, or -1 when none is pending. */
export function runTimers() {
  const now = N.now();
  while (timers.length && timers[0].due <= now) {
    const t = timers.shift();
    try { t.fn(...t.args); } catch (e) { N.log(2, "timer: " + e); }
  }
  return timers.length ? Math.max(0, timers[0].due - N.now()) : -1;
}

export function pendingTimers() { return timers.length; }

globalThis.setTimeout = setTimeout;
globalThis.clearTimeout = clearTimeout;
globalThis.setInterval = (fn, ms, ...a) => {
  const box = { id: 0 };
  const tick = () => { box.id = setTimeout(tick, ms); fn(...a); };
  box.id = setTimeout(tick, ms);
  return box;
};
globalThis.clearInterval = (box) => box && clearTimeout(box.id);

globalThis.performance = { now: () => N.now() };

const levels = { error: 2, warn: 2, info: 1, log: 1, debug: 0, trace: 0 };
globalThis.console = {};
for (const [k, lvl] of Object.entries(levels)) {
  globalThis.console[k] = (...a) => N.log(lvl, a.map((v) => (typeof v === "string" ? v : safeString(v))).join(" "));
}
function safeString(v) {
  try { return JSON.stringify(v, (k, x) => (ArrayBuffer.isView(x) ? Array.from(x) : x)); } catch { return String(v); }
}

globalThis.crypto = { getRandomValues: (a) => { N.random(a); return a; } };

if (typeof globalThis.structuredClone !== "function") {
  globalThis.structuredClone = (v) => (v === undefined ? v : JSON.parse(JSON.stringify(v)));
}

/* Glorious keeps its last-written lighting here; on the dongle it lives as long as the session. */
const store = new Map();
globalThis.localStorage = {
  getItem: (k) => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => { store.set(k, String(v)); },
  removeItem: (k) => { store.delete(k); },
};

if (typeof globalThis.TextDecoder !== "function") {
  globalThis.TextDecoder = class TextDecoder {
    constructor(label = "utf-8") { this.encoding = String(label).toLowerCase(); }
    decode(buf) {
      if (buf === undefined) return "";
      const b = ArrayBuffer.isView(buf) ? new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength) : new Uint8Array(buf);
      let s = "";
      if (this.encoding.startsWith("utf-16")) {
        for (let i = 0; i + 1 < b.length; i += 2) s += String.fromCharCode(b[i] | (b[i + 1] << 8));
        return s;
      }
      for (let i = 0; i < b.length; i++) {
        const c = b[i];
        if (c < 0x80) { s += String.fromCharCode(c); continue; }
        const n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        let cp = c & (0x3f >> n);
        for (let k = 0; k < n && i + 1 < b.length; k++) cp = (cp << 6) | (b[++i] & 0x3f);
        s += String.fromCodePoint(n ? cp : 0xfffd);
      }
      return s;
    }
  };
}
if (typeof globalThis.TextEncoder !== "function") {
  globalThis.TextEncoder = class TextEncoder {
    encode(s = "") {
      const out = [];
      for (const ch of String(s)) {
        const cp = ch.codePointAt(0);
        if (cp < 0x80) out.push(cp);
        else if (cp < 0x800) out.push(0xc0 | (cp >> 6), 0x80 | (cp & 63));
        else if (cp < 0x10000) out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
        else out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 63), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
      }
      return new Uint8Array(out);
    }
  };
}
