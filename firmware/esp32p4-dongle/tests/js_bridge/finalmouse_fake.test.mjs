/*
 * The bridge against a fake Finalmouse UltralightX dongle (361d:0100) built from the report
 * descriptor the dongle captured from the real one. Unlike the Razer, the Finalmouse keeps its
 * vendor protocol on the SAME interface as the mouse and answers on INPUT reports (ids 3 and 5)
 * that the host's interrupt IN pipe delivers, so this is the first test of that path:
 *   host in_cb -> in_q -> om_poll_input -> amv_js deliver_inputs -> amv.input() -> inputreport.
 * Runs under Node (the qjs shell is not needed):   node tests/js_bridge/finalmouse_fake.test.mjs
 * (from firmware/esp32p4-dongle). Options via env: FM_LATENCY_MS, FM_SILENT=1, FM_NO_DELIVER=1.
 */
import { readFileSync } from "node:fs";

const t0 = Date.now();
let failures = 0;
const say = console.log.bind(console);                 // the prelude replaces console and the timers
const later = setTimeout;                              // so keep Node's own
const check = (c, w) => { say(`${c ? "ok  " : "FAIL"} ${w}`); if (!c) failures++; };

const LAT = Number(process.env.FM_LATENCY_MS ?? 3);
const sent = [];
const inbox = [];                                      // raw input reports waiting for the host loop

/* What the real dongle does on wakeAll (hid_driver.c / drv_finalmouse.c): a burst of input report 5,
 * each [5, innerLen, cmd, payloadLen, payload...] padded to 63 data bytes. */
function inReport(id, cmd, payload) {
  const r = new Uint8Array(64);
  r[0] = id; r[1] = 2 + payload.length; r[2] = cmd; r[3] = payload.length; r.set(payload, 4);
  return r;
}
const u16 = (v) => [v & 255, v >> 8];
const ascii = (s) => [...s].map((c) => c.charCodeAt(0)).concat([0]);
function burst() {
  const out = [
    inReport(5, 3, u16(3200)), inReport(5, 4, u16(2000)), inReport(5, 5, u16(3880)),
    inReport(5, 12, ascii("1.2.3")), inReport(5, 13, [0xed]), inReport(5, 18, [1]),
    inReport(3, 0x0b, ascii("dongle")),
  ];
  return out;
}

globalThis.__amv = {
  now: () => Date.now() - t0,
  log: (lvl, msg) => { if (lvl >= 1 || process.env.FM_VERBOSE) say(`[js${lvl}] ${msg}`); },
  random: (a) => { for (let i = 0; i < a.length; i++) a[i] = (Math.random() * 256) | 0; },
  send: (sid, x, type, id, buf) => {
    const d = new Uint8Array(buf);
    sent.push({ x, type, id, len: d.length, cmd: d[1] & 0x7f });
    if (process.env.FM_SILENT) return;
    // output report 4 (main) command 0x60 = wakeAll: the mouse answers with its burst
    if (id === 4 && (d[1] & 0x7f) === 0x60) {
      later(() => inbox.push(...burst()), LAT);
    }
  },
  recv: () => new ArrayBuffer(0),
};

await import("../../components/amv_js/generated/openmouse.js");
const amv = globalThis.amv;

/* The engine loop of amv_js.c: await_value() = deliver_inputs, pump timers, 1 ms tick. */
function drive(promise, timeoutMs = 3000) {
  return new Promise((resolve, reject) => {
    let done = false;
    promise.then((v) => { done = true; resolve(v); }, (e) => { done = true; reject(e); });
    const start = Date.now();
    const tick = () => {
      if (done) return;
      if (Date.now() - start > timeoutMs) { reject(new Error("timed out")); return; }
      if (!process.env.FM_NO_DELIVER) {
        for (let n = 0; n < 32 && inbox.length; n++) amv.input(1, -1, inbox.shift().buffer);
      }
      amv.pump();
      later(tick, 1);
    };
    tick();
  });
}

const hex = readFileSync(new URL("../descriptors/finalmouse_ulx_dongle_361d_0100_if0.hex", import.meta.url), "utf8")
  .trim().split(/\s+/).map((h) => parseInt(h, 16));
const opened = amv.open(1, 0x361d, 0x0100, "UltralightX dongle", [{ x: -1, desc: new Uint8Array(hex).buffer }]);
check(opened && opened.brand === "Finalmouse", `registry picks the Finalmouse driver (${JSON.stringify(opened)})`);
check(opened && opened.x === -1, "on the mouse's own interface");

let st = null, err = null;
const t1 = Date.now();
try { st = await drive(amv.probe(1)); } catch (e) { err = e; }
const took = Date.now() - t1;
say(`     probe took ${took} ms: ${err ? "ERROR " + err.message : JSON.stringify(st)}`);
say(`     sent: ${JSON.stringify(sent.map((s) => `${s.type === 2 ? "out" : "feat"}#${s.id} cmd 0x${s.cmd.toString(16)} len ${s.len}`))}`);

if (!process.env.FM_SILENT && !process.env.FM_NO_DELIVER) {
  check(!err && st, "readStatus succeeds when the mouse answers");
  check(st && st.cpi === 3200, "CPI read: 3200");
  check(st && st.hz === 2000, "polling read: 2000 Hz");
  check(st && st.battery > 0, `battery read: ${st && st.battery}%`);
  check(st && st.motionSync === 1, "Motion Sync read: on");
  check(sent.every((s) => s.type === 2 && s.x === -1), "all requests are OUTPUT reports on the mouse interface");
  check(sent.some((s) => s.id === 4 && s.len === 63), "main command goes out as output report 4 with 63 data bytes");
  check(sent.some((s) => s.id === 2), "dongle-info goes out as output report 2");
} else {
  check(!!err, "with no answer (or no delivery) the driver throws, as seen on the board");
}
await drive(amv.close(1));
process.exit(failures ? 1 : 0);
