// The ring: a live picture of the strip, and the main control. What dragging around it does depends on the lens that
// is open: brightness by default, hue in Colour, gradient stops in Gradient, zone edges in Zones. The middle is the
// power button; swipe across it to step through the effects.
import { For, Index, Show, createSignal, createMemo, onMount, onCleanup } from 'solid-js';
import { state, opt, send } from './store';
import { act } from './actions';
import { grad } from './grad';
import { paint, inputs, shown } from './engine';
import { SOURCE, effectOf } from './effects';
import { hsv, rgbHsv, hexRgb, rgbHex, accentOf, clamp } from './color';

const C = 100, R = 78, R_LEVEL = 61, R_ZONE = 91, GAP = 16;          // viewBox 0..200
const LV_FROM = 135, LV_SWEEP = 270;                                  // the brightness arc: bottom left, over the top, bottom right
const rad = d => d * Math.PI / 180;
const at = (deg, r) => [C + r * Math.cos(rad(deg)), C + r * Math.sin(rad(deg))];
// Strip position (0..1) to angle: clockwise from the top, with a small gap there marking where the strip starts and ends.
export const angOf = f => -90 + GAP / 2 + f * (360 - GAP);
const fOfAng = a => clamp(((((a + 90 - GAP / 2) % 360) + 360) % 360) / (360 - GAP), 0, 1);
const arc = (r, a0, a1) => { const [x0, y0] = at(a0, r), [x1, y1] = at(a1, r); return `M${x0} ${y0}A${r} ${r} 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${x1} ${y1}`; };

