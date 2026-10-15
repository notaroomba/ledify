// The cards. Each reads the board's state through the store and sends commands on the same topics the board uses.
import { For, Index, Show, createSignal, createEffect } from 'solid-js';
import { ui, dev, state, send, toast, logText, optimistic, opt, touch, settled, LOCAL } from './store';
import { Range, Slider, Chips, Switch, confirmDialog, debounce, rgbHex } from './ui';
import { listen, feed } from './audio';
import { sp } from './spotify';

const EFFECTS = {
  solid: 'one colour', rainbow: 'moving rainbow', temp: 'colour from the temperature', spectrum: 'bass, mid and treble bars',
  meter: 'green to red along the strip', palette: 'your gradient, from the Gradient card', cover: 'the album cover colours from Spotify', sound: 'brightness follows the sound',
  vu: 'lit length follows the sound', pulse: 'flashes on the beat',
};
const OVERLAYS = ['none', 'vu', 'pulse'];
const PALETTE = ['#ffffff', '#ffd6a0', '#ff8c00', '#ffd700', '#ff0040', '#ff00ff', '#8000ff', '#0080ff', '#00e5ff', '#00ff80'];
const CHIPS = [['ws2812', 'WS2812 / B / C / D / E, WS2813, WS2815'], ['ws2811', 'WS2811 (12 V, RGB order)'], ['sk6812', 'SK6812'], ['sk6812w', 'SK6812 RGBW'], ['ws2816', 'WS2816']];
const mmss = s => Math.floor(s / 60) + ':' + ('0' + Math.round(s % 60)).slice(-2);
const supply = () => { const s = state(); return s.pd && s.pd.ready ? (s.pd.mv / 1000) + ' V ' + (s.pd.ma / 1000) + ' A, ' + (s.pd.mv * s.pd.ma / 1e6).toFixed(0) + ' W' : 'no USB-PD contract, assuming 7.5 W'; };

export function Hero() {
  const [level, setLevel] = optimistic('level', () => state().level);
  const [power, setPower] = optimistic('power', () => state().power);
  const [limit, setLimit] = optimistic('limit', () => state().limit);
  const note = () => {
    const s = state(), need = (s.need_mw || 0) / 1000, budget = (s.budget_mw || 0) / 1000;
    if (!s.power) return '';
    if (s.limit && s.cap < s.level) return 'Dimmed to ' + Math.round(s.cap * 100 / 255) + '% to stay within the supply (' + supply() + ').';
    if (need > budget) return 'Estimated draw ' + need.toFixed(1) + ' W, the supply gives about ' + budget.toFixed(1) + ' W (' + supply() + '). Expect dimming or flicker; turn on the limit or use a stronger charger.';
    return '';
  };
  return <section class="card md:col-span-2">
    <div class="flex items-center gap-5">
      <button type="button" class="power" aria-pressed={power() ? 'true' : 'false'} aria-label="Power" onClick={() => { const on = !power(); setPower(on ? 1 : 0); send(on ? 'on' : 'off'); }}>
        <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M12 3v9" /><path d="M6.3 6.3a8 8 0 1 0 11.4 0" /></svg>
      </button>
      <div class="min-w-0 grow">
        <div class="flex items-baseline gap-2"><span class="text-4xl font-semibold tabular-nums">{level() ?? 0}</span><span class="text-sm text-muted">/ 255 brightness</span></div>
        <Range class="mt-1" label="Brightness" min={0} max={255} value={level} set={setLevel} send={v => 'level ' + v} />
        <Show when={note()}><p class="mt-1 text-sm text-warn">{note()}</p></Show>
        <Show when={state().hot > 0}><p class="mt-1 text-sm text-warn">Dimmed {state().hot} % because the board is at {state().board_c} °C (limit {state().tmax} °C, set under Advanced).</p></Show>
        <div class="mt-2 flex items-center justify-between gap-3">
          <span class="hint">Dim automatically to stay within the supply</span>
          <Switch label="Power limit" checked={() => !!limit()} onChange={on => { setLimit(on ? 1 : 0); send('limit ' + (on ? 1 : 0)); }} />
        </div>
        <div class="mt-3 flex flex-wrap items-center gap-2">
          <span class="hint mr-1">Sleep timer</span>
          <For each={[15, 30, 60, 120]}>{m => <button type="button" class="chip h-9 px-3 text-xs" onClick={() => send('sleep ' + m)}>{m} min</button>}</For>
          <Show when={state().sleep > 0}><button type="button" class="chip h-9 px-3 text-xs" aria-pressed="true" onClick={() => send('sleep 0')}>fading out, {mmss(state().sleep)} left · cancel</button></Show>
          <Show when={state().rise > 0}><span class="chip h-9 px-3 text-xs" aria-pressed="true">sunrise, {mmss(state().rise)} to go</span></Show>
        </div>
      </div>
    </div>
  </section>;
}

