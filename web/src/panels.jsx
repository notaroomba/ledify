// The cards. Each reads the board's state through the store and sends commands on the same topics the board uses.
import { For, Show, createSignal, createEffect, createMemo } from 'solid-js';
import { ui, dev, state, send, toast, logText, optimistic, opt } from './store';
import { Range, Slider, Chips, Switch, confirmDialog, debounce, rgbHex } from './ui';
import { listen, feed } from './audio';
import { sp } from './spotify';

const EFFECTS = {
  solid: 'one colour', rainbow: 'moving rainbow', temp: 'colour from the temperature', spectrum: 'bass, mid and treble bars',
  meter: 'green to red along the strip', palette: 'gradient of the album cover colours', sound: 'brightness follows the sound',
  vu: 'lit length follows the sound', pulse: 'flashes on the beat',
};
const OVERLAYS = ['none', 'vu', 'pulse'];
const PALETTE = ['#ffffff', '#ffd6a0', '#ff8c00', '#ffd700', '#ff0040', '#ff00ff', '#8000ff', '#0080ff', '#00e5ff', '#00ff80'];
const CHIPS = [['ws2812', 'WS2812 / B / C / D / E, WS2813, WS2815'], ['ws2811', 'WS2811 (12 V, RGB order)'], ['sk6812', 'SK6812'], ['sk6812w', 'SK6812 RGBW'], ['ws2816', 'WS2816']];
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
        <div class="mt-2 flex items-center justify-between gap-3">
          <span class="hint">Dim automatically to stay within the supply</span>
          <Switch label="Power limit" checked={() => !!limit()} onChange={on => { setLimit(on ? 1 : 0); send('limit ' + (on ? 1 : 0)); }} />
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
      <div><div class="text-sm">Gradient from the album art</div><div class="hint">every new track spreads its cover's main colours along the strip</div></div>
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
    a[name] = { rgb: c.r + ' ' + c.g + ' ' + c.b, level: s.level, mode: s.mode, overlay: s.overlay, auto: s.auto, gain: s.gain, power: s.power };
    store(a); nameEl.value = ''; toast('Saved "' + name + '"', 'ok', 2000);
  };
  const apply = (name, p) => { send('rgb ' + p.rgb); send('level ' + p.level); send('mode ' + p.mode); if (p.overlay) send('overlay ' + p.overlay); send('auto ' + (p.auto ? 'on' : 'off')); send('gain ' + p.gain); if (p.power) send('on'); toast('Applied "' + name + '"', 'ok', 2000); };
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
  let tempIn, otaUrl, cmd;
  const ota = async () => { if (await confirmDialog('Update the firmware?', 'From ' + otaUrl.value + '. The strip restarts when the download is done, about a minute.', 'Update')) send('ota ' + otaUrl.value); };
  const sendCmd = () => { const c = cmd.value.trim(); if (c) { send(c); cmd.value = ''; } };
  return <section class="card always md:col-span-2">
    <details>
      <summary>Advanced: temperature colour, firmware update, raw commands</summary>
      <Slider rowClass="mt-3" label="Cold colour at" min={-10} max={40} value={tempLo} set={setTempLo} send={v => 'temp ' + v + ' ' + tempHi()} fmt={v => v + ' °C'} />
      <Slider label="Hot colour at" min={0} max={60} value={tempHi} set={setTempHi} send={v => 'temp ' + tempLo() + ' ' + v} fmt={v => v + ' °C'} />
      <div class="row mt-2"><label class="hint w-[92px]">Feed °C</label><input ref={tempIn} type="number" step="0.5" class="field w-24" placeholder="21.5" aria-label="Temperature to feed" /><button type="button" class="btn btn-sm" onClick={() => tempIn.value !== '' && send('temp ' + tempIn.value)}>Send</button><span class="hint">used by the "temp" effect instead of the board's sensor</span></div>
      <div class="row mt-4"><label class="hint w-[92px]">Firmware</label><input ref={otaUrl} type="url" class="field grow" aria-label="Firmware URL" value={location.origin + '/firmware/ledify.bin'} /><button type="button" class="btn" onClick={ota}>Update over the air</button></div>
      <div class="row mt-3"><label class="hint w-[92px]">Command</label><input ref={cmd} type="text" class="field grow" placeholder="e.g. status" aria-label="Command" onKeyDown={e => e.key === 'Enter' && sendCmd()} /><button type="button" class="btn" onClick={sendCmd}>Send</button></div>
      <div class="log">{logText()}</div>
    </details>
  </section>;
}
