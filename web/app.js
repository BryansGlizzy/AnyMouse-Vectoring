// AnyMouse Studio: UI and state handling. The protocol lives in dongle.js, the demo device in mock_device.js.
import { P } from './proto.js';
import { DongleClient, DongleError, statusName } from './dongle.js';
import { MockHid } from './mock_device.js';
import { OCTANTS, PRESETS, MIRROR_LR, MIRROR_UD, clampWeight, round2, mean, exportProfile, importProfile } from './presets.js';
import { Radar } from './radar.js';

const $ = (s) => document.querySelector(s);
const DEMO = new URLSearchParams(location.search).has('demo');
// Only the vendor usage page is matched here; whether a device really is a dongle is decided by
// the HELLO handshake (dongle.js), so a changed USB VID:PID does not matter.
const FILTER = { usagePage: P.VENDOR_USAGE_PAGE };

const store = {
  get(k, d) { try { const v = localStorage.getItem('anymouse.' + k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem('anymouse.' + k, JSON.stringify(v)); } catch { /* storage blocked */ } },
};

const S = {
  client: null, hid: null, info: null,
  dev: null,                 // last state confirmed by the dongle {gen, w, bypass, slot}
  w: Array(8).fill(1),       // what the UI shows (may be ahead of the dongle while an edit is in flight)
  inFlight: false, dirty: false,
  slotsMask: 0, selSlot: 0, demo: DEMO,
  cpi: store.get('cpi', 800), linkLR: store.get('linkLR', false), linkUD: store.get('linkUD', false),
};

// ---------- small helpers ----------
let toastTimer;
function toast(msg, kind = '') {
  const t = $('#toast');
  t.textContent = msg; t.className = 'toast ' + kind; t.hidden = false;
  clearTimeout(toastTimer); toastTimer = setTimeout(() => { t.hidden = true; }, kind === 'bad' ? 6000 : 3200);
}
function banner(html, bad = false) {
  const b = $('#banner');
  if (!html) { b.hidden = true; return; }
  b.innerHTML = html; b.className = 'banner' + (bad ? ' bad' : ''); b.hidden = false;
}
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const sameW = (a, b) => a.length === b.length && a.every((x, i) => Math.abs(x - b[i]) < 5e-3);
const connected = () => !!S.client && !!S.dev;

// ---------- build static UI ----------
const rowsEl = $('#rows');
OCTANTS.forEach((o, i) => {
  const row = document.createElement('div');
  row.className = 'row'; row.dataset.i = i;
  row.innerHTML = `<label for="n${i}"><b>${o.short}</b><span>${o.name}</span></label>
    <input type="range" class="sl dev" min="0.1" max="3" step="0.01" aria-label="${o.name} weight" title="${o.hint}">
    <input type="number" id="n${i}" class="num dev" min="${P.WEIGHT_MIN}" max="${P.WEIGHT_MAX}" step="0.01">
    <span class="dpi" aria-label="effective DPI"></span>`;
  rowsEl.append(row);
  row.querySelector('.sl').addEventListener('input', (e) => userEdit(i, parseFloat(e.target.value)));
  row.querySelector('.num').addEventListener('change', (e) => {
    const v = parseFloat(e.target.value);
    if (!Number.isFinite(v)) { render(); return; }
    userEdit(i, v);
  });
});
const presetsEl = $('#presets');
PRESETS.forEach((p) => {
  const b = document.createElement('button');
  b.type = 'button'; b.className = 'dev'; b.textContent = p.label;
  b.addEventListener('click', () => { S.w = p.w.slice(); render(); scheduleSend(); });
  presetsEl.append(b);
});
const slotsEl = $('#slots');
for (let i = 0; i < P.NUM_SLOTS; i++) {
  const b = document.createElement('button');
  b.type = 'button'; b.className = 'slot dev'; b.dataset.i = i; b.setAttribute('role', 'radio');
  b.innerHTML = `<span class="badge"></span>Slot ${i + 1}<small></small>`;
  b.addEventListener('click', () => { S.selSlot = i; render(); });
  slotsEl.append(b);
}

const radar = new Radar($('#radar'), {
  onChange: (i, v) => userEdit(i, v, { fromRadar: true }),
});

// ---------- editing ----------
function partners(i) {
  const seen = new Set([i]), maps = [];
  if (S.linkLR) maps.push(MIRROR_LR);
  if (S.linkUD) maps.push(MIRROR_UD);
  let grew = true;
  while (grew) {
    grew = false;
    for (const m of maps) for (const [a, b] of m) {
      if (seen.has(a) && !seen.has(b)) { seen.add(b); grew = true; }
      if (seen.has(b) && !seen.has(a)) { seen.add(a); grew = true; }
    }
  }
  return seen;
}

function userEdit(i, v, { fromRadar = false } = {}) {
  if (!connected()) return;
  v = clampWeight(round2(v));
  for (const k of partners(i)) S.w[k] = v;
  render({ skipRadar: fromRadar });
  scheduleSend();
}

function scheduleSend() {
  if (S.inFlight) { S.dirty = true; return; }
  flush();
}

async function flush() {
  S.inFlight = true;
  try {
    do {
      S.dirty = false;
      const sent = S.w.slice();
      const r = await S.client.setWeights(sent, S.dev.gen);
      if (r.state) S.dev = r.state;
      if (r.ok) {
        // keep any newer edit the user made while this one was in flight
        if (!S.dirty) S.w = r.state.w.slice();
      } else if (r.status === P.ST.STALE) {
        S.w = r.state.w.slice(); S.dirty = false;
        toast('Changed elsewhere, so your edit was dropped and the page refreshed.', 'bad');
      } else {
        if (r.state) S.w = r.state.w.slice();
        S.dirty = false;
        toast(`The dongle refused that (${statusName(r.status)}).`, 'bad');
      }
      render();
      if (S.dirty) await sleep(25);              // coalesce rapid drags to about 30 updates a second
    } while (S.dirty);
  } catch (e) {
    S.dirty = false;
    failure(e);
  } finally {
    S.inFlight = false;
  }
}

async function idle() { while (S.inFlight || S.dirty) await sleep(10); }

// ---------- device events ----------
function onDeviceState(ev) {
  const e = ev.detail;
  if (S.dev && e.gen < S.dev.gen) return;                       // older than what we already know
  const changed = !S.dev || !sameW(e.w, S.dev.w);
  S.dev = { gen: e.gen, w: e.w, bypass: e.bypass, slot: e.slot };
  S.selSlot = e.slot;
  if (!S.inFlight && !S.dirty) {
    if (changed && !sameW(e.w, S.w)) {
      S.w = e.w.slice();
      toast(e.source === P.SRC.DEVICE ? 'The dongle changed its weights.' : e.source === P.SRC.LOAD ? 'A profile was loaded on the dongle.' : 'Weights updated from another app.');
    }
  }
  refreshSlots();
  render();
}

// ---------- connection ----------
function setState(state, text) {
  document.body.dataset.state = state;
  $('#chipText').textContent = text;
  $('#btnConnect').hidden = state === 'connected' || state === 'demo';
  $('#btnDisconnect').hidden = !(state === 'connected' || state === 'demo');
}

function setEnabled(on) {
  document.querySelectorAll('.dev').forEach((el) => { el.disabled = !on; });
  radar.setEnabled(on);
}

function hint(e) {
  const m = String(e?.message || e);
  if (/denied|NotAllowed|access/i.test(m)) {
    return `<b>Could not open the dongle.</b>Close any other program using it (the AnyMouse app, another browser tab). On Linux, add the udev rule from <code>web/README.md</code> so the browser may open <code>/dev/hidraw*</code>; Flatpak and Snap browsers cannot see it at all.`;
  }
  return `<b>Could not talk to the dongle.</b>${esc(m)}`;
}

async function attach(hid, demo = false) {
  if (S.client) await detach(true);
  const client = new DongleClient(hid);
  try {
    await client.open();
    S.client = client; S.hid = hid; S.demo = demo;
    S.hello = await client.hello();
    S.info = await client.getInfo();
    S.dev = await client.getState();
    S.w = S.dev.w.slice(); S.selSlot = S.dev.slot;
    S.slotsMask = await client.getSlotsMask();
    client.addEventListener('state', onDeviceState);
    banner('');
    setState(demo ? 'demo' : 'connected', demo ? 'Demo (simulated dongle)' : `Connected · firmware ${S.info.fw}`);
    setEnabled(true);
    render();
  } catch (e) {
    S.client = null; S.dev = null;
    try { await client.close(); } catch { /* ignore */ }
    setState('error', 'Not connected'); setEnabled(false);
    banner(hint(e), true);
    render();
  }
}

async function detach(silent = false) {
  const c = S.client;
  S.client = null; S.dev = null; S.info = null; S.inFlight = false; S.dirty = false;
  try { await c?.close(); } catch { /* already gone */ }
  if (!silent) { setState('disconnected', 'Not connected'); setEnabled(false); render(); }
}

function failure(e) {
  if (e instanceof DongleError && (e.status === -1 || e.status === -2)) {
    detach();
    setState('error', 'Connection lost');
    banner(`<b>Lost contact with the dongle.</b>${esc(e.message)}. Reconnect it, then press <i>Connect dongle</i>.`, true);
  } else {
    toast(e.message || String(e), 'bad');
  }
}

async function connectClicked() {
  if (DEMO) { await attach(new MockHid({ gen: 7, w: [1, 1.15, 1.3, 1.15, 1, 1, 1, 1], filledSlots: [0, 2], slot: 0 }, { latencyMs: 3 }), true); return; }
  try {
    const [dev] = await navigator.hid.requestDevice({ filters: [FILTER] });
    if (!dev) { toast('No device selected.'); return; }
    await attach(dev);
  } catch (e) {
    setState('error', 'Not connected'); banner(hint(e), true);
  }
}

async function refreshSlots() {
  if (!S.client) return;
  try { S.slotsMask = await S.client.getSlotsMask(); render(); } catch { /* shown elsewhere */ }
}

// ---------- actions ----------
async function act(fn) {
  if (!connected()) return;
  try { await idle(); await fn(); } catch (e) { failure(e); }
}

$('#btnConnect').addEventListener('click', connectClicked);
$('#btnDisconnect').addEventListener('click', () => detach());

$('#bypass').addEventListener('change', (e) => act(async () => {
  const r = await S.client.setBypass(e.target.checked);
  if (r.state) { S.dev = r.state; }
  render();
}));

$('#master').addEventListener('input', (e) => {
  if (!connected()) return;
  const target = parseFloat(e.target.value), m = mean(S.w);
  if (!(m > 0)) return;
  S.w = S.w.map((x) => clampWeight(round2(x * (target / m))));
  render({ keepMaster: true });
  scheduleSend();
});

for (const id of ['linkLR', 'linkUD']) {
  $('#' + id).checked = S[id];
  $('#' + id).addEventListener('change', (e) => { S[id] = e.target.checked; store.set(id, S[id]); });
}
$('#cpi').value = S.cpi;
$('#cpi').addEventListener('change', (e) => {
  const v = parseInt(e.target.value, 10);
  if (v >= 100 && v <= 50000) { S.cpi = v; store.set('cpi', v); }
  e.target.value = S.cpi; render();
});

$('#btnLoad').addEventListener('click', () => act(async () => {
  const r = await S.client.loadSlot(S.selSlot);
  if (r.status === P.ST.EMPTY) { toast(`Slot ${S.selSlot + 1} is empty.`); return; }
  if (!r.ok) { toast(`Could not load (${statusName(r.status)}).`, 'bad'); return; }
  S.dev = r.state; S.w = r.state.w.slice(); toast(`Loaded slot ${S.selSlot + 1}.`, 'good'); await refreshSlots(); render();
}));

$('#btnSave').addEventListener('click', () => act(async () => {
  const filled = S.slotsMask >> S.selSlot & 1;
  if (filled && S.dev.slot !== S.selSlot && !confirm(`Overwrite slot ${S.selSlot + 1}?`)) return;
  const r = await S.client.saveSlot(S.selSlot);
  if (!r.ok) { toast(`Could not save (${statusName(r.status)}).`, 'bad'); return; }
  S.dev = r.state; toast(`Saved to slot ${S.selSlot + 1}.`, 'good'); await refreshSlots(); render();
}));

$('#btnExport').addEventListener('click', () => {
  const blob = new Blob([exportProfile(S.w, S.cpi)], { type: 'application/json' });
  const a = Object.assign(document.createElement('a'), { href: URL.createObjectURL(blob), download: 'anymouse-profile.json' });
  document.body.append(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(a.href), 2000);
});
$('#btnImport').addEventListener('click', () => $('#fileImport').click());
$('#fileImport').addEventListener('change', async (e) => {
  const f = e.target.files[0]; e.target.value = '';
  if (!f) return;
  try {
    const { weights, note, cpi } = importProfile(await f.text());
    S.w = weights; if (cpi >= 100 && cpi <= 50000) { S.cpi = cpi; store.set('cpi', cpi); $('#cpi').value = cpi; }
    render(); scheduleSend(); toast(note || 'Profile imported.', 'good');
  } catch (err) { toast(`Import failed: ${err.message}`, 'bad'); }
});

$('#btnFactory').addEventListener('click', () => act(async () => {
  if (!confirm('Erase all saved profiles on the dongle and reset every weight to 1.0?')) return;
  const r = await S.client.factoryReset();
  if (r.state) { S.dev = r.state; S.w = r.state.w.slice(); }
  await refreshSlots(); render(); toast('Dongle reset to factory defaults.', 'good');
}));

$('#btnScale').addEventListener('click', () => act(async () => {
  const dx = parseInt($('#tsDx').value, 10) || 0, dy = parseInt($('#tsDy').value, 10) || 0;
  const r = await S.client.testScale(dx, dy, true);
  $('#scaleOut').textContent = `in (${dx}, ${dy}) → out (${r.dx}, ${r.dy}), direction ${OCTANTS[r.octant].short}, leftover (${r.remX.toFixed(3)}, ${r.remY.toFixed(3)})`;
}));
$('#btnStats').addEventListener('click', () => act(async () => {
  const s = await S.client.getStats(), o = $('#statsOut');
  o.textContent = Object.entries(s).map(([k, v]) => `${k.padEnd(16)}${v}`).join('\n'); o.hidden = false;
}));
$('#btnReboot').addEventListener('click', () => act(async () => {
  if (!confirm('Reboot the dongle? It will reconnect on its own.')) return;
  await S.client.reboot();
  toast('Rebooting…');
  if (S.demo) return;
  await detach(); setState('disconnected', 'Rebooting…');
}));

// ---------- render ----------
function render({ skipRadar = false, keepMaster = false } = {}) {
  const on = connected();
  document.querySelectorAll('.row').forEach((row) => {
    const i = +row.dataset.i, v = S.w[i];
    const sl = row.querySelector('.sl'), num = row.querySelector('.num');
    if (document.activeElement !== sl) sl.value = v;
    if (document.activeElement !== num) num.value = v.toFixed(2);
    row.querySelector('.dpi').textContent = `${Math.round(v * S.cpi)} DPI`;
  });
  if (!skipRadar) radar.setWeights(S.w);
  else { radar.w = S.w.slice(); radar.draw(); }
  const m = mean(S.w);
  if (!keepMaster) $('#master').value = m;
  $('#masterOut').textContent = m.toFixed(2);
  $('#bypass').checked = !!S.dev?.bypass;

  slotsEl.querySelectorAll('.slot').forEach((b) => {
    const i = +b.dataset.i, filled = S.slotsMask >> i & 1, active = S.dev?.slot === i;
    b.classList.toggle('filled', !!filled); b.classList.toggle('active', active && on);
    b.setAttribute('aria-checked', String(S.selSlot === i));
    b.querySelector('small').textContent = filled ? 'saved' : 'empty';
  });

  const rows = [];
  if (on) {
    const pending = S.inFlight || S.dirty;
    rows.push(['Firmware', `${S.info.fw} (protocol ${S.info.proto})`], ['Chip revision', `v${(S.info.chipRev / 100).toFixed(2)}`],
      ['Serial (MAC)', S.info.mac], ['Active profile', `slot ${S.dev.slot + 1}`], ['Revision', `#${S.dev.gen}${pending ? ' (sending…)' : ''}`],
      ['Last round trip', S.client.lastRttMs == null ? '—' : `${S.client.lastRttMs.toFixed(1)} ms`],
      ['Mouse input', S.info.hasMouse ? 'connected' : 'not available in this firmware yet']);
  } else {
    rows.push(['Status', 'Connect the dongle with its second USB-C cable to the computer, then press Connect.']);
  }
  $('#status').innerHTML = rows.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${esc(v)}</dd>`).join('');
}

// ---------- startup ----------
function init() {
  radar.setEnabled(false); setEnabled(false); render();
  $('#demoLink').hidden = DEMO;
  if (DEMO) {
    $('#demoLink').hidden = true;
    banner('<b>Demo mode.</b>This page is talking to a simulated dongle built into the page. Nothing is sent over USB.');
    connectClicked();
    return;
  }
  if (!('hid' in navigator)) {
    $('#btnConnect').disabled = true;
    banner('<b>This browser cannot talk to USB devices.</b>AnyMouse Studio needs WebHID: use Chrome, Edge, Brave or Opera on a computer (not Firefox, Safari or a phone). You can still <a href="?demo">try the demo</a>.', true);
    return;
  }
  const matches = (d) => d.collections.some((c) => c.usagePage === FILTER.usagePage);
  navigator.hid.getDevices().then((ds) => { const d = ds.find(matches); if (d) attach(d); }).catch(() => {});
  navigator.hid.addEventListener('connect', (e) => { if (matches(e.device) && !S.client) attach(e.device); });
  navigator.hid.addEventListener('disconnect', (e) => {
    if (e.device === S.hid) { detach(); setState('disconnected', 'Dongle unplugged or rebooting…'); }
  });
  if ('serviceWorker' in navigator && (location.protocol === 'https:' || location.hostname === 'localhost')) {
    navigator.serviceWorker.register('sw.js').catch(() => {});
  }
}
init();
