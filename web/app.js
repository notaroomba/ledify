'use strict';
// Ledify control panel. Talks MQTT over WebSockets straight to the broker, on the same topics the board uses.
// Sections: config, helpers, connection, rendering, controls, live stream + listen, audio feed, Spotify, presets.

// ---------------------------------------------------------------- config
// Filled in when the container starts (see Dockerfile); "__X__" means a local copy without those values.
const BUILT_IN = { url: 'wss://mosquitto-production-595e.up.railway.app', user: 'ledify', pass: '__MQTT_PASS__' };
const SPOTIFY_CLIENT_ID = '__SPOTIFY_CLIENT_ID__';
const EFFECTS = {
  solid: 'one colour', rainbow: 'moving rainbow', temp: 'colour from the temperature', spectrum: 'bass, mid and treble bars',
  meter: 'green to red along the strip', palette: 'gradient of the album cover colours', sound: 'brightness follows the sound',
  vu: 'lit length follows the sound', pulse: 'flashes on the beat',
};
const OVERLAYS = ['none', 'vu', 'pulse'];
const PALETTE = ['#ffffff', '#ffd6a0', '#ff8c00', '#ffd700', '#ff0040', '#ff00ff', '#8000ff', '#0080ff', '#00e5ff', '#00ff80'];
const CHIPS = [['ws2812', 'WS2812 / B / C / D / E, WS2813, WS2815'], ['ws2811', 'WS2811 (12 V, RGB order)'], ['sk6812', 'SK6812'], ['sk6812w', 'SK6812 RGBW'], ['ws2816', 'WS2816']];
const ECHO_HOLD_MS = 1500;       // after the user touches a control, ignore the board's echo of older values this long
const SETTINGS = ['count', 'gain', 'lux_lo', 'lux_hi', 'night', 'temp_lo', 'temp_hi'];

// ---------------------------------------------------------------- helpers
const $ = id => document.getElementById(id);
const touched = new Map();
const touch = key => touched.set(key, Date.now());
const settled = key => Date.now() - (touched.get(key) || 0) > ECHO_HOLD_MS;
const hex2 = v => ('0' + Math.round(v).toString(16)).slice(-2);
const rgbHex = (r, g, b) => '#' + hex2(r) + hex2(g) + hex2(b);
const fill = el => el.style.setProperty('--p', ((el.value - el.min) * 100 / (el.max - el.min)) + '%');

function toast(text, kind = 'info', ms = 3500) {
  const t = document.createElement('div');
  t.className = 'toast toast-' + kind;
  t.textContent = text;
  $('toasts').appendChild(t);
  setTimeout(() => { t.style.opacity = '0'; t.style.transition = 'opacity .3s'; setTimeout(() => t.remove(), 300); }, ms);
}
function confirmDialog(title, text, yes = 'Confirm') {
  return new Promise(resolve => {
    const d = $('confirm');
    $('confirm-title').textContent = title; $('confirm-text').textContent = text; $('confirm-yes').textContent = yes;
    const done = v => { d.close(); $('confirm-yes').onclick = $('confirm-no').onclick = null; resolve(v); };
    $('confirm-yes').onclick = () => done(true);
    $('confirm-no').onclick = () => done(false);
    d.oncancel = () => done(false);
    d.showModal();
  });
}
function log(line) {
  const l = $('log');
  l.textContent = (new Date().toLocaleTimeString() + '  ' + line + '\n' + l.textContent).slice(0, 4000);
}

// ---------------------------------------------------------------- state and connection
const devices = {};
let client = null, current = null;
const dev = () => devices[current] || {};

function send(cmd) {
  if (!client || !current) return;
  client.publish('ledify/' + current + '/cmd', cmd);
  log('> ' + cmd);
}

