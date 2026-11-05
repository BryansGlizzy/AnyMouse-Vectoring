// Presets and profile import/export. Dongle weights are plain multipliers on the mouse's own
// movement (1.0 = unchanged), unlike the Linux engine, whose weights also divide down a high sensor CPI.
import { P } from './proto.js';

export const OCTANTS = [
  { short: 'E',  name: 'East',       angle: 0,   hint: 'Right lateral swipe' },
  { short: 'NE', name: 'North-East', angle: 45,  hint: 'Up-right flick' },
  { short: 'N',  name: 'North',      angle: 90,  hint: 'Upward push' },
  { short: 'NW', name: 'North-West', angle: 135, hint: 'Up-left flick' },
  { short: 'W',  name: 'West',       angle: 180, hint: 'Left lateral swipe' },
  { short: 'SW', name: 'South-West', angle: 225, hint: 'Down-left pull' },
  { short: 'S',  name: 'South',      angle: 270, hint: 'Downward pull' },
  { short: 'SE', name: 'South-East', angle: 315, hint: 'Down-right pull' },
];

// The Linux engine's presets divided by its 0.25 baseline, so the shapes carry over.
export const PRESETS = [
  { id: 'neutral',    label: 'Neutral',          w: [1, 1, 1, 1, 1, 1, 1, 1] },
  { id: 'vertical',   label: 'Vertical boost',   w: [1, 1.4, 1.8, 1.4, 1, 1, 1, 1] },
  { id: 'horizontal', label: 'Horizontal boost', w: [1.6, 1, 1, 1, 1.6, 1, 1, 1] },
  { id: 'diagonal',   label: 'Diagonal ULX',     w: [1, 1.2, 1, 1.12, 1, 0.88, 1, 1.04] },
  { id: 'half',       label: 'Half (0.5x)',      w: [0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5] },
  { id: 'double',     label: 'Double (2x)',      w: [2, 2, 2, 2, 2, 2, 2, 2] },
];

// Mirror pairs for the symmetry links (index = octant, 0 = E, counter-clockwise).
export const MIRROR_LR = [[0, 4], [1, 3], [7, 5]];   // east <-> west
export const MIRROR_UD = [[1, 7], [2, 6], [3, 5]];   // north <-> south

export const PROFILE_FORMAT = 'anymouse-dongle-profile';

export const clampWeight = (v) => Math.min(P.WEIGHT_MAX, Math.max(P.WEIGHT_MIN, v));
export const round2 = (v) => Math.round(v * 100) / 100;
export const mean = (w) => w.reduce((a, b) => a + b, 0) / w.length;

export function weightsValid(w) {
  return Array.isArray(w) && w.length === 8 && w.every((x) => typeof x === 'number' && Number.isFinite(x) && x >= P.WEIGHT_MIN && x <= P.WEIGHT_MAX);
}

export function exportProfile(w, mouseCpi) {
  return JSON.stringify({ format: PROFILE_FORMAT, version: 1, octant_weights: w.map(round2), mouse_cpi: mouseCpi, exported: new Date().toISOString() }, null, 2) + '\n';
}

/** Returns {weights, note}. Accepts this app's format, or a profile.json written by the Linux engine. */
export function importProfile(text) {
  let j;
  try { j = JSON.parse(text); } catch { throw new Error('Not valid JSON'); }
  const w = j?.octant_weights;
  if (!Array.isArray(w) || w.length !== 8 || !w.every((x) => typeof x === 'number' && Number.isFinite(x))) {
    throw new Error('No octant_weights array of 8 numbers found');
  }
  if (j.format === PROFILE_FORMAT) {
    if (!weightsValid(w)) throw new Error(`Weights must be between ${P.WEIGHT_MIN} and ${P.WEIGHT_MAX}`);
    return { weights: w.slice(), note: '', cpi: j.mouse_cpi };
  }
  // Linux-engine profile: weights are relative to a high sensor CPI, so keep the shape and average to 1.0.
  const m = mean(w);
  if (!(m > 0)) throw new Error('Weights must be positive');
  const conv = w.map((x) => round2(clampWeight(x / m)));
  return { weights: conv, note: 'Linux-engine profile: kept the shape and scaled it so the average is 1.0.', cpi: j.hardware_cpi };
}
