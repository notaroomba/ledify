import { opt } from './store';
import { act } from './actions';
// Brightness for the lenses that take the ring over (Colour, Gradient, Zones), where dragging the ring does something else.
export function Brightness() {
  return <div class="slider bright-row"><label>Brightness</label>
    <input type="range" class="range" min="0" max="255" value={opt.level() ?? 0} aria-label="Brightness"
      style={{ '--p': ((opt.level() ?? 0) * 100 / 255) + '%' }} onInput={e => act.level(+e.currentTarget.value)} onChange={e => act.level(+e.currentTarget.value, true)} />
    <output>{Math.round((opt.level() ?? 0) * 100 / 255)} %</output></div>;
}