export function Colour() {
  const [color, setColor] = optimistic('color', () => { const c = (dev().light || {}).color; return c ? rgbHex(c.r, c.g, c.b) : undefined; });
  const [hue, setHue] = createSignal(0);
  const later = debounce();
  const rgbCmd = hex => 'rgb ' + parseInt(hex.slice(1, 3), 16) + ' ' + parseInt(hex.slice(3, 5), 16) + ' ' + parseInt(hex.slice(5, 7), 16);
  const pick = (hex, now) => { setColor(hex); opt.setMode('solid'); const f = () => send(rgbCmd(hex)); now ? f() : later(f); };
  const hueHex = h => { const f = (n, k = (n + h / 60) % 6) => 255 * (1 - Math.max(0, Math.min(k, 4 - k, 1))); return rgbHex(f(5), f(3), f(1)); };
  return <section class="card">
    <h2 class="card-title">Colour</h2>
    <input type="color" class="big" aria-label="Colour" value={color() || '#ffffff'} onInput={e => pick(e.currentTarget.value)} onChange={e => pick(e.currentTarget.value, true)} />
    <div class="slider mt-3"><label>Hue</label><Range class="hue" label="Hue" min={0} max={359} value={hue} set={v => { setHue(v); setColor(hueHex(v)); opt.setMode('solid'); }} send={v => rgbCmd(hueHex(v))} /><output>{hue()}°</output></div>
    <div class="mt-2 grid grid-cols-5 gap-2 sm:grid-cols-10 md:grid-cols-5">
      <For each={PALETTE}>{c => <button type="button" class="swatch" style={{ background: c }} title={c} aria-label={'colour ' + c} onClick={() => pick(c, true)} />}</For>
    </div>
  </section>;
}

export function Effect() {
  const [overlay, setOverlay] = optimistic('overlay', () => state().overlay);
  const [auto, setAuto] = optimistic('auto', () => state().auto);
  const [luxLo, setLuxLo] = optimistic('lux_lo', () => state().lux_lo);
  const [luxHi, setLuxHi] = optimistic('lux_hi', () => state().lux_hi);
  const [night, setNight] = optimistic('night', () => state().night);
  return <section class="card">
    <h2 class="card-title">Effect</h2>
    <Chips items={Object.keys(EFFECTS).map(k => [k, k])} value={opt.mode} onPick={v => { opt.setMode(v); send('mode ' + v); }} />
    <p class="hint mt-2 min-h-5">{EFFECTS[opt.mode()] || ''}</p>
    <h2 class="card-title mt-4">Overlay</h2>
    <Chips items={OVERLAYS.map(k => [k, k])} value={overlay} onPick={v => { setOverlay(v); send('overlay ' + v); }} />
    <div class="mt-4 flex items-center justify-between gap-3">
      <div><div class="text-sm">Auto dim</div><div class="hint">brightness follows the room's light</div></div>
      <Switch label="Auto dim" checked={() => !!auto()} onChange={on => { setAuto(on ? 1 : 0); send('auto ' + (on ? 'on' : 'off')); }} />
    </div>
    <Show when={auto()}>
      <div class="mt-2 border-t border-line pt-2">
        <Slider label="Dim below" min={0} max={1000} step={10} value={luxLo} set={setLuxLo} send={v => 'lux ' + v + ' ' + luxHi()} fmt={v => v + ' lx'} />
        <Slider label="Full above" min={0} max={1000} step={10} value={luxHi} set={setLuxHi} send={v => 'lux ' + luxLo() + ' ' + v} fmt={v => v + ' lx'} />
        <Slider label="Off below" min={0} max={200} step={5} value={night} set={setNight} send={v => 'night ' + v} fmt={v => v == 0 ? 'never' : v + ' lx'} />
        <p class="hint">lux from the board's light sensor; "off below" 0 means never</p>
      </div>
    </Show>
  </section>;
}

