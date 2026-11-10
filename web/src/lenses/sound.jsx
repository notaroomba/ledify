// Sound: what the board hears, how it reacts, and music or a mic from this device instead.
import { For, Show } from 'solid-js';
import { ui, state, optimistic } from '../store';
import { Slider, Group } from '../ui';
import { feed } from '../audio';

export function SoundLens() {
  const [gain, setGain] = optimistic('gain', () => state().gain);
  const [react, setReact] = optimistic('react', () => state().react);
  const pct = v => ((v || 0) * 100 / 255) + '%';
  const spec = () => ui.live.spec && ui.live.spec.length ? ui.live.spec : null;
  let file;
  return <>
    <div class="meters">
      <Show when={spec()} fallback={<>
        <For each={[['bass', 0, 'var(--c-bass)'], ['mid', 1, 'var(--c-mid)'], ['treble', 2, 'var(--c-treble)']]}>{([name, k, c]) =>
          <div class="meter"><i style={{ height: pct(ui.live.bands[k]), background: c }} /><span>{name}</span></div>}</For>
      </>}>
        <div class="spec"><For each={spec()}>{(v, i) => <i style={{ height: pct(v), background: `hsl(${i() * 22} 90% 60%)` }} />}</For></div>
      </Show>
      <div class="meter loud"><i style={{ height: pct(ui.live.loud) }} /><span>loud</span></div>
      <div class="beat" classList={{ hit: ui.live.pulse > 128 }} aria-label="beat" />
    </div>
    <p class="hint text-center">{feed.active() ? 'reacting to this device' : "reacting to the board's mic"}{ui.live.db != null ? ' · ' + Math.round(ui.live.db) + ' dB' : ''}{ui.live.lat ? ' · ' + ui.live.lat + ' ms to the LEDs' : ''}</p>
    <Group title="Reaction">
      <Slider label="Mic gain" min={1} max={100} value={gain} set={setGain} send={v => 'gain ' + v} />
      <Show when={state().react !== undefined}>
        <Slider label="Snappiness" min={1} max={100} value={react} set={setReact} send={v => 'react ' + v} />
      </Show>
    </Group>
    <Group title="Music from this device" aside="sent to the board 20 times a second">
      <div class="row">
        <label class="btn grow cursor-pointer"><input ref={file} type="file" accept="audio/*" class="sr-only" aria-label="Audio file" onChange={() => feed.playFile(file.files[0])} />Play a file</label>
        <button type="button" class="btn grow" onClick={feed.mic}>Use this mic</button>
        <Show when={feed.active()}><button type="button" class="btn" onClick={feed.stop}>Stop</button></Show>
      </div>
      <Slider label="Sensitivity" min={1} max={10} value={feed.sens} set={feed.setSens} />
      <audio ref={el => feed.attach(el)} controls class="mt-2 w-full" classList={{ hidden: !feed.active() }} />
    </Group>
  </>;
}
