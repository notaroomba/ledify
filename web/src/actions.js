// What the controls do, in one place so the ring, the lenses and the command bar all behave the same.
import { opt, send, toast, state } from './store';
import { debounce } from './ui';
import { hexRgb } from './color';
import { EFFECTS } from './effects';

const laterLevel = debounce(), laterColor = debounce();
const SOURCES = ['solid', 'rainbow', 'temp', 'spectrum', 'meter', 'palette', 'cover'];

export const act = {
  power(on) { opt.setPower(on ? 1 : 0); send(on ? 'on' : 'off'); },
  togglePower() { act.power(!opt.power()); },
  level(v, now) {
    v = Math.round(Math.max(0, Math.min(255, v)));
    opt.setLevel(v);
    const f = () => send('level ' + v);
    now ? (laterLevel(() => {}, 0), f()) : laterLevel(f);
  },
  color(hex, now) {
    opt.setColor(hex); opt.setMode('solid'); opt.setOverlay('none');
    const f = () => send('rgb ' + hexRgb(hex).join(' '));
    now ? (laterColor(() => {}, 0), f()) : laterColor(f);
  },
  // Mirrors the board's presets: a colour source resets sound and overlay, vu brings the meter colours.
  mode(m) {
    opt.setMode(m);
    if (SOURCES.includes(m) || m === 'sound') opt.setOverlay('none');
    else if (m === 'vu' || m === 'pulse') opt.setOverlay(m);
    send('mode ' + m);
  },
  overlay(v) { opt.setOverlay(v); send('overlay ' + v); },
  stepMode(d) {
    const ids = EFFECTS.map(e => e.id), i = ids.indexOf(opt.mode());
    const next = EFFECTS[(i + d + ids.length) % ids.length];
    act.mode(next.id);
    toast(next.label + ' · ' + next.desc, 'info', 1600);
  },
  sleep(m) { send('sleep ' + m); },
};
export const sleepLeft = () => state().sleep || 0;
