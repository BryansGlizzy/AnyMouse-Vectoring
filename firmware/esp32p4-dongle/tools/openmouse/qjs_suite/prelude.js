// Browser globals OpenMouse's drivers touch, supplied the way firmware would.
globalThis.window = globalThis;

if (typeof globalThis.setTimeout !== "function" && globalThis.os) {
  globalThis.setTimeout = globalThis.os.setTimeout;
  globalThis.clearTimeout = globalThis.os.clearTimeout;
}
if (typeof globalThis.setTimeout !== "function") throw new Error("engine has no setTimeout");
// Collapse hardware settle delays, as OpenMouse's own fast-test-timers.mjs does
// (it exempts the files that assert real timing; so does QJS_REAL_TIMERS).
if (!(globalThis.std && globalThis.std.getenv("QJS_REAL_TIMERS"))) {
  const realSetTimeout = globalThis.setTimeout;
  globalThis.setTimeout = (cb, ms = 0, ...a) => realSetTimeout(cb, Math.min(Number(ms) || 0, 1), ...a);
}

for (const k of ["debug", "info", "warn", "error", "trace"]) {
  if (typeof console[k] !== "function") console[k] = console.log;
}

if (typeof globalThis.structuredClone !== "function") {
  globalThis.structuredClone = (v) => (v === undefined ? v : JSON.parse(JSON.stringify(v)));
}
if (!globalThis.crypto) {
  globalThis.crypto = {
    getRandomValues(a) { for (let i = 0; i < a.length; i++) a[i] = (Math.random() * 256) | 0; return a; },
  };
}
if (!globalThis.localStorage) {
  const m = new Map();
  globalThis.localStorage = {
    getItem: (k) => (m.has(k) ? m.get(k) : null),
    setItem: (k, v) => m.set(k, String(v)),
    removeItem: (k) => m.delete(k),
  };
}
if (typeof globalThis.TextDecoder !== "function") {
  globalThis.TextDecoder = class {
    constructor(label = "utf-8") { this.label = label; }
    decode(buf) {
      const b = buf instanceof Uint8Array ? buf : new Uint8Array(buf?.buffer ?? buf ?? [], buf?.byteOffset ?? 0, buf?.byteLength);
      if (this.label.toLowerCase().startsWith("utf-16")) {
        let s = ""; for (let i = 0; i + 1 < b.length; i += 2) s += String.fromCharCode(b[i] | (b[i + 1] << 8)); return s;
      }
      let s = ""; for (const c of b) s += "%" + c.toString(16).padStart(2, "0");
      try { return decodeURIComponent(s); } catch { return String.fromCharCode(...b); }
    }
  };
}
if (typeof globalThis.TextEncoder !== "function") {
  globalThis.TextEncoder = class {
    encode(s) { return new Uint8Array(unescape(encodeURIComponent(s)).split("").map((c) => c.charCodeAt(0))); }
  };
}
