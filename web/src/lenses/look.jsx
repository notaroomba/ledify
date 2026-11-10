// Colour, Effects and Gradient: what the strip shows.
import { For, Index, Show, createSignal, onMount, onCleanup } from 'solid-js';
import { state, opt, send, optimistic } from '../store';
import { act } from '../actions';
import { grad } from '../grad';
import { inputs, paint } from '../engine';
import { EFFECTS } from '../effects';
import { onTick } from '../ticker';
import { Range, Slider, Chips, Toggle, Group, confirmDialog } from '../ui';
import { hsv, rgbHsv, hexRgb, rgbHex, clamp } from '../color';
import { MAX_STOPS, parseStops, stopsCss, distinctColours, sortedStops } from '../gradient';

const SWATCHES = ['#ff0040', '#ff8c00', '#ffd700', '#00ff80', '#00e5ff', '#0080ff', '#8000ff', '#ff00ff'];
const WHITES = [['candle', '#ff9329'], ['warm', '#ffb46b'], ['soft', '#ffd6a0'], ['neutral', '#fff4e5'], ['daylight', '#ffffff'], ['cool', '#d6e4ff']];
const loadRecent = () => { try { return JSON.parse(localStorage.getItem('ledify.recent')) || []; } catch (e) { return []; } };
const [recent, setRecent] = createSignal(loadRecent());
const remember = hex => { const r = [hex, ...recent().filter(c => c !== hex)].slice(0, 8); setRecent(r); try { localStorage.setItem('ledify.recent', JSON.stringify(r)); } catch (e) { /* private mode */ } };

export function ColourLens() {
  const hsvNow = () => rgbHsv(hexRgb(opt.color() || '#ffffff'));
  const pick = (hex, now) => { act.color(hex, now); if (now) remember(hex); };
  return <>
    <p class="lens-tip">Drag around the ring to pick a hue. Tap the middle to switch the light on or off.</p>
    <Group title="Saturation">
      <Range class="sat" label="Saturation" min={0} max={100} value={() => Math.round(hsvNow()[1] * 100)}
        set={v => { const [h] = hsvNow(); opt.setColor(rgbHex(...hsv(h, v / 100, 1))); }}
        send={v => { const [h] = hsvNow(); const hex = rgbHex(...hsv(h, v / 100, 1)); act.color(hex, true); return null; }}
        style={{ '--hue': rgbHex(...hsv(hsvNow()[0], 1, 1)) }} />
    </Group>
    <Group title="Whites">
      <div class="swatches"><For each={WHITES}>{([name, c]) => <button type="button" class="swatch" style={{ background: c }} title={name} aria-label={name + ' white'} aria-pressed={opt.color() === c ? 'true' : 'false'} onClick={() => pick(c, true)}><span>{name}</span></button>}</For></div>
    </Group>
    <Group title="Colours">
      <div class="swatches">
        <For each={SWATCHES}>{c => <button type="button" class="swatch" style={{ background: c }} aria-label={'colour ' + c} aria-pressed={opt.color() === c ? 'true' : 'false'} onClick={() => pick(c, true)} />}</For>
        <label class="swatch picker" title="Any colour"><input type="color" aria-label="Any colour" value={opt.color() || '#ffffff'} onInput={e => pick(e.currentTarget.value)} onChange={e => pick(e.currentTarget.value, true)} /><span>+</span></label>
      </div>
    </Group>
    <Show when={recent().length}>
      <Group title="Recent">
        <div class="swatches small"><For each={recent()}>{c => <button type="button" class="swatch" style={{ background: c }} aria-label={'recent colour ' + c} onClick={() => pick(c, true)} />}</For></div>
      </Group>
    </Show>
  </>;
}

// A tiny live strip for an effect card.
function Preview(p) {
  let cv;
  onMount(() => {
    const n = 28, px = Array.from({ length: n }, () => [0, 0, 0]);
    const off = onTick(t => {
      const g = cv.getContext('2d'), w = cv.width, h = cv.height, x = inputs();
      paint(px, n, t, x, { mode: p.id });
      g.clearRect(0, 0, w, h);
      for (let i = 0; i < n; i++) { const [r, gg, b] = px[i]; g.fillStyle = Math.max(r, gg, b) > 6 ? `rgb(${r | 0},${gg | 0},${b | 0})` : 'rgba(255,255,255,.08)'; g.beginPath(); g.arc((i + 0.5) * w / n, h / 2, h * 0.3, 0, 7); g.fill(); }
    });
    onCleanup(off);
  });
  return <canvas ref={cv} width="224" height="16" class="fx-preview" aria-hidden="true" />;
}