function connect(c) {
  $('conn').textContent = 'connecting';
  if (client) client.end(true);
  client = mqtt.connect(c.url, { username: c.user, password: c.pass, keepalive: 30, reconnectPeriod: 2000, clientId: 'panel-' + Math.random().toString(16).slice(2, 8) });
  client.on('connect', () => {
    if (c !== BUILT_IN) localStorage.setItem('ledify.creds', JSON.stringify(c));
    $('broker').close();
    client.subscribe(['ledify/+/status', 'ledify/+/state', 'ledify/+/sensors', 'ledify/+/light/state', 'ledify/+/live', 'ledify/+/audio', 'ledify/+/reply']);
    keepLive();
  });
  client.on('error', e => { $('broker-msg').textContent = 'Broker said: ' + e.message; $('conn').textContent = 'broker error'; $('broker').showModal(); });
  client.on('close', () => { $('conn').textContent = 'reconnecting'; $('dot').className = 'dot off'; });
  client.on('message', (topic, payload) => {
    const m = topic.match(/^ledify\/([^/]+)\/(.+)$/);
    if (!m) return;
    const id = m[1], what = m[2], d = devices[id] || (devices[id] = {});
    if (!current) { current = id; send('status'); keepLive(); }
    if (what === 'audio') { if (id === current) playChunk(payload); return; }
    const text = payload.toString();
    if (what === 'status') d.online = text === 'online';
    else if (what === 'reply') { if (id === current) onReply(text); return; }
    else { try { d[what] = JSON.parse(text); } catch (e) { return; } }
    d.seen = Date.now();
    renderHeader();
    if (id !== current) return;
    if (what === 'live') renderLive(d.live);
    else render(d, what);
  });
}
function onReply(text) {
  log('< ' + text);
  if (/^err/i.test(text) || /destroys|needs|refused|busy/i.test(text)) toast(text.replace(/^err[,:]?\s*/i, ''), 'warn', 5000);
  else if (text !== 'ok') toast(text, 'info');
}

const savedCreds = localStorage.getItem('ledify.creds');
if (savedCreds && localStorage.getItem('ledify.override')) {
  const c = JSON.parse(savedCreds); $('url').value = c.url; $('user').value = c.user; $('pass').value = c.pass; connect(c);
} else if (!BUILT_IN.pass.startsWith('__')) {
  $('url').value = BUILT_IN.url; $('user').value = BUILT_IN.user; connect(BUILT_IN);
} else {
  $('broker-msg').textContent = 'local copy: enter the broker password'; $('broker').showModal();
}
$('broker-btn').onclick = () => $('broker').showModal();
$('broker-connect').onclick = () => { localStorage.setItem('ledify.override', '1'); connect({ url: $('url').value.trim(), user: $('user').value.trim(), pass: $('pass').value }); };
$('broker-builtin').onclick = () => { localStorage.removeItem('ledify.creds'); localStorage.removeItem('ledify.override'); location.reload(); };

// The board streams loudness/bands at 10 Hz while a lease is active; renew it while this tab is visible.
function keepLive() { if (current && document.visibilityState === 'visible') send('live 90'); }
setInterval(keepLive, 60000);
document.addEventListener('visibilitychange', keepLive);

// ---------------------------------------------------------------- rendering
function renderHeader() {
  const ids = Object.keys(devices).sort(), sel = $('device');
  sel.hidden = ids.length < 2;
  if (sel.options.length !== ids.length) { sel.innerHTML = ''; ids.forEach(id => sel.add(new Option(id, id))); }
  sel.value = current;
  const on = !!dev().online;
  $('dot').className = 'dot ' + (on ? 'on' : 'off');
  $('conn').textContent = on ? 'online' : 'offline';
  $('conn').title = current || '';
  $('banner').hidden = on;
  $('banner').textContent = on ? '' : 'The board is offline: unplugged, or without Wi-Fi. Controls will apply when it is back.';
  $('main').classList.toggle('offline', !on);
}
$('device').onchange = () => { current = $('device').value; render(dev(), 'all'); send('status'); keepLive(); };

