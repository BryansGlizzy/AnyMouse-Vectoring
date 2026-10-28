/*
 * Host stand-in for the native `__amv` object amv_js.c installs on the dongle, so the bridge
 * bundle (components/amv_js/generated/openmouse.js) runs under the qjs shell on a PC.
 * A test sets globalThis.__fake = { send(sid, x, type, id, bytes), recv(sid, x, id, len) }.
 */
const ms = () => os.now() / 1000;          // QuickJS-ng's os.now() is in microseconds
const t0 = ms();
globalThis.__amv = {
  now: () => Math.floor(ms() - t0),
  log: (lvl, msg) => { if (lvl >= 1 || globalThis.__verbose) print(`[js${lvl}] ${msg}`); },
  random: (a) => { for (let i = 0; i < a.length; i++) a[i] = (Math.random() * 256) | 0; },
  send: (sid, x, type, id, buf) => globalThis.__fake.send(sid, x, type, id, new Uint8Array(buf)),
  recv: (sid, x, id, len) => globalThis.__fake.recv(sid, x, id, len).buffer,
};

/* Pump the bridge's timers until the promise settles, as amv_js.c's event loop does. */
export function drive(promise, timeoutMs = 10000) {
  return new Promise((resolve, reject) => {
    let done = false;
    promise.then((v) => { done = true; resolve(v); }, (e) => { done = true; reject(e); });
    const start = ms();
    const tick = () => {
      if (done) return;
      if (ms() - start > timeoutMs) { reject(new Error("timed out")); return; }
      const wait = globalThis.amv.pump();
      os.setTimeout(tick, wait < 0 ? 1 : Math.min(wait, 5));
    };
    tick();
  });
}

export function hexFile(path) {
  const txt = std.loadFile(path);
  if (txt == null) throw new Error("cannot read " + path);
  return new Uint8Array(txt.trim().split(/\s+/).map((h) => parseInt(h, 16)));
}
