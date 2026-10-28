/*
 * AnyMouse bridge: runs the OpenMouse protocol library's own WebHID drivers on the dongle
 * (AGPL-3.0, github.com/OpenMouse-Project/mouse-protocol). amv_js.c calls the `amv` object
 * below; every answer is a plain object of numbers and strings that the C side copies into
 * amv_drv_ctx_t. The drivers themselves are bundled unchanged (tools/openmouse/gen_js_bundle.sh).
 */
import { runTimers, pendingTimers } from "./prelude.js";
import { AmvHidDevice } from "./hid_device.js";
import { clientSupportScore, createSupportedClient, deviceBrand } from "@openmouse/protocol/drivers";

const N = globalThis.__amv;
const sessions = new Map();          // sid -> { devs, dev, client, brand, opened }

/* Chrome's navigator.hid after the user granted the mouse: every interface of every attached mouse.
 * QuickJS-ng already has a read-only `navigator` (userAgent), so hid is added to it. */
const hid = {
  getDevices: async () => [...sessions.values()].flatMap((s) => s.devs),
  requestDevice: async () => [],
  addEventListener() {},
  removeEventListener() {},
};
if (globalThis.navigator) Object.defineProperty(globalThis.navigator, "hid", { value: hid, configurable: true });
else globalThis.navigator = { hid };

const LOD_LEVEL = { Low: 1, Medium: 2, High: 3 };
const LOD_NAME = ["", "Low", "Medium", "High"];

function charging(state) {
  switch (state) {
    case "Charging": case "Charging slowly": return 2;
    case "Full": case "Almost full": return 3;
    case "Discharging": return 1;
    default: return 0;
  }
}

function firmwareText(f) {
  if (Array.isArray(f)) return f.filter(Boolean).join(" ");
  return typeof f === "string" ? f : "";
}

/* MouseStatus (src/drivers/mouse-types.ts) -> the flat shape amv_js.c reads. */
function flatten(st, s) {
  const c = s.client;
  const ui = st.ui ?? {};
  const range = ui.dpiStageEditor;
  const lods = (st.supportedLiftOffDistances ?? (st.liftOffDistance ? ["Low", "Medium", "High"] : []))
    .map((n) => LOD_LEVEL[n]).filter(Boolean);
  const rates = (st.supportedPollingRates ?? []).filter((r) => Number.isFinite(r) && r > 0);
  return {
    brand: s.brand,
    family: ui.family ?? "",
    name: st.name ?? "",
    cpi: Number.isFinite(st.dpi) ? st.dpi : 0,
    hz: Number.isFinite(st.pollingRateHz) ? st.pollingRateHz : 0,
    rates,
    battery: Number.isFinite(st.batteryPercent) ? Math.round(st.batteryPercent) : -1,
    charging: charging(st.batteryState),
    link: st.connectionType === "Wireless" ? 1 : 0,
    firmware: firmwareText(st.firmware),
    cpiMin: range?.minDpi ?? 0,
    cpiMax: range?.maxDpi ?? 0,
    cpiStep: range?.stepDpi ?? 0,
    lod: LOD_LEVEL[st.liftOffDistance] ?? 0,
    lods,
    motionSync: st.motionSync === true ? 1 : st.motionSync === false ? 0 : -1,
    canCpi: typeof c.setDpi === "function",
    canRate: typeof c.setPollingRate === "function" && !ui.pollingReadOnly && rates.length > 0,
    canLod: typeof c.setLiftOffDistance === "function" && lods.length > 0,
    canMotionSync: typeof c.setMotionSync === "function" && st.motionSync != null && !ui.hideMotionSync,
    verified: ui.valuesVerified !== false,
  };
}

function session(sid) {
  const s = sessions.get(sid);
  if (!s) throw new Error("no session " + sid);
  return s;
}

async function ensureOpen(s) {
  if (s.opened) return;
  if (typeof s.client.open === "function") await s.client.open();
  else await s.dev.open();
  s.opened = true;
}

globalThis.amv = {
  /* Timers due now; returns ms to the next one, or -1. */
  pump: () => runTimers(),
  pending: () => pendingTimers(),

  input(sid, x, buf) {
    const s = sessions.get(sid);
    const d = s && s.devs.find((v) => v._x === x);
    if (d) d._input(new Uint8Array(buf));
  },

  /*
   * A mouse attached: one HIDDevice per interface, then OpenMouse's own registry picks the
   * driver (clientSupportScore / createSupportedClient), exactly as its control panel does.
   * ifaces: [{ x, desc: ArrayBuffer }]. Returns { brand, x } or null (no OpenMouse driver).
   */
  open(sid, vid, pid, name, ifaces) {
    const devs = ifaces.map((i) => new AmvHidDevice(sid, i.x, vid, pid, name, new Uint8Array(i.desc)));
    let best = null, bestScore = 0;
    for (const d of devs) {
      let sc = 0;
      try { sc = clientSupportScore(d); } catch (e) { N.log(2, "score: " + e); }
      if (sc > bestScore) { best = d; bestScore = sc; }
    }
    if (!best) return null;
    const client = createSupportedClient(best);
    if (!client) return null;
    const s = { devs, dev: best, client, brand: deviceBrand(client), opened: false };
    sessions.set(sid, s);
    return { brand: s.brand, x: best._x, score: bestScore };
  },

  async probe(sid) {
    const s = session(sid);
    await ensureOpen(s);
    const read = s.client.readStatus ?? s.client.getStatus;
    return flatten(await read.call(s.client), s);
  },

  /* id: AMV_SET_CPI 1, POLLING 2, LOD 3, MOTION_SYNC 4. Resolves to the status read back. */
  async set(sid, id, value) {
    const s = session(sid);
    await ensureOpen(s);
    const c = s.client;
    if (id === 1) await c.setDpi(value);
    else if (id === 2) await c.setPollingRate(value);
    else if (id === 3) await c.setLiftOffDistance(LOD_NAME[value]);
    else if (id === 4) await c.setMotionSync(value !== 0);
    else throw new Error("setting " + id);
    return flatten(await (c.readStatus ?? c.getStatus).call(c), s);
  },

  async close(sid) {
    const s = sessions.get(sid);
    if (!s) return;
    sessions.delete(sid);
    try { if (s.opened && typeof s.client.close === "function") await s.client.close(); } catch (e) { N.log(1, "close: " + e); }
  },
};
