// The command bar (⌘K, / or the search button): type to jump to a lens, switch an effect, apply a look or gradient,
// set a timer, or send the board any command it understands.
import { For, Show, createSignal, createMemo, createEffect } from 'solid-js';
import { send, logText, setUi, LOCAL, DEMO, opt } from './store';
import { act } from './actions';
import { EFFECTS } from './effects';
import { grad } from './grad';
import { presets } from './presets';
import { setSetupOpen } from './dialogs';

export const [cmdOpen, setCmdOpen] = createSignal(false);

export function CommandBar(p) {
  let d, input;
  const [q, setQ] = createSignal(''), [hi, setHi] = createSignal(0);
  createEffect(() => { if (cmdOpen()) { if (!d.open) d.showModal(); setQ(''); setHi(0); queueMicrotask(() => input.focus()); } else if (d.open) d.close(); });
  const all = () => [
    { label: opt.power() ? 'Turn off' : 'Turn on', kind: 'power', run: act.togglePower, keys: 'power on off light' },
    ...[10, 25, 50, 75, 100].map(v => ({ label: 'Brightness ' + v + ' %', kind: 'level', run: () => act.level(v * 2.55, true) })),
    ...p.lenses.map(l => ({ label: 'Open ' + l.name, kind: 'lens', run: () => p.open(l.id), keys: l.keys })),
    ...EFFECTS.map(e => ({ label: e.label, kind: 'effect', hint: e.desc, run: () => act.mode(e.id), keys: e.id + ' mode effect' })),
    ...Object.entries(presets.all()).map(([n, pr]) => ({ label: n, kind: 'look', run: () => presets.apply(n, pr), keys: 'preset look' })),
    ...Object.entries(grad.lib()).map(([n, str]) => ({ label: n, kind: 'gradient', run: () => grad.place(str), keys: 'gradient palette' })),
    ...[15, 30, 60, 120].map(m => ({ label: 'Sleep in ' + m + ' min', kind: 'timer', run: () => act.sleep(m), keys: 'sleep timer fade' })),
    { label: 'Cancel sleep timer', kind: 'timer', run: () => act.sleep(0), keys: 'sleep stop' },
    { label: 'Sunrise over 20 min', kind: 'timer', run: () => send('rise 20'), keys: 'wake alarm' },
    { label: 'Set up a board over Bluetooth', kind: 'setup', run: () => setSetupOpen(true), keys: 'wifi ble' },
    ...(!LOCAL && !DEMO ? [{ label: 'Broker settings', kind: 'setup', run: () => setUi('brokerOpen', true), keys: 'mqtt' }] : []),
    ...(!LOCAL ? [{ label: 'Open the effect simulator', kind: 'tool', run: () => window.open('/sim.html', '_blank'), keys: 'sim preview test audio' }] : []),
  ];
  // Every word typed must appear somewhere in the label, kind or keywords; earlier matches rank higher.
  const items = createMemo(() => {
    const words = q().toLowerCase().trim().split(/\s+/).filter(Boolean);
    let list = all();
    if (words.length) {
      list = list.map(it => { const hay = (it.label + ' ' + it.kind + ' ' + (it.keys || '')).toLowerCase(); return { it, s: words.every(w => hay.includes(w)) ? words.reduce((a, w) => a + hay.indexOf(w), 0) : -1 }; })
        .filter(x => x.s >= 0).sort((a, b) => a.s - b.s).map(x => x.it);
      list = [...list.slice(0, 7), { label: 'Send "' + q().trim() + '" to the board', kind: 'command', run: () => send(q().trim()) }];
    } else list = list.slice(0, 9);
    return list;
  });
  const run = it => { setCmdOpen(false); it.run(); };
  const keys = e => {
    const n = items().length;
    if (e.key === 'ArrowDown') { e.preventDefault(); setHi((hi() + 1) % n); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); setHi((hi() - 1 + n) % n); }
    else if (e.key === 'Enter') { e.preventDefault(); const it = items()[hi()]; if (it) run(it); }
  };
  return <dialog ref={d} class="cmd" onClose={() => setCmdOpen(false)} onClick={e => { if (e.target === d) setCmdOpen(false); }}>
    <div class="cmd-in">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="11" cy="11" r="7" /><path d="m20 20-3.5-3.5" /></svg>
      <input ref={input} type="text" placeholder="Rainbow, sleep 30, open zones, or any board command…" aria-label="Command" value={q()} onInput={e => { setQ(e.currentTarget.value); setHi(0); }} onKeyDown={keys} />
      <kbd>esc</kbd>
    </div>
    <ul class="cmd-list" role="listbox">
      <For each={items()}>{(it, i) => <li role="option" aria-selected={hi() === i()} classList={{ hi: hi() === i() }} onPointerMove={() => setHi(i())} onClick={() => run(it)}>
        <span class="cmd-kind">{it.kind}</span><span class="grow truncate">{it.label}</span><Show when={it.hint}><span class="cmd-hint">{it.hint}</span></Show>
      </li>}</For>
    </ul>
    <details class="cmd-log"><summary>Recent traffic</summary><div class="log">{logText()}</div></details>
  </dialog>;
}
