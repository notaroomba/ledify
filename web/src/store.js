// Connection and state. One MQTT client over WebSockets; everything the board publishes lands in a Solid store
// and components react to the exact fields they read.
import { createSignal, createEffect, createRoot } from 'solid-js';
import { createStore } from 'solid-js/store';
import mqtt from 'mqtt';

const CFG = window.LEDIFY_CONFIG || {};
const unset = v => !v || v.startsWith('__');          // "__X__" means a local copy without that value injected
export const BUILT_IN = { url: CFG.mqttUrl || '', user: CFG.mqttUser || 'ledify', pass: unset(CFG.mqttPass) ? '' : CFG.mqttPass };
export const SPOTIFY_CLIENT_ID = unset(CFG.spotifyClientId) ? '' : CFG.spotifyClientId;
const ECHO_HOLD_MS = 1500;      // after the user touches a control, ignore the board's echo of older values this long

export const [ui, setUi] = createStore({
  conn: 'connecting', current: null, devices: {},
  live: { loud: 0, bands: [0, 0, 0], pulse: 0, db: null },
  toasts: [], brokerOpen: false, brokerMsg: '',
});
export const dev = () => ui.devices[ui.current] || {};
export const state = () => dev().state || {};
export const online = () => !!dev().online;
export const hooks = { audio: null };              // raw PCM chunks from the board go straight to the listen player

const touched = new Map();
export const touch = k => touched.set(k, Date.now());
export const settled = k => Date.now() - (touched.get(k) || 0) > ECHO_HOLD_MS;

// A control's value: what the user last set, until the board has had time to echo it; then whatever the board says.
export function optimistic(key, source) {
  const [v, setV] = createSignal(source());
  createEffect(() => { dev().seen; const s = source(); if (s !== undefined && settled(key)) setV(() => s); });
  return [v, nv => { touch(key); setV(() => nv); }];
}
// Shared across cards: the colour picker and the Spotify gradient also switch the effect.
export const opt = createRoot(() => { const [mode, setMode] = optimistic('mode', () => state().mode); return { mode, setMode }; });

export const [logText, setLogText] = createSignal('');
export const log = line => setLogText(l => (new Date().toLocaleTimeString() + '  ' + line + '\n' + l).slice(0, 4000));

let client = null, toastId = 0;
export function toast(text, kind = 'info', ms = 3500) {
  const id = ++toastId;
  setUi('toasts', t => [...t, { id, text, kind }]);
  setTimeout(() => setUi('toasts', t => t.filter(x => x.id !== id)), ms);
}
export function send(cmd) {
  if (!client || !ui.current) return;
  client.publish('ledify/' + ui.current + '/cmd', cmd);
  log('> ' + cmd);
}
export function publishRaw(suffix, payload) {
  if (client && ui.current) client.publish('ledify/' + ui.current + '/' + suffix, payload);
}
// The board streams loudness/bands at 10 Hz while a lease is active; renew it while this tab is visible.
export function keepLive() { if (ui.current && document.visibilityState === 'visible') send('live 90'); }

function onReply(text) {
  log('< ' + text);
  if (/^err/i.test(text) || /destroys|needs|refused|busy/i.test(text)) toast(text.replace(/^err[,:]?\s*/i, ''), 'warn', 5000);
  else if (text !== 'ok') toast(text, 'info');
}
function onMessage(topic, payload) {
  const m = topic.match(/^ledify\/([^/]+)\/(.+)$/);
  if (!m) return;
  const id = m[1], what = m[2];
  if (!ui.devices[id]) setUi('devices', id, { online: false, seen: 0 });
  if (!ui.current) { setUi('current', id); send('status'); keepLive(); }
  const mine = id === ui.current;
  if (what === 'audio') { if (mine && hooks.audio) hooks.audio(payload); return; }
  const text = payload.toString();
  if (what === 'reply') { if (mine) onReply(text); return; }
  if (what === 'live') {
    if (mine) { try { const j = JSON.parse(text); if (j.loud !== undefined) setUi('live', j); } catch (e) { /* diagnostics payloads */ } }
    return;
  }
  if (what === 'status') setUi('devices', id, 'online', text === 'online');
  else { let j; try { j = JSON.parse(text); } catch (e) { return; } setUi('devices', id, what === 'light/state' ? 'light' : what, j); }
  setUi('devices', id, 'seen', Date.now());
  setUi('conn', online() ? 'online' : 'offline');
}
export function connect(c) {
  setUi('conn', 'connecting');
  if (client) client.end(true);
  client = mqtt.connect(c.url, { username: c.user, password: c.pass, keepalive: 30, reconnectPeriod: 2000, clientId: 'panel-' + Math.random().toString(16).slice(2, 8) });
  client.on('connect', () => {
    if (c !== BUILT_IN) localStorage.setItem('ledify.creds', JSON.stringify(c));
    setUi({ brokerOpen: false, brokerMsg: '' });
    client.subscribe(['ledify/+/status', 'ledify/+/state', 'ledify/+/sensors', 'ledify/+/light/state', 'ledify/+/live', 'ledify/+/audio', 'ledify/+/reply']);
    keepLive();
  });
  client.on('error', e => setUi({ brokerMsg: 'Broker said: ' + e.message, conn: 'broker error', brokerOpen: true }));
  client.on('close', () => setUi('conn', 'reconnecting'));
  client.on('message', onMessage);
}
export function selectDevice(id) { setUi('current', id); send('status'); keepLive(); }
export function savedCreds() { try { return JSON.parse(localStorage.getItem('ledify.creds')); } catch (e) { return null; } }
export function init() {
  const saved = savedCreds();
  if (saved && localStorage.getItem('ledify.override')) connect(saved);
  else if (BUILT_IN.pass) connect(BUILT_IN);
  else setUi({ brokerOpen: true, brokerMsg: 'local copy: enter the broker password' });
  setInterval(keepLive, 60000);
  document.addEventListener('visibilitychange', keepLive);
}
export function useOverride(c) { localStorage.setItem('ledify.override', '1'); connect(c); }
export function useBuiltIn() { localStorage.removeItem('ledify.creds'); localStorage.removeItem('ledify.override'); location.reload(); }
