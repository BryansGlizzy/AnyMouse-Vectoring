// Polar chart of the eight direction weights. The outline is the real sensitivity envelope: the same
// cosine blend between neighbouring directions that the dongle applies, not straight lines between handles.
import { OCTANTS, clampWeight, round2 } from './presets.js';

const RAD = Math.PI / 180;

export class Radar {
  constructor(canvas, { onChange, onCommit } = {}) {
    this.c = canvas;
    this.ctx = canvas.getContext('2d');
    this.w = Array(8).fill(1);
    this.enabled = false;
    this.onChange = onChange;
    this.onCommit = onCommit;
    this.drag = -1;
    this.hover = -1;
    this.lockedMax = null;
    canvas.addEventListener('pointerdown', (e) => this._down(e));
    canvas.addEventListener('pointermove', (e) => this._move(e));
    canvas.addEventListener('pointerup', (e) => this._up(e));
    canvas.addEventListener('pointercancel', (e) => this._up(e));
    canvas.addEventListener('pointerleave', () => { if (this.drag < 0) { this.hover = -1; this.draw(); } });
    new ResizeObserver(() => this.draw()).observe(canvas);
  }

  setWeights(w) { this.w = w.slice(); this.draw(); }
  setEnabled(on) { this.enabled = on; this.c.style.cursor = on ? 'default' : 'not-allowed'; this.draw(); }

  get rMax() {
    return this.lockedMax ?? Math.max(2, Math.ceil(Math.max(...this.w) * 2) / 2);
  }

  _geom() {
    const r = this.c.getBoundingClientRect();
    const pad = 38;
    return { cx: r.width / 2, cy: r.height / 2, R: Math.max(10, Math.min(r.width, r.height) / 2 - pad), w: r.width, h: r.height };
  }

  _handle(g, k, v = this.w[k]) {
    const a = OCTANTS[k].angle * RAD, r = (v / this.rMax) * g.R;
    return { x: g.cx + r * Math.cos(a), y: g.cy - r * Math.sin(a) };
  }

  _pos(e) { const r = this.c.getBoundingClientRect(); return { x: e.clientX - r.left, y: e.clientY - r.top }; }

  _nearest(p) {
    const g = this._geom();
    let best = -1, bd = 22;
    for (let k = 0; k < 8; k++) {
      const h = this._handle(g, k), d = Math.hypot(h.x - p.x, h.y - p.y);
      if (d < bd) { bd = d; best = k; }
    }
    return best;
  }

  _down(e) {
    if (!this.enabled) return;
    const k = this._nearest(this._pos(e));
    if (k < 0) return;
    this.drag = k;
    this.lockedMax = Math.max(this.rMax, 3);       // keep the scale steady while dragging
    this.c.setPointerCapture(e.pointerId);
    this._move(e);
  }

  _move(e) {
    const p = this._pos(e);
    if (this.drag < 0) {
      const k = this.enabled ? this._nearest(p) : -1;
      if (k !== this.hover) { this.hover = k; this.c.style.cursor = k >= 0 ? 'grab' : (this.enabled ? 'default' : 'not-allowed'); this.draw(); }
      return;
    }
    const g = this._geom(), a = OCTANTS[this.drag].angle * RAD;
    const along = (p.x - g.cx) * Math.cos(a) - (p.y - g.cy) * Math.sin(a);   // distance along the spoke
    const v = clampWeight(round2((along / g.R) * this.rMax));
    if (v !== this.w[this.drag]) {
      this.w[this.drag] = v;
      this.onChange?.(this.drag, v);
    }
    this.draw();
  }

  _up(e) {
    if (this.drag < 0) return;
    this.drag = -1;
    this.lockedMax = null;
    try { this.c.releasePointerCapture(e.pointerId); } catch { /* already released */ }
    this.onCommit?.();
    this.draw();
  }