export function EffectsLens() {
  const [auto, setAuto] = optimistic('auto', () => state().auto);
  const [luxLo, setLuxLo] = optimistic('lux_lo', () => state().lux_lo);
  const [luxHi, setLuxHi] = optimistic('lux_hi', () => state().lux_hi);
  const [night, setNight] = optimistic('night', () => state().night);
  const [fadeMs, setFadeMs] = optimistic('fade', () => state().fade);
  const avail = g => EFFECTS.filter(e => e.group === g && (!e.fw || state()[e.fw] !== undefined));
  const card = e => <button type="button" class="fx" aria-pressed={opt.mode() === e.id ? 'true' : 'false'} onClick={() => act.mode(e.id)}>
    <Preview id={e.id} /><b>{e.label}</b><span>{e.desc}</span>
  </button>;
  return <>
    <Group title="Colour"><div class="fx-grid"><For each={avail('colour')}>{card}</For></div></Group>
    <Group title="Sound" aside="from the board's mic, or music on this device (Sound lens)"><div class="fx-grid"><For each={avail('sound')}>{card}</For></div></Group>
    <Group title="On top of any effect">
      <Chips items={[['none', 'nothing'], ['vu', 'VU length'], ['pulse', 'beat pulse']]} value={opt.overlay} onPick={act.overlay} />
    </Group>
    <Group>
      <Toggle label="Auto dim" hint="brightness follows the room's light" checked={() => !!auto()} onChange={on => { setAuto(on ? 1 : 0); send('auto ' + (on ? 'on' : 'off')); }} />
      <Show when={auto()}>
        <Slider label="Dim below" min={0} max={1000} step={10} value={luxLo} set={setLuxLo} send={v => 'lux ' + v + ' ' + luxHi()} fmt={v => v + ' lx'} />
        <Slider label="Full above" min={0} max={1000} step={10} value={luxHi} set={setLuxHi} send={v => 'lux ' + luxLo() + ' ' + v} fmt={v => v + ' lx'} />
        <Slider label="Off below" min={0} max={200} step={5} value={night} set={setNight} send={v => 'night ' + v} fmt={v => v == 0 ? 'never' : v + ' lx'} />
      </Show>
      <Slider label="Transition" min={0} max={2000} step={50} value={fadeMs} set={setFadeMs} send={v => 'fade ' + v} fmt={v => v ? v + ' ms' : 'instant'} />
    </Group>
  </>;
}

