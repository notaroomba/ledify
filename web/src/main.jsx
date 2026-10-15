import { render } from 'solid-js/web';
import { For, Show, createSignal, createEffect, createMemo, onMount } from 'solid-js';
import './index.css';
import { ui, setUi, state, init, selectDevice, useOverride, useBuiltIn, savedCreds, BUILT_IN, BOARD_MQTT, LOCAL, DEMO } from './store';
import { Confirm } from './ui';
import { Hero, Colour, Effect, Gradient, Sound, Strip, Sensors, Spotify, Presets, Advanced, Segments, Schedule } from './panels';
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

// First setup over Bluetooth (Chrome and Edge on a computer or Android): the board advertises a Nordic UART
// service; the Wi-Fi and broker settings go up as the same commands the USB console takes.
const NUS = '6e400001-b5a3-f393-e0a9-e50e24dcca9e', NUS_RX = '6e400002-b5a3-f393-e0a9-e50e24dcca9e', NUS_TX = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';
export const [setupOpen, setSetupOpen] = createSignal(false);
function Setup() {
  let d;
  const f = {};
  const [out, setOut] = createSignal('');
  createEffect(() => { if (setupOpen()) { if (!d.open) d.showModal(); } else if (d.open) d.close(); });
  const go = async () => {
    if (!navigator.bluetooth) { setOut('This browser has no Web Bluetooth. Use Chrome or Edge on a computer or Android phone.'); return; }
    const cmds = [];
    const v = k => f[k].value.trim();
    if (v('ssid')) cmds.push('wifi ' + v('ssid') + ' ' + f.pass.value);
    if (v('ssid2')) cmds.push('wifi2 ' + v('ssid2') + ' ' + f.pass2.value);
    if (v('url')) cmds.push('mqtt ' + v('url') + (v('user') ? ' ' + v('user') + ' ' + f.bpass.value : ''));
    if (!cmds.length) { setOut('Fill in a network or a broker first.'); return; }
    if ([v('ssid'), v('ssid2'), f.pass.value, f.pass2.value].some(x => /\s/.test(x))) { setOut('Names and passwords cannot contain spaces (the board reads them as words).'); return; }
    try {
      setOut('Pick the board in the Bluetooth chooser…\n');
      const dev = await navigator.bluetooth.requestDevice({ filters: [{ services: [NUS] }] });
      const srv = await (await dev.gatt.connect()).getPrimaryService(NUS);
      const rx = await srv.getCharacteristic(NUS_RX), tx = await srv.getCharacteristic(NUS_TX);
      await tx.startNotifications();
      tx.addEventListener('characteristicvaluechanged', e => setOut(o => o + '< ' + new TextDecoder().decode(e.target.value) + '\n'));
      setOut(o => o + 'Connected to ' + (dev.name || 'the board') + '\n');
      for (const c of cmds) {
        await rx.writeValueWithResponse(new TextEncoder().encode(c));
        setOut(o => o + '> ' + c.replace(/ \S+$/, ' ••••') + '\n');
        await new Promise(r => setTimeout(r, 400));
      }
      setOut(o => o + 'Saved. The board restarts in a couple of seconds and should come online.\n');
    } catch (e) { setOut(o => o + 'Bluetooth: ' + e.message + '\n'); }
  };
  return <dialog ref={d} class="dialog" onClose={() => setSetupOpen(false)}>
    <h3>Set up a board over Bluetooth</h3>
    <p class="hint">Power the board, then send it the Wi-Fi it should join and the broker it should talk to. Only what you fill in is sent.</p>
    <div class="mt-3 grid gap-2">
      <div class="grid grid-cols-2 gap-2"><input ref={el => f.ssid = el} class="field" placeholder="Wi-Fi name" aria-label="Wi-Fi name" /><input ref={el => f.pass = el} type="password" class="field" placeholder="Wi-Fi password" aria-label="Wi-Fi password" /></div>
      <div class="grid grid-cols-2 gap-2"><input ref={el => f.ssid2 = el} class="field" placeholder="fallback Wi-Fi (optional)" aria-label="Fallback Wi-Fi name" /><input ref={el => f.pass2 = el} type="password" class="field" placeholder="its password" aria-label="Fallback Wi-Fi password" /></div>
      <input ref={el => f.url = el} class="field" placeholder="broker, e.g. mqtt://host:1883" aria-label="Broker URL" value={BOARD_MQTT} />
      <div class="grid grid-cols-2 gap-2"><input ref={el => f.user = el} class="field" placeholder="broker user" aria-label="Broker user" value="ledify" /><input ref={el => f.bpass = el} type="password" class="field" placeholder="the board's broker password" aria-label="Broker password" /></div>
    </div>
    <pre class="log mt-3 min-h-10">{out()}</pre>
    <div class="mt-3 flex justify-end gap-2"><button type="button" class="btn" onClick={() => setSetupOpen(false)}>Close</button><button type="button" class="btn btn-primary" onClick={go}>Connect and send</button></div>
  </dialog>;
}

function App() {
  onMount(() => {
    init();
    sp.init();
    if ('serviceWorker' in navigator && !LOCAL && !DEMO && location.protocol === 'https:') navigator.serviceWorker.register('/sw.js').catch(() => {});
  });
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
        <span class="hidden text-xs text-muted sm:inline">{state().fw ? 'fw ' + state().fw : ''}{LOCAL ? ' · local' : DEMO ? ' · demo' : ''}</span>
        <button type="button" class="btn btn-sm" onClick={() => setSetupOpen(true)}>Setup</button>
        <Show when={!LOCAL && !DEMO}><button type="button" class="btn btn-sm" onClick={() => setUi('brokerOpen', true)}>Broker</button></Show>
      </div>
      <Show when={DEMO}><div class="bg-accent/15 py-1.5 text-center text-sm text-accent">Demo: a pretend board. Everything here is make-believe.</div></Show>
      <Show when={off()}><div class="bg-warn/15 py-1.5 text-center text-sm text-warn">The board is offline: unplugged, or without Wi-Fi. Controls will apply when it is back.</div></Show>
    </header>
    <main class="mx-auto grid max-w-5xl gap-4 px-4 py-4 pb-24 md:grid-cols-2" classList={{ offline: off() }}>
      <Hero /><Colour /><Effect /><Gradient /><Sound /><Strip /><Segments /><Schedule /><Sensors /><Spotify /><Presets /><Advanced />
    </main>
    <div class="pointer-events-none fixed inset-x-0 bottom-5 z-30 flex flex-col items-center gap-2 px-4"><For each={ui.toasts}>{t => <div class={{ warn: 'toast toast-warn', ok: 'toast toast-ok' }[t.kind] || 'toast toast-info'}>{t.text}</div>}</For></div>
    <Confirm />
    <Broker />
    <Setup />
  </>;
}
render(() => <App />, document.getElementById('root'));
