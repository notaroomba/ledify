// The preview engine: draws, frame by frame, roughly what the strip is showing, from the same state the board reports
// (and the user's optimistic edits). The board's own pipeline is colour source -> sound brightness -> overlay -> zones
// -> brightness; this follows it.
import { ui, dev, state, opt } from './store';
import { grad } from './grad';
import { hexRgb } from './color';
import { parseStops, distinctColours } from './gradient';
import { SOURCE, SOURCES, effectOf } from './effects';

export function inputs() {
  const s = state(), z = dev().sensors || {};
  const spec = ui.live.spec;
  const lens = (() => { const st = opt.stripe(); return !st || st === '0' ? [] : st.split(',').map(v => Math.max(1, +v || 1)); })();
  return {
    rgb: hexRgb(opt.color() || '#ffffff'), stops: grad.stops(), cover: parseStops(s.cover || 'ffffff@0'),
    stripes: lens, stripeCols: distinctColours(grad.stops()).map(hexRgb), count: s.count || 300, scroll: opt.scroll() || 0,
    live: ui.live, temp: z.temp_c ?? s.temp_c ?? 20, tempLo: s.temp_lo ?? 15, tempHi: s.temp_hi ?? 30,
    spec: b => spec ? (spec[b] || 0) / 255 : (ui.live.bands[Math.min(2, Math.floor(b * 3 / 16))] || 0) / 255 * (0.6 + 0.4 * Math.sin(b * 1.7 + performance.now() / 300)),
  };
}

// What the mode means: colour source, whether brightness follows the sound, overlay, or a custom painter.
export function resolve(mode, s = state(), overlay = opt.overlay()) {
  let src = SOURCES.includes(s.color) ? s.color : 'solid', snd = s.bright === 'sound', custom = null;
  if (SOURCES.includes(mode)) { src = mode; snd = false; }
  else if (mode === 'sound') snd = true;
  else if (mode === 'vu') { src = 'meter'; snd = false; }
  else if (mode === 'pulse') snd = false;
  else if (mode && mode !== 'auto') { const e = effectOf(mode); if (e) custom = e; }
  return { src, snd, ov: overlay || 'none', custom };
}

// Fill px (n colours) for time t. opts.mode overrides the current mode (the effect cards preview their own).
export function paint(px, n, t, x, opts = {}) {
  const mode = opts.mode ?? opt.mode();
  const r = opts.mode ? resolve(mode, state(), ['vu', 'pulse'].includes(mode) ? mode : 'none') : resolve(mode);
  if (r.custom && r.custom.paint && !SOURCES.includes(r.custom.id)) r.custom.paint(px, n, t, x);
  else if (r.custom && !r.custom.paint) shimmer(px, n, t, x);
  else SOURCE[r.src](px, n, t, x);
  const loud = x.live.loud / 255;
  if (r.snd) for (const p of px) { p[0] *= loud; p[1] *= loud; p[2] *= loud; }
  if (r.ov === 'vu') px.forEach((p, i) => { if (i / n >= loud) p.fill(0); });
  else if (r.ov === 'pulse') { const f = 0.25 + 0.75 * x.live.pulse / 255; for (const p of px) { p[0] *= f; p[1] *= f; p[2] *= f; } }
  if (!opts.mode) zones(px, n, t, x);
}
function shimmer(px, n, t, x) {
  const loud = x.live.loud / 255;
  px.forEach((p, i) => { const f = 0.25 + 0.75 * loud * (0.6 + 0.4 * Math.sin(i * 0.9 + t * 7)); p[0] = x.rgb[0] * f; p[1] = x.rgb[1] * f; p[2] = x.rgb[2] * f; });
}
const tmp = [];
function zones(px, n, t, x) {
  const segs = state().segs;
  if (!segs || !segs.length) return;
  const count = x.count;
  for (const s of segs) {
    const a = Math.floor(s.a * n / count), b = Math.ceil(s.b * n / count);
    while (tmp.length < n) tmp.push([0, 0, 0]);
    const sub = tmp.slice(0, n);
    const fn = SOURCE[s.color] || SOURCE.solid;
    fn(sub, n, t, s.color === 'solid' ? { ...x, rgb: hexRgb('#' + (s.rgb || 'ffffff')) } : x);
    for (let i = Math.max(0, a); i < Math.min(n, b); i++) { px[i][0] = sub[i][0]; px[i][1] = sub[i][1]; px[i][2] = sub[i][2]; }
  }
}
// Brightness as the eye sees it on a screen: the strip's 0..255 level, with a floor so a dim strip still reads.
export const shown = () => {
  const s = state();
  if (!opt.power()) return 0;
  let lv = (opt.level() ?? 128) / 255;
  if (s.limit && s.cap < s.level) lv = Math.min(lv, s.cap / 255);
  return 0.18 + 0.82 * Math.sqrt(lv);
};
