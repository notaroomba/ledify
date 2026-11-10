// Every effect the panel offers, in one list. The board's mode names are the ids. paint(px, n, t, x) fills px (an array
// of n [r, g, b] 0..255 colours, before brightness) for the preview ring; x carries the inputs (colour, gradient,
// cover, live sound, temperature). An effect without paint gets a sound-tinted shimmer of the current colour.
// `fw: 'react'` marks modes that need newer firmware: they show once the board reports that state field.
// Motion modes read x.live.mo ([tilt, side, heading, energy, height cm], from the board's IMU while the panel is open); without it
// the previews sway on their own.
import { hsv, mix, rgbHsv } from './color';
import { rgbAt } from './gradient';

const fill = (px, c) => { for (const p of px) { p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; } };
const scale = (c, f) => [c[0] * f, c[1] * f, c[2] * f];
const band = (x, k) => (x.live.bands[k] || 0) / 255;
const clamp1 = v => Math.max(-1, Math.min(1, v));
const motion = (x, t) => {
  const m = x.live.mo;
  return m ? { tilt: m[0] / 100, side: m[1] / 100, heading: m[2], energy: m[3] / 100, height: m[4] ?? 0 }
    : { tilt: 0.3 * Math.sin(t * 0.9), side: 0.25 * Math.sin(t * 0.6 + 1), heading: (t * 40) % 360, energy: Math.max(0, Math.sin(t * 1.3)) ** 4, height: 25 * Math.sin(t * 0.7) };
};
// Fused modes: colours from their own list, or along the user's gradient when "fxpal" is on (as on the board).
const AURORA = [[0, 255, 70], [0, 220, 150], [0, 130, 255], [110, 40, 255], [210, 0, 170]];
const lutAt = (l, f) => { const x = Math.max(0, Math.min(1, f)) * (l.length - 1), k = Math.min(l.length - 2, Math.floor(x)); return mix(l[k], l[k + 1], x - k); };
const tone = (x, own, f) => x.fxpal ? rgbAt(x.stops, Math.max(0, Math.min(1, f)) * 100) : own ? lutAt(own, f) : hsv(((f % 1) + 1) % 1);
const heat = (x, h) => x.fxpal ? scale(rgbAt(x.stops, h * 100), Math.min(1, h * 3))
  : h < 1 / 3 ? [765 * h, 0, 0] : h < 2 / 3 ? [255, 765 * (h - 1 / 3), 0] : [255, 255, 765 * (h - 2 / 3)];
const noise = (i, k) => { const v = Math.sin(i * 127.1 + k * 311.7) * 43758.5453; return v - Math.floor(v); };

export const SOURCE = {
  solid: (px, n, t, x) => fill(px, x.rgb),
  rainbow: (px, n, t) => px.forEach((p, i) => p.splice(0, 3, ...hsv(((i / n) + t * 0.12) % 1))),
  temp: (px, n, t, x) => {
    const f = Math.max(0, Math.min(1, (x.temp - x.tempLo) / Math.max(1, x.tempHi - x.tempLo)));
    fill(px, f < 0.5 ? mix([40, 90, 255], [255, 255, 255], f * 2) : mix([255, 255, 255], [255, 40, 20], f * 2 - 1));
  },
  spectrum: (px, n, t, x) => px.forEach((p, i) => {
    const k = Math.min(2, Math.floor(i * 3 / n)), pos = (i - k * n / 3) / (n / 3);
    p.splice(0, 3, ...(pos < band(x, k) ? [[255, 30, 30], [40, 255, 60], [40, 120, 255]][k] : [0, 0, 0]));
  }),
  meter: (px, n) => px.forEach((p, i) => p.splice(0, 3, ...hsv((1 - i / n) / 3))),
  palette: (px, n, t, x) => {
    const shift = t * (x.scroll || 0);
    if (x.stripes.length) {
      const cols = x.stripeCols, lens = cols.map((_, k) => x.stripes[k % x.stripes.length]), total = lens.reduce((a, b) => a + b, 0) || 1;
      const scaleTo = x.count / n;
      px.forEach((p, i) => {
        let pos = (((i * scaleTo - shift) % total) + total) % total, k = 0;
        while (k < lens.length - 1 && pos >= lens[k]) { pos -= lens[k]; k++; }
        p.splice(0, 3, ...cols[k]);
      });
    } else px.forEach((p, i) => { const q = (((i - shift * n / x.count) % n) + n) % n; p.splice(0, 3, ...rgbAt(x.stops, q * 100 / n)); });
  },
  cover: (px, n, t, x) => px.forEach((p, i) => p.splice(0, 3, ...rgbAt(x.cover, i * 100 / n))),
};

