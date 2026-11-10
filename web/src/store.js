// Connection and state. Three ways in: MQTT over WebSockets to the broker (the hosted panel), the board's own
// WebSocket when the page is served by the board (http://ledify.local), or a demo device (?demo). Everything the
// board publishes lands in a Solid store and components react to the exact fields they read.
import { createSignal, createEffect, createRoot } from 'solid-js';
import { createStore } from 'solid-js/store';
import mqtt from 'mqtt';
import { rgbHex } from './color';

const CFG = window.LEDIFY_CONFIG || {};
const unset = v => !v || v.startsWith('__');          // "__X__" means a local copy without that value injected
export const LOCAL = !!CFG.local;
export const AUTH = !!CFG.auth && !CFG.local;      // the hosted panel with accounts: the server hands out broker logins
export const DEMO = new URLSearchParams(location.search).has('demo');
export const BUILT_IN = { url: CFG.mqttUrl || '', user: unset(CFG.mqttUser) ? 'ledify' : CFG.mqttUser, pass: unset(CFG.mqttPass) ? '' : CFG.mqttPass };
export const BOARD_MQTT = CFG.boardMqtt || '';
export const SPOTIFY_CLIENT_ID = unset(CFG.spotifyClientId) ? '' : CFG.spotifyClientId;
const ECHO_HOLD_MS = 1500;      // after the user touches a control, ignore the board's echo of older values this long

export const [ui, setUi] = createStore({
  conn: 'connecting', current: null, devices: {},
  live: { loud: 0, bands: [0, 0, 0], pulse: 0, db: null, spec: null },
  toasts: [], brokerOpen: false, brokerMsg: '',
  me: null,                 // AUTH: { owner, boards, label } from the server
  pair: null,               // the board's own page: { id, code } while its setup window is open
});
export const dev = () => ui.devices[ui.current] || {};
export const state = () => dev().state || {};
export const online = () => !!dev().online;
// What to call a board: the name it was given (its hostname, "name" in its state), or its MQTT id while it still has
// the factory name. Two boards with the same name get the end of their id added so the picker can tell them apart.
export const DEFAULT_NAME = 'ledify';
export function boardLabel(id) {
  const name = n => ((ui.devices[n] || {}).state || {}).name;
  const own = name(id);
  if (!own || own === DEFAULT_NAME) return id;
  const twin = Object.keys(ui.devices).some(o => o !== id && name(o) === own);
  return twin ? own + ' (' + id.slice(-6) + ')' : own;
}
// A typed name made into a valid hostname: spaces become dashes, anything else not a letter, digit or dash is dropped.
export const slugName = t => t.trim().replace(/\s+/g, '-').replace(/[^A-Za-z0-9-]/g, '').replace(/-+/g, '-').replace(/^-|-$/g, '').slice(0, 31);
export const hooks = { demo: null };                  // demo takes commands

const touched = new Map();
export const touch = k => touched.set(k, Date.now());
export const settled = k => Date.now() - (touched.get(k) || 0) > ECHO_HOLD_MS;

// A control's value: what the user last set, until the board has had time to echo it; then whatever the board says.
export function optimistic(key, source) {
  const [v, setV] = createSignal(source());
  createEffect(() => { dev().seen; const s = source(); if (s !== undefined && settled(key)) setV(() => s); });
  return [v, nv => { touch(key); setV(() => nv); }];
}
// The look of the strip, shared by the ring, the lenses and the command bar: each is what the user last set until the
// board has echoed it, then whatever the board says.
export const opt = createRoot(() => {
  const [mode, setMode] = optimistic('mode', () => state().mode);
  const [color, setColor] = optimistic('color', () => { const c = (dev().light || {}).color; return c ? rgbHex(c.r, c.g, c.b) : undefined; });
  const [level, setLevel] = optimistic('level', () => state().level);
  const [power, setPower] = optimistic('power', () => state().power);
  const [overlay, setOverlay] = optimistic('overlay', () => state().overlay);
  const [scroll, setScroll] = optimistic('scroll', () => state().scroll);
  const [stripe, setStripe] = optimistic('stripe', () => state().stripe === undefined ? undefined : String(state().stripe));
  return { mode, setMode, color, setColor, level, setLevel, power, setPower, overlay, setOverlay, scroll, setScroll, stripe, setStripe };
});