function setPressed(host, value) { [...host.children].forEach(b => b.setAttribute('aria-pressed', b.dataset.v === String(value))); }
function setSwitch(id, on) { $(id).setAttribute('aria-checked', on ? 'true' : 'false'); }

function render(d, what) {
  const s = d.state, l = d['light/state'], z = d.sensors;
  if (s && (what === 'state' || what === 'all')) {
    $('fw').textContent = s.fw ? 'fw ' + s.fw : '';
    if (settled('power')) $('power').setAttribute('aria-pressed', !!s.power);
    if (settled('level')) $('level').set(s.level);
    if (settled('mode')) { setPressed($('effects'), s.mode); $('effect-hint').textContent = EFFECTS[s.mode] || ''; }
    if (settled('overlay')) setPressed($('overlays'), s.overlay);
    if (settled('auto')) { setSwitch('auto', !!s.auto); $('auto-settings').hidden = !s.auto; }
    if (settled('limit')) setSwitch('limit', !!s.limit);
    for (const k of SETTINGS) if (k in s && settled(k)) $(k).set(s[k]);
    if (document.activeElement !== $('count-n')) $('count-n').value = s.count;
    if (settled('type')) $('type').value = s.type;
    if (settled('volt')) setPressed($('volts'), s.volt);
    const need = (s.need_mw || 0) / 1000, budget = (s.budget_mw || 0) / 1000;
    const supply = s.pd && s.pd.ready ? (s.pd.mv / 1000) + ' V ' + (s.pd.ma / 1000) + ' A, ' + (s.pd.mv * s.pd.ma / 1e6).toFixed(0) + ' W' : 'no USB-PD contract, assuming 7.5 W';
    let note = '';
    if (s.power && s.limit && s.cap < s.level) note = 'Dimmed to ' + Math.round(s.cap * 100 / 255) + '% to stay within the supply (' + supply + ').';
    else if (s.power && need > budget) note = 'Estimated draw ' + need.toFixed(1) + ' W, the supply gives about ' + budget.toFixed(1) + ' W (' + supply + '). Expect dimming or flicker; turn on the limit or use a stronger charger.';
    $('limit-note').hidden = !note; $('limit-note').textContent = note;
    $('cap').textContent = (s.limit ? s.cap : s.level) + ' / ' + s.level;
    $('pd').textContent = supply;
    $('pdos').textContent = s.pdos && s.pdos.length ? 'Charger offers: ' + s.pdos.map(x => (x[0] / 1000) + ' V ' + (x[1] / 1000) + ' A').join(' · ') + '. The board takes the most powerful one.' : '';
    $('net').textContent = s.net || '';
    if ('lux' in s) $('lux').textContent = Math.round(s.lux);
    if ('temp_c' in s) $('temp').textContent = s.temp_c.toFixed(1);
    if ('pressure_pa' in s) $('kpa').textContent = (s.pressure_pa / 1000).toFixed(2);
    if ('sound_db' in s) $('db').textContent = s.sound_db.toFixed(0) + (s.db_peak ? ' · ' + s.db_peak : '');
  }
  if (z && (what === 'sensors' || what === 'all')) {
    $('lux').textContent = Math.round(z.lux); $('temp').textContent = z.temp_c.toFixed(1); $('kpa').textContent = z.pressure_kpa.toFixed(2);
  }
  if (l && (what === 'light/state' || what === 'all') && l.color && settled('color')) {
    $('color').value = rgbHex(l.color.r, l.color.g, l.color.b);
  }
}
function renderLive(v) {
  if (!v || v.loud === undefined) return;
  $('loud').style.width = (v.loud * 100 / 255) + '%';
  for (let k = 0; k < 3; k++) $('b' + k).style.width = (v.bands[k] * 100 / 255) + '%';
  $('pulse').className = 'dot ' + (v.pulse > 128 ? 'on' : '');
  const peak = ($('db').textContent.split(' · ')[1] || '');
  $('db').textContent = v.db + (peak ? ' · ' + peak : '');
}

