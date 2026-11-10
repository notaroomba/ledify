// Matrix: tell the board its strip is a grid (size, wiring, first pixel), then play a video, a picture, a screen or
// tab, the camera or scrolling text on it.
import { For, Show, createSignal, createEffect, onMount, onCleanup } from 'solid-js';
import { state, send, LOCAL } from '../store';
import { Slider, Chips, Toggle, Group } from '../ui';
import { player, size, matrixIndex, SERP, COLS, RIGHT, BOTTOM, MAX_FPS } from '../matrix';

const CORNERS = [[0, 'Top left'], [RIGHT, 'Top right'], [BOTTOM, 'Bottom left'], [RIGHT | BOTTOM, 'Bottom right']];
const CORNER_ARG = { 0: 'tl', [RIGHT]: 'tr', [BOTTOM]: 'bl', [RIGHT | BOTTOM]: 'br' };

// The wiring drawn as the strip's path through the grid, first pixel marked, so the choice can be checked by eye.
function Wiring(p) {
  const pts = () => {
    const w = Math.min(p.w(), 16), h = Math.min(p.h(), 16), at = [];
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) at[matrixIndex(w, h, p.lay(), x, y)] = [x, y];
    return { w, h, at };
  };
  const cell = () => 132 / Math.max(pts().w, pts().h);
  const c = v => (v + 0.5) * cell();
  return <svg class="wiring" viewBox={`0 0 ${pts().w * cell()} ${pts().h * cell()}`} role="img" aria-label="How the strip runs through the matrix">
    <For each={pts().at}>{([x, y]) => <circle cx={c(x)} cy={c(y)} r={Math.max(1.2, cell() * 0.16)} class="wiring-px" />}</For>
    <polyline points={pts().at.map(([x, y]) => c(x) + ',' + c(y)).join(' ')} class="wiring-path" />
    <Show when={pts().at[0]}><circle cx={c(pts().at[0][0])} cy={c(pts().at[0][1])} r={Math.max(2.5, cell() * 0.3)} class="wiring-start" /></Show>
  </svg>;
}

