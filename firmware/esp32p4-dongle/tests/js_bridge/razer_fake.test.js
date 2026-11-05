/*
 * The bridge against a fake Razer Basilisk Ultimate (1532:0086) built from its real captured
 * report descriptors: OpenMouse's registry must pick its Razer driver for interface 0, read the
 * values the fake holds, and write CPI and polling rate through it.
 *   qjs --std --module tests/js_bridge/razer_fake.test.js      (from firmware/esp32p4-dongle)
 */
import { drive, hexFile } from "./host_native.js";
import "../../components/amv_js/generated/openmouse.js";

let failures = 0;
function check(cond, what) {
  print(`${cond ? "ok  " : "FAIL"} ${what}`);
  if (!cond) failures++;
}

const PKT = 90;
const state = { dpi: 1600, divisor: 1, battery: 255, fw: [1, 7] };
let pending = new Uint8Array(PKT);
const sent = [];

function reply(req, status, args) {
  const p = new Uint8Array(PKT);
  p[0] = status;
  p[1] = req[1];
  p[5] = req[5];
  p[6] = req[6];
  p[7] = req[7];
  p.set(args, 8);
  let x = 0;
  for (let i = 2; i < 88; i++) x ^= p[i];
  p[88] = x;
  return p;
}

globalThis.__fake = {
  send(sid, x, type, id, d) {
    sent.push({ x, type, id, len: d.length, cls: d[6], cmd: d[7] });
    if (type !== 3 || x !== -1 || d.length !== PKT) { pending = new Uint8Array(PKT); return; }
    const [cls, cmd] = [d[6], d[7]];
    const ok = (args) => { pending = reply(d, 0x02, args); };
    if (cls === 0x04 && cmd === 0x05) { state.dpi = (d[9] << 8) | d[10]; ok([d[8], d[9], d[10], d[11], d[12], 0, 0]); }
    else if (cls === 0x04 && cmd === 0x85) ok([d[8], state.dpi >> 8, state.dpi & 255, state.dpi >> 8, state.dpi & 255, 0, 0]);
    else if (cls === 0x00 && cmd === 0x05) { state.divisor = d[8]; ok([d[8]]); }
    else if (cls === 0x00 && cmd === 0x85) ok([state.divisor]);
    else if (cls === 0x07 && cmd === 0x80) ok([0, state.battery]);
    else if (cls === 0x07 && cmd === 0x84) ok([0, 0]);
    else if (cls === 0x00 && cmd === 0x81) ok(state.fw);
    else pending = reply(d, 0x05, []);                     // unsupported
  },
  recv(sid, x, id, len) {
    return pending.slice(0, len);
  },
};

const dir = "tests/descriptors/razer_basilisk_ultimate_1532_0086_if";
const ifaces = [
  { x: -1, desc: hexFile(dir + "0.hex").buffer },
  { x: 0, desc: hexFile(dir + "1.hex").buffer },
  { x: 1, desc: hexFile(dir + "2.hex").buffer },
];

const opened = amv.open(1, 0x1532, 0x0086, "Razer Basilisk Ultimate", ifaces);
check(opened && opened.brand === "Razer", `registry picks the Razer driver (${JSON.stringify(opened)})`);
check(opened && opened.x === -1, "on the mouse interface (interface 0)");

const st = await drive(amv.probe(1));
print("     status " + JSON.stringify(st));
check(st.cpi === 1600, "CPI read: 1600");
check(st.hz === 1000, "polling read: 1000 Hz");
check(st.battery === 100, "battery read: 100%");
check(st.canCpi && st.canRate, "CPI and polling are writable by the driver");
check(sent.every((s) => s.type === 3 && s.x === -1 && s.len === PKT), "every request is a 90-byte feature report on interface 0");

const w1 = await drive(amv.set(1, 1, 800));
check(state.dpi === 800 && w1.cpi === 800, "CPI write 800 reaches the mouse and reads back");
const w2 = await drive(amv.set(1, 2, 500));
check(state.divisor === 2 && w2.hz === 500, "polling write 500 Hz reaches the mouse (divisor 2) and reads back");

await drive(amv.close(1));
check(amv.pending() === 0, "no timers left after close");

print(failures ? `${failures} FAILED` : "all passed");
std.exit(failures ? 1 : 0);