// ---------------------------------------------------------------- controls
// A slider shows its value, fills its track, sends while moving (every 60 ms) and once more on release.
function slider(id, toCmd, fmt = v => v) {
  const el = $(id), out = $(id + '-v');
  let timer;
  const show = () => { if (out) out.textContent = fmt(el.value); fill(el); };
  el.addEventListener('input', () => { touch(id); show(); clearTimeout(timer); timer = setTimeout(() => { const c = toCmd(el.value); if (c) send(c); }, 60); });
  el.addEventListener('change', () => { touch(id); clearTimeout(timer); const c = toCmd(el.value); if (c) send(c); });
  el.set = v => { if (v !== undefined) { el.value = v; show(); } };
  show();
  return el;
}
function chips(host, items, onPick, cls = '') {
  items.forEach(([value, label]) => {
    const b = document.createElement('button');
    b.className = 'chip ' + cls; b.textContent = label; b.dataset.v = value; b.setAttribute('aria-pressed', 'false');
    b.onclick = () => onPick(value, b);
    host.appendChild(b);
  });
}
function toggle(id, key, onChange) {
  $(id).onclick = () => { const on = $(id).getAttribute('aria-checked') !== 'true'; touch(key); setSwitch(id, on); onChange(on); };
}

slider('level', v => 'level ' + v);
slider('count', v => 'count ' + v);
slider('gain', v => 'gain ' + v);
slider('lux_lo', v => 'lux ' + v + ' ' + $('lux_hi').value, v => v + ' lx');
slider('lux_hi', v => 'lux ' + $('lux_lo').value + ' ' + v, v => v + ' lx');
slider('night', v => 'night ' + v, v => v == 0 ? 'never' : v + ' lx');
slider('temp_lo', v => 'temp ' + v + ' ' + $('temp_hi').value, v => v + ' °C');
slider('temp_hi', v => 'temp ' + $('temp_lo').value + ' ' + v, v => v + ' °C');
slider('lvol', () => null);
slider('asens', () => null);
$('level').addEventListener('input', () => { $('level-v').textContent = $('level').value; });
$('level').set = v => { if (v !== undefined) { $('level').value = v; $('level-v').textContent = v; fill($('level')); } };
$('level').set($('level').value);

chips($('effects'), Object.keys(EFFECTS).map(k => [k, k]), (v, b) => { touch('mode'); setPressed($('effects'), v); $('effect-hint').textContent = EFFECTS[v]; send('mode ' + v); });
chips($('overlays'), OVERLAYS.map(k => [k, k]), v => { touch('overlay'); setPressed($('overlays'), v); send('overlay ' + v); });
chips($('volts'), [['5', '5 V'], ['12', '12 V']], async v => {
  if (v === '12' && !await confirmDialog('Switch the strip to 12 V?', 'Only for 12 V strips (WS2815, WS2811). 5 V pixels are destroyed instantly. The output turns off first, so press power again afterwards.', 'Switch to 12 V')) return;
  touch('volt'); setPressed($('volts'), v); send(v === '12' ? 'volt 12 yes' : 'volt 5');
}, 'danger');
CHIPS.forEach(([v, label]) => $('type').add(new Option(label, v)));
$('type').onchange = () => { touch('type'); send('type ' + $('type').value); };
$('power').onclick = () => { const on = $('power').getAttribute('aria-pressed') !== 'true'; touch('power'); $('power').setAttribute('aria-pressed', on); send(on ? 'on' : 'off'); };
toggle('auto', 'auto', on => { $('auto-settings').hidden = !on; send('auto ' + (on ? 'on' : 'off')); });
toggle('limit', 'limit', on => send('limit ' + (on ? 1 : 0)));
$('count-set').onclick = () => { const v = Math.max(1, Math.min(1000, +$('count-n').value || 1)); touch('count'); $('count').set(v); send('count ' + v); };

