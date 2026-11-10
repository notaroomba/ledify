// The dialogs: broker override and first setup over Bluetooth.
import { createSignal, createEffect } from 'solid-js';
import { Show } from 'solid-js';
import { ui, setUi, useOverride, useBuiltIn, savedCreds, BUILT_IN, BOARD_MQTT, AUTH, api, authConnect } from './store';

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
    if (!AUTH && v('url')) cmds.push('mqtt ' + v('url') + (v('user') ? ' ' + v('user') + ' ' + f.bpass.value : ''));
    if (!cmds.length && !AUTH) { setOut('Fill in a network or a broker first.'); return; }
    if ([v('ssid'), v('ssid2'), f.pass.value, f.pass2.value].some(x => /\s/.test(x))) { setOut('Names and passwords cannot contain spaces (the board reads them as words).'); return; }
    try {
      setOut('Pick the board in the Bluetooth chooser…\n');
      const dev = await navigator.bluetooth.requestDevice({ filters: [{ services: [NUS] }] });
      const srv = await (await dev.gatt.connect()).getPrimaryService(NUS);
      const rx = await srv.getCharacteristic(NUS_RX), tx = await srv.getCharacteristic(NUS_TX);
      await tx.startNotifications();
      let pairLine = null;
      tx.addEventListener('characteristicvaluechanged', e => {
        const t = new TextDecoder().decode(e.target.value);
        if (t.startsWith('pair ')) { pairLine = t.trim().split(' '); return; }          // never shown: it is the board's secret
        setOut(o => o + '< ' + t + '\n');
      });
      setOut(o => o + 'Connected to ' + (dev.name || 'the board') + '\n');
      if (AUTH) {                       // the board's id and setup code; the server gives an owner its broker login
        await rx.writeValueWithResponse(new TextEncoder().encode('pair'));
        await new Promise(r => setTimeout(r, 600));
        if (!pairLine || pairLine[2] === 'none') { setOut(o => o + 'Setup is locked: hold the board\'s button for 3 s until it flashes blue, then try again.\n'); return; }
        const [, board, code] = pairLine;
        if (ui.me?.owner) cmds.push((await api('/api/provision', { board })).line);
        else { await api('/api/link', { board, code }); setOut(o => o + 'Linked ' + board + ' to this browser.\n'); }
      }
      for (const c of cmds) {
        await rx.writeValueWithResponse(new TextEncoder().encode(c));
        setOut(o => o + '> ' + (c.endsWith(' -') ? c : c.replace(/ \S+$/, ' ••••')) + '\n');
        if (c.startsWith('mqtt ')) setOut(o => o + '(its own broker login, from the server)\n');
        await new Promise(r => setTimeout(r, 400));
      }
      if (cmds.length) setOut(o => o + 'Saved. The board restarts in a couple of seconds and should come online.\n');
      if (AUTH) authConnect(true);
    } catch (e) { setOut(o => o + 'Bluetooth: ' + e.message + '\n'); }
  };
  return <dialog ref={d} class="dialog" onClose={() => setSetupOpen(false)}>
    <h3>{AUTH && !ui.me?.owner ? 'Link a board over Bluetooth' : 'Set up a board over Bluetooth'}</h3>
    <p class="hint">Power the board, then send it the Wi-Fi it should join and the broker it should talk to. Only what you fill in is sent.</p>
    <div class="mt-3 grid gap-2">
      <div class="grid grid-cols-2 gap-2"><input ref={el => f.ssid = el} class="field" placeholder="Wi-Fi name" aria-label="Wi-Fi name" /><input ref={el => f.pass = el} type="password" class="field" placeholder="Wi-Fi password" aria-label="Wi-Fi password" /></div>
      <div class="grid grid-cols-2 gap-2"><input ref={el => f.ssid2 = el} class="field" placeholder="fallback Wi-Fi (optional)" aria-label="Fallback Wi-Fi name" /><input ref={el => f.pass2 = el} type="password" class="field" placeholder="its password (empty if open)" aria-label="Fallback Wi-Fi password" /></div>
      <Show when={!AUTH} fallback={<p class="hint">Hold the board's button for 3 s first. {ui.me?.owner ? 'The server gives the board its own broker login.' : 'This links the board to this browser (it must already be online).'}</p>}>
        <input ref={el => f.url = el} class="field" placeholder="broker, e.g. mqtt://host:1883" aria-label="Broker URL" value={BOARD_MQTT} />
        <div class="grid grid-cols-2 gap-2"><input ref={el => f.user = el} class="field" placeholder="broker user" aria-label="Broker user" value="ledify" /><input ref={el => f.bpass = el} type="password" class="field" placeholder="the board's broker password" aria-label="Broker password" /></div>
      </Show>
    </div>
    <pre class="log mt-3 min-h-10">{out()}</pre>
    <div class="mt-3 flex justify-end gap-2"><button type="button" class="btn" onClick={() => setSetupOpen(false)}>Close</button><button type="button" class="btn btn-primary" onClick={go}>Connect and send</button></div>
  </dialog>;
}

