// Generates components/amv_catalog/amv_catalog.generated.h from a pinned OpenMouse checkout.
//
//   OPENMOUSE_DIR=<checkout> tools/openmouse/gen_catalog.sh
//
// Source of truth is OpenMouse itself, not a copy of its tables: the device list is its
// SUPPORTED_HID_FILTERS (what its browser picker offers), and each entry is resolved to an engine
// by running the registry's own isSupported() checks against a synthetic device, in registry
// order, exactly as the browser does. Nothing here is hand-maintained, so re-running after an
// upstream bump shows what changed (PLAN.md section 4.3, phase E).
import { execSync } from "node:child_process";
import { readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { join, relative } from "node:path";

const OM = process.env.OPENMOUSE_DIR;
const OUT = process.argv[2];
if (!OM || !OUT) throw new Error("usage: OPENMOUSE_DIR=<checkout> gen_catalog.mjs <out.h>");

const reg = await import(OM + "/src/drivers/registry.ts");
const ven = await import(OM + "/src/drivers/vendors.ts");
const commit = execSync("git rev-parse HEAD", { cwd: OM }).toString().trim();

// ---------- synthetic devices ----------
// A driver's isSupported() looks at the vendor/product id and at the HID collections: usage
// page and usage, and sometimes the size of a report. The probe offers every report id with a
// range of payload sizes and lets the driver pick what it wants.
const SIZES = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,56,57,58,59,60,61,62,63,64,65,90,91,127,128,255,256,1040];
const PAGES = [0xff00,0xff01,0xff02,0xff03,0xff0a,0xff0b,0xff0c,0xff60,0xff80,0xffa0,0xffa1,0xffb0,0xffc0,0xffc1,0xff81,0xff82,0xff83,0xff84,0xff85,0xff86,0xff87,0xffff,0xfff1,0xff10,0xff11,0xff12,0xff13,0xff14,0xff15,0xff16,0xff17,0xff18,0xff19,0xff1a,0xff1b,0xff1c,0xff1d,0xff1e,0xff1f,0xff20,0x008c,0xff8c,0xff50,0xff4d,0xff90,0x0001,0x000c,0xff30,0xff31,0xff32,0xff33,0xff34,0xff35,0xff40,0xff51,0xff52,0xff53,0xff54,0xff55,0xff56,0xff57,0xff58,0xff59,0xff5a,0xff70,0xff71,0xff72,0xff73,0xff74,0xff75,0xff76,0xff77,0xff78,0xff79,0xff7a,0xff7b,0xff7c,0xff7d,0xff7e,0xff7f];
const USAGES = [1,2,3,4,5,6,0x0e,0x10,0x20,0x80,0x0a,0x0b];

const reportCache = new Map();
function reports(n) {
  if (!reportCache.has(n)) {
    const a = [];
    for (let id = 0; id < 256; id++) a.push({ reportId: id, items: [{ reportSize: 8, reportCount: n }] });
    reportCache.set(n, a);
  }
  return reportCache.get(n);
}
function col(page, usage, n) {
  const r = reports(n);
  return { usagePage: page, usage, type: "application", children: [], inputReports: r, outputReports: r, featureReports: r };
}
const dev = (vid, pid, cols) => ({ vendorId: vid, productId: pid, collections: cols, productName: "", opened: false });
const driverIndex = (d) => reg.DEVICE_DRIVERS.findIndex((x) => { try { return x.supports(d); } catch { return false; } });