export const EFFECTS = [
  { id: 'solid', label: 'Solid', desc: 'one steady colour', group: 'colour', paint: SOURCE.solid },
  { id: 'palette', label: 'Gradient', desc: 'your gradient along the strip', group: 'colour', paint: SOURCE.palette },
  { id: 'rainbow', label: 'Rainbow', desc: 'a rainbow drifting along', group: 'colour', paint: SOURCE.rainbow },
  { id: 'cover', label: 'Album', desc: 'the colours of the album playing on Spotify', group: 'colour', paint: SOURCE.cover },
  { id: 'temp', label: 'Thermo', desc: 'blue to red with the temperature', group: 'colour', paint: SOURCE.temp },
  { id: 'meter', label: 'Meter', desc: 'green to red along the strip', group: 'colour', paint: SOURCE.meter },
  { id: 'sound', label: 'Breathe', desc: 'brightness follows the sound', group: 'sound' },
  { id: 'vu', label: 'VU', desc: 'lit length follows the sound', group: 'sound' },
  { id: 'pulse', label: 'Pulse', desc: 'flashes on every beat', group: 'sound' },
  { id: 'spectrum', label: 'Bars', desc: 'bass, mid and treble bars', group: 'sound', paint: SOURCE.spectrum },
  // Audio-reactive modes from the newer firmware.
  { id: 'eq', label: 'EQ', desc: '16 bands along the strip, hue by band', group: 'sound', fw: 'react',
    paint: (px, n, t, x) => px.forEach((p, i) => { const b = Math.floor(i * 16 / n), v = x.spec(b); p.splice(0, 3, ...hsv(b / 16, 1, v)); }) },
  { id: 'ripple', label: 'Ripple', desc: 'each beat sends a ring out from the centre', group: 'sound', fw: 'react',
    paint: (px, n, t, x) => {
      const hue = 0.66 * (1 - band(x, 0) / Math.max(0.01, band(x, 0) + band(x, 2))), age = (t * 1.2) % 1;
      px.forEach((p, i) => { const d = Math.abs(i / n - 0.5) * 2, v = Math.max(0, 1 - Math.abs(d - age) * 8) * (1 - age); p.splice(0, 3, ...hsv(hue, 1, v)); });
    } },
  { id: 'mirror', label: 'Mirror', desc: 'a VU growing out from the centre both ways', group: 'sound', fw: 'react',
    paint: (px, n, t, x) => { const l = x.live.loud / 255; px.forEach((p, i) => { const d = Math.abs(i / n - 0.5) * 2; p.splice(0, 3, ...(d < l ? hsv((1 - d) / 3) : Math.abs(d - Math.min(1, l + 0.08)) < 0.03 ? [255, 255, 255] : [0, 0, 0])); }); } },
  { id: 'chroma', label: 'Chroma', desc: 'bass red, mid green, treble blue, all at once', group: 'sound', fw: 'react',
    paint: (px, n, t, x) => fill(px, [255 * band(x, 0), 255 * band(x, 1), 255 * band(x, 2)]) },
  { id: 'flow', label: 'Flow', desc: 'the sound colour pours in and scrolls along', group: 'sound', fw: 'react',
    paint: (px, n, t, x) => px.forEach((p, i) => { const k = (i / n) * 6 - t * 2; p.splice(0, 3, ...scale(hsv(((Math.sin(k) + 1) / 2) * 0.8, 1, 1), 0.3 + 0.7 * x.live.loud / 255)); }) },
  // Motion modes: the board's accelerometer and gyroscope, fused on the board (firmware/main/fusion.c).
  { id: 'bubble', label: 'Level', desc: 'a spirit-level bubble that glides to the raised end', group: 'motion', fw: 'axis',
    paint: (px, n, t, x) => {
      const b = clamp1(motion(x, t).tilt / 0.5), w = Math.max(1.5, n / 12), c = (n - 1) / 2 + b * ((n - 1) / 2 - w), col = hsv((1 - Math.abs(b)) / 3);
      px.forEach((p, i) => { const d = Math.abs(i - c) / w, f = d < 1 ? 1 - d * d : 0; p.splice(0, 3, ...x.rgb.map((v, k) => Math.min(255, v / 12 + col[k] * f))); });
    } },
  { id: 'tilt', label: 'Tilt', desc: 'lean it to pick the colour; the low end glows', group: 'motion', fw: 'axis',
    paint: (px, n, t, x) => {
      const m = motion(x, t), mag = Math.min(1, Math.hypot(m.tilt, m.side) / 0.5), hue = (Math.atan2(m.side, m.tilt) + Math.PI) / (2 * Math.PI);
      const col = mix(x.rgb, hsv(hue), mag), slope = clamp1(m.tilt / 0.5) * 0.47;
      px.forEach((p, i) => { const q = n > 1 ? (2 * i - (n - 1)) / (n - 1) : 0, f = Math.max(0, Math.min(1, 1 - slope * q - Math.abs(slope) / 2)); p.splice(0, 3, ...scale(col, f)); });
    } },
  { id: 'shake', label: 'Shake', desc: 'sparks fly while it moves; a knock flashes', group: 'motion', fw: 'axis',
    paint: (px, n, t, x) => {
      const m = motion(x, t), k = Math.floor(t * 12);
      px.forEach((p, i) => { const on = noise(i, k) < m.energy * 0.5, c = hsv((m.heading / 360 + (noise(i, k + 7) - 0.5) * 0.2 + 1) % 1); p.splice(0, 3, ...(on ? c : scale(x.rgb, Math.min(1, m.energy) * 0.16))); });
    } },
  { id: 'spin', label: 'Spin', desc: 'a rainbow that stays put in the room as you turn it', group: 'motion', fw: 'axis',
    paint: (px, n, t, x) => { const h = motion(x, t).heading / 360; px.forEach((p, i) => p.splice(0, 3, ...hsv(((i / n - h) % 1 + 1) % 1, 1, 0.8))); } },
  { id: 'slosh', label: 'Liquid', desc: 'half a strip of liquid that runs downhill and sloshes', group: 'motion', fw: 'axis',
    paint: (px, n, t, x) => {
      const m = motion(x, t), pos = -clamp1(m.tilt / 0.35) + 0.25 * m.energy * Math.sin(t * 6), len = n / 2;
      const c = (n - 1) / 2 + clamp1(pos) * (n - len) / 2, froth = Math.min(1, m.energy);
      px.forEach((p, i) => { const inn = Math.min(i - (c - len / 2), c + len / 2 - i), a = Math.max(0, Math.min(1, inn + 1)), f = inn < 2 ? froth * (2 - Math.max(0, inn)) / 2 : 0; p.splice(0, 3, ...mix(x.rgb, [255, 255, 255], f).map(v => v * a)); });
    } },
  { id: 'height', label: 'Height', desc: 'raise or lower it: a new colour every few cm (barometer)', group: 'motion', fw: 'hstep',
    paint: (px, n, t, x) => {
      const pos = motion(x, t).height / Math.max(1, x.hstep), band = Math.floor(pos), frac = pos - band;
      const col = band === 0 ? x.rgb : hsv((((rgbHsv(x.rgb)[0] + band / 8) % 1) + 1) % 1), dot = Math.round(frac * (n - 1));
      px.forEach((p, i) => p.splice(0, 3, ...(i === dot ? mix(col, [255, 255, 255], 0.5) : col)));
    } },
  // Fused modes: the mic and the IMU together (firmware/main/fx.c).
  { id: 'aurora', label: 'Aurora', desc: 'slow curtains of light; music swells them, they drift downhill', group: 'fused', fw: 'fxpal',
    paint: (px, n, t, x) => {
      const m = motion(x, t), loud = x.live.loud / 255, d = clamp1(m.tilt / 0.5);
      px.forEach((p, i) => {
        const u = i / n, f = (Math.sin(2 * Math.PI * (3 * u + t * (0.06 + 0.35 * d))) + Math.sin(2 * Math.PI * (5 * u - t * 0.1)) + 2) / 4;
        const v = 0.08 + 0.92 * Math.max(0, (f - 0.27) / 0.73) ** 2 * (0.35 + 0.65 * (Math.sin(2 * Math.PI * (u + t * 0.03)) + 1) / 2);
        p.splice(0, 3, ...scale(tone(x, AURORA, (Math.sin(2 * Math.PI * (2 * u - t * 0.05)) + 1) * 0.375), v * (0.5 + 0.5 * loud)));
      });
    } },
  { id: 'fire', label: 'Fire', desc: 'flames rise from whichever end is down; sound feeds them', group: 'fused', fw: 'fxpal',
    paint: (px, n, t, x) => {
      const m = motion(x, t), fuel = Math.min(1, x.live.loud / 255 * 0.8 + m.energy * 0.6), k = Math.floor(t * 20);
      const dir = m.tilt > 0.15 ? 1 : m.tilt < -0.15 ? -1 : 0, tall = 0.25 + 0.6 * fuel;
      px.forEach((p, i) => {
        const y = dir > 0 ? i / n : dir < 0 ? 1 - i / n : Math.min(i, n - 1 - i) / (n / 2);
        p.splice(0, 3, ...heat(x, Math.max(0, Math.min(1, (1 - y / tall) * (0.7 + 0.5 * noise(i, k))))));
      });
    } },
  { id: 'bounce', label: 'Bounce', desc: 'balls under real gravity; every beat kicks them up', group: 'fused', fw: 'fxpal',
    paint: (px, n, t, x) => {
      const m = motion(x, t), down = m.tilt < 0 ? 1 : 0, r = Math.max(1.5, n / 40);
      const balls = [0, 1, 2].map(b => { const ph = (t * (1.1 + b * 0.27) + b * 0.3) % 1, h = 4 * ph * (1 - ph) * (0.4 + b * 0.2); return [down ? 1 - h : h, tone(x, null, x.fxpal ? b / 2 : m.heading / 360 + b / 3)]; });
      px.forEach((p, i) => { let c = [0, 0, 0]; for (const [pos, col] of balls) { const f = Math.max(0, 1 - Math.abs(i - pos * (n - 1)) / r); c = c.map((v, k) => Math.max(v, col[k] * f * f)); } p.splice(0, 3, ...c); });
    } },
  { id: 'comet', label: 'Comet', desc: 'two comets cross in the middle on every beat, in tempo', group: 'fused', fw: 'fxpal',
    paint: (px, n, t, x) => {
      const m = motion(x, t), bpm = x.live.bpm && x.live.lock >= 30 ? x.live.bpm : 100, ph = t * bpm / 120, s = 0.5 - 0.5 * Math.cos(2 * Math.PI * ph);
      const heads = [[s, tone(x, null, x.fxpal ? 0 : m.heading / 360)], [1 - s, tone(x, null, x.fxpal ? 1 : m.heading / 360 + 0.5)]];
      const dir = Math.sin(2 * Math.PI * ph) >= 0 ? 1 : -1, len = n / 6;
      px.forEach((p, i) => {
        let c = [0, 0, 0];
        heads.forEach(([pos, col], k) => { const d = (pos * (n - 1) - i) * (k ? -dir : dir), f = d >= -1 && d < len ? (d < 0 ? 1 + d : 1 - d / len) : 0; c = c.map((v, j) => Math.max(v, col[j] * f * f)); });
        p.splice(0, 3, ...c);
      });
    } },
];
export const effectOf = id => EFFECTS.find(e => e.id === id);
export const SOURCES = Object.keys(SOURCE);