function sendColor(hex) { touch('color'); touch('mode'); setPressed($('effects'), 'solid'); $('effect-hint').textContent = EFFECTS.solid; send('rgb ' + parseInt(hex.slice(1, 3), 16) + ' ' + parseInt(hex.slice(3, 5), 16) + ' ' + parseInt(hex.slice(5, 7), 16)); }
let colorTimer;
$('color').addEventListener('input', () => { clearTimeout(colorTimer); colorTimer = setTimeout(() => sendColor($('color').value), 60); });
function hueHex(h) { const f = (n, k = (n + h / 60) % 6) => 255 * (1 - Math.max(0, Math.min(k, 4 - k, 1))); return rgbHex(f(5), f(3), f(1)); }
$('hue').addEventListener('input', () => { $('hue-v').textContent = $('hue').value + '°'; fill($('hue')); $('color').value = hueHex(+$('hue').value); clearTimeout(colorTimer); colorTimer = setTimeout(() => sendColor($('color').value), 60); });
fill($('hue'));
PALETTE.forEach(c => { const b = document.createElement('button'); b.className = 'swatch'; b.style.background = c; b.title = c; b.setAttribute('aria-label', 'colour ' + c); b.onclick = () => { $('color').value = c; sendColor(c); }; $('palette').appendChild(b); });

$('temp-send').onclick = () => { if ($('temp-in').value !== '') send('temp ' + $('temp-in').value); };
$('ota').onclick = async () => { if (await confirmDialog('Update the firmware?', 'From ' + $('ota-url').value + '. The strip restarts when the download is done, about a minute.', 'Update')) send('ota ' + $('ota-url').value); };
$('send').onclick = () => { const c = $('cmd').value.trim(); if (c) { send(c); $('cmd').value = ''; } };
$('cmd').addEventListener('keydown', e => { if (e.key === 'Enter') $('send').onclick(); });

// ---------------------------------------------------------------- listen: the board streams 16 kHz 16-bit PCM while a lease is active
let lctx, lnext = 0, listening = false, ltimer;
function playChunk(buf) {
  if (!listening) return;
  const pcm = new Int16Array(buf.buffer, buf.byteOffset, buf.length >> 1);
  const ab = lctx.createBuffer(1, pcm.length, 16000), ch = ab.getChannelData(0), g = $('lvol').value / 32768;
  for (let i = 0; i < pcm.length; i++) ch[i] = Math.max(-1, Math.min(1, pcm[i] * g));
  const src = lctx.createBufferSource(); src.buffer = ab; src.connect(lctx.destination);
  const t = Math.max(lnext, lctx.currentTime + 0.08);
  src.start(t); lnext = t + ab.duration;
  $('listen-state').textContent = 'playing';
}
$('listen').onclick = () => {
  if (listening) {
    listening = false; clearInterval(ltimer); send('listen 0');
    $('listen').textContent = "Listen to the board's mic"; $('listen-state').textContent = 'off';
    return;
  }
  if (!lctx) lctx = new (window.AudioContext || window.webkitAudioContext)();
  if (lctx.state === 'suspended') lctx.resume();
  listening = true; lnext = 0; send('listen 60'); ltimer = setInterval(() => send('listen 60'), 45000);
  $('listen').textContent = 'Stop listening'; $('listen-state').textContent = 'waiting for audio';
};

