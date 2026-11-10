// Gradient strings as the board speaks them: "rrggbb@pct,rrggbb@pct,...".
import { rgbHex, hexRgb } from './color';

export const MAX_STOPS = 16;
export const BUILT_IN_GRADIENTS = {
  sunset: 'ff3c00@0,ff8c00@35,ff0066@70,4b0082@100',
  ocean: '00e5ff@0,0080ff@50,001a66@100',
  forest: '0b3d0b@0,2e8b57@50,b2ff59@100',
  candy: 'ff6ec7@0,ffffff@50,7afcff@100',
  'warm to cool': 'ffb46b@0,ffffff@50,b4d2ff@100',
  colombia: 'fcd116@0,fcd116@50,003893@50,003893@75,ff0000@75,ff0000@100',
  lesbian: 'd52d00@0,d52d00@14,ef7627@14,ef7627@29,ff9a56@29,ff9a56@43,ffffff@43,ffffff@57,d162a4@57,d162a4@71,b55690@71,b55690@86,a30262@86,a30262@100',
};
export const parseStops = str => (str || '').split(',').map((s, i, a) => {
  const [c, p] = s.split('@');
  return { c: '#' + c, p: p === undefined ? Math.round(i * 100 / Math.max(1, a.length - 1)) : +p };
}).filter(s => /^#[0-9a-f]{6}$/i.test(s.c) && s.p >= 0 && s.p <= 100);
export const sortedStops = st => [...st].sort((a, b) => a.p - b.p);
export const stopsCmd = st => sortedStops(st).map(s => s.c.slice(1).toLowerCase() + '@' + s.p).join(',');
export const stopsCss = (st, deg = 90) => 'linear-gradient(' + deg + 'deg, ' + sortedStops(st).map(s => s.c + ' ' + s.p + '%').join(', ') + ')';
export const distinctColours = st => sortedStops(st).map(s => s.c.toLowerCase()).filter((c, i, a) => !i || c !== a[i - 1]);
// The gradient's colour at p %, the same blend the board does. Returns [r, g, b].
export const rgbAt = (st, p) => {
  const s = sortedStops(st), last = s[s.length - 1];
  if (!s.length) return [255, 255, 255];
  if (p <= s[0].p) return hexRgb(s[0].c);
  if (p >= last.p) return hexRgb(last.c);
  let k = 0;
  while (k < s.length - 2 && p >= s[k + 1].p) k++;
  const a = s[k], b = s[k + 1], f = b.p === a.p ? 0 : (p - a.p) / (b.p - a.p), A = hexRgb(a.c), B = hexRgb(b.c);
  return A.map((v, i) => v + (B[i] - v) * f);
};
export const colorAt = (st, p) => rgbHex(...rgbAt(st, p));
