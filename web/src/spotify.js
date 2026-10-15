// Spotify: authorization code + PKCE in the browser, no server. Polls the current track and, when asked, spreads the
// cover's main colours along the strip as a gradient.
import { createSignal } from 'solid-js';
import { send, toast, opt, SPOTIFY_CLIENT_ID } from './store';
import { hex2 } from './ui';

const REDIRECT = location.origin + location.pathname;
const tok = () => { try { return JSON.parse(localStorage.getItem('ledify.sp.token')); } catch (e) { return null; } };
const [client, setClientSig] = createSignal(localStorage.getItem('ledify.sp.client') || '');
const [auto, setAutoSig] = createSignal(localStorage.getItem('ledify.sp.auto') === '1');
const [spState, setSpState] = createSignal('not connected');
const [track, setTrack] = createSignal('—'), [artist, setArtist] = createSignal(''), [art, setArt] = createSignal('');
const [palette, setPalette] = createSignal([]), [connected, setConnected] = createSignal(!!tok());
const clientId = () => client() || SPOTIFY_CLIENT_ID;
let last = '', armed = false;      // armed: the switch was just turned on, so the next cover also selects the effect

async function login() {
  const id = clientId();
  if (!id) { toast('Enter the Spotify client ID first', 'warn'); return; }
  const verifier = [...crypto.getRandomValues(new Uint8Array(48))].map(b => hex2(b)).join('');
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(verifier));
  const challenge = btoa(String.fromCharCode(...new Uint8Array(digest))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  localStorage.setItem('ledify.sp.verifier', verifier);
  const q = new URLSearchParams({ client_id: id, response_type: 'code', redirect_uri: REDIRECT, scope: 'user-read-currently-playing user-read-playback-state', code_challenge_method: 'S256', code_challenge: challenge });
  location.href = 'https://accounts.spotify.com/authorize?' + q;
}
async function token(body) {
  const r = await fetch('https://accounts.spotify.com/api/token', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ client_id: clientId(), ...body }) });
  if (!r.ok) throw new Error('token ' + r.status);
  const t = await r.json(); t.expires_at = Date.now() + t.expires_in * 1000 - 60000;
  if (!t.refresh_token) t.refresh_token = (tok() || {}).refresh_token;
  localStorage.setItem('ledify.sp.token', JSON.stringify(t));
  return t;
}
async function access() {
  let t = tok(); if (!t) return null;
  if (Date.now() > t.expires_at) t = await token({ grant_type: 'refresh_token', refresh_token: t.refresh_token });
  return t.access_token;
}
function toggle() {
  if (tok()) { localStorage.removeItem('ledify.sp.token'); setConnected(false); setSpState('not connected'); setTrack('—'); setArtist(''); setArt(''); setPalette([]); }
  else login();
}
async function poll() {
  if (!tok()) return;
  try {
    const r = await fetch('https://api.spotify.com/v1/me/player/currently-playing', { headers: { Authorization: 'Bearer ' + await access() } });
    if (r.status === 204) { setSpState('nothing playing'); return; }
    if (!r.ok) throw new Error('api ' + r.status);
    const j = await r.json(), it = j.item;
    if (!it) { setSpState('nothing playing'); return; }
    setSpState(j.is_playing ? 'playing' : 'paused');
    setTrack(it.name);
    setArtist((it.artists || []).map(a => a.name).join(', ') + (it.album ? ' · ' + it.album.name : ''));
    const cover = it.album && it.album.images && it.album.images.length ? it.album.images[Math.min(1, it.album.images.length - 1)].url : '';
    if (cover) setArt(cover);
    if (it.id !== last) {
      const changed = last !== '';                               // on the first poll after a load only the cover slot is refreshed
      last = it.id;
      if (cover && auto()) gradientFromArt(cover, armed || (changed && opt.mode() === 'cover'));
      armed = false;
    }
  } catch (e) { setSpState('Spotify: ' + e.message); }
}
// Up to five prominent colours of the cover: skip near-black, near-white and grey pixels, bucket the rest, rank by
// size and saturation, keep ones that differ clearly, order by hue, push each to full intensity, send as a gradient.
function gradientFromArt(url, show) {
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
    setPalette(hexes);
    send('cover ' + hexes.join(','));                               // its own slot: the user's gradient is untouched
    if (show) { opt.setMode('cover'); send('mode cover'); }
  };
  img.onerror = () => toast('Could not load the cover', 'warn');
  img.src = url;
}
async function init() {
  const code = new URLSearchParams(location.search).get('code');
  if (code) {
    try { await token({ grant_type: 'authorization_code', code, redirect_uri: REDIRECT, code_verifier: localStorage.getItem('ledify.sp.verifier') }); setConnected(true); }
    catch (e) { toast('Spotify login failed: ' + e.message, 'warn', 6000); }
    history.replaceState(null, '', location.pathname);
  }
  poll(); setInterval(poll, 5000);
}
export const sp = {
  client, setClient: v => { v = v.trim(); setClientSig(v); localStorage.setItem('ledify.sp.client', v); },
  auto, setAuto: on => { setAutoSig(on); localStorage.setItem('ledify.sp.auto', on ? '1' : '0'); if (on) { last = ''; armed = true; } },
  state: spState, track, artist, art, palette, connected, toggle, init,
};
