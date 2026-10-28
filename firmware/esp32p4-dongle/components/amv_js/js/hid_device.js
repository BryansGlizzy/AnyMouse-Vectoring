/*
 * A WebHID HIDDevice over the dongle's USB host. One per HID interface, as Chrome makes one per
 * interface. `x` selects the native transport: -1 is the mouse's own interface (SET/GET_REPORT,
 * vendor input reports from its queue), 0.. a claimed vendor interface (amv_hl_*).
 */
const N = globalThis.__amv;

/* Report descriptor -> HIDDevice.collections (top-level collections, reports grouped by ID). */
export function parseCollections(desc) {
  const top = [];
  const stack = [];
  let g = { page: 0, size: 0, count: 0, id: 0 };
  const gstack = [];
  let usages = [];
  let usesIds = false;
  for (let i = 0; i < desc.length;) {
    const p = desc[i++];
    if (p === 0xfe) { i += 2 + (desc[i] ?? 0); continue; }          // long item
    const len = [0, 1, 2, 4][p & 3];
    let v = 0;
    for (let k = 0; k < len; k++) v |= (desc[i + k] ?? 0) << (8 * k);
    v >>>= 0;
    i += len;
    const type = (p >> 2) & 3, tag = p >> 4;
    if (type === 1) {                                              // global
      if (tag === 0) g.page = v;
      else if (tag === 7) g.size = v;
      else if (tag === 8) { g.id = v; usesIds = true; }
      else if (tag === 9) g.count = v;
      else if (tag === 10) gstack.push({ ...g });
      else if (tag === 11 && gstack.length) g = gstack.pop();
    } else if (type === 2) {                                       // local
      if (tag === 0) usages.push(len === 4 ? v : ((g.page << 16) | v) >>> 0);
    } else if (type === 0) {                                       // main
      if (tag === 10) {
        const u = usages[0] ?? (g.page << 16);
        const c = { usagePage: u >>> 16, usage: u & 0xffff, type: v, children: [], inputReports: [], outputReports: [], featureReports: [] };
        (stack.length ? stack[stack.length - 1].children : top).push(c);
        stack.push(c);
      } else if (tag === 12) {
        stack.pop();
      } else if (tag === 8 || tag === 9 || tag === 11) {
        const c = stack[stack.length - 1];
        if (c) {
          const list = tag === 8 ? c.inputReports : tag === 9 ? c.outputReports : c.featureReports;
          let r = list.find((x) => x.reportId === g.id);
          if (!r) list.push((r = { reportId: g.id, items: [] }));
          r.items.push({ reportSize: g.size, reportCount: g.count, usagePage: g.page, isConstant: (v & 1) === 1 });
        }
      }
      usages = [];
    }
  }
  return { collections: top, usesIds };
}

function reportBytes(collections, kind, id) {
  let bits = 0;
  const walk = (cs) => {
    for (const c of cs) {
      for (const r of c[kind]) if (r.reportId === id) for (const it of r.items) bits += it.reportSize * it.reportCount;
      walk(c.children);
    }
  };
  walk(collections);
  return Math.ceil(bits / 8);
}

function bytesOf(data) {
  if (data instanceof ArrayBuffer) return new Uint8Array(data.slice(0));
  return new Uint8Array(data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength));
}

export class AmvHidDevice {
  constructor(sid, x, vendorId, productId, productName, desc) {
    const { collections, usesIds } = parseCollections(desc);
    this._sid = sid;
    this._x = x;
    this._usesIds = usesIds;
    this._listeners = new Set();
    this.vendorId = vendorId;
    this.productId = productId;
    this.productName = productName;
    this.collections = collections;
    this.opened = false;
    this.oninputreport = null;
  }

  async open() { this.opened = true; }
  async close() { this.opened = false; }
  async forget() { this.opened = false; }

  async sendReport(reportId, data) {
    N.send(this._sid, this._x, 2, reportId, bytesOf(data).buffer);
  }

  async sendFeatureReport(reportId, data) {
    N.send(this._sid, this._x, 3, reportId, bytesOf(data).buffer);
  }

  /* As in Chrome, a numbered report comes back with its ID at byte 0. */
  async receiveFeatureReport(reportId) {
    const len = reportBytes(this.collections, "featureReports", reportId) || 64;
    const got = new Uint8Array(N.recv(this._sid, this._x, reportId, len));
    if (!reportId) return new DataView(got.buffer);
    const out = new Uint8Array(got.length + 1);
    out[0] = reportId;
    out.set(got, 1);
    return new DataView(out.buffer);
  }

  addEventListener(type, fn) { if (type === "inputreport" && fn) this._listeners.add(fn); }
  removeEventListener(type, fn) { if (type === "inputreport") this._listeners.delete(fn); }

  /* A raw input report from the interface (report ID first when the descriptor uses IDs). */
  _input(raw) {
    const id = this._usesIds ? raw[0] : 0;
    const off = this._usesIds ? 1 : 0;
    const ev = { type: "inputreport", device: this, reportId: id, data: new DataView(raw.buffer, raw.byteOffset + off, raw.byteLength - off) };
    for (const fn of [...this._listeners]) {
      try { typeof fn === "function" ? fn.call(this, ev) : fn.handleEvent(ev); } catch (e) { N.log(2, "inputreport: " + e); }
    }
    if (typeof this.oninputreport === "function") {
      try { this.oninputreport(ev); } catch (e) { N.log(2, "oninputreport: " + e); }
    }
  }
}
