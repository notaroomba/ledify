// The panel: one screen, no pages. The ring in the middle is the strip and its main control; the dock opens one lens
// at a time beside it (a sheet from the bottom on a phone, a panel on the side on a wide screen) while the ring keeps
// showing what the strip does. The command bar reaches everything by typing.
import { render, Dynamic } from 'solid-js/web';
import { For, Show, createSignal, createMemo, onMount, onCleanup } from 'solid-js';
import './index.css';
import { ui, dev, state, opt, init, selectDevice, LOCAL, DEMO } from './store';
import { Confirm } from './ui';
import { Ring } from './ring';
import { act } from './actions';
import { effectOf } from './effects';
import { sp } from './spotify';
import { Broker, Setup } from './dialogs';
import { CommandBar, setCmdOpen } from './command';
import { ColourLens, EffectsLens, GradientLens } from './lenses/look';
import { SoundLens } from './lenses/sound';
import { ZonesLens, TimeLens } from './lenses/space';
import { LooksLens, BoardLens } from './lenses/more';

const I = d => <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" innerHTML={d} />;
const LENSES = [
  { id: 'colour', name: 'Colour', keys: 'hue white color', icon: '<circle cx="12" cy="12" r="9"/><path d="M12 3a9 9 0 0 1 0 18"/><circle cx="12" cy="12" r="3.2"/>', view: ColourLens },
  { id: 'effects', name: 'Effects', keys: 'mode animation overlay auto dim', icon: '<path d="M12 3v3M12 18v3M3 12h3M18 12h3M5.6 5.6l2.1 2.1M16.3 16.3l2.1 2.1M5.6 18.4l2.1-2.1M16.3 7.7l2.1-2.1"/><circle cx="12" cy="12" r="2.5"/>', view: EffectsLens },
  { id: 'gradient', name: 'Gradient', keys: 'palette stripes scroll', icon: '<rect x="3" y="8" width="18" height="8" rx="4"/><circle cx="8" cy="12" r="1.6"/><circle cx="15" cy="12" r="1.6"/>', view: GradientLens },
  { id: 'sound', name: 'Sound', keys: 'mic music audio gain listen', icon: '<path d="M4 10v4M8 7v10M12 4v16M16 8v8M20 11v2"/>', view: SoundLens },
  { id: 'zones', name: 'Zones', keys: 'segments split', icon: '<path d="M12 3a9 9 0 0 1 9 9"/><path d="M21 12a9 9 0 0 1-9 9" opacity=".55"/><path d="M12 21a9 9 0 0 1-9-9" opacity=".3"/><path d="M3 12a9 9 0 0 1 9-9" opacity=".8"/>', view: ZonesLens },
  { id: 'time', name: 'Time', keys: 'schedule sleep sunrise alarm timezone', icon: '<circle cx="12" cy="13" r="8"/><path d="M12 9v4l2.5 2.5M9 2h6"/>', view: TimeLens },
  { id: 'looks', name: 'Looks', keys: 'presets spotify album saved', icon: '<path d="M12 3l2.6 5.6 6 .7-4.5 4.1 1.2 6L12 16.4 6.7 19.4l1.2-6L3.4 9.3l6-.7z"/>', view: LooksLens },
  { id: 'board', name: 'Board', keys: 'strip pixels sensors firmware power supply voltage setup broker console', icon: '<rect x="5" y="5" width="14" height="14" rx="2"/><path d="M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3"/><rect x="9" y="9" width="6" height="6" rx="1"/>', view: BoardLens },
];
const byId = id => LENSES.find(l => l.id === id);

