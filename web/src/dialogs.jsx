// The dialogs: broker override and first setup over Bluetooth.
import { createSignal, createEffect } from 'solid-js';
import { ui, setUi, useOverride, useBuiltIn, savedCreds, BUILT_IN, BOARD_MQTT } from './store';

export function Broker() {
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
export function Setup() {
  let d;
  const f = {};
  const [out, setOut] = createSignal('');
  createEffect(() => { if (setupOpen()) { if (!d.open) d.showModal(); } else if (d.open) d.close(); });
  const go = async () => {
    if (!navigator.bluetooth) { setOut('This browser has no Web Bluetooth. Use Chrome or Edge on a computer or Android phone.'); return; }
    const cmds = [];
    const v = k => f[k].value.trim();
    if (v('ssid') && !f.pass.value) { setOut('The main network needs its password. Put an open network in the fallback row instead.'); return; }
    if (v('ssid')) cmds.push('wifi ' + v('ssid') + ' ' + f.pass.value);
    if (v('ssid2')) cmds.push('wifi2 ' + v('ssid2') + ' ' + (f.pass2.value || '-'));     // "-" adds an open network
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
        setOut(o => o + '> ' + (c.endsWith(' -') ? c : c.replace(/ \S+$/, ' ••••')) + '\n');
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
      <div class="grid grid-cols-2 gap-2"><input ref={el => f.ssid2 = el} class="field" placeholder="fallback Wi-Fi (optional)" aria-label="Fallback Wi-Fi name" /><input ref={el => f.pass2 = el} type="password" class="field" placeholder="its password (empty if open)" aria-label="Fallback Wi-Fi password" /></div>
      <input ref={el => f.url = el} class="field" placeholder="broker, e.g. mqtt://host:1883" aria-label="Broker URL" value={BOARD_MQTT} />
      <div class="grid grid-cols-2 gap-2"><input ref={el => f.user = el} class="field" placeholder="broker user" aria-label="Broker user" value="ledify" /><input ref={el => f.bpass = el} type="password" class="field" placeholder="the board's broker password" aria-label="Broker password" /></div>
    </div>
    <pre class="log mt-3 min-h-10">{out()}</pre>
    <div class="mt-3 flex justify-end gap-2"><button type="button" class="btn" onClick={() => setSetupOpen(false)}>Close</button><button type="button" class="btn btn-primary" onClick={go}>Connect and send</button></div>
  </dialog>;
}

