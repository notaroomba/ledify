// Looks (saved presets and Spotify's album colours) and Board (sensors, supply, the strip's hardware, firmware).
import { For, Show, createEffect, createSignal } from 'solid-js';
import { ui, dev, state, send, logText, optimistic, opt, LOCAL, DEMO, AUTH, setUi, slugName, DEFAULT_NAME } from '../store';
import { Slider, Chips, Switch, Toggle, Group, confirmDialog } from '../ui';
import { presets } from '../presets';
import { sp } from '../spotify';
import { parseStops, stopsCss } from '../gradient';
import { setSetupOpen } from '../dialogs';
import { setAccessOpen } from '../access';

export const supply = () => { const s = state(); return s.pd && s.pd.ready ? (s.pd.mv / 1000) + ' V ' + (s.pd.ma / 1000) + ' A, ' + (s.pd.mv * s.pd.ma / 1e6).toFixed(0) + ' W' : 'no USB-PD contract, assuming 7.5 W'; };

export function LooksLens() {
  let nameEl;
  const save = () => { presets.save(nameEl.value); nameEl.value = ''; };
  const del = async name => { if (await confirmDialog('Delete look?', '"' + name + '" will be removed from this browser.', 'Delete')) presets.del(name); };
  const bg = p => p.mode === 'palette' && p.palette ? stopsCss(parseStops(p.palette), 135) : p.mode === 'rainbow' ? 'linear-gradient(135deg,#f00,#ff0,#0f0,#0ff,#00f,#f0f)' : 'rgb(' + p.rgb.split(' ').join(',') + ')';
  return <>
    <Group title="Saved looks" aside="this browser">
      <div class="looks">
        <For each={Object.entries(presets.all())} fallback={<p class="empty">Set up a look you like, then save it here.</p>}>{([name, p]) => <div class="look">
          <button type="button" class="look-face" style={{ background: bg(p) }} onClick={() => presets.apply(name, p)}><span>{name}</span><small>{p.mode} · {Math.round((p.level || 0) * 100 / 255)}%</small></button>
          <button type="button" class="look-x" aria-label={'delete ' + name} onClick={() => del(name)}>✕</button>
        </div>}</For>
      </div>
      <div class="row mt-3"><input ref={nameEl} type="text" class="field grow" placeholder="name what's showing now" aria-label="Look name" onKeyDown={e => e.key === 'Enter' && save()} /><button type="button" class="btn btn-primary btn-sm" onClick={save}>Save look</button></div>
    </Group>
    <Group title="Spotify" aside={sp.state()}>
      <div class="np">
        <Show when={sp.art()} fallback={<div class="np-art" />}><img class="np-art" alt="" src={sp.art()} /></Show>
        <div class="min-w-0 grow"><div class="truncate font-semibold">{sp.track()}</div><div class="hint truncate">{sp.artist()}</div>
          <div class="mt-2 flex gap-1.5"><For each={sp.palette()}>{hx => <span class="h-5 w-5 rounded-full border border-line" style={{ background: '#' + hx }} />}</For></div>
        </div>
        <button type="button" class="btn btn-sm" onClick={sp.toggle}>{sp.connected() ? 'Disconnect' : 'Connect'}</button>
      </div>
      <Toggle label="Follow the album art" hint={'each new track recolours the "Album" effect; your gradient stays put'} checked={sp.auto} onChange={sp.setAuto} />
      <details class="mt-2"><summary>Client ID</summary><input type="text" class="field mt-2 w-full" placeholder="only needed if the site has none" aria-label="Spotify client ID" value={sp.client()} onChange={e => sp.setClient(e.currentTarget.value)} /></details>
    </Group>
  </>;
}

