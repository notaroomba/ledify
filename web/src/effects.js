// Every effect the panel offers, in one list. The board's mode names are the ids. paint(px, n, t, x) fills px (an array
// of n [r, g, b] 0..255 colours, before brightness) for the preview ring; x carries the inputs (colour, gradient,
// cover, live sound, temperature). An effect without paint gets a sound-tinted shimmer of the current colour.
// `fw: 'react'` marks modes that need newer firmware: they show once the board reports that state field.
import { hsv, mix } from './color';
import { rgbAt } from './gradient';

const fill = (px, c) => { for (const p of px) { p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; } };
const scale = (c, f) => [c[0] * f, c[1] * f, c[2] * f];
const band = (x, k) => (x.live.bands[k] || 0) / 255;

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
];
export const effectOf = id => EFFECTS.find(e => e.id === id);
export const SOURCES = Object.keys(SOURCE);