function App() {
  const fromHash = () => byId(location.hash.slice(1)) ? location.hash.slice(1) : null;
  const [lens, setLens] = createSignal(fromHash());
  let pushed = false;
  const open = id => {
    if (lens() === id) return close();
    if (pushed) history.replaceState(null, '', '#' + id); else { history.pushState(null, '', '#' + id); pushed = true; }
    setLens(id);
  };
  const close = () => { if (!lens()) return; if (pushed) { pushed = false; history.back(); } else { history.replaceState(null, '', location.pathname + location.search); setLens(null); } };
  const onHash = () => { setLens(fromHash()); if (!fromHash()) pushed = false; };

  onMount(() => {
    init();
    sp.init();
    if ('serviceWorker' in navigator && !LOCAL && !DEMO && location.protocol === 'https:') navigator.serviceWorker.register('/sw.js').catch(() => {});
    window.addEventListener('popstate', onHash);
    window.addEventListener('keydown', keys);
    onCleanup(() => { window.removeEventListener('popstate', onHash); window.removeEventListener('keydown', keys); });
  });

  // Keyboard: ⌘K or / for the command bar, 1–8 for the lenses, space for power, arrows for brightness, [ ] for effects.
  const keys = e => {
    if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') { e.preventDefault(); setCmdOpen(true); return; }
    if (e.metaKey || e.ctrlKey || e.altKey || document.querySelector('dialog[open]')) return;
    const t = e.target, typing = t.closest && t.closest('input, select, textarea, [role="slider"], [contenteditable]');
    if (e.key === 'Escape' && lens()) { e.preventDefault(); close(); return; }
    if (typing) return;
    if (e.key === '/') { e.preventDefault(); setCmdOpen(true); }
    else if (/^[1-8]$/.test(e.key)) open(LENSES[+e.key - 1].id);
    else if (e.key === ' ' && !t.closest('button')) { e.preventDefault(); act.togglePower(); }
    else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') { e.preventDefault(); act.level((opt.level() ?? 0) + (e.key === 'ArrowUp' ? 13 : -13), true); }
    else if (e.key === '[' || e.key === ']') act.stepMode(e.key === ']' ? 1 : -1);
  };

  // Drag the sheet's grabber down to close it (phones).
  let sheet, sy = null;
  const grabDown = e => { sy = e.clientY; e.currentTarget.setPointerCapture(e.pointerId); sheet.style.transition = 'none'; };
  const grabMove = e => { if (sy !== null) sheet.style.transform = `translateY(${Math.max(0, e.clientY - sy)}px)`; };
  const grabUp = e => { if (sy === null) return; const dy = e.clientY - sy; sy = null; sheet.style.transition = ''; sheet.style.transform = ''; if (dy > 70) close(); };

  const ids = createMemo(() => Object.keys(ui.devices).sort());
  const off = () => ui.conn === 'offline';
  const cur = () => byId(lens());
  const s = state;
  const fmt = (v, d) => v === undefined || v === null ? '–' : (+v).toFixed(d);
  const alerts = () => {
    const a = [], st = s(), need = (st.need_mw || 0) / 1000, budget = (st.budget_mw || 0) / 1000;
    if (DEMO) a.push(['info', 'Demo: a pretend board, nothing is wired', null]);
    if (off()) a.push(['warn', 'The board is offline. Changes apply when it is back.', null]);
    if (st.power && st.limit && st.cap < st.level) a.push(['warn', 'Dimmed to ' + Math.round(st.cap * 100 / 255) + '% to stay within the supply', 'board']);
    else if (st.power && need > budget && budget) a.push(['warn', 'Needs ~' + need.toFixed(1) + ' W, the supply gives ' + budget.toFixed(1) + ' W', 'board']);
    if (st.hot > 0) a.push(['warn', 'Dimmed ' + st.hot + ' % for heat (' + st.board_c + ' °C)', 'board']);
    if (st.stream) a.push(['info', 'Showing pixels from ' + st.stream.src, 'board']);
    return a;
  };
  const sat = () => {
    const st = s(), z = dev().sensors || {};
    return [
      ['lux', fmt(z.lux ?? st.lux, 0), 'board'],
      ['°C', fmt(z.temp_c ?? st.temp_c, 1), 'board'],
      ['W', st.need_mw !== undefined ? fmt(st.need_mw / 1000, 1) : '–', 'board'],
      ['dB', fmt(ui.live.db ?? st.sound_db, 0), 'sound'],
    ];
  };

  return <div class="app" classList={{ 'has-lens': !!lens(), offline: off() }}>
    <div class="ambient" aria-hidden="true" />
    <header class="top">
      <span class="brand">LEDIFY</span>
      <Show when={ids().length > 1}>
        <select class="board-pick" aria-label="Board" value={ui.current} onChange={e => selectDevice(e.currentTarget.value)}><For each={ids()}>{id => <option value={id}>{id}</option>}</For></select>
      </Show>
      <span class="conn" title={ui.current || ''}><span class={'dot ' + (ui.conn === 'online' ? 'on' : ui.conn === 'connecting' ? '' : 'off')} />{ui.conn}</span>
      <span class="grow" />
      <button type="button" class="search" onClick={() => setCmdOpen(true)} aria-label="Command bar">
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="11" cy="11" r="7" /><path d="m20 20-3.5-3.5" /></svg>
        <span class="search-text">Search or command</span><kbd>⌘K</kbd>
      </button>
    </header>

    <main class="stage">
      <div class="alerts"><For each={alerts()}>{([k, text, to]) => <button type="button" class={'alert alert-' + k} disabled={!to} onClick={() => to && open(to)}>{text}</button>}</For></div>
      <Ring lens={lens} />
      <div class="sats"><For each={sat()}>{([u, v, to]) => <button type="button" class="sat" onClick={() => open(to)}><b>{v}</b><span>{u}</span></button>}</For></div>
      <div class="fx-step">
        <button type="button" class="step" aria-label="Previous effect" onClick={() => act.stepMode(-1)}>‹</button>
        <button type="button" class="fx-name" onClick={() => open('effects')}>{effectOf(opt.mode())?.label || opt.mode() || '–'}</button>
        <button type="button" class="step" aria-label="Next effect" onClick={() => act.stepMode(1)}>›</button>
      </div>
    </main>

    <aside ref={sheet} class="sheet" aria-label={cur() ? cur().name : undefined} aria-hidden={lens() ? 'false' : 'true'} inert={lens() ? undefined : true}>
      <div class="grabber" onPointerDown={grabDown} onPointerMove={grabMove} onPointerUp={grabUp} onPointerCancel={grabUp}><i /></div>
      <Show when={cur()}>
        <div class="sheet-head">
          <span class="sheet-icon">{I(cur().icon)}</span>
          <h2>{cur().name}</h2>
          <button type="button" class="icon-btn" aria-label="Close" onClick={close}>✕</button>
        </div>
        <div class="sheet-body" classList={{ dim: off() && cur().id !== 'board' }}><Dynamic component={cur().view} /></div>
      </Show>
    </aside>

    <nav class="dock" aria-label="Lenses">
      <For each={LENSES}>{(l, i) => <button type="button" class="dock-btn" aria-pressed={lens() === l.id ? 'true' : 'false'} onClick={() => open(l.id)} title={l.name + ' (' + (i() + 1) + ')'}>
        {I(l.icon)}<span>{l.name}</span>
      </button>}</For>
    </nav>

    <div class="toasts"><For each={ui.toasts}>{t => <div class={'toast toast-' + (t.kind || 'info')}>{t.text}</div>}</For></div>
    <CommandBar lenses={LENSES} open={open} />
    <Confirm />
    <Broker />
    <Setup />
  </div>;
}
render(() => <App />, document.getElementById('root'));
