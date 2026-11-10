// The gradient being edited, shared by the ring (handles around it) and the Gradient lens. The board's palette is
// mirrored in once an edit settles, so a gradient set from another phone shows up here too.
import { createSignal, createEffect, createRoot } from 'solid-js';
import { dev, state, send, toast, touch, settled, opt } from './store';
import { debounce } from './ui';
import { MAX_STOPS, BUILT_IN_GRADIENTS, parseStops, sortedStops, stopsCmd, colorAt } from './gradient';

const loadLib = () => {                     // built-ins are always offered; one the user deleted is remembered as null
  let saved = {};
  try { saved = JSON.parse(localStorage.getItem('ledify.gradients')) || {}; } catch (e) { /* none saved */ }
  const all = { ...BUILT_IN_GRADIENTS };
  for (const [k, v] of Object.entries(saved)) { if (v === null) delete all[k]; else all[k] = v; }
  return all;
};

export const grad = createRoot(() => {
  const [stops, setStops] = createSignal(parseStops(BUILT_IN_GRADIENTS.sunset));
  const [sel, setSel] = createSignal(0);
  const [lib, setLib] = createSignal(loadLib());
  createEffect(() => { dev().seen; const str = state().palette; if (str && settled('palette') && stopsCmd(stops()) !== str) { setStops(parseStops(str)); setSel(0); } });
  const later = debounce();
  const g = {
    stops, sel, setSel, lib,
    cur: () => stops()[Math.min(sel(), stops().length - 1)] || { c: '#ffffff', p: 0 },
    update(next, now) { touch('palette'); setStops(next); opt.setMode('palette'); opt.setOverlay('none'); const f = () => send('palette ' + stopsCmd(next)); now ? f() : later(f); },
    edit(i, patch, now) { g.update(stops().map((s, k) => k === i ? { ...s, ...patch } : s), now); },
    remove(i) { if (stops().length < 2) return; g.update(stops().filter((_, k) => k !== i), true); setSel(Math.min(i, stops().length - 1)); },
    addAt(p) {
      if (stops().length >= MAX_STOPS) { toast('At most ' + MAX_STOPS + ' stops', 'warn'); return; }
      g.update([...stops(), { c: colorAt(stops(), p), p: Math.round(p) }], true); setSel(stops().length - 1);
    },
    even() { g.update(sortedStops(stops()).map((s, i, a) => ({ ...s, p: a.length > 1 ? Math.round(i * 100 / (a.length - 1)) : 0 })), true); },
    flip() { g.update([...stops()].reverse().map(s => ({ ...s, p: 100 - s.p })), true); },
    // A saved gradient into a-b % of the strip; what sits outside that range stays. Stops on an inner boundary stay,
    // giving a hard edge there. Nothing stays on the strip's own ends.
    place(str, from = 0, to = 100) {
      const a = Math.min(from, to), b = Math.max(from, to);
      const mapped = parseStops(str).map(s => ({ c: s.c, p: Math.round(a + (b - a) * s.p / 100) }));
      const below = stops().filter(s => s.p < a || (s.p === a && a > 0)), above = stops().filter(s => s.p > b || (s.p === b && b < 100));
      const next = [...below, ...mapped, ...above];
      if (next.length > MAX_STOPS) { toast('That needs more than ' + MAX_STOPS + ' stops', 'warn'); return; }
      g.update(next, true); setSel(below.length);
    },
    storeLib(l) {
      const saved = { ...l };
      for (const k of Object.keys(BUILT_IN_GRADIENTS)) { if (!(k in l)) saved[k] = null; else if (l[k] === BUILT_IN_GRADIENTS[k]) delete saved[k]; }
      localStorage.setItem('ledify.gradients', JSON.stringify(saved)); setLib(l);
    },
    save(name) { name = (name || '').trim() || 'gradient ' + (Object.keys(lib()).length + 1); g.storeLib({ ...lib(), [name]: stopsCmd(stops()) }); toast('Saved "' + name + '"', 'ok', 2000); },
    del(name) { const l = { ...lib() }; delete l[name]; g.storeLib(l); },
  };
  return g;
});