  draw() {
    const c = this.c, dpr = window.devicePixelRatio || 1;
    const rect = c.getBoundingClientRect();
    if (!rect.width) return;
    if (c.width !== Math.round(rect.width * dpr) || c.height !== Math.round(rect.height * dpr)) {
      c.width = Math.round(rect.width * dpr); c.height = Math.round(rect.height * dpr);
    }
    const ctx = this.ctx;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, rect.width, rect.height);
    const css = getComputedStyle(c), col = (n) => css.getPropertyValue(n).trim() || '#888';
    const accent = col('--accent'), line = col('--line'), muted = col('--muted'), text = col('--text');
    const g = this._geom(), rMax = this.rMax;
    ctx.font = '12px system-ui, sans-serif';
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';

    // rings every 0.5; the 1.0 ring (the mouse's own sensitivity) is dashed and brighter
    for (let v = 0.5; v <= rMax + 1e-6; v += 0.5) {
      const r = (v / rMax) * g.R, unity = Math.abs(v - 1) < 1e-6;
      ctx.beginPath(); ctx.arc(g.cx, g.cy, r, 0, 2 * Math.PI);
      ctx.strokeStyle = unity ? muted : line; ctx.lineWidth = unity ? 1.2 : 1;
      ctx.setLineDash(unity ? [5, 4] : []); ctx.stroke(); ctx.setLineDash([]);
      ctx.fillStyle = muted; ctx.textAlign = 'left';
      ctx.fillText(v.toFixed(1), g.cx + r * Math.cos(20 * RAD) + 3, g.cy + r * Math.sin(20 * RAD));   // just below the east spoke
    }
    ctx.textAlign = 'center';

    // spokes and direction labels
    for (let k = 0; k < 8; k++) {
      const a = OCTANTS[k].angle * RAD;
      ctx.beginPath(); ctx.moveTo(g.cx, g.cy); ctx.lineTo(g.cx + g.R * Math.cos(a), g.cy - g.R * Math.sin(a));
      ctx.strokeStyle = line; ctx.lineWidth = 1; ctx.stroke();
      ctx.fillStyle = (k === this.hover || k === this.drag) ? accent : text;
      ctx.font = `${k % 2 ? 500 : 650} 13px system-ui, sans-serif`;
      ctx.fillText(OCTANTS[k].short, g.cx + (g.R + 20) * Math.cos(a), g.cy - (g.R + 20) * Math.sin(a));
    }

    // the true envelope: cosine blend between neighbouring directions
    ctx.beginPath();
    for (let deg = 0; deg <= 360; deg += 2) {
      const pos = deg / 45, k0 = Math.floor(pos) & 7, k1 = (k0 + 1) & 7, t = pos - Math.floor(pos);
      const sm = (1 - Math.cos(t * Math.PI)) / 2, s = this.w[k0] * (1 - sm) + this.w[k1] * sm;
      const r = (s / rMax) * g.R, a = deg * RAD;
      const x = g.cx + r * Math.cos(a), y = g.cy - r * Math.sin(a);
      deg === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    }
    ctx.closePath();
    ctx.fillStyle = accent + '2e'; ctx.fill();
    ctx.strokeStyle = accent; ctx.lineWidth = 2.2; ctx.lineJoin = 'round'; ctx.stroke();

    // handles
    for (let k = 0; k < 8; k++) {
      const h = this._handle(g, k), on = k === this.hover || k === this.drag;
      ctx.beginPath(); ctx.arc(h.x, h.y, on ? 9 : 6.5, 0, 2 * Math.PI);
      ctx.fillStyle = this.enabled ? (on ? '#fff' : accent) : muted; ctx.fill();
      ctx.strokeStyle = col('--panel'); ctx.lineWidth = 2; ctx.stroke();
      if (on) {
        ctx.fillStyle = text; ctx.font = '650 12px system-ui, sans-serif';
        ctx.fillText(this.w[k].toFixed(2), h.x, h.y - 18);
      }
    }
  }
}
