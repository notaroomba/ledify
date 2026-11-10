// Zones (parts of the strip with their own colour) and Time (sleep, sunrise, the schedule).
import { For, Show, createEffect } from 'solid-js';
import { state, send, toast } from '../store';
import { Chips, Group } from '../ui';

const SRC = ['solid', 'rainbow', 'palette', 'temp', 'meter', 'spectrum'];
export function ZonesLens() {
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
  return <>
    <p class="lens-tip">A zone gives part of the strip its own colour. Drag a zone's ends on the outer ring; pixels outside every zone keep the main effect.</p>
    <div class="grid gap-2">
      <For each={segs()} fallback={<p class="empty">No zones. The whole strip follows the main effect.</p>}>{s => <div class="zone-row">
        <div class="flex items-center gap-2">
          <b class="zone-badge" style={{ background: s.color === 'solid' ? '#' + s.rgb : undefined }}>{s.i}</b>
          <input type="number" min="0" max={count()} class="field h-9 w-20" aria-label={'zone ' + s.i + ' start'} value={s.a} onChange={e => num(s, 'a', e)} />
          <span class="hint">to</span>
          <input type="number" min="1" max={count()} class="field h-9 w-20" aria-label={'zone ' + s.i + ' end'} value={s.b} onChange={e => num(s, 'b', e)} />
          <Show when={s.color === 'solid'}><input type="color" class="mini-color h-9" aria-label={'zone ' + s.i + ' colour'} value={'#' + s.rgb} onChange={e => put(s.i, s.a, s.b, s.color, e.currentTarget.value.slice(1))} /></Show>
          <span class="grow" />
          <button type="button" class="icon-btn" aria-label={'remove zone ' + s.i} onClick={() => send('seg del ' + s.i)}>✕</button>
        </div>
        <Chips class="mt-2" chip="chip-sm" items={SRC.map(k => [k, k])} value={() => s.color} onPick={v => put(s.i, s.a, s.b, v, s.rgb)} />
      </div>}</For>
    </div>
    <div class="row mt-3">
      <button type="button" class="btn btn-primary btn-sm" onClick={add}>Add zone</button>
      <Show when={segs().length}><button type="button" class="btn btn-sm" onClick={() => send('seg off')}>Remove all</button></Show>
      <span class="hint">pixels count from the board; the end is not included</span>
    </div>
  </>;
}

const mmss = s => Math.floor(s / 60) + ':' + ('0' + Math.round(s % 60)).slice(-2);
const TZS = [['New York', 'EST5EDT,M3.2.0,M11.1.0'], ['Chicago', 'CST6CDT,M3.2.0,M11.1.0'], ['Denver', 'MST7MDT,M3.2.0,M11.1.0'], ['Los Angeles', 'PST8PDT,M3.2.0,M11.1.0'],
  ['London', 'GMT0BST,M3.5.0/1,M10.5.0'], ['Berlin, Paris, Madrid', 'CET-1CEST,M3.5.0,M10.5.0/3'], ['Bogotá, Lima', '<-05>5'], ['Tokyo', 'JST-9'], ['Sydney', 'AEST-10AEDT,M10.1.0,M4.1.0/3'], ['UTC', 'UTC0']];
const QUICK = [['On', 'on'], ['Off', 'off'], ['Sunrise 20 min', 'rise 20'], ['Sleep 30 min', 'sleep 30'], ['Warm evening', 'rgb 255 170 100'], ['Dim to 40 %', 'level 100'], ['Rainbow', 'mode rainbow']];
export function TimeLens() {
  const list = () => state().sched || [];
  let at, cmd, tzSel;
  createEffect(() => { if (tzSel) tzSel.value = state().tz || 'UTC0'; });
  const add = () => { const c = cmd.value.trim(); if (!at.value || !c) { toast('Pick a time and a command', 'warn'); return; } send('sched add ' + at.value + ' ' + c); cmd.value = ''; };
  return <>
    <Group title="Now">
      <div class="timer-grid">
        <For each={[15, 30, 60, 120]}>{m => <button type="button" class="timer" onClick={() => send('sleep ' + m)}><b>{m < 60 ? m : m / 60}</b><span>{m < 60 ? 'min' : m === 60 ? 'hour' : 'hours'}</span></button>}</For>
      </div>
      <p class="hint mt-1">Fade out and switch off after that long.</p>
      <Show when={state().sleep > 0}><button type="button" class="status-chip mt-2" onClick={() => send('sleep 0')}>fading out · {mmss(state().sleep)} left · cancel</button></Show>
      <Show when={state().rise > 0}><span class="status-chip mt-2">sunrise · {mmss(state().rise)} to go</span></Show>
      <div class="row mt-2"><button type="button" class="btn btn-sm" onClick={() => send('rise 20')}>Sunrise over 20 min, now</button></div>
    </Group>
    <Group title="Every day" aside={state().time ? 'board time ' + state().time : 'the board has no time yet'}>
      <div class="row">
        <input ref={at} type="time" class="field" value="07:00" aria-label="Time" />
        <input ref={cmd} type="text" class="field min-w-[8rem] grow" placeholder="command, e.g. rise 20" aria-label="Scheduled command" onKeyDown={e => e.key === 'Enter' && add()} />
        <button type="button" class="btn btn-primary btn-sm" onClick={add}>Add</button>
      </div>
      <div class="mt-2 flex flex-wrap gap-1.5"><For each={QUICK}>{([l, c]) => <button type="button" class="chip chip-sm" onClick={() => { cmd.value = c; }}>{l}</button>}</For></div>
      <div class="timeline mt-3">
        <For each={list()} fallback={<p class="empty">Nothing scheduled.</p>}>{(e, i) => <div class="tl-item">
          <b>{e.at}</b><code>{e.cmd}</code>
          <button type="button" class="icon-btn" aria-label={'delete schedule ' + (i() + 1)} onClick={() => send('sched del ' + (i() + 1))}>✕</button>
        </div>}</For>
      </div>
      <div class="row mt-3">
        <label class="hint">Time zone</label>
        <select ref={tzSel} class="field grow" aria-label="Time zone" onChange={e => send('tz ' + e.currentTarget.value)}>
          <For each={TZS}>{([name, tz]) => <option value={tz}>{name}</option>}</For>
          <Show when={state().tz && !TZS.some(t => t[1] === state().tz)}><option value={state().tz}>{state().tz}</option></Show>
        </select>
      </div>
      <p class="hint mt-2">A command is anything the board understands: on, off, level N, rgb R G B, mode NAME, rise N, sleep N, palette, seg … Up to 8 entries.</p>
    </Group>
  </>;
}