// ---------------------------------------------------------------- audio feed: analyse a file or this device's mic, send "audio L B M T P" at 20 Hz
let actx, analyser, asrc, atimer, apeak = 1, bpeak = [1, 1, 1], aavg = 0, apulse = 0;
function astart(node) {
  analyser = actx.createAnalyser(); analyser.fftSize = 1024; analyser.smoothingTimeConstant = 0.4;
  node.connect(analyser);
  const td = new Uint8Array(analyser.fftSize), fd = new Uint8Array(analyser.frequencyBinCount);
  const hz = actx.sampleRate / analyser.fftSize, edges = [20, 250, 2000, 8000].map(f => Math.round(f / hz));
  clearInterval(atimer);
  atimer = setInterval(() => {
    analyser.getByteTimeDomainData(td); analyser.getByteFrequencyData(fd);
    let acc = 0; for (const x of td) { const v = (x - 128) / 128; acc += v * v; }
    const rms = Math.sqrt(acc / td.length) * $('asens').value;
    apeak = Math.max(rms, apeak * 0.995, 0.05);
    const loud = Math.min(255, Math.round(rms / apeak * 255));
    const bands = [];
    for (let k = 0; k < 3; k++) {
      let m = 0; for (let i = edges[k]; i < edges[k + 1]; i++) m = Math.max(m, fd[i]);
      bpeak[k] = Math.max(m, bpeak[k] * 0.995, 40);
      bands.push(Math.min(255, Math.round(m / bpeak[k] * 255)));
    }
    aavg += (rms - aavg) / 20;
    apulse = rms > 1.5 * aavg && rms > 0.05 ? 255 : Math.round(apulse * 0.8);
    if (client && current) client.publish('ledify/' + current + '/audio/set', loud + ' ' + bands.join(' ') + ' ' + apulse);
    renderLive({ loud, bands, pulse: apulse, db: $('db').textContent.split(' · ')[0] });
  }, 50);
  $('sound-src').textContent = 'this device';
}
function actxInit() { if (!actx) actx = new (window.AudioContext || window.webkitAudioContext)(); if (actx.state === 'suspended') actx.resume(); }
$('aplay').onclick = () => {
  const f = $('afile').files[0];
  if (!f) { toast('Pick an audio file first', 'warn'); return; }
  actxInit(); astop();
  const a = $('aelem'); a.hidden = false; a.src = URL.createObjectURL(f);
  asrc = actx.createMediaElementSource(a); asrc.connect(actx.destination); astart(asrc); a.play();
};
$('amic').onclick = async () => {
  try { actxInit(); astop(); const st = await navigator.mediaDevices.getUserMedia({ audio: true }); asrc = actx.createMediaStreamSource(st); asrc.stream = st; astart(asrc); }
  catch (e) { toast('Microphone: ' + e.message, 'warn'); }
};
function astop() {
  clearInterval(atimer); atimer = null;
  if (asrc) { try { asrc.disconnect(); if (asrc.stream) asrc.stream.getTracks().forEach(t => t.stop()); } catch (e) { /* already gone */ } asrc = null; }
  const a = $('aelem'); a.pause(); a.hidden = true;
  $('sound-src').textContent = 'board mic';
}
$('astop').onclick = astop;