// ---------- names ----------
// Product tables are exported as Maps (pid -> { name }) or arrays ({ productId, name }) from the
// family modules. Collected per source folder; matched to an entry by brand.
const names = new Map();   // pid -> [{ folder, name }]
function walk(dir, acc = []) {
  for (const e of readdirSync(dir)) {
    const p = join(dir, e);
    if (statSync(p).isDirectory()) walk(p, acc);
    else if (p.endsWith(".ts") && !p.endsWith(".test.ts") && !p.endsWith(".d.ts")) acc.push(p);
  }
  return acc;
}
function addName(folder, pid, name) {
  if (typeof pid !== "number" || typeof name !== "string" || !name) return;
  if (!names.has(pid)) names.set(pid, []);
  names.get(pid).push({ folder, name });
}
for (const file of walk(OM + "/src")) {
  let mod;
  try { mod = await import(file); } catch { continue; }
  const folder = relative(OM + "/src", file).split("/").filter((s) => s !== "drivers")[0].toLowerCase();
  for (const v of Object.values(mod)) {
    if (v instanceof Map) {
      for (const [k, val] of v) if (val && typeof val === "object") addName(folder, k, val.name ?? val.model ?? val.label);
    } else if (Array.isArray(v)) {
      for (const val of v) if (val && typeof val === "object" && "productId" in val) addName(folder, val.productId, val.name ?? val.model);
    }
  }
}
const norm = (s) => s.toLowerCase().replace(/[^a-z0-9]/g, "");
function nameFor(pid, brand, engine) {
  const c = names.get(pid);
  if (!c) return "";
  const b = norm(brand), e = norm(engine);
  const pick = c.find((x) => norm(x.folder).includes(b) || b.includes(norm(x.folder))) ??
               c.find((x) => e.includes(norm(x.folder))) ??
               (new Set(c.map((x) => x.name)).size === 1 ? c[0] : undefined);
  return pick ? pick.name : "";
}

// ---------- resolve every filter ----------
const entries = [];
const brands = [], engines = [];
const idx = (list, v) => { let i = list.indexOf(v); if (i < 0) { i = list.length; list.push(v); } return i; };
let unresolved = 0;

for (const f of ven.SUPPORTED_HID_FILTERS) {
  const pages = f.usagePage !== undefined ? [f.usagePage] : PAGES;
  const usages = f.usage !== undefined ? [f.usage] : USAGES;
  const pid = f.productId ?? 0;
  let hit = null;
  for (const n of SIZES) {
    const cols = [];
    for (const p of pages) for (const u of usages) cols.push(col(p, u, n));
    const i = driverIndex(dev(f.vendorId, pid, cols));
    if (i >= 0) { hit = { i, n }; break; }
  }
  let page = f.usagePage ?? 0, usage = f.usage ?? 0;
  let brand = "Unknown", engine = "None";
  if (hit) {
    const drv = reg.DEVICE_DRIVERS[hit.i];
    brand = drv.brand;
    try { engine = drv.create(dev(f.vendorId, pid, [col(page || 0xff00, usage || 1, hit.n)]))?.constructor?.name ?? ""; } catch { engine = ""; }
    if (!engine) engine = `Driver${hit.i}`;
    if (!page) {
      // Which single collection satisfies the matcher: that is the interface the engine talks to.
      outer: for (const p of PAGES) for (const u of USAGES) {
        if (driverIndex(dev(f.vendorId, pid, [col(p, u, hit.n)])) === hit.i) { page = p; usage = u; break outer; }
      }
    }
  } else unresolved++;
  entries.push({
    vid: f.vendorId, pid: f.productId ?? 0xFFFF, vidOnly: f.productId === undefined,
    page, usage, brand, engine, name: f.productId === undefined ? "" : nameFor(pid, brand, engine),
  });
}

// Vendor-wide rows (Logitech, Fantech, ...) match any product of the vendor. Name the products
// OpenMouse's tables know for that family by adding exact rows that inherit the rule, so e.g. a
// Lightspeed receiver is reported by model instead of as "any Logitech".
for (const w of entries.filter((e) => e.vidOnly && e.engine !== "None")) {
  for (const [pid, cands] of names) {
    if (pid > 0xFFFE) continue;
    const c = cands.find((x) => norm(x.folder).includes(norm(w.brand)) || norm(w.brand).includes(norm(x.folder)));
    if (!c) continue;
    entries.push({ ...w, pid, vidOnly: false, name: c.name });
  }
}

