import { render } from 'solid-js/web';
import { For, Show, createSignal, createEffect, createMemo, onMount } from 'solid-js';
import './index.css';
import { ui, setUi, state, init, selectDevice, useOverride, useBuiltIn, savedCreds, BUILT_IN } from './store';
import { Confirm } from './ui';
import { Hero, Colour, Effect, Sound, Strip, Sensors, Spotify, Presets, Advanced } from './panels';
import { sp } from './spotify';

function Broker() {
  let d;
  const saved = (localStorage.getItem('ledify.override') && savedCreds()) || BUILT_IN;
  const [url, setUrl] = createSignal(saved.url), [user, setUser] = createSignal(saved.user), [pass, setPass] = createSignal(saved === BUILT_IN ? '' : saved.pass);
  createEffect(() => { if (ui.brokerOpen) { if (!d.open) d.showModal(); } else if (d.open) d.close(); });
  return <dialog ref={d} class="dialog" onClose={() => setUi('brokerOpen', false)}>
    <h3>Broker</h3>
    <p class="hint">The site already knows its own broker. Fill this in only to use a different one.</p>
    <div class="mt-3 grid gap-2">
      <input type="url" class="field" placeholder="wss://host" aria-label="Broker URL" value={url()} onInput={e => setUrl(e.currentTarget.value)} />
      <input type="text" class="field" placeholder="username" aria-label="Username" value={user()} onInput={e => setUser(e.currentTarget.value)} />
      <input type="password" class="field" placeholder="password" autocomplete="current-password" aria-label="Password" value={pass()} onInput={e => setPass(e.currentTarget.value)} />
    </div>
    <p class="hint mt-2 min-h-5">{ui.brokerMsg}</p>
    <div class="mt-3 flex flex-wrap justify-end gap-2">
      <button type="button" class="btn" onClick={useBuiltIn}>Use the site's broker</button>
      <button type="button" class="btn btn-primary" onClick={() => useOverride({ url: url().trim(), user: user().trim(), pass: pass() })}>Connect</button>
    </div>
  </dialog>;
}

function App() {
  onMount(() => { init(); sp.init(); });
  const ids = createMemo(() => Object.keys(ui.devices).sort());
  const off = () => ui.conn === 'offline';
  const dot = () => ui.conn === 'online' ? 'on' : ui.conn === 'connecting' ? '' : 'off';
  return <>
    <header class="sticky top-0 z-20 border-b border-line bg-ink/85 backdrop-blur">
      <div class="mx-auto flex max-w-5xl items-center gap-3 px-4 py-3">
        <span class="text-sm font-bold tracking-[.22em]">LEDIFY</span>
        <Show when={ids().length > 1}>
          <select class="field h-9 py-0 text-xs" aria-label="Board" value={ui.current} onChange={e => selectDevice(e.currentTarget.value)}><For each={ids()}>{id => <option value={id}>{id}</option>}</For></select>
        </Show>
        <span class="flex items-center gap-2 text-sm text-muted"><span class={'dot ' + dot()} /><span title={ui.current || ''}>{ui.conn}</span></span>
        <span class="grow" />
        <span class="hidden text-xs text-muted sm:inline">{state().fw ? 'fw ' + state().fw : ''}</span>
        <button type="button" class="btn btn-sm" onClick={() => setUi('brokerOpen', true)}>Broker</button>
      </div>
      <Show when={off()}><div class="bg-warn/15 py-1.5 text-center text-sm text-warn">The board is offline: unplugged, or without Wi-Fi. Controls will apply when it is back.</div></Show>
    </header>
    <main class="mx-auto grid max-w-5xl gap-4 px-4 py-4 pb-24 md:grid-cols-2" classList={{ offline: off() }}>
      <Hero /><Colour /><Effect /><Sound /><Strip /><Sensors /><Spotify /><Presets /><Advanced />
    </main>
    <div class="pointer-events-none fixed inset-x-0 bottom-5 z-30 flex flex-col items-center gap-2 px-4"><For each={ui.toasts}>{t => <div class={'toast toast-' + t.kind}>{t.text}</div>}</For></div>
    <Confirm />
    <Broker />
  </>;
}
render(() => <App />, document.getElementById('root'));