// ---------------------------------------------------------------- Spotify: authorization code + PKCE in the browser, no server
const SP_REDIRECT = location.origin + location.pathname;
const spClientId = () => localStorage.getItem('ledify.sp.client') || (SPOTIFY_CLIENT_ID.startsWith('__') ? '' : SPOTIFY_CLIENT_ID);
const spTok = () => { try { return JSON.parse(localStorage.getItem('ledify.sp.token')); } catch (e) { return null; } };
let spLast = '';
$('sp-client').value = localStorage.getItem('ledify.sp.client') || '';
$('sp-client').onchange = () => localStorage.setItem('ledify.sp.client', $('sp-client').value.trim());
setSwitch('sp-auto', localStorage.getItem('ledify.sp.auto') === '1');
$('sp-auto').onclick = () => { const on = $('sp-auto').getAttribute('aria-checked') !== 'true'; setSwitch('sp-auto', on); localStorage.setItem('ledify.sp.auto', on ? '1' : '0'); if (on) spLast = ''; };
async function spLogin() {
  const id = spClientId();
  if (!id) { toast('Enter the Spotify client ID first', 'warn'); return; }
  const verifier = [...crypto.getRandomValues(new Uint8Array(48))].map(b => hex2(b)).join('');
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(verifier));
  const challenge = btoa(String.fromCharCode(...new Uint8Array(digest))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  localStorage.setItem('ledify.sp.verifier', verifier);
  const q = new URLSearchParams({ client_id: id, response_type: 'code', redirect_uri: SP_REDIRECT, scope: 'user-read-currently-playing user-read-playback-state', code_challenge_method: 'S256', code_challenge: challenge });
  location.href = 'https://accounts.spotify.com/authorize?' + q;
}
async function spToken(body) {
  const r = await fetch('https://accounts.spotify.com/api/token', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ client_id: spClientId(), ...body }) });
  if (!r.ok) throw new Error('token ' + r.status);
  const t = await r.json(); t.expires_at = Date.now() + t.expires_in * 1000 - 60000;
  if (!t.refresh_token) t.refresh_token = (spTok() || {}).refresh_token;
  localStorage.setItem('ledify.sp.token', JSON.stringify(t));
  return t;
}
async function spAccess() {
  let t = spTok(); if (!t) return null;
  if (Date.now() > t.expires_at) t = await spToken({ grant_type: 'refresh_token', refresh_token: t.refresh_token });
  return t.access_token;
}
$('sp-login').onclick = () => {
  if (spTok()) { localStorage.removeItem('ledify.sp.token'); $('sp-login').textContent = 'Connect Spotify'; $('sp-state').textContent = 'not connected'; $('sp-track').textContent = '—'; $('sp-artist').textContent = ''; $('sp-art').removeAttribute('src'); $('sp-palette').innerHTML = ''; }
  else spLogin();
};
(async () => {
  const code = new URLSearchParams(location.search).get('code');
  if (code) {
    try { await spToken({ grant_type: 'authorization_code', code, redirect_uri: SP_REDIRECT, code_verifier: localStorage.getItem('ledify.sp.verifier') }); }
    catch (e) { toast('Spotify login failed: ' + e.message, 'warn', 6000); }
    history.replaceState(null, '', location.pathname);
  }
  if (spTok()) { $('sp-login').textContent = 'Disconnect'; spPoll(); }
})();
async function spPoll() {
  if (!spTok()) return;
  try {
    const tok = await spAccess();
    const r = await fetch('https://api.spotify.com/v1/me/player/currently-playing', { headers: { Authorization: 'Bearer ' + tok } });
    if (r.status === 204) { $('sp-state').textContent = 'nothing playing'; return; }
    if (!r.ok) throw new Error('api ' + r.status);
    const j = await r.json(), it = j.item;
    if (!it) { $('sp-state').textContent = 'nothing playing'; return; }
    $('sp-state').textContent = j.is_playing ? 'playing' : 'paused';
    $('sp-track').textContent = it.name;
    $('sp-artist').textContent = (it.artists || []).map(a => a.name).join(', ') + (it.album ? ' · ' + it.album.name : '');
    const art = it.album && it.album.images && it.album.images.length ? it.album.images[Math.min(1, it.album.images.length - 1)].url : '';
    if (art) $('sp-art').src = art;
    if (it.id !== spLast) { spLast = it.id; if (art && $('sp-auto').getAttribute('aria-checked') === 'true') gradientFromArt(art); }
  } catch (e) { $('sp-state').textContent = 'Spotify: ' + e.message; }
}
setInterval(spPoll, 5000);
// Up to five prominent colours of the cover: skip near-black, near-white and grey pixels, bucket the rest, rank by
// size and saturation, keep ones that differ clearly, order by hue, push each to full intensity, send as a gradient.
function gradientFromArt(url) {
  const img = new Image(); img.crossOrigin = 'anonymous';
  img.onload = () => {
    const c = document.createElement('canvas'); c.width = c.height = 40;
    const g = c.getContext('2d'); g.drawImage(img, 0, 0, 40, 40);
    let d; try { d = g.getImageData(0, 0, 40, 40).data; } catch (e) { toast('The cover image blocks colour extraction', 'warn'); return; }
    const bins = {};
    for (let i = 0; i < d.length; i += 4) {
      const r = d[i], gg = d[i + 1], b = d[i + 2], mx = Math.max(r, gg, b), mn = Math.min(r, gg, b), sat = mx ? (mx - mn) / mx : 0;
      if (mx < 50 || sat < 0.3) continue;
      const k = (r >> 5) + ',' + (gg >> 5) + ',' + (b >> 5), bn = bins[k] || (bins[k] = { n: 0, r: 0, g: 0, b: 0, s: 0 });
      bn.n++; bn.r += r; bn.g += gg; bn.b += b; bn.s += sat;
    }
    const ranked = Object.values(bins).map(bn => ({ r: bn.r / bn.n, g: bn.g / bn.n, b: bn.b / bn.n, score: bn.n * (bn.s / bn.n + 0.2) })).sort((a, b) => b.score - a.score);
    const picked = [];
    for (const col of ranked) { if (picked.length >= 5) break; if (picked.every(p => Math.hypot(p.r - col.r, p.g - col.g, p.b - col.b) > 70)) picked.push(col); }
    if (!picked.length) { toast('This cover has no strong colour', 'info'); return; }
    const hue = col => { const mx = Math.max(col.r, col.g, col.b), mn = Math.min(col.r, col.g, col.b), dd = mx - mn || 1; return mx === col.r ? ((col.g - col.b) / dd + 6) % 6 : mx === col.g ? (col.b - col.r) / dd + 2 : (col.r - col.g) / dd + 4; };
    picked.sort((a, b) => hue(a) - hue(b));
    const hexes = picked.map(col => { const mx = Math.max(col.r, col.g, col.b); return [col.r, col.g, col.b].map(v => hex2(v * 255 / mx)).join(''); });
    $('sp-palette').innerHTML = hexes.map(hx => '<span class="h-7 w-7 rounded-full border border-line" style="background:#' + hx + '"></span>').join('');
    touch('mode'); setPressed($('effects'), 'palette'); $('effect-hint').textContent = EFFECTS.palette;
    send('palette ' + hexes.join(','));
  };
  img.onerror = () => toast('Could not load the cover', 'warn');
  img.src = url;
}

