// ?demo: a pretend board for trying the panel (or working on it) without hardware or a broker. Commands update a
// state object that is published back the way the real board would; the live stream is a sine wave.
import { ingest, hooks, setUi } from './store';

const st = {
  fw: 'demo', power: 1, volt: 5, mode: 'solid', color: 'solid', bright: 'level', overlay: 'none', auto: 0, level: 160, count: 300,
  type: 'ws2812', cap: 160, gain: 25, lux_lo: 0, lux_hi: 500, night: 0, temp_lo: 15, temp_hi: 30, limit: 0, pdmax: 20000,
  pddelay: 100, droop: 0, tmax: 70, fade: 300, stripe: '0', cover: 'ffffff@0', pd: { attached: 1, ready: 1, mv: 20000, ma: 3000 }, net: 'demo board, nothing is wired',
  pdos: [[5000, 3000], [9000, 3000], [15000, 3000], [20000, 3000]], need_mw: 15000, budget_mw: 51000,
  palette: 'ff3c00@0,ff8c00@35,ff0066@70,4b0082@100', time: '', tz: 'UTC0', name: 'ledify', ip: '', clients: 0, hot: 0,
  board_c: 31.2, sleep: 0, rise: 0, sched: [], segs: [], stream: null, boots: 1, rst: 'poweron', boot_log: [],
  lux: 142, temp_c: 31.2, pressure_pa: 101325, sound_db: 48, db_peak: 52, loud: 0,
};
let color = { r: 255, g: 255, b: 255 };

function pub() {
  ingest('ledify/demo/status', 'online');
  ingest('ledify/demo/state', JSON.stringify(st));
  ingest('ledify/demo/light/state', JSON.stringify({ state: st.power ? 'ON' : 'OFF', brightness: st.level, color, effect: st.mode }));
  ingest('ledify/demo/sensors', JSON.stringify({ lux: st.lux, temp_c: st.temp_c, pressure_kpa: st.pressure_pa / 1000, sound_db: st.sound_db }));
}
function run(cmd) {
  const t = cmd.trim().split(/\s+/), k = t[0];
  let reply = 'ok';
  if (k === 'on') st.power = 1;
  else if (k === 'off') st.power = 0;
  else if (k === 'rgb') { color = { r: +t[1], g: +t[2], b: +t[3] }; st.mode = 'solid'; }
  else if (k === 'mode') st.mode = t[1];
  else if (k === 'palette' || k === 'cover') { st[k] = t[1].split(',').map((s, i, a) => s.includes('@') ? s : s + '@' + Math.round(i * 100 / Math.max(1, a.length - 1))).join(','); if (k === 'palette') st.mode = 'palette'; }
  else if (k === 'seg') {
    if (t[1] === 'off' || t[1] === 'clear') st.segs = [];
    else if (t[1] === 'del') st.segs = st.segs.filter(s => s.i !== +t[2]);
    else if (t.length >= 4) { const i = +t[1]; st.segs = [...st.segs.filter(s => s.i !== i), { i, a: +t[2], b: +t[3], color: t[4] || 'solid', rgb: t[5] || 'ffffff' }].sort((a, b) => a.i - b.i); }
  } else if (k === 'sched') {
    if (t[1] === 'add') st.sched = [...st.sched, { at: t[2], cmd: t.slice(3).join(' ') }];
    else if (t[1] === 'del') st.sched = st.sched.filter((_, i) => i !== +t[2] - 1);
    else if (t[1] === 'clear') st.sched = [];
  } else if (k === 'sleep') { st.sleep = +t[1] * 60; if (st.sleep) st.power = 1; }
  else if (k === 'rise') { st.rise = +t[1] * 60; st.power = 1; }
  else if (k === 'tz') st.tz = t[1];
  else if (k === 'stripe') st.stripe = t[1];
  else if (k === 'name') st.name = t[1];
  else if (k === 'volt' && t[1] === '12' && t[2] !== 'yes') reply = '12 V destroys 5 V strips. If the strip really is 12 V send: volt 12 yes';
  else if (k === 'volt') st.volt = +t[1];
  else if (k === 'lux' && t.length === 3) { st.lux_lo = +t[1]; st.lux_hi = +t[2]; }
  else if (k === 'temp' && t.length === 3) { st.temp_lo = +t[1]; st.temp_hi = +t[2]; }
  else if (k === 'auto') st.auto = t[1] === 'on' ? 1 : 0;
  else if (k === 'overlay') st.overlay = t[1];
  else if (k === 'type') st.type = t[1];
  else if (t.length === 2 && typeof st[k] === 'number') st[k] = +t[1];
  else if (['status', 'live', 'listen', 'audio', 'ota', 'otabase'].includes(k)) { /* nothing to show */ }
  else reply = 'demo: that command is only pretend here';
  st.cap = st.limit ? Math.min(st.level, 120) : st.level;
  pub();
  if (k !== 'status' && k !== 'live' && k !== 'audio') ingest('ledify/demo/reply', reply);
}
export function startDemo() {
  hooks.demo = run;
  pub();
  let t = 0;
  setInterval(() => {
    t += 0.1;
    const w = (f, p = 0) => Math.round(128 + 120 * Math.sin(t * f + p));
    ingest('ledify/demo/live', JSON.stringify({ loud: w(2), bands: [w(3), w(5, 1), w(7, 2)], pulse: Math.sin(t * 2) > 0.95 ? 255 : 0, db: 50 + Math.round(10 * Math.sin(t)), cap: st.cap }));
    if (st.sleep > 0 && (st.sleep = Math.max(0, st.sleep - 0.1)) === 0) { st.power = 0; pub(); }
    if (st.rise > 0) st.rise = Math.max(0, st.rise - 0.1);
  }, 100);
  setInterval(() => { st.time = new Date().toTimeString().slice(0, 5); pub(); }, 5000);
  setUi('conn', 'online');
}