// Gradient editor in the Figma style: a bar with one handle per colour stop. Drag a handle to move it, click the bar
// to add a stop there (with the colour the gradient has at that point), pick a handle to edit its colour and exact
// position. Saved gradients replace the whole strip or go into a range of it, so several can sit side by side.
// The board's palette (status field) is mirrored in once an edit settles, so a gradient set from Spotify or another
// phone shows up here too.
const MAX_STOPS = 16;
const BUILT_IN_GRADIENTS = {
  sunset: 'ff3c00@0,ff8c00@35,ff0066@70,4b0082@100',
  ocean: '00e5ff@0,0080ff@50,001a66@100',
  forest: '0b3d0b@0,2e8b57@50,b2ff59@100',
  candy: 'ff6ec7@0,ffffff@50,7afcff@100',
  'warm to cool': 'ffb46b@0,ffffff@50,b4d2ff@100',
  colombia: 'fcd116@0,fcd116@50,003893@50,003893@75,ff0000@75,ff0000@100',
};
const parseStops = str => (str || '').split(',').map(s => { const [c, p] = s.split('@'); return { c: '#' + c, p: +p }; }).filter(s => /^#[0-9a-f]{6}$/i.test(s.c) && s.p >= 0 && s.p <= 100);
const sortedStops = st => [...st].sort((a, b) => a.p - b.p);
const stopsCmd = st => sortedStops(st).map(s => s.c.slice(1).toLowerCase() + '@' + s.p).join(',');
const stopsCss = st => 'linear-gradient(90deg, ' + sortedStops(st).map(s => s.c + ' ' + s.p + '%').join(', ') + ')';
const stripesCss = (st, lens, count) => {         // the distinct colours in turn, each its own run, as the board draws them
  const cols = sortedStops(st).map(s => s.c.toLowerCase()).filter((c, i, a) => !i || c !== a[i - 1]);
  let at = 0;
  return 'repeating-linear-gradient(90deg, ' + cols.map((c, i) => { const a = at, w = lens[i] * 100 / count; at += w; return c + ' ' + a + '% ' + (a + w) + '%'; }).join(', ') + ')';
};
const hexRgb = h => [1, 3, 5].map(i => parseInt(h.slice(i, i + 2), 16));
const colorAt = (st, p) => {                        // the gradient's colour at p %, the same blend the board does
  const s = sortedStops(st), last = s[s.length - 1];
  if (!s.length || p <= s[0].p) return s.length ? s[0].c : '#ffffff';
  if (p >= last.p) return last.c;
  let k = 0;
  while (k < s.length - 2 && p >= s[k + 1].p) k++;
  const a = s[k], b = s[k + 1], f = b.p === a.p ? 0 : (p - a.p) / (b.p - a.p), A = hexRgb(a.c), B = hexRgb(b.c);
  return rgbHex(...A.map((v, i) => v + (B[i] - v) * f));
};
const loadGradients = () => {                     // built-ins are always offered; one the user deleted is remembered as null
  let saved = {};
  try { saved = JSON.parse(localStorage.getItem('ledify.gradients')) || {}; } catch (e) { /* none saved */ }
  const all = { ...BUILT_IN_GRADIENTS };
  for (const [k, v] of Object.entries(saved)) { if (v === null) delete all[k]; else all[k] = v; }
  return all;
};
const pctOf = e => Math.max(0, Math.min(100, Math.round(+e.currentTarget.value || 0)));
export function Gradient() {
  const [stops, setStops] = createSignal(parseStops(BUILT_IN_GRADIENTS.sunset));
  const [sel, setSel] = createSignal(0);
  const [lib, setLib] = createSignal(loadGradients());
  const [from, setFrom] = createSignal(0), [to, setTo] = createSignal(100);
  const [stripeStr, setStripeStr] = optimistic('stripe', () => state().stripe === undefined ? undefined : String(state().stripe));
  const lens = () => { const s = stripeStr(); return !s || s === '0' ? [] : s.split(',').map(x => Math.max(1, +x || 1)); };
  const distinct = () => sortedStops(stops()).map(s => s.c.toLowerCase()).filter((c, i, a) => !i || c !== a[i - 1]);
  const perColour = () => distinct().map((_, k) => lens()[k % lens().length]);
  const setLens = l => { const str = l.length ? l.join(',') : '0'; setStripeStr(str); send('stripe ' + str); };
  let bar, nameEl;
  createEffect(() => { dev().seen; const str = state().palette; if (str && settled('palette') && stopsCmd(stops()) !== str) { setStops(parseStops(str)); setSel(0); } });
  const cur = () => stops()[Math.min(sel(), stops().length - 1)];
  const later = debounce();
  const update = (next, now) => { touch('palette'); setStops(next); opt.setMode('palette'); const f = () => send('palette ' + stopsCmd(next)); now ? f() : later(f); };
  const edit = (i, patch, now) => update(stops().map((s, k) => k === i ? { ...s, ...patch } : s), now);
  const remove = i => { if (stops().length < 2) return; update(stops().filter((_, k) => k !== i), true); setSel(Math.min(i, stops().length - 1)); };
  const pctAt = e => { const r = bar.getBoundingClientRect(); return Math.round(Math.max(0, Math.min(100, (e.clientX - r.left) * 100 / r.width))); };
  const addAt = p => { if (stops().length >= MAX_STOPS) { toast('At most ' + MAX_STOPS + ' stops', 'warn'); return; } update([...stops(), { c: colorAt(stops(), p), p }], true); setSel(stops().length - 1); };
  const drag = (i, e) => {
    e.stopPropagation(); setSel(i);
    const h = e.currentTarget; h.setPointerCapture(e.pointerId);
    const move = ev => edit(i, { p: pctAt(ev) });
    const up = ev => { edit(i, { p: pctAt(ev) }, true); h.removeEventListener('pointermove', move); h.removeEventListener('pointerup', up); h.removeEventListener('pointercancel', up); };
    h.addEventListener('pointermove', move); h.addEventListener('pointerup', up); h.addEventListener('pointercancel', up);
  };
  const keys = (i, e) => {
    const d = e.key === 'ArrowLeft' ? -1 : e.key === 'ArrowRight' ? 1 : 0;
    if (d) { e.preventDefault(); edit(i, { p: Math.max(0, Math.min(100, stops()[i].p + d * (e.shiftKey ? 10 : 1))) }); }
    else if (e.key === 'Delete' || e.key === 'Backspace') { e.preventDefault(); remove(i); }
  };
  const place = str => {                          // a saved gradient into from–to %; what sits outside that range stays
    const a = Math.min(from(), to()), b = Math.max(from(), to());
    const mapped = parseStops(str).map(s => ({ c: s.c, p: Math.round(a + (b - a) * s.p / 100) }));
    // Stops on an inner boundary stay, giving a hard edge there: the old colour ends, the new one starts (order matters
    // for stops at the same position). Nothing stays on the strip's own ends.
    const below = stops().filter(s => s.p < a || (s.p === a && a > 0)), above = stops().filter(s => s.p > b || (s.p === b && b < 100));
    const next = [...below, ...mapped, ...above];
    if (next.length > MAX_STOPS) { toast('That needs more than ' + MAX_STOPS + ' stops', 'warn'); return; }
    update(next, true); setSel(below.length);
  };
  const storeLib = l => { const saved = { ...l }; for (const k of Object.keys(BUILT_IN_GRADIENTS)) { if (!(k in l)) saved[k] = null; else if (l[k] === BUILT_IN_GRADIENTS[k]) delete saved[k]; } localStorage.setItem('ledify.gradients', JSON.stringify(saved)); setLib(l); };
  const saveGradient = () => { const name = nameEl.value.trim() || 'gradient ' + (Object.keys(lib()).length + 1); storeLib({ ...lib(), [name]: stopsCmd(stops()) }); nameEl.value = ''; toast('Saved "' + name + '"', 'ok', 2000); };
  const delGradient = async name => { if (await confirmDialog('Delete gradient?', '"' + name + '" will be removed from this browser.', 'Delete')) { const l = { ...lib() }; delete l[name]; storeLib(l); } };
  return <section class="card md:col-span-2">
    <h2 class="card-title">Gradient <small>click the bar to add a stop, drag a handle to move it · {stops().length} of {MAX_STOPS}</small></h2>
    <div ref={bar} class="relative mx-3 h-12 cursor-copy rounded-xl border border-line" style={{ background: lens().length ? stripesCss(stops(), perColour(), state().count || 300) : stopsCss(stops()) }} onPointerDown={e => { if (e.target === bar) addAt(pctAt(e)); }}>
      <Index each={stops()}>{(s, i) => <button type="button" class="absolute top-1/2 h-7 w-7 -translate-x-1/2 -translate-y-1/2 cursor-grab touch-none rounded-full border-2 border-white shadow-md active:cursor-grabbing" classList={{ 'ring-2 ring-accent ring-offset-2 ring-offset-surface': sel() === i }} style={{ left: s().p + '%', background: s().c }} aria-label={'stop ' + (i + 1) + ' at ' + s().p + '%'} onPointerDown={e => drag(i, e)} onKeyDown={e => keys(i, e)} />}</Index>
    </div>
    <div class="row mt-4">
      <input type="color" class="h-10 w-14 cursor-pointer rounded-lg border border-line bg-raised p-0.5" aria-label="selected stop colour" value={cur().c} onInput={e => edit(sel(), { c: e.currentTarget.value })} onChange={e => edit(sel(), { c: e.currentTarget.value }, true)} />
      <span class="hint">at</span><input type="number" min="0" max="100" class="field w-20" aria-label="selected stop position" value={cur().p} onChange={e => edit(sel(), { p: pctOf(e) }, true)} /><span class="hint">%</span>
      <button type="button" class="btn btn-sm" disabled={stops().length < 2} onClick={() => remove(sel())}>Remove stop</button>
      <button type="button" class="btn btn-sm" onClick={() => update(sortedStops(stops()).map((s, i, a) => ({ ...s, p: a.length > 1 ? Math.round(i * 100 / (a.length - 1)) : 0 })), true)}>Space evenly</button>
      <button type="button" class="btn btn-sm" onClick={() => update([...stops()].reverse().map(s => ({ ...s, p: 100 - s.p })), true)}>Flip</button>
    </div>
    <div class="row mt-3">
      <span class="hint w-[92px]">Stripes</span>
      <Chips items={[['blend', 'blend'], ['stripes', 'stripes']]} value={() => lens().length ? 'stripes' : 'blend'} onPick={v => setLens(v === 'blend' ? [] : distinct().map(() => 2))} />
      <Show when={lens().length}>
        <For each={distinct()}>{(c, i) => <label class="flex items-center gap-1 text-xs text-muted"><span class="inline-block h-5 w-5 rounded-full border border-line" style={{ background: c }} /><input type="number" min="1" max="100" class="field h-9 w-16" aria-label={'pixels for ' + c} value={perColour()[i()]} onChange={e => { const l = perColour(); l[i()] = Math.max(1, Math.min(100, Math.round(+e.currentTarget.value || 1))); setLens(l); }} /> px</label>}</For>
      </Show>
    </div>
    <p class="hint">Blend fades between the stops; stripes paint each colour of the gradient in turn for the number of pixels set next to it, repeating along the strip. Spotify's album colours live in the separate "cover" effect and never replace this gradient.</p>
    <h2 class="card-title mt-5">Saved gradients <small>this browser</small></h2>
    <div class="row">
      <span class="hint">place one into</span>
      <input type="number" min="0" max="100" class="field w-20" aria-label="from percent" value={from()} onChange={e => setFrom(pctOf(e))} /><span class="hint">to</span>
      <input type="number" min="0" max="100" class="field w-20" aria-label="to percent" value={to()} onChange={e => setTo(pctOf(e))} /><span class="hint">% of the strip; 0 to 100 replaces everything</span>
    </div>
    <div class="mt-2 flex flex-wrap gap-2">
      <For each={Object.entries(lib())}>{([name, str]) => <span class="inline-flex overflow-hidden rounded-xl border border-line">
        <button type="button" class="h-10 px-3.5 text-sm font-medium text-white [text-shadow:0_1px_2px_rgba(0,0,0,.8)]" style={{ background: stopsCss(parseStops(str)) }} onClick={() => place(str)}>{name}</button>
        <button type="button" class="h-10 bg-raised px-2.5 text-xs text-muted hover:text-fg" aria-label={'delete ' + name} onClick={() => delGradient(name)}>✕</button>
      </span>}</For>
    </div>
    <div class="row mt-3"><input ref={nameEl} type="text" class="field grow" placeholder="name the current gradient" aria-label="Gradient name" onKeyDown={e => e.key === 'Enter' && saveGradient()} /><button type="button" class="btn btn-primary btn-sm" onClick={saveGradient}>Save gradient</button></div>
  </section>;
}

export function Sound() {
  const [gain, setGain] = optimistic('gain', () => state().gain);
  const pct = v => (v * 100 / 255) + '%';
  let file, aelem;
  return <section class="card">
    <h2 class="card-title">Sound <small>{feed.active() ? 'this device' : 'board mic'}</small></h2>
    <div class="grid grid-cols-[64px_1fr] items-center gap-x-3 gap-y-2 text-xs text-muted">
      <span>Loudness</span><div class="flex items-center gap-3"><div class="bar grow"><i class="bg-gradient-to-r from-ok via-warn to-bad" style={{ width: pct(ui.live.loud) }} /></div><span class={'dot ' + (ui.live.pulse > 128 ? 'on' : '')} /></div>
      <span>Bass</span><div class="bar"><i class="bg-bad" style={{ width: pct(ui.live.bands[0]) }} /></div>
      <span>Mid</span><div class="bar"><i class="bg-ok" style={{ width: pct(ui.live.bands[1]) }} /></div>
      <span>Treble</span><div class="bar"><i class="bg-accent" style={{ width: pct(ui.live.bands[2]) }} /></div>
    </div>
    <Slider rowClass="mt-3" label="Mic gain" min={1} max={100} value={gain} set={setGain} send={v => 'gain ' + v} />
    <div class="row mt-1">
      <button type="button" class="btn" onClick={listen.toggle}>{listen.on() ? 'Stop listening' : "Listen to the board's mic"}</button>
      <span class="hint">{listen.state()}</span>
    </div>
    <Slider label="Volume" min={1} max={200} value={listen.vol} set={listen.setVol} />
    <details class="mt-2 border-t border-line pt-2">
      <summary>Use music or the mic from this device instead</summary>
      <p class="hint mt-2">Plays a file here and sends loudness, bass, mid, treble and beats to the board 20 times a second, so the sound effects follow it.</p>
      <div class="row mt-2">
        <input ref={file} type="file" accept="audio/*" class="field min-w-[180px] grow py-2 text-xs" aria-label="Audio file" />
        <button type="button" class="btn" onClick={() => feed.playFile(file.files[0])}>Play file</button>
        <button type="button" class="btn" onClick={feed.mic}>Microphone</button>
        <button type="button" class="btn" onClick={feed.stop}>Stop</button>
      </div>
      <Slider label="Sensitivity" min={1} max={10} value={feed.sens} set={feed.setSens} />
      <audio ref={el => { aelem = el; feed.attach(el); }} controls class="mt-2 w-full" classList={{ hidden: !feed.active() }} />
    </details>
  </section>;
}

export function Strip() {
  const [count, setCount] = optimistic('count', () => state().count);
  const [type, setType] = optimistic('type', () => state().type);
  const [volt, setVolt] = optimistic('volt', () => state().volt);
  const [droop, setDroop] = optimistic('droop', () => state().droop);
  let num, sel;
  createEffect(() => { const v = count(); if (v !== undefined && document.activeElement !== num) num.value = v; });
  createEffect(() => { sel.value = type() || 'ws2812'; });
  const setExact = () => { const v = Math.max(1, Math.min(1000, +num.value || 1)); num.value = v; setCount(v); send('count ' + v); };
  const pickVolt = async v => {
    if (v === '12' && !await confirmDialog('Switch the strip to 12 V?', 'Only for 12 V strips (WS2815, WS2811). 5 V pixels are destroyed instantly. The output turns off first, so press power again afterwards.', 'Switch to 12 V')) return;
    setVolt(+v); send(v === '12' ? 'volt 12 yes' : 'volt 5');
  };
  return <section class="card">
    <h2 class="card-title">Strip</h2>
    <Slider label="Pixels" min={1} max={1000} value={count} set={setCount} send={v => 'count ' + v} />
    <div class="row">
      <label class="hint">exact</label><input ref={num} type="number" min="1" max="1000" class="field w-24" aria-label="Exact pixel count" onKeyDown={e => e.key === 'Enter' && setExact()} />
      <button type="button" class="btn btn-sm" onClick={setExact}>Set</button>
    </div>
    <div class="row mt-4"><label class="hint w-[92px]">LED chip</label>
      <select ref={sel} class="field grow" aria-label="LED chip" onChange={e => { setType(e.currentTarget.value); send('type ' + e.currentTarget.value); }}>
        <For each={CHIPS}>{([v, label]) => <option value={v}>{label}</option>}</For>
      </select>
    </div>
    <div class="row mt-3"><span class="hint w-[92px]">Strip voltage</span><Chips chip="danger" items={[['5', '5 V'], ['12', '12 V']]} value={volt} onPick={pickVolt} /></div>
    <p class="hint mt-2">12 V only for 12 V strips (WS2815, WS2811); it needs a charger offering 15 V or more.</p>
    <Show when={state().stream}><p class="hint mt-3 text-accent">Pixels are coming from {state().stream.src} over the LAN at {state().stream.fps} fps (DDP or E1.31); the styles resume when it stops.</p></Show>
    <Slider rowClass="mt-4" label="Tail colour fix" min={0} max={100} value={droop} set={setDroop} send={v => 'droop ' + v} fmt={v => v ? v + ' %' : 'off'} />
    <p class="hint">A strip fed from one end goes warm towards the far end as its voltage sags. This cuts red along the strip to balance it; set it so the far end looks white at your usual brightness. The real fix is a second 5 V feed into the far end.</p>
  </section>;
}

export function Sensors() {
  const z = () => dev().sensors || {};
  const lux = () => z().lux ?? state().lux, temp = () => z().temp_c ?? state().temp_c;
  const kpa = () => z().pressure_kpa ?? (state().pressure_pa !== undefined ? state().pressure_pa / 1000 : undefined);
  const db = () => { const s = state(), v = ui.live.db ?? s.sound_db; return v === undefined || v === null ? '–' : Math.round(v) + (s.db_peak ? ' · ' + s.db_peak : ''); };
  const cap = () => { const s = state(); return s.level === undefined ? '–' : (s.limit ? s.cap : s.level) + ' / ' + s.level; };
  const fmt = (v, d) => v === undefined ? '–' : (+v).toFixed(d);
  return <section class="card md:col-span-2">
    <h2 class="card-title">Sensors <small>{state().fw ? supply() : ''}</small></h2>
    <div class="grid grid-cols-2 gap-3 sm:grid-cols-3 lg:grid-cols-5">
      <div class="tile"><b>{fmt(lux(), 0)}</b><span>lux</span></div>
      <div class="tile"><b>{fmt(temp(), 1)}</b><span>°C on the board</span></div>
      <div class="tile"><b>{fmt(kpa(), 2)}</b><span>kPa</span></div>
      <div class="tile"><b>{db()}</b><span>dB, peak of the last minute</span></div>
      <div class="tile"><b>{cap()}</b><span>brightness sent</span></div>
    </div>
    <Show when={state().pdos && state().pdos.length}><p class="hint mt-3">Charger offers: {state().pdos.map(x => (x[0] / 1000) + ' V ' + (x[1] / 1000) + ' A').join(' · ')}. The board takes the most powerful one.</p></Show>
    <p class="hint">{state().net || ''}</p>
    <Show when={state().name && state().ip}><p class="hint">Also on the local network at <a class="text-accent" href={'http://' + state().name + '.local/'}>http://{state().name}.local</a> (no cloud needed){state().clients ? ', ' + state().clients + ' local panel' + (state().clients > 1 ? 's' : '') + ' open' : ''}</p></Show>
  </section>;
}

// Segments: zones of the strip with their own colour source. Edits send "seg N START END color rrggbb" and the
// board's state echoes the list back.
const SRC = ['solid', 'rainbow', 'palette', 'temp', 'meter', 'spectrum'];
export function Segments() {
  const segs = () => state().segs || [];
  const count = () => state().count || 300;
  const put = (i, a, b, color, rgb) => send('seg ' + i + ' ' + a + ' ' + b + ' ' + color + ' ' + rgb);
  const add = () => {
    const used = segs().map(s => s.i), i = [1, 2, 3, 4, 5, 6].find(k => !used.includes(k));
    if (!i) { toast('Six zones at most', 'warn'); return; }
    const last = segs().reduce((m, s) => Math.max(m, s.b), 0);
    put(i, last < count() ? last : 0, count(), 'solid', 'ffffff');
  };
  const num = (s, key, e) => { const v = Math.max(0, Math.min(count(), Math.round(+e.currentTarget.value || 0))); const a = key === 'a' ? v : s.a, b = key === 'b' ? v : s.b; if (a < b) put(s.i, a, b, s.color, s.rgb); else toast('The end must come after the start', 'warn'); };
  return <section class="card md:col-span-2">
    <h2 class="card-title">Zones <small>pixels outside every zone keep the strip's own colour and effect</small></h2>
    <div class="relative h-6 overflow-hidden rounded-lg bg-raised">
      <For each={segs()}>{s => <div class="absolute top-0 h-full border-r border-ink text-center text-[10px] leading-6 text-white [text-shadow:0_1px_2px_#000]" style={{ left: (s.a * 100 / count()) + '%', width: ((s.b - s.a) * 100 / count()) + '%', background: s.color === 'solid' ? '#' + s.rgb : s.color === 'rainbow' ? 'linear-gradient(90deg,#f00,#ff0,#0f0,#0ff,#00f,#f0f,#f00)' : s.color === 'meter' ? 'linear-gradient(90deg,#0f0,#ff0,#f00)' : s.color === 'palette' ? '#8000ff' : '#58c6ff' }}>{s.i}</div>}</For>
    </div>
    <div class="mt-3 grid gap-2">
      <For each={segs()}>{s => <div class="flex flex-wrap items-center gap-2 rounded-xl bg-raised p-2">
        <b class="w-6 text-center text-sm">{s.i}</b>
        <input type="number" min="0" max={count()} class="field h-9 w-20" aria-label={'zone ' + s.i + ' start'} value={s.a} onChange={e => num(s, 'a', e)} />
        <span class="hint">to</span>
        <input type="number" min="1" max={count()} class="field h-9 w-20" aria-label={'zone ' + s.i + ' end'} value={s.b} onChange={e => num(s, 'b', e)} />
        <Chips items={SRC.map(k => [k, k])} value={() => s.color} onPick={v => put(s.i, s.a, s.b, v, s.rgb)} />
        <Show when={s.color === 'solid'}><input type="color" class="h-9 w-12 cursor-pointer rounded-lg border border-line bg-raised p-0.5" aria-label={'zone ' + s.i + ' colour'} value={'#' + s.rgb} onChange={e => put(s.i, s.a, s.b, s.color, e.currentTarget.value.slice(1))} /></Show>
        <button type="button" class="chip ml-auto h-9 px-2" aria-label={'remove zone ' + s.i} onClick={() => send('seg del ' + s.i)}>✕</button>
      </div>}</For>
    </div>
    <div class="row mt-3">
      <button type="button" class="btn btn-sm" onClick={add}>Add zone</button>
      <Show when={segs().length}><button type="button" class="btn btn-sm" onClick={() => send('seg off')}>Remove all</button></Show>
      <span class="hint">pixel numbers count from the board; the end is not included</span>
    </div>
  </section>;
}

// Schedules run on the board's clock (set from the internet once it has Wi-Fi), every day at that time.
const TZS = [['New York', 'EST5EDT,M3.2.0,M11.1.0'], ['Chicago', 'CST6CDT,M3.2.0,M11.1.0'], ['Denver', 'MST7MDT,M3.2.0,M11.1.0'], ['Los Angeles', 'PST8PDT,M3.2.0,M11.1.0'],
  ['London', 'GMT0BST,M3.5.0/1,M10.5.0'], ['Berlin, Paris, Madrid', 'CET-1CEST,M3.5.0,M10.5.0/3'], ['Bogotá, Lima', '<-05>5'], ['Tokyo', 'JST-9'], ['Sydney', 'AEST-10AEDT,M10.1.0,M4.1.0/3'], ['UTC', 'UTC0']];
const QUICK = [['On', 'on'], ['Off', 'off'], ['Sunrise over 20 min', 'rise 20'], ['Sleep in 30 min', 'sleep 30'], ['Warm evening colour', 'rgb 255 170 100'], ['Dim to 40 %', 'level 100'], ['Rainbow', 'mode rainbow']];
export function Schedule() {
  const list = () => state().sched || [];
  let at, cmd, tzSel;
  createEffect(() => { if (tzSel) tzSel.value = state().tz || 'UTC0'; });
  const add = () => { const c = cmd.value.trim(); if (!at.value || !c) { toast('Pick a time and a command', 'warn'); return; } send('sched add ' + at.value + ' ' + c); cmd.value = ''; };
  return <section class="card md:col-span-2">
    <h2 class="card-title">Schedule <small>{state().time ? 'board time ' + state().time : 'the board has no time yet'}</small></h2>
    <div class="row">
      <input ref={at} type="time" class="field" value="07:00" aria-label="Time" />
      <input ref={cmd} type="text" class="field min-w-[200px] grow" placeholder="command, e.g. rise 20" aria-label="Scheduled command" onKeyDown={e => e.key === 'Enter' && add()} />
      <button type="button" class="btn btn-primary btn-sm" onClick={add}>Add</button>
    </div>
    <div class="mt-2 flex flex-wrap gap-2"><For each={QUICK}>{([l, c]) => <button type="button" class="chip h-9 px-3 text-xs" onClick={() => { cmd.value = c; }}>{l}</button>}</For></div>
    <div class="mt-3 grid gap-1">
      <For each={list()} fallback={<span class="hint">nothing scheduled</span>}>{(e, i) => <div class="flex items-center gap-3 rounded-xl bg-raised px-3 py-2 text-sm">
        <b class="tabular-nums">{e.at}</b><span class="grow font-mono text-xs text-muted">{e.cmd}</span>
        <button type="button" class="chip h-8 px-2 text-xs" aria-label={'delete schedule ' + (i() + 1)} onClick={() => send('sched del ' + (i() + 1))}>✕</button>
      </div>}</For>
    </div>
    <div class="row mt-3">
      <label class="hint w-[92px]">Time zone</label>
      <select ref={tzSel} class="field grow" aria-label="Time zone" onChange={e => send('tz ' + e.currentTarget.value)}>
        <For each={TZS}>{([name, tz]) => <option value={tz}>{name}</option>}</For>
        <Show when={state().tz && !TZS.some(t => t[1] === state().tz)}><option value={state().tz}>{state().tz}</option></Show>
      </select>
    </div>
    <p class="hint mt-2">A command is anything the board understands: on, off, level N, rgb R G B, mode NAME, rise N (sunrise over N minutes), sleep N (fade out over N minutes), palette, seg … Up to 8 entries.</p>
  </section>;
}

export function Spotify() {
  return <section class="card md:col-span-2">
    <h2 class="card-title">Spotify <small>{sp.state()}</small></h2>
    <div class="row">
      <Show when={sp.art()} fallback={<div class="h-16 w-16 shrink-0 rounded-lg bg-raised" />}><img class="h-16 w-16 shrink-0 rounded-lg bg-raised object-cover" alt="" src={sp.art()} /></Show>
      <div class="min-w-[160px] grow"><div class="font-semibold">{sp.track()}</div><div class="hint">{sp.artist()}</div></div>
      <button type="button" class="btn" onClick={sp.toggle}>{sp.connected() ? 'Disconnect' : 'Connect Spotify'}</button>
    </div>
    <div class="mt-3 flex items-center justify-between gap-3">
      <div><div class="text-sm">Follow the album art</div><div class="hint">each new track recolours the "cover" effect; your own gradient stays under "palette"</div></div>
      <Switch label="Album art gradient" checked={sp.auto} onChange={sp.setAuto} />
    </div>
    <div class="mt-3 flex gap-2"><For each={sp.palette()}>{hx => <span class="h-7 w-7 rounded-full border border-line" style={{ background: '#' + hx }} />}</For></div>
    <div class="row mt-3"><label class="hint w-[92px]">Client ID</label><input type="text" class="field grow" placeholder="only needed if the site has none" aria-label="Spotify client ID" value={sp.client()} onChange={e => sp.setClient(e.currentTarget.value)} /></div>
  </section>;
}

export function Presets() {
  const load = () => { try { return JSON.parse(localStorage.getItem('ledify.presets') || '{}'); } catch (e) { return {}; } };
  const [all, setAll] = createSignal(load());
  const store = a => { localStorage.setItem('ledify.presets', JSON.stringify(a)); setAll({ ...a }); };
  let nameEl;
  const save = () => {
    const s = state(), c = (dev().light || {}).color || { r: 255, g: 255, b: 255 }, a = all();
    const name = nameEl.value.trim() || 'look ' + (Object.keys(a).length + 1);
    a[name] = { rgb: c.r + ' ' + c.g + ' ' + c.b, level: s.level, mode: s.mode, overlay: s.overlay, auto: s.auto, gain: s.gain, power: s.power, palette: s.mode === 'palette' ? s.palette : undefined };
    store(a); nameEl.value = ''; toast('Saved "' + name + '"', 'ok', 2000);
  };
  const apply = (name, p) => { send('rgb ' + p.rgb); send('level ' + p.level); if (p.palette) send('palette ' + p.palette); send('mode ' + p.mode); if (p.overlay) send('overlay ' + p.overlay); send('auto ' + (p.auto ? 'on' : 'off')); send('gain ' + p.gain); if (p.power) send('on'); toast('Applied "' + name + '"', 'ok', 2000); };
  const del = async name => { if (await confirmDialog('Delete preset?', '"' + name + '" will be removed from this browser.', 'Delete')) { const a = all(); delete a[name]; store(a); } };
  return <section class="card md:col-span-2">
    <h2 class="card-title">Presets <small>saved in this browser</small></h2>
    <div class="row"><input ref={nameEl} type="text" class="field grow" placeholder="name this look" aria-label="Preset name" onKeyDown={e => e.key === 'Enter' && save()} /><button type="button" class="btn btn-primary" onClick={save}>Save current</button></div>
    <div class="mt-3 flex flex-wrap gap-2">
      <Show when={Object.keys(all()).length} fallback={<span class="hint">none yet: set up a look, then save it</span>}>
        <For each={Object.entries(all())}>{([name, p]) => <>
          <button type="button" class="chip" onClick={() => apply(name, p)}>{name}</button>
          <button type="button" class="chip" title={'delete ' + name} aria-label={'delete ' + name} onClick={() => del(name)}>✕</button>
        </>}</For>
      </Show>
    </div>
  </section>;
}

export function Advanced() {
  const [tempLo, setTempLo] = optimistic('temp_lo', () => state().temp_lo);
  const [tempHi, setTempHi] = optimistic('temp_hi', () => state().temp_hi);
  const [fadeMs, setFadeMs] = optimistic('fade', () => state().fade);
  const [tmax, setTmax] = optimistic('tmax', () => state().tmax);
  let tempIn, otaUrl, cmd;
  const ota = async () => { if (await confirmDialog('Update the firmware?', 'From ' + otaUrl.value + '. The strip restarts when the download is done, about a minute.', 'Update')) send('ota ' + otaUrl.value); };
  const sendCmd = () => { const c = cmd.value.trim(); if (c) { send(c); cmd.value = ''; } };
  return <section class="card always md:col-span-2">
    <details>
      <summary>Advanced: temperature colour, firmware update, raw commands</summary>
      <Slider rowClass="mt-3" label="Cold colour at" min={-10} max={40} value={tempLo} set={setTempLo} send={v => 'temp ' + v + ' ' + tempHi()} fmt={v => v + ' °C'} />
      <Slider label="Hot colour at" min={0} max={60} value={tempHi} set={setTempHi} send={v => 'temp ' + tempLo() + ' ' + v} fmt={v => v + ' °C'} />
      <div class="row mt-2"><label class="hint w-[92px]">Feed °C</label><input ref={tempIn} type="number" step="0.5" class="field w-24" placeholder="21.5" aria-label="Temperature to feed" /><button type="button" class="btn btn-sm" onClick={() => tempIn.value !== '' && send('temp ' + tempIn.value)}>Send</button><span class="hint">used by the "temp" effect instead of the board's sensor</span></div>
      <Slider rowClass="mt-3" label="Transition" min={0} max={2000} step={50} value={fadeMs} set={setFadeMs} send={v => 'fade ' + v} fmt={v => v ? v + ' ms' : 'instant'} />
      <Slider label="Thermal limit" min={0} max={120} step={5} value={tmax} set={setTmax} send={v => 'tmax ' + v} fmt={v => v ? v + ' °C' : 'off'} />
      <p class="hint">Colour and brightness changes ease over the transition time. The strip dims as the board warms over the last 10 °C before the thermal limit and is dark at the limit (it is at {state().board_c ?? '–'} °C now).</p>
      <div class="row mt-4"><label class="hint w-[92px]">Firmware</label><input ref={otaUrl} type="url" class="field grow" aria-label="Firmware URL" value={location.origin + '/firmware/ledify.bin'} /><button type="button" class="btn" onClick={ota}>Update over the air</button></div>
      <Show when={!LOCAL}><div class="row mt-2"><span class="hint w-[92px]">OTA source</span><button type="button" class="btn btn-sm" onClick={() => send('otabase ' + location.origin + '/')}>Only accept images from this site</button><span class="hint">so a leaked broker login cannot flash other firmware; "otabase any" undoes it</span></div></Show>
      <div class="row mt-3"><label class="hint w-[92px]">Command</label><input ref={cmd} type="text" class="field grow" placeholder="e.g. status" aria-label="Command" onKeyDown={e => e.key === 'Enter' && sendCmd()} /><button type="button" class="btn" onClick={sendCmd}>Send</button></div>
      <div class="log">{logText()}</div>
    </details>
  </section>;
}
