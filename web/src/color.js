// Colour maths shared by the preview engine and the controls.
export const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
export const hex2 = v => ('0' + Math.round(clamp(v, 0, 255)).toString(16)).slice(-2);
export const rgbHex = (r, g, b) => '#' + hex2(r) + hex2(g) + hex2(b);
export const hexRgb = h => [1, 3, 5].map(i => parseInt(h.slice(i, i + 2), 16) || 0);
// h in turns (0..1), s and v 0..1 -> [r, g, b] 0..255
export const hsv = (h, s = 1, v = 1) => {
  const f = n => { const k = (n + h * 6) % 6; return v * 255 * (1 - s * Math.max(0, Math.min(k, 4 - k, 1))); };
  return [f(5), f(3), f(1)];
};
export const rgbHsv = ([r, g, b]) => {
  const mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn;
  let h = 0;
  if (d) h = mx === r ? ((g - b) / d + 6) % 6 : mx === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return [h / 6, mx ? d / mx : 0, mx / 255];
};
export const mix = (a, b, f) => a.map((v, i) => v + (b[i] - v) * f);
// A colour bright enough to use as an accent on the dark UI, whatever the strip shows.
export const accentOf = rgb => {
  const [h, s] = rgbHsv(rgb);
  const [r, g, b] = hsv(h, Math.min(s, 0.85), 1);
  return s < 0.08 ? [235, 238, 245] : [r, g, b];
};