// Duplicate (vid, pid) rows: keep the first, which is the one the registry order prefers.
const seen = new Set();
const rows = entries.filter((e) => { const k = e.vid + ":" + e.pid; if (seen.has(k)) return false; seen.add(k); return true; });
rows.sort((a, b) => a.vid - b.vid || a.pid - b.pid);
for (const r of rows) { r.b = idx(brands, r.brand); r.e = idx(engines, r.engine); }

// ---------- emit ----------
const esc = (s) => s.replace(/\\/g, "\\\\").replace(/"/g, '\\"');
const ident = (s) => "AMV_ENG_" + s.replace(/HidClient$|Client$/, "").replace(/([a-z0-9])([A-Z])/g, "$1_$2").replace(/[^A-Za-z0-9]+/g, "_").toUpperCase();
const engIds = engines.map(ident);

let h = `/* GENERATED by tools/openmouse/gen_catalog.mjs from OpenMouse commit ${commit}. Do not edit.
 *
 * Every device in OpenMouse's SUPPORTED_HID_FILTERS, with the engine its registry picks, the
 * vendor interface that engine talks to (usage page and usage, 0 = the mouse interface), the
 * brand, and a model name where OpenMouse's product tables carry one.
 * ${rows.length} rows, ${brands.length} brands, ${engines.length} engines; ${unresolved} filters had no matching engine.
 */
#pragma once
#include <stdint.h>

#define AMV_CATALOG_OPENMOUSE_COMMIT "${commit}"
#define AMV_CATALOG_COUNT ${rows.length}

#define AMV_CAT_VID_ONLY 0x01   /* every product of this vendor matches (pid is 0xFFFF) */

enum {
    AMV_ENG_NONE = 0,
`;
engines.forEach((e, i) => { if (engIds[i] !== "AMV_ENG_NONE") h += `    ${engIds[i]} = ${i + 1000},\n`; });
h += `};

typedef struct {
    uint16_t vid, pid;
    uint16_t page;       /* vendor interface usage page, 0 = none recorded */
    uint16_t engine;     /* AMV_ENG_*, indexes amv_cat_engines */
    uint16_t usage;
    uint8_t  brand;      /* index into amv_cat_brands */
    uint8_t  flags;
    const char *model;   /* "" when OpenMouse has no name for this id */
} amv_cat_t;

static const char *const amv_cat_brands[] = {
${brands.map((b) => `    "${esc(b)}",`).join("\n")}
};

/* engine id minus 1000 indexes this table; the TypeScript client class OpenMouse uses */
static const char *const amv_cat_engines[] = {
${engines.map((e) => `    "${esc(e)}",`).join("\n")}
};

/* sorted by (vid, pid) */
static const amv_cat_t amv_catalog[] = {
`;
for (const r of rows) {
  const eng = engIds[r.e] === "AMV_ENG_NONE" ? "AMV_ENG_NONE" : engIds[r.e];
  h += `    { 0x${r.vid.toString(16).padStart(4, "0")}, 0x${r.pid.toString(16).padStart(4, "0")}, 0x${r.page.toString(16).padStart(4, "0")}, ${eng}, ${r.usage}, ${r.b}, ${r.vidOnly ? "AMV_CAT_VID_ONLY" : "0"}, "${esc(r.name)}" },\n`;
}
h += "};\n";
writeFileSync(OUT, h);

const named = rows.filter((r) => r.name).length;
console.log(`catalog: ${rows.length} rows (${named} named, ${rows.filter((r) => r.vidOnly).length} vendor-wide), ${brands.length} brands, ${engines.length} engines, ${unresolved} filters unresolved -> ${OUT}`);
console.log(engines.map((e, i) => `${e}:${rows.filter((r) => r.e === i).length}`).join(" "));