// ---------------------------------------------------------------- presets (this browser)
const presets = () => { try { return JSON.parse(localStorage.getItem('ledify.presets') || '{}'); } catch (e) { return {}; } };
function renderPresets() {
  const host = $('presets'), all = presets(); host.innerHTML = '';
  if (!Object.keys(all).length) { host.innerHTML = '<span class="hint">none yet: set up a look, then save it</span>'; return; }
  for (const [name, p] of Object.entries(all)) {
    const b = document.createElement('button'); b.className = 'chip'; b.textContent = name;
    b.onclick = () => { send('rgb ' + p.rgb); send('level ' + p.level); send('mode ' + p.mode); if (p.overlay) send('overlay ' + p.overlay); send('auto ' + (p.auto ? 'on' : 'off')); send('gain ' + p.gain); if (p.power) send('on'); toast('Applied "' + name + '"', 'ok', 2000); };
    const x = document.createElement('button'); x.className = 'chip'; x.textContent = '✕'; x.title = 'delete ' + name;
    x.onclick = async () => { if (await confirmDialog('Delete preset?', '"' + name + '" will be removed from this browser.', 'Delete')) { const a = presets(); delete a[name]; localStorage.setItem('ledify.presets', JSON.stringify(a)); renderPresets(); } };
    host.appendChild(b); host.appendChild(x);
  }
}
$('preset-save').onclick = () => {
  const d = dev(), s = d.state || {}, l = d['light/state'] || {}, name = $('preset-name').value.trim() || 'look ' + (Object.keys(presets()).length + 1);
  const c = l.color || { r: 255, g: 255, b: 255 };
  const all = presets(); all[name] = { rgb: c.r + ' ' + c.g + ' ' + c.b, level: s.level, mode: s.mode, overlay: s.overlay, auto: s.auto, gain: s.gain, power: s.power };
  localStorage.setItem('ledify.presets', JSON.stringify(all)); $('preset-name').value = ''; renderPresets();
  toast('Saved "' + name + '"', 'ok', 2000);
};
renderPresets();