const CHIPS = [['ws2812', 'WS2812 / B / C / D / E, WS2813, WS2815'], ['ws2811', 'WS2811 (12 V, RGB order)'], ['sk6812', 'SK6812'], ['sk6812w', 'SK6812 RGBW'], ['ws2816', 'WS2816']];
export function BoardLens() {
  const [limit, setLimit] = optimistic('limit', () => state().limit);
  const [count, setCount] = optimistic('count', () => state().count);
  const [type, setType] = optimistic('type', () => state().type);
  const [volt, setVolt] = optimistic('volt', () => state().volt);
  const [droop, setDroop] = optimistic('droop', () => state().droop);
  const [tempLo, setTempLo] = optimistic('temp_lo', () => state().temp_lo);
  const [tempHi, setTempHi] = optimistic('temp_hi', () => state().temp_hi);
  const [tmax, setTmax] = optimistic('tmax', () => state().tmax);
  const z = () => dev().sensors || {};
  const lux = () => z().lux ?? state().lux, temp = () => z().temp_c ?? state().temp_c;
  const kpa = () => z().pressure_kpa ?? (state().pressure_pa !== undefined ? state().pressure_pa / 1000 : undefined);
  const db = () => { const s = state(), v = ui.live.db ?? s.sound_db; return v === undefined || v === null ? '–' : Math.round(v) + (s.db_peak ? ' · ' + s.db_peak : ''); };
  const cap = () => { const s = state(); return s.level === undefined ? '–' : (s.limit ? s.cap : s.level) + ' / ' + s.level; };
  const fmt = (v, d) => v === undefined || v === null ? '–' : (+v).toFixed(d);
  let num, sel, tempIn, otaUrl, cmd, nameIn;
  const [typed, setTyped] = createSignal('');
  createEffect(() => { const n = state().name; if (nameIn && n !== undefined && document.activeElement !== nameIn) { nameIn.value = n; setTyped(n); } });
  const rename = () => {
    const n = slugName(nameIn.value);
    if (!n) { nameIn.value = state().name || ''; return; }
    nameIn.value = n; setTyped(n); nameIn.blur();
    if (n !== state().name) { send('name ' + n); setTimeout(() => send('status'), 400); }
  };
  createEffect(() => { const v = count(); if (num && v !== undefined && document.activeElement !== num) num.value = v; });
  createEffect(() => { if (sel) sel.value = type() || 'ws2812'; });
  const setExact = () => { const v = Math.max(1, Math.min(1000, +num.value || 1)); num.value = v; setCount(v); send('count ' + v); };
  const pickVolt = async v => {
    if (v === '12' && !await confirmDialog('Switch the strip to 12 V?', 'Only for 12 V strips (WS2815, WS2811). 5 V pixels are destroyed instantly. The output turns off first, so press power again afterwards.', 'Switch to 12 V')) return;
    setVolt(+v); send(v === '12' ? 'volt 12 yes' : 'volt 5');
  };
  const ota = async () => { if (await confirmDialog('Update the firmware?', 'From ' + otaUrl.value + '. The strip restarts when the download is done, about a minute.', 'Update')) send('ota ' + otaUrl.value); };
  const sendCmd = () => { const c = cmd.value.trim(); if (c) { send(c); cmd.value = ''; } };
  return <>
    <Group title="Name" aside={ui.current || ''}>
      <div class="row"><input ref={nameIn} type="text" maxLength={40} class="field grow" placeholder="e.g. desk or shelf" aria-label="Board name" onInput={e => setTyped(e.currentTarget.value)} onKeyDown={e => e.key === 'Enter' && rename()} /><button type="button" class="btn btn-primary btn-sm" disabled={!slugName(typed()) || slugName(typed()) === state().name} onClick={rename}>Rename</button></div>
      <p class="hint">{slugName(typed()) && slugName(typed()) !== typed().trim() ? 'Will be saved as "' + slugName(typed()) + '": letters, digits and dashes only. ' : ''}Shown in the board picker here and in every browser, and the board answers at {slugName(typed()) || state().name || DEFAULT_NAME}.local on your network.</p>
    </Group>
    <div class="tiles">
      <div class="tile"><b>{fmt(lux(), 0)}</b><span>lux</span></div>
      <div class="tile"><b>{fmt(temp(), 1)}</b><span>°C board</span></div>
      <div class="tile"><b>{fmt(kpa(), 2)}</b><span>kPa</span></div>
      <div class="tile"><b>{db()}</b><span>dB · peak</span></div>
      <div class="tile"><b>{cap()}</b><span>brightness sent</span></div>
    </div>
    <Group title="Power" aside={state().fw ? supply() : ''}>
      <Toggle label="Stay within the supply" hint="dim automatically so the charger is never overdrawn" checked={() => !!limit()} onChange={on => { setLimit(on ? 1 : 0); send('limit ' + (on ? 1 : 0)); }} />
      <Show when={state().pdos && state().pdos.length}><p class="hint">Charger offers {state().pdos.map(x => (x[0] / 1000) + ' V ' + (x[1] / 1000) + ' A').join(' · ')}; the board takes the most powerful.</p></Show>
      <Slider label="Heat limit" min={0} max={120} step={5} value={tmax} set={setTmax} send={v => 'tmax ' + v} fmt={v => v ? v + ' °C' : 'off'} />
      <p class="hint">The strip dims over the last 10 °C before the limit and is dark at it (board at {state().board_c ?? '–'} °C now).</p>
    </Group>
    <Group title="Strip">
      <Slider label="Pixels" min={1} max={1000} value={count} set={setCount} send={v => 'count ' + v} />
      <div class="row"><label class="hint">exact</label><input ref={num} type="number" min="1" max="1000" class="field w-24" aria-label="Exact pixel count" onKeyDown={e => e.key === 'Enter' && setExact()} /><button type="button" class="btn btn-sm" onClick={setExact}>Set</button></div>
      <div class="row mt-2"><label class="hint">Chip</label>
        <select ref={sel} class="field grow" aria-label="LED chip" onChange={e => { setType(e.currentTarget.value); send('type ' + e.currentTarget.value); }}><For each={CHIPS}>{([v, label]) => <option value={v}>{label}</option>}</For></select>
      </div>
      <div class="row mt-2"><span class="hint">Voltage</span><Chips chip="danger" items={[['5', '5 V'], ['12', '12 V']]} value={volt} onPick={pickVolt} /><span class="hint">12 V only for 12 V strips; needs a 15 V+ charger</span></div>
      <Show when={state().stream}><p class="hint mt-2 text-accent">Pixels are coming from {state().stream.src} over the LAN at {state().stream.fps} fps (DDP or E1.31); the effects resume when it stops.</p></Show>
      <Slider label="Tail fix" min={0} max={100} value={droop} set={setDroop} send={v => 'droop ' + v} fmt={v => v ? v + ' %' : 'off'} />
      <p class="hint">Cuts red towards the far end of a strip fed from one end, so it stays white as the voltage sags.</p>
    </Group>
    <Group title="Thermo effect">
      <Slider label="Cold at" min={-10} max={40} value={tempLo} set={setTempLo} send={v => 'temp ' + v + ' ' + tempHi()} fmt={v => v + ' °C'} />
      <Slider label="Hot at" min={0} max={60} value={tempHi} set={setTempHi} send={v => 'temp ' + tempLo() + ' ' + v} fmt={v => v + ' °C'} />
      <div class="row"><label class="hint">Feed °C</label><input ref={tempIn} type="number" step="0.5" class="field w-24" placeholder="21.5" aria-label="Temperature to feed" /><button type="button" class="btn btn-sm" onClick={() => tempIn.value !== '' && send('temp ' + tempIn.value)}>Send</button><span class="hint">instead of the board's sensor</span></div>
    </Group>
    <Group title="Connection">
      <p class="hint">{state().net || ''}</p>
      <Show when={state().name && state().ip}><p class="hint">On the local network at <a class="text-accent underline" href={'http://' + state().name + '.local/'}>{state().name}.local</a>, no cloud needed{state().clients ? ' · ' + state().clients + ' local panel' + (state().clients > 1 ? 's' : '') + ' open' : ''}</p></Show>
      <div class="row mt-2">
        <button type="button" class="btn btn-sm" onClick={() => setSetupOpen(true)}>Set up a board over Bluetooth</button>
        <Show when={!LOCAL && !DEMO && !AUTH}><button type="button" class="btn btn-sm" onClick={() => setUi('brokerOpen', true)}>Broker</button></Show>
        <Show when={AUTH}><button type="button" class="btn btn-sm" onClick={() => setAccessOpen(true)}>Access and sharing</button></Show>
      </div>
      <p class="hint mt-2">{state().setup ? 'Setup is open for ' + state().setup + ' s more.' : 'Wi-Fi, broker and update-source changes need the board\'s button held for 3 s first.'}</p>
    </Group>
    <Group title="Firmware" aside={state().fw ? 'running ' + state().fw : ''}>
      <div class="row"><input ref={otaUrl} type="url" class="field grow" aria-label="Firmware URL" value={location.origin + '/firmware/ledify.bin'} /><button type="button" class="btn btn-sm" onClick={ota}>Update</button></div>
      <Show when={!LOCAL}><div class="row mt-2"><button type="button" class="btn btn-sm" onClick={() => send('otabase ' + location.origin + '/')}>Only accept images from this site</button><span class="hint">"otabase any" undoes it</span></div></Show>
    </Group>
    <Group title="Console">
      <div class="row"><input ref={cmd} type="text" class="field grow font-mono" placeholder="any command, e.g. status" aria-label="Command" onKeyDown={e => e.key === 'Enter' && sendCmd()} /><button type="button" class="btn btn-sm" onClick={sendCmd}>Send</button></div>
      <div class="log">{logText()}</div>
    </Group>
  </>;
}
