// Small building blocks: sliders that send while moving, chip groups, switches, toggles rows, the confirm dialog.
import { For, Show, createSignal, createEffect } from 'solid-js';
import { send } from './store';
export { hex2, rgbHex } from './color';

export const debounce = () => { let t; return (fn, ms = 60) => { clearTimeout(t); t = setTimeout(fn, ms); }; };

// value: accessor; set: local update (optimistic); send: value -> command, sent every 60 ms while moving and on release.
export function Range(p) {
  const later = debounce();
  const min = () => p.min ?? 0, max = () => p.max ?? 100;
  const v = () => p.value() ?? min();
  const go = (val, now) => { p.set(val); if (!p.send) return; const fire = () => { const c = p.send(val); if (c) send(c); }; now ? (later(() => {}, 0), fire()) : later(fire); };
  return <input type="range" class={'range ' + (p.class || '')} min={min()} max={max()} step={p.step || 1} value={v()} aria-label={p.label}
    style={{ '--p': ((v() - min()) * 100 / (max() - min())) + '%', ...(p.style || {}) }}
    onInput={e => go(+e.currentTarget.value, false)} onChange={e => go(+e.currentTarget.value, true)} />;
}
export function Slider(p) {
  return <div class={'slider ' + (p.rowClass || '')}><label>{p.label}</label><Range {...p} /><output>{p.value() === undefined ? '–' : (p.fmt || (x => x))(p.value())}</output></div>;
}
export function Chips(p) {
  return <div class={'flex flex-wrap gap-2 ' + (p.class || '')}>
    <For each={p.items}>{([v, label]) => <button type="button" class={'chip ' + (p.chip || '')} aria-pressed={String(p.value()) === String(v) ? 'true' : 'false'} onClick={() => p.onPick(v)}>{label}</button>}</For>
  </div>;
}
export function Switch(p) {
  return <button type="button" role="switch" class="switch" aria-checked={p.checked() ? 'true' : 'false'} aria-label={p.label} onClick={() => p.onChange(!p.checked())} />;
}
// A labelled on/off row: the whole row is the target, not just the little switch.
export function Toggle(p) {
  return <div class="toggle" onClick={e => { if (!e.target.closest('.switch')) p.onChange(!p.checked()); }}>
    <div class="min-w-0 grow"><div class="text-sm">{p.label}</div><Show when={p.hint}><div class="hint">{p.hint}</div></Show></div>
    <Switch label={p.label} checked={p.checked} onChange={p.onChange} />
  </div>;
}
// A group inside a lens, with a small heading.
export function Group(p) {
  return <section class={'group ' + (p.class || '')}><Show when={p.title}><h3 class="group-title">{p.title}<Show when={p.aside}><small>{p.aside}</small></Show></h3></Show>{p.children}</section>;
}

const [ask, setAsk] = createSignal(null);
export const confirmDialog = (title, text, yes = 'Confirm') => new Promise(resolve => setAsk({ title, text, yes, resolve }));
export function Confirm() {
  let d;
  const done = v => { const a = ask(); setAsk(null); if (d.open) d.close(); a && a.resolve(v); };
  createEffect(() => { if (ask() && !d.open) d.showModal(); });
  return <dialog ref={d} class="dialog" onCancel={e => { e.preventDefault(); done(false); }}>
    <h3>{ask()?.title}</h3>
    <p class="hint">{ask()?.text}</p>
    <div class="mt-4 flex justify-end gap-2"><button type="button" class="btn" onClick={() => done(false)}>Cancel</button><button type="button" class="btn btn-primary" onClick={() => done(true)}>{ask()?.yes}</button></div>
  </dialog>;
}