export function MatrixLens() {
  const s = state;
  const [w, setW] = createSignal(16), [h, setH] = createSignal(16), [lay, setLay] = createSignal(SERP);
  // Follow the board until the user edits the form.
  let edited = false;
  createEffect(() => { const st = s(); if (!edited && st.mw > 0 && st.mh > 0) { setW(st.mw); setH(st.mh); setLay(st.mlay ?? SERP); } });
  const edit = f => v => { edited = true; f(v); };
  const saved = () => s().mw === w() && s().mh === h() && s().mlay === lay();
  const apply = () => {
    send(`matrix ${w()} ${h()} ${lay() & SERP ? 'serp' : 'prog'} ${lay() & COLS ? 'cols' : 'rows'} ${CORNER_ARG[lay() & (RIGHT | BOTTOM)]}`);
    edited = false;
    player.test();
  };
  const num = (v, set) => <input type="number" class="field w-20" min="1" max="255" value={v()} onInput={e => { const n = Math.max(1, Math.min(255, +e.currentTarget.value || 1)); edit(set)(n); }} />;
  const tooMany = () => w() * h() > 1000;
  const kbs = () => { const z = size(); return (z.w * z.h * 3 + 4) * player.fps() / 1024; };
  const bs = () => s().stream;

  let host, cv, file;
  onMount(() => player.attach(host, cv));
  onCleanup(() => player.attach(null, null));
  const playing = () => player.source();
  const LABEL = { video: 'Video', image: 'Picture', screen: 'Screen', camera: 'Camera', text: 'Text', test: 'Test pattern' };

  const Layout = () => <>
    <Group title="Layout" aside={saved() ? 'saved on the board' : 'not applied yet'}>
      <div class="row">
        <span class="hint">Size</span>{num(w, setW)}<span class="hint">×</span>{num(h, setH)}<span class="hint">pixels</span>
      </div>
      <Show when={tooMany()}><p class="hint mt-1 text-warn">The board drives up to 1000 pixels.</p></Show>
      <div class="matrix-setup mt-3">
        <Wiring w={w} h={h} lay={lay} />
        <div class="min-w-0 grow">
          <div class="hint mb-1">First pixel</div>
          <Chips chip="chip-sm" items={CORNERS} value={() => lay() & (RIGHT | BOTTOM)} onPick={v => edit(setLay)((lay() & (SERP | COLS)) | v)} />
          <div class="hint mb-1 mt-3">The strip runs</div>
          <Chips chip="chip-sm" items={[[0, 'Along rows'], [COLS, 'Down columns']]} value={() => lay() & COLS} onPick={v => edit(setLay)((lay() & ~COLS) | v)} />
        </div>
      </div>
      <Toggle label="Zig-zag" hint="every other row runs back the other way (most panels); off if every row starts at the same side" checked={() => !!(lay() & SERP)} onChange={on => edit(setLay)(on ? lay() | SERP : lay() & ~SERP)} />
      <div class="row mt-2">
        <button type="button" class="btn btn-primary grow" disabled={tooMany()} onClick={apply}>Apply and show test pattern</button>
        <Show when={s().mw > 0}><button type="button" class="btn" onClick={() => { send('matrix off'); edited = false; }}>Not a matrix</button></Show>
      </div>
      <p class="hint mt-2">The test pattern has red along the top, green down the left side, white in the top left corner and blue on the diagonal. If it looks turned or mirrored, change the first pixel or the direction.</p>
    </Group>
  </>;

  return <>
    <Show when={!s().mw && s().mw !== undefined}>
      <p class="empty">The strip isn't set up as a matrix yet: pictures go along it as one row. Set the size and wiring below.</p>
    </Show>
    <Show when={s().mw === undefined && s().fw}>
      <p class="empty">This board's firmware doesn't know about matrices yet. Update it first.</p>
    </Show>
    <Show when={!(s().mw > 0)}><Layout /></Show>

    <Group title="Now playing" aside={playing() ? player.sent() + ' fps sent' + (player.skipped() ? ' · ' + player.skipped() + ' held back' : '') + (bs() ? ' · board shows ' + bs().fps : '') : 'nothing'}>
      <div class="matrix-view" style={{ 'aspect-ratio': size().w + ' / ' + size().h }}>
        <canvas ref={cv} class="matrix-canvas" aria-label="What the matrix is sent" />
        <Show when={!playing()}><span class="matrix-idle">{size().w} × {size().h}</span></Show>
      </div>
      <div ref={host} classList={{ hidden: playing() !== 'video' }} />
      <div class="row mt-3">
        <label class="btn grow cursor-pointer"><input ref={file} type="file" accept="video/*,image/*" class="sr-only" aria-label="Video or picture file" onChange={() => { player.playFile(file.files[0]); file.value = ''; }} />Video or picture</label>
        <Show when={navigator.mediaDevices && navigator.mediaDevices.getDisplayMedia}><button type="button" class="btn grow" onClick={player.screen}>Screen or tab</button></Show>
        <button type="button" class="btn grow" onClick={player.camera}>Camera</button>
        <button type="button" class="btn grow" onClick={player.showText}>Text</button>
        <Show when={playing()}><button type="button" class="btn btn-primary" onClick={player.stop}>Stop {LABEL[playing()].toLowerCase()}</button></Show>
      </div>
      <Show when={playing() === 'text'}>
        <div class="row mt-2">
          <input type="text" class="field grow" value={player.text()} onInput={e => player.setText(e.currentTarget.value)} aria-label="Text to scroll" />
          <input type="color" class="field w-14 p-1" value={player.ink()} onInput={e => player.setInk(e.currentTarget.value)} aria-label="Text colour" />
        </div>
      </Show>
    </Group>

    <Group title="Picture">
      <Slider label="Brightness" min={5} max={100} value={player.gain} set={player.setGain} fmt={v => v + '%'} />
      <Slider label="Frame rate" min={1} max={MAX_FPS} value={player.fps} set={player.setFps} fmt={v => v + ' fps'} />
      <Chips items={[['cover', 'Fill'], ['contain', 'Whole picture'], ['stretch', 'Stretch']]} value={player.fit} onPick={player.setFit} class="mt-2" />
      <Toggle label="Mirror" hint="flip left and right, like a selfie camera" checked={player.mirror} onChange={player.setMirror} />
      <Toggle label="Natural colours" hint="LEDs are linear, video isn't: this curve stops it looking washed out" checked={player.gamma} onChange={player.setGamma} />
      <p class="hint mt-2">{LOCAL
        ? `On the board's own page frames go straight over your network: 30 fps works right up to the 1000 pixels the board can drive.`
        : `Frames go through the broker here (about ${kbs().toFixed(0)} KB/s): expect around 10 to 15 fps with some lag. For smooth video open the board's own page on the same network.`}
        {' '}The board's brightness and power limit still apply. Keep this tab in front: a hidden tab slows to a frame a second, and 2.5 s after the last frame the board goes back to its effect.</p>
    </Group>

    <Show when={s().mw > 0}><Layout /></Show>
  </>;
}