export function Ring(p) {
  let wrap, svg, crisp, glow;
  const lens = () => p.lens();
  const ringMode = () => ({ colour: 'hue', gradient: 'gradient', zones: 'zones' }[lens()] || 'level');
  const [size, setSize] = createSignal(300);
  const [drag, setDrag] = createSignal(null);       // zone edge being dragged: { i, end, px }

  // Pointer position -> [radius in viewBox units, angle in degrees].
  const polar = e => {
    const r = svg.getBoundingClientRect(), x = (e.clientX - r.left) * 200 / r.width - C, y = (e.clientY - r.top) * 200 / r.height - C;
    return [Math.hypot(x, y), Math.atan2(y, x) * 180 / Math.PI];
  };
  const levelOfAng = (a, prev) => {
    let d = ((a - LV_FROM) % 360 + 360) % 360;
    if (d > LV_SWEEP) d = prev > 127 ? LV_SWEEP : 0;                  // in the dead zone at the bottom: stay at the nearer end
    return Math.round(d * 255 / LV_SWEEP);
  };
  const hueOf = () => rgbHsv(hexRgb(opt.color() || '#ffffff'));
  const setHue = (a, now) => {
    const [, s] = hueOf(), h = (((a + 90) % 360) + 360) % 360 / 360;
    act.color(rgbHex(...hsv(h, s < 0.05 ? 1 : s, 1)), now);
  };

  const down = e => {
    if (e.button > 0) return;
    const [r, a] = polar(e);
    if (r < 46 || r > 104) return;
    const m = ringMode();
    if (m === 'gradient') { if (r > 66 && r < 92) grad.addAt(fOfAng(a) * 100); return; }
    if (m === 'zones') return;
    e.preventDefault();
    svg.setPointerCapture(e.pointerId);
    const apply = (ev, now) => { const [, an] = polar(ev); m === 'hue' ? setHue(an, now) : act.level(levelOfAng(an, opt.level() ?? 0), now); };
    apply(e);
    const move = ev => apply(ev), up = ev => { apply(ev, true); svg.removeEventListener('pointermove', move); svg.removeEventListener('pointerup', up); svg.removeEventListener('pointercancel', up); };
    svg.addEventListener('pointermove', move); svg.addEventListener('pointerup', up); svg.addEventListener('pointercancel', up);
  };
  const dialKeys = e => {
    const big = e.shiftKey || e.key === 'PageUp' || e.key === 'PageDown';
    const d = { ArrowRight: 1, ArrowUp: 1, PageUp: 1, ArrowLeft: -1, ArrowDown: -1, PageDown: -1 }[e.key];
    if (e.key === 'Home') { e.preventDefault(); act.level(0, true); }
    else if (e.key === 'End') { e.preventDefault(); act.level(255, true); }
    else if (d) { e.preventDefault(); act.level((opt.level() ?? 0) + d * (big ? 26 : 5), true); }
  };

  // Gradient handles: drag around the ring to move a stop; Delete removes the selected one.
  const stopDown = (i, e) => {
    e.stopPropagation(); e.preventDefault(); grad.setSel(i);
    const h = e.currentTarget; h.setPointerCapture(e.pointerId);
    const move = ev => grad.edit(i, { p: Math.round(fOfAng(polar(ev)[1]) * 100) });
    const up = ev => { grad.edit(i, { p: Math.round(fOfAng(polar(ev)[1]) * 100) }, true); h.removeEventListener('pointermove', move); h.removeEventListener('pointerup', up); h.removeEventListener('pointercancel', up); };
    h.addEventListener('pointermove', move); h.addEventListener('pointerup', up); h.addEventListener('pointercancel', up);
  };
  const stopKeys = (i, e) => {
    const d = { ArrowLeft: -1, ArrowDown: -1, ArrowRight: 1, ArrowUp: 1 }[e.key];
    if (d) { e.preventDefault(); grad.edit(i, { p: clamp(grad.stops()[i].p + d * (e.shiftKey ? 10 : 1), 0, 100) }); }
    else if (e.key === 'Delete' || e.key === 'Backspace') { e.preventDefault(); grad.remove(i); }
  };

  // Zone edges: drag an end of an arc; the board gets the new range on release.
  const count = () => state().count || 300;
  const segs = createMemo(() => (state().segs || []).map(s => drag() && drag().i === s.i ? { ...s, [drag().end]: drag().px } : s));
  const edgeDown = (s, end, e) => {
    e.stopPropagation(); e.preventDefault();
    const h = e.currentTarget; h.setPointerCapture(e.pointerId);
    const pxAt = ev => { const v = Math.round(fOfAng(polar(ev)[1]) * count()); return end === 'a' ? clamp(v, 0, s.b - 1) : clamp(v, s.a + 1, count()); };
    const move = ev => setDrag({ i: s.i, end, px: pxAt(ev) });
    const up = ev => {
      const v = pxAt(ev), a = end === 'a' ? v : s.a, b = end === 'b' ? v : s.b;
      send('seg ' + s.i + ' ' + a + ' ' + b + ' ' + s.color + ' ' + s.rgb);
      setTimeout(() => setDrag(null), 600);
      h.removeEventListener('pointermove', move); h.removeEventListener('pointerup', up); h.removeEventListener('pointercancel', up);
    };
    h.addEventListener('pointermove', move); h.addEventListener('pointerup', up); h.addEventListener('pointercancel', up);
  };

  // Power in the middle: a tap toggles it, a horizontal swipe steps the effect.
  let sx = null;
  const centreDown = e => { sx = e.clientX; e.currentTarget.setPointerCapture(e.pointerId); };
  const centreUp = e => {
    if (sx === null) return;
    const dx = e.clientX - sx; sx = null;
    if (Math.abs(dx) > 36) act.stepMode(dx < 0 ? 1 : -1); else act.togglePower();
  };

  // Drawing: two canvases, a crisp one for the pixels and a blurred one under it for the glow.
  onMount(() => {
    const ro = new ResizeObserver(([en]) => setSize(Math.max(120, Math.min(720, en.contentRect.width, en.contentRect.height * 0.97))));
    ro.observe(wrap);
    let raf, frame = 0, px = [], t0 = performance.now();
    const root = document.documentElement.style;
    const draw = () => {
      raf = requestAnimationFrame(draw);
      if (document.hidden) return;
      const S = size(), dpr = Math.min(2, window.devicePixelRatio || 1), W = Math.round(S * dpr);
      if (crisp.width !== W) { crisp.width = crisp.height = W; glow.width = glow.height = Math.round(W / 2); }
      const n = clamp(count(), 24, 144), t = (performance.now() - t0) / 1000;
      while (px.length < n) px.push([0, 0, 0]);
      if (px.length > n) px.length = n;
      const x = inputs(), m = ringMode();
      if (m === 'hue') px.forEach((q, i) => q.splice(0, 3, ...hsv((((angOf(i / n) + 90) % 360) + 360) % 360 / 360)));
      else if (m === 'gradient') SOURCE.palette(px, n, 0, { ...x, scroll: 0 });
      else paint(px, n, t, x);
      const lv = m === 'level' || m === 'zones' ? shown() : 1;
      const k = W / 200, cg = crisp.getContext('2d'), gg = glow.getContext('2d');
      cg.clearRect(0, 0, W, W); gg.clearRect(0, 0, glow.width, glow.height);
      const dot = Math.min(Math.PI * 2 * R / n * 0.36, 3.6) * k;
      let ar = 0, ag = 0, ab = 0;
      for (let i = 0; i < n; i++) {
        const [r, g, b] = px[i], [x0, y0] = at(angOf((i + 0.5) / n), R);
        const cr = r * lv, cgn = g * lv, cb = b * lv;
        ar += cr; ag += cgn; ab += cb;
        const lit = Math.max(cr, cgn, cb) > 6;
        cg.fillStyle = lit ? `rgb(${cr | 0},${cgn | 0},${cb | 0})` : 'rgba(255,255,255,.07)';
        cg.beginPath(); cg.arc(x0 * k, y0 * k, dot, 0, 7); cg.fill();
        if (lit) { gg.fillStyle = cg.fillStyle; gg.beginPath(); gg.arc(x0 * k / 2, y0 * k / 2, dot * 1.6, 0, 7); gg.fill(); }
      }
      if (++frame % 8 === 0) {
        const avg = [ar / n, ag / n, ab / n], mx = Math.max(...avg);
        root.setProperty('--led', avg.map(v => v | 0).join(' '));
        root.setProperty('--led-on', String(Math.min(1, mx / 160)));
        root.setProperty('--accent', accentOf(mx > 4 ? avg : hexRgb(opt.color() || '#58c6ff')).map(v => v | 0).join(' '));
      }
    };
    draw();
    onCleanup(() => { cancelAnimationFrame(raf); ro.disconnect(); });
  });

  const lvAng = () => LV_FROM + (opt.level() ?? 0) * LV_SWEEP / 255;
  const hueAng = () => hueOf()[0] * 360 - 90;
  const pct = () => Math.round((opt.level() ?? 0) * 100 / 255);
  const eff = () => effectOf(opt.mode());
  const label = () => {
    const s = state();
    if (!opt.power()) return 'off';
    if (s.sleep > 0) return 'fading out';
    if (s.rise > 0) return 'sunrise';
    return eff() ? eff().label : opt.mode() || '';
  };

  return <div ref={wrap} class="orb-wrap">
    <div class="orb" style={{ width: size() + 'px', height: size() + 'px' }}>
      <canvas ref={glow} class="orb-glow" aria-hidden="true" />
      <canvas ref={crisp} class="orb-px" aria-hidden="true" />
      <svg ref={svg} viewBox="0 0 200 200" class="orb-svg" classList={{ grabbable: ringMode() === 'level' || ringMode() === 'hue', addable: ringMode() === 'gradient' }} onPointerDown={down}>
        <path d={arc(R, -90 - GAP / 2 + 1, -90 + GAP / 2 - 1)} class="orb-gap" />
        <Show when={ringMode() === 'level'}>
          <path d={arc(R_LEVEL, LV_FROM, LV_FROM + LV_SWEEP)} class="lv-track" />
          <Show when={(opt.level() ?? 0) > 0}><path d={arc(R_LEVEL, LV_FROM, lvAng())} class="lv-fill" /></Show>
          <circle cx={at(lvAng(), R_LEVEL)[0]} cy={at(lvAng(), R_LEVEL)[1]} r="6.5" class="knob" tabindex="0" role="slider" aria-label="Brightness"
            aria-valuemin="0" aria-valuemax="255" aria-valuenow={opt.level() ?? 0} aria-valuetext={pct() + ' %'} onKeyDown={dialKeys} />
        </Show>
        <Show when={ringMode() === 'hue'}>
          <circle cx={at(hueAng(), R)[0]} cy={at(hueAng(), R)[1]} r="9" class="knob" style={{ fill: opt.color() || '#fff' }} />
        </Show>
        <Show when={ringMode() === 'gradient'}>
          <Index each={grad.stops()}>{(s, i) => {
            const pos = () => at(angOf(s().p / 100), R);
            return <g class="stop" classList={{ sel: grad.sel() === i }} tabindex="0" role="slider" aria-label={'gradient stop ' + (i + 1)} aria-valuemin="0" aria-valuemax="100" aria-valuenow={s().p}
              onPointerDown={e => stopDown(i, e)} onKeyDown={e => stopKeys(i, e)} onFocus={() => grad.setSel(i)}>
              <circle cx={pos()[0]} cy={pos()[1]} r="11" class="stop-hit" />
              <circle cx={pos()[0]} cy={pos()[1]} r="6" style={{ fill: s().c }} class="stop-dot" />
            </g>;
          }}</Index>
        </Show>
        <Show when={ringMode() === 'zones'}>
          <For each={segs()}>{s => {
            const a0 = () => angOf(s.a / count()), a1 = () => angOf(s.b / count());
            return <g class="zone">
              <path d={arc(R_ZONE, a0(), Math.max(a0() + 0.5, a1()))} class="zone-arc" style={{ stroke: s.color === 'solid' ? '#' + s.rgb : 'rgb(var(--accent))' }} />
              <text x={at((a0() + a1()) / 2, R_ZONE + 0.5)[0]} y={at((a0() + a1()) / 2, R_ZONE + 0.5)[1]} class="zone-num">{s.i}</text>
              <circle cx={at(a0(), R_ZONE)[0]} cy={at(a0(), R_ZONE)[1]} r="5" class="zone-edge" onPointerDown={e => edgeDown(s, 'a', e)} />
              <circle cx={at(a1(), R_ZONE)[0]} cy={at(a1(), R_ZONE)[1]} r="5" class="zone-edge" onPointerDown={e => edgeDown(s, 'b', e)} />
            </g>;
          }}</For>
        </Show>
      </svg>
      <button type="button" class="orb-core" classList={{ on: !!opt.power() }} aria-pressed={opt.power() ? 'true' : 'false'} aria-label="Power (swipe sideways to change effect)"
        onPointerDown={centreDown} onPointerUp={centreUp} onPointerCancel={() => { sx = null; }} onKeyDown={e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); act.togglePower(); } }}>
        <Show when={ringMode() === 'hue'} fallback={<>
          <span class="core-num">{opt.power() ? pct() : ''}<Show when={opt.power()}><small>%</small></Show>
            <Show when={!opt.power()}><svg width="34" height="34" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><path d="M12 3v9" /><path d="M6.3 6.3a8 8 0 1 0 11.4 0" /></svg></Show>
          </span>
          <span class="core-label">{label()}</span>
        </>}>
          <span class="core-swatch" style={{ background: opt.color() || '#fff' }} />
          <span class="core-label">{(opt.color() || '').toUpperCase()}</span>
        </Show>
      </button>
    </div>
  </div>;
}