const pctOf = e => clamp(Math.round(+e.currentTarget.value || 0), 0, 100);
export function GradientLens() {
  const [from, setFrom] = createSignal(0), [to, setTo] = createSignal(100);
  const lens = () => { const s = opt.stripe(); return !s || s === '0' ? [] : s.split(',').map(x => Math.max(1, +x || 1)); };
  const distinct = () => distinctColours(grad.stops());
  const perColour = () => distinct().map((_, k) => lens()[k % lens().length]);
  const setLens = l => { const str = l.length ? l.join(',') : '0'; opt.setStripe(str); send('stripe ' + str); };
  let bar, nameEl;
  const pctAt = e => { const r = bar.getBoundingClientRect(); return Math.round(clamp((e.clientX - r.left) * 100 / r.width, 0, 100)); };
  const drag = (i, e) => {
    e.stopPropagation(); grad.setSel(i);
    const h = e.currentTarget; h.setPointerCapture(e.pointerId);
    const move = ev => grad.edit(i, { p: pctAt(ev) });
    const up = ev => { grad.edit(i, { p: pctAt(ev) }, true); h.removeEventListener('pointermove', move); h.removeEventListener('pointerup', up); h.removeEventListener('pointercancel', up); };
    h.addEventListener('pointermove', move); h.addEventListener('pointerup', up); h.addEventListener('pointercancel', up);
  };
  const del = async name => { if (await confirmDialog('Delete gradient?', '"' + name + '" will be removed from this browser.', 'Delete')) grad.del(name); };
  const save = () => { grad.save(nameEl.value); nameEl.value = ''; };
  return <>
    <p class="lens-tip">The ring is the strip: drag the dots around it to move colours, tap the ring to add one. The gap at the top is where the strip starts.</p>
    <div ref={bar} class="grad-bar" style={{ background: stopsCss(grad.stops()) }} onPointerDown={e => { if (e.target === bar) grad.addAt(pctAt(e)); }}>
      <Index each={grad.stops()}>{(s, i) => <button type="button" class="grad-handle" classList={{ sel: grad.sel() === i }} style={{ left: s().p + '%', background: s().c }} aria-label={'stop ' + (i + 1) + ' at ' + s().p + '%'} onPointerDown={e => drag(i, e)} onFocus={() => grad.setSel(i)} />}</Index>
    </div>
    <div class="row mt-3">
      <input type="color" class="mini-color" aria-label="selected stop colour" value={grad.cur().c} onInput={e => grad.edit(grad.sel(), { c: e.currentTarget.value })} onChange={e => grad.edit(grad.sel(), { c: e.currentTarget.value }, true)} />
      <input type="number" min="0" max="100" class="field w-[4.5rem]" aria-label="selected stop position" value={grad.cur().p} onChange={e => grad.edit(grad.sel(), { p: pctOf(e) }, true)} /><span class="hint">%</span>
      <span class="grow" />
      <button type="button" class="btn btn-sm" disabled={grad.stops().length < 2} onClick={() => grad.remove(grad.sel())}>Remove</button>
      <button type="button" class="btn btn-sm" onClick={grad.even}>Even</button>
      <button type="button" class="btn btn-sm" onClick={grad.flip}>Flip</button>
    </div>
    <p class="hint mt-1">{grad.stops().length} of {MAX_STOPS} stops</p>
    <Group title="Style">
      <Chips items={[['blend', 'blend'], ['stripes', 'stripes']]} value={() => lens().length ? 'stripes' : 'blend'} onPick={v => setLens(v === 'blend' ? [] : distinct().map(() => 2))} />
      <Show when={lens().length}>
        <div class="mt-2 flex flex-wrap gap-2">
          <For each={distinct()}>{(c, i) => <label class="flex items-center gap-1 text-xs text-muted"><span class="inline-block h-5 w-5 rounded-full border border-line" style={{ background: c }} /><input type="number" min="1" max="100" class="field h-9 w-16" aria-label={'pixels for ' + c} value={perColour()[i()]} onChange={e => { const l = perColour(); l[i()] = clamp(Math.round(+e.currentTarget.value || 1), 1, 100); setLens(l); }} /> px</label>}</For>
        </div>
      </Show>
      <Slider label="Scroll" min={-60} max={60} value={opt.scroll} set={opt.setScroll} send={v => 'scroll ' + v} fmt={v => v > 0 ? '↻ ' + v + ' px/s' : v < 0 ? '↺ ' + (-v) + ' px/s' : 'still'} />
    </Group>
    <Group title="Library" aside="saved in this browser">
      <div class="lib">
        <For each={Object.entries(grad.lib())}>{([name, str]) => <span class="lib-item">
          <button type="button" style={{ background: stopsCss(parseStops(str)) }} onClick={() => grad.place(str, from(), to())}>{name}</button>
          <button type="button" class="lib-x" aria-label={'delete ' + name} onClick={() => del(name)}>✕</button>
        </span>}</For>
      </div>
      <div class="row mt-2 text-sm">
        <span class="hint">place into</span>
        <input type="number" min="0" max="100" class="field h-9 w-16" aria-label="from percent" value={from()} onChange={e => setFrom(pctOf(e))} /><span class="hint">to</span>
        <input type="number" min="0" max="100" class="field h-9 w-16" aria-label="to percent" value={to()} onChange={e => setTo(pctOf(e))} /><span class="hint">% of the strip</span>
      </div>
      <div class="row mt-2"><input ref={nameEl} type="text" class="field grow" placeholder="name this gradient" aria-label="Gradient name" onKeyDown={e => e.key === 'Enter' && save()} /><button type="button" class="btn btn-primary btn-sm" onClick={save}>Save</button></div>
    </Group>
  </>;
}