export const [logText, setLogText] = createSignal('');
export const log = line => setLogText(l => (new Date().toLocaleTimeString() + '  ' + line + '\n' + l).slice(0, 4000));

let client = null, ws = null, toastId = 0;
export function toast(text, kind = 'info', ms = 3500) {
  if (ui.toasts.some(t => t.text === text)) return;                 // the same message again just stays up
  const id = ++toastId;
  setUi('toasts', t => [...t, { id, text, kind }].slice(-4));
  setTimeout(() => setUi('toasts', t => t.filter(x => x.id !== id)), ms);
}
export function send(cmd) {
  if (!ui.current) return;
  if (DEMO) { if (hooks.demo) hooks.demo(cmd); }
  else if (LOCAL) { if (ws && ws.readyState === 1) ws.send(cmd); }
  else if (client) client.publish('ledify/' + ui.current + '/cmd', cmd);
  log('> ' + cmd);
}
export function publishRaw(suffix, payload) {
  if (!ui.current || DEMO) return;
  if (LOCAL) { if (ws && ws.readyState === 1) ws.send(suffix === 'audio/set' ? 'audio ' + payload : payload); }
  else if (client) client.publish('ledify/' + ui.current + '/' + suffix, payload);
}
// The board streams loudness/bands at 10 Hz while a lease is active; renew it while this tab is visible.
export function keepLive() { if (ui.current && document.visibilityState === 'visible') send('live 90'); }

