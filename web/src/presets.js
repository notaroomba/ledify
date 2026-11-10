// Presets ("looks"): a snapshot of the light saved in this browser, applied as the commands that rebuild it.
import { createSignal, createRoot } from 'solid-js';
import { dev, state, send, toast } from './store';

const load = () => { try { return JSON.parse(localStorage.getItem('ledify.presets') || '{}'); } catch (e) { return {}; } };
export const presets = createRoot(() => {
  const [all, setAll] = createSignal(load());
  const store = a => { localStorage.setItem('ledify.presets', JSON.stringify(a)); setAll({ ...a }); };
  return {
    all,
    save(name) {
      const s = state(), c = (dev().light || {}).color || { r: 255, g: 255, b: 255 }, a = { ...all() };
      name = (name || '').trim() || 'look ' + (Object.keys(a).length + 1);
      a[name] = { rgb: c.r + ' ' + c.g + ' ' + c.b, level: s.level, mode: s.mode, overlay: s.overlay, auto: s.auto, gain: s.gain, power: s.power, palette: s.mode === 'palette' ? s.palette : undefined };
      store(a); toast('Saved "' + name + '"', 'ok', 2000);
    },
    apply(name, p) {
      send('rgb ' + p.rgb); send('level ' + p.level); if (p.palette) send('palette ' + p.palette); send('mode ' + p.mode);
      if (p.overlay) send('overlay ' + p.overlay); send('auto ' + (p.auto ? 'on' : 'off')); send('gain ' + p.gain); if (p.power) send('on');
      toast('Applied "' + name + '"', 'ok', 2000);
    },
    del(name) { const a = { ...all() }; delete a[name]; store(a); },
  };
});