// Replies to commands. "status" answers with the whole state as JSON: that is state, not a message to show.
function onReply(text, id) {
  log('< ' + text);
  if (/^\s*[{[]/.test(text)) {
    try { const j = JSON.parse(text); if (j && j.fw !== undefined) setUi('devices', id, 'state', j); } catch (e) { /* not JSON after all */ }
    return;
  }
  const short = text.length > 140 ? text.slice(0, 137) + '…' : text;
  if (/^err/i.test(text) || /destroys|needs|refused|busy/i.test(text)) toast(short.replace(/^err[,:]?\s*/i, ''), 'warn', 5000);
  else if (text !== 'ok') toast(short, 'info');
}
export function ingest(topic, payload) {
  const m = topic.match(/^ledify\/([^/]+)\/(.+)$/);
  if (!m) return;
  const id = m[1], what = m[2];
  if (!ui.devices[id]) setUi('devices', id, { online: false, seen: 0 });
  if (!ui.current) { setUi('current', id); send('status'); keepLive(); }
  const mine = id === ui.current;
  const text = payload.toString();
  if (what === 'reply') { if (mine) onReply(text, id); return; }
  if (what === 'pair') { const [pid, code] = text.split(' '); setUi('pair', code && code !== 'none' ? { id: pid, code } : null); return; }
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
    if (c !== BUILT_IN && !AUTH) localStorage.setItem('ledify.creds', JSON.stringify(c));
    setUi({ brokerOpen: false, brokerMsg: '' });
    // With accounts the broker only lets a browser subscribe to its own boards (the owner may use the wildcard).
    if (AUTH && !c.owner) client.subscribe(c.boards.map(id => 'ledify/' + id + '/#'));
    else client.subscribe(['ledify/+/status', 'ledify/+/state', 'ledify/+/sensors', 'ledify/+/light/state', 'ledify/+/live', 'ledify/+/reply']);
    keepLive();
  });
  client.on('error', e => {
    if (AUTH) { setUi('conn', 'reconnecting'); if (/auth/i.test(e.message)) authConnect(); return; }   // login rotated or access changed
    setUi({ brokerMsg: 'Broker said: ' + e.message, conn: 'broker error', brokerOpen: true });
  });
  client.on('close', () => setUi('conn', 'reconnecting'));
  client.on('message', ingest);
}
// Served by the board: one WebSocket, text frames "<topic suffix> <payload>" down, command lines up, binary = audio.
function connectLocal() {
  const id = CFG.id || 'local';
  setUi('conn', 'connecting');
  ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { setUi('devices', id, { online: true, seen: Date.now() }); setUi({ current: id, conn: 'online' }); send('status'); keepLive(); ws.send('pair'); };
  ws.onmessage = e => {
    if (typeof e.data !== 'string') return;
    const sp = e.data.indexOf(' ');
    if (sp > 0) ingest('ledify/' + id + '/' + e.data.slice(0, sp), e.data.slice(sp + 1));
  };
  ws.onclose = () => { ws = null; setUi('conn', 'reconnecting'); setUi('devices', id, 'online', false); setTimeout(connectLocal, 2000); };
  ws.onerror = () => ws && ws.close();
}
// Accounts: ask the server who we are and for a broker login for this tab. Called again when the broker refuses the
// login (the server rotated it, or the owner changed what this browser may see).
export async function api(path, body) {
  const r = await fetch(path, { method: body ? 'POST' : 'GET', headers: { 'X-Ledify': '1', 'Content-Type': 'application/json' }, body: body && JSON.stringify(body), credentials: 'same-origin' });
  let j = {}; try { j = await r.json(); } catch (e) { /* empty */ }
  if (!r.ok) throw new Error(j.error || 'server said ' + r.status);
  return j;
}
let authBusy = false, authAt = 0;
export async function authConnect(force) {
  if (authBusy || (!force && Date.now() - authAt < 4000)) return;
  authBusy = true; authAt = Date.now();
  try {
    setUi('me', await api('/api/me'));
    if (!ui.me.owner && !ui.me.boards.length) { if (client) client.end(true); client = null; setUi({ conn: 'no boards', devices: {}, current: null }); return; }
    const b = await api('/api/broker', {});
    for (const id of Object.keys(ui.devices)) if (!b.owner && !b.boards.includes(id)) setUi('devices', id, undefined);
    if (ui.current && !ui.devices[ui.current]) setUi('current', null);
    connect({ url: b.url, user: b.user, pass: b.pass, owner: b.owner, boards: b.boards });
  } catch (e) { setUi('conn', 'server error'); toast(e.message, 'warn'); }
  finally { authBusy = false; }
}
// A link in the address bar: "#link=<board>.<code>" from the board's own page, "#join=<token>" from a share link.
async function takeLink() {
  const m = location.hash.match(/^#(link|join)=([^&]+)/);
  if (!m) return;
  history.replaceState(null, '', location.pathname + location.search);
  try {
    if (m[1] === 'link') { const [board, code] = decodeURIComponent(m[2]).split('.'); await api('/api/link', { board, code }); }
    else await api('/api/join', { token: m[2] });
    toast('Board linked to this browser', 'info');
  } catch (e) { toast(e.message, 'warn', 7000); }
}

export function selectDevice(id) { setUi('current', id); send('status'); keepLive(); }
export function savedCreds() { try { return JSON.parse(localStorage.getItem('ledify.creds')); } catch (e) { return null; } }
export function init() {
  if (DEMO) { import('./demo').then(m => m.startDemo()); return; }
  if (LOCAL) connectLocal();
  else if (AUTH) { takeLink().then(() => authConnect(true)); window.addEventListener('hashchange', () => takeLink().then(() => authConnect(true))); }
  else {
    const saved = savedCreds();
    if (saved && localStorage.getItem('ledify.override')) connect(saved);
    else if (BUILT_IN.pass) connect(BUILT_IN);
    else setUi({ brokerOpen: true, brokerMsg: 'local copy: enter the broker password' });
  }
  setInterval(keepLive, 60000);
  document.addEventListener('visibilitychange', keepLive);
}
export function useOverride(c) { localStorage.setItem('ledify.override', '1'); connect(c); }
export function useBuiltIn() { localStorage.removeItem('ledify.creds'); localStorage.removeItem('ledify.override'); location.reload(); }
