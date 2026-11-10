// The hosted panel's server: serves the built panel and the firmware image, and (when OWNER_PASS and the broker admin
// login are set) handles who may use which board. Without them it behaves like the old Caddy setup: the broker login
// goes into config.js and PANEL_USER/PANEL_PASS put a basic-auth login in front of the page.
//
// With accounts on:
//   - every browser gets a device cookie on its first visit; it remembers the boards that browser was linked to
//   - a browser is linked to a board in person (the board's setup window code, from its own page or Bluetooth) or by
//     a share link the owner made; the owner logs in with OWNER_PASS ("remember me" cookie) and sees every board
//   - the browser never holds a long-lived broker password: /api/broker makes it a broker account (Mosquitto dynamic
//     security) that may only touch its own boards' topics, and each board gets an account for its own topics only
//
// Environment: PORT, STATIC_DIR, DATA_DIR, OWNER_PASS, BROKER_ADMIN_URL/USER/PASS (the internal broker address and the
// dynamic security admin), MQTT_WS_URL (what browsers connect to), BOARD_MQTT_URL (what boards are told), HA_MQTT_USER/
// HA_MQTT_PASS (Home Assistant's account), LEGACY_MQTT_USER/LEGACY_MQTT_PASS (the old shared login, kept while boards
// move over), MQTT_USER/MQTT_PASS and PANEL_USER/PANEL_PASS (old mode only), SPOTIFY_CLIENT_ID.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import mqtt from 'mqtt';

const env = process.env;
const here = path.dirname(fileURLToPath(import.meta.url));
const STATIC = path.resolve(env.STATIC_DIR || path.join(here, '../dist'));
const DATA = path.resolve(env.DATA_DIR || '/data');
const AUTH = !!(env.OWNER_PASS && env.BROKER_ADMIN_USER && env.BROKER_ADMIN_PASS);
const SECURE = env.INSECURE_COOKIES ? '' : '; Secure';
const DAY = 864e5;
const DEVICE_DAYS = 365, OWNER_DAYS = 90, SHARE_HOURS = 24, PAIR_MS = 120e3;

const sha = s => crypto.createHash('sha256').update(s).digest('hex');
const token = (n = 32) => crypto.randomBytes(n).toString('base64url');
const same = (a, b) => { const x = Buffer.from(String(a)), y = Buffer.from(String(b)); return x.length === y.length && crypto.timingSafeEqual(x, y); };
const BOARD_ID = /^ledify_[0-9a-f]{6}$/;
const log = (...a) => console.log(new Date().toISOString(), ...a);

// ---------- stored state: one small JSON file on the Railway volume ----------
const FILE = path.join(DATA, 'ledify.json');
let db = { devices: {}, owners: {}, boards: {}, shares: {}, pw: {} };
if (AUTH) {
  try { db = { ...db, ...JSON.parse(fs.readFileSync(FILE, 'utf8')) }; } catch (e) { if (e.code !== 'ENOENT') throw e; }
}
let saveTimer = null;
function save() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    fs.mkdirSync(DATA, { recursive: true });
    fs.writeFileSync(FILE + '.tmp', JSON.stringify(db, null, 1));
    fs.renameSync(FILE + '.tmp', FILE);
  }, 200);
}
function sweep() {
  const now = Date.now();
  for (const [k, v] of Object.entries(db.owners)) if (v.exp < now) delete db.owners[k];
  for (const [k, v] of Object.entries(db.shares)) if (v.exp < now) delete db.shares[k];
  for (const [k, d] of Object.entries(db.devices)) if (d.seen + DEVICE_DAYS * DAY < now) { dropClients(d); delete db.devices[k]; }
  save();
}

// ---------- Mosquitto dynamic security ----------
const CONTROL = '$CONTROL/dynamic-security/v1';
let admin = null, adminUp = false;
const waiting = new Map();
const pairs = new Map();          // board id -> { code, at } from ledify/<id>/pair
const names = new Map();          // board id -> its name, from its state

function dynsec(commands) {
  if (!adminUp) return Promise.reject(new Error('broker admin not connected'));
  const corr = token(8);
  commands.forEach((c, i) => { c.correlationData = corr + ':' + i; });
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => { waiting.delete(corr); reject(new Error('broker admin timed out')); }, 8000);
    waiting.set(corr, r => { clearTimeout(t); resolve(r); });
    admin.publish(CONTROL, JSON.stringify({ commands }));
  });
}
// Errors that just mean "already so" are fine when making things idempotent.
const settled = rs => rs.filter(r => r.error && !/exist|not found/i.test(r.error));
async function run(commands) {
  const rs = await dynsec(commands);
  const bad = settled(rs);
  if (bad.length) log('dynsec:', bad.map(r => r.command + ': ' + r.error).join('; '));
  return rs;
}
const acl = (acltype, topic, allow = true, priority = 0) => ({ acltype, topic, allow, priority });
const noPair = t => acl('publishClientReceive', t, false, 10);       // the setup code is for the server only
function boardAcls(id) {
  return [acl('publishClientSend', `ledify/${id}/#`), acl('subscribeLiteral', `ledify/${id}/cmd`),
    acl('subscribePattern', `ledify/${id}/+/set`), acl('publishClientSend', 'homeassistant/#')];
}
function webAcls(boards) {
  return boards.flatMap(id => [acl('subscribePattern', `ledify/${id}/#`), acl('publishClientSend', `ledify/${id}/cmd`),
    acl('publishClientSend', `ledify/${id}/audio/set`), acl('publishClientSend', `ledify/${id}/frame/set`), noPair(`ledify/${id}/pair`)]);
}
const OWNER_ACLS = [acl('subscribePattern', 'ledify/#'), acl('publishClientSend', 'ledify/+/cmd'),
  acl('publishClientSend', 'ledify/+/audio/set'), acl('publishClientSend', 'ledify/+/frame/set'), noPair('ledify/+/pair')];
const FULL_ACLS = [acl('subscribePattern', 'ledify/#'), acl('subscribePattern', 'homeassistant/#'),
  acl('publishClientSend', 'ledify/#'), acl('publishClientSend', 'homeassistant/#'), noPair('ledify/+/pair')];
const SERVER_ACLS = [acl('subscribePattern', 'ledify/+/pair'), acl('subscribePattern', 'ledify/+/state'),
  acl('publishClientSend', 'ledify/+/cmd')];

// Changing a client's roles (or the default access) makes Mosquitto drop that client, so only change what differs.
const aclKey = a => [a.acltype, a.topic, !!a.allow, a.priority || 0].join(' ');
async function putRole(rolename, acls) {
  const [r] = await run([{ command: 'getRole', rolename }]);
  if (r.error) return run([{ command: 'createRole', rolename, acls }]);
  const have = new Map((r.data.role.acls || []).map(a => [aclKey(a), a])), want = new Map(acls.map(a => [aclKey(a), a]));
  const ops = [
    ...[...have].filter(([k]) => !want.has(k)).map(([, a]) => ({ command: 'removeRoleACL', rolename, acltype: a.acltype, topic: a.topic })),
    ...[...want].filter(([k]) => !have.has(k)).map(([, a]) => ({ command: 'addRoleACL', rolename, ...a })),
  ];
  if (ops.length) await run(ops);
}
// Setting a password also drops that user's connections, so it is only sent when it changed (the broker never shows
// passwords back; db.pw keeps a hash of the last one set). Otherwise every panel restart kicked the boards.
async function putClient(username, password, rolename) {
  const [c] = await run([{ command: 'getClient', username }]);
  const h = password && sha(username + '\n' + password);
  if (c.error) {
    await run([{ command: 'createClient', username, password, roles: [{ rolename }] }]);
  } else {
    const ops = password && db.pw[username] !== h ? [{ command: 'setClientPassword', username, password }] : [];
    if (!(c.data.client.roles || []).some(r => r.rolename === rolename)) ops.push({ command: 'addClientRole', username, rolename });
    if (ops.length) await run(ops);
  }
  if (h && !username.startsWith('web_') && !username.startsWith('owner_')) { db.pw[username] = h; save(); }   // browsers' logins are one-off
}
async function putBoard(id) {
  const b = db.boards[id];
  await putRole('board_' + id, boardAcls(id));
  await putClient(id, b.pass, 'board_' + id);
}
// Everything the broker should know, after it (or this server) restarts. Browsers' accounts are made on demand.
async function reconcile() {
  const want = { publishClientSend: false, publishClientReceive: true, subscribe: false, unsubscribe: true };
  const [da] = await run([{ command: 'getDefaultACLAccess' }]);
  const have = Object.fromEntries(((da.data || {}).acls || []).map(a => [a.acltype, a.allow]));
  if (Object.entries(want).some(([k, v]) => have[k] !== v))
    await run([{ command: 'setDefaultACLAccess', acls: Object.entries(want).map(([acltype, allow]) => ({ acltype, allow })) }]);
  await putRole('ledify_server', SERVER_ACLS);
  await putClient(env.BROKER_ADMIN_USER, null, 'ledify_server');   // may drop this connection once; the next one finds it done
  await putRole('ledify_owner', OWNER_ACLS);
  await putRole('ledify_full', FULL_ACLS);
  if (env.HA_MQTT_USER && env.HA_MQTT_PASS) await putClient(env.HA_MQTT_USER, env.HA_MQTT_PASS, 'ledify_full');
  if (env.LEGACY_MQTT_USER && env.LEGACY_MQTT_PASS) await putClient(env.LEGACY_MQTT_USER, env.LEGACY_MQTT_PASS, 'ledify_full');
  for (const id of Object.keys(db.boards)) await putBoard(id);
  admin.subscribe(['ledify/+/pair', 'ledify/+/state']);
  log('broker accounts in place:', Object.keys(db.boards).length, 'boards');
}
function startAdmin() {
  admin = mqtt.connect(env.BROKER_ADMIN_URL || 'mqtt://mosquitto.railway.internal:1883', {
    username: env.BROKER_ADMIN_USER, password: env.BROKER_ADMIN_PASS, clientId: 'ledify-server', reconnectPeriod: 3000 });
  admin.on('connect', () => {
    adminUp = true;
    admin.subscribe(CONTROL + '/response', () => reconcile().catch(e => log('reconcile failed:', e.message)));
  });
  admin.on('close', () => { adminUp = false; });
  admin.on('error', e => log('broker admin:', e.message));
  admin.on('message', (topic, buf) => {
    if (topic === CONTROL + '/response') {
      let j; try { j = JSON.parse(buf); } catch (e) { return; }
      const rs = j.responses || [], corr = String((rs[0] || {}).correlationData || '').split(':')[0];
      const done = waiting.get(corr);
      if (done) { waiting.delete(corr); done(rs); }
      return;
    }
    const m = topic.match(/^ledify\/([^/]+)\/(pair|state)$/);
    if (!m || !BOARD_ID.test(m[1])) return;
    if (m[2] === 'pair') pairs.set(m[1], { code: buf.toString().trim(), at: Date.now() });
    else { try { const n = JSON.parse(buf).name; if (n) names.set(m[1], String(n).slice(0, 32)); } catch (e) { /* not JSON */ } }
  });
}

// A browser's broker accounts: one per tab session, all with the device's role. Changing what a device may see
// deletes them, which drops their connections; the panel then asks /api/broker again and gets the new rules.
function dropClients(d) {
  const users = (d.clients || []).map(c => c.user);
  d.clients = [];
  if (users.length && adminUp) run(users.map(username => ({ command: 'deleteClient', username }))).catch(() => {});
}
function grant(d, board) {
  if (!d.boards.includes(board)) d.boards.push(board);
  dropClients(d);
  save();
}
async function ensureBoard(id) {                  // a board's own account; the first link makes it
  if (db.boards[id]) return db.boards[id];
  db.boards[id] = { pass: token(24), created: Date.now() };
  save();
  await putBoard(id);
  return db.boards[id];
}
const boardLine = id => `mqtt ${env.BOARD_MQTT_URL || env.MQTT_WS_URL} ${id} ${db.boards[id].pass}`;

// ---------- HTTP ----------
const cookies = req => Object.fromEntries((req.headers.cookie || '').split(/;\s*/).filter(Boolean).map(c => {
  const i = c.indexOf('='); return [c.slice(0, i), decodeURIComponent(c.slice(i + 1))];
}));
const setCookie = (name, value, days) => `${name}=${value}; Path=/; Max-Age=${Math.round(days * 86400)}; HttpOnly; SameSite=Lax${SECURE}`;
function json(res, code, body, extra = {}) {
  res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', ...extra });
  res.end(JSON.stringify(body));
}
function readBody(req) {
  return new Promise((resolve, reject) => {
    let s = '';
    req.on('data', c => { s += c; if (s.length > 4096) { reject(new Error('too big')); req.destroy(); } });
    req.on('end', () => { try { resolve(s ? JSON.parse(s) : {}); } catch (e) { reject(e); } });
  });
}
// Simple per-IP limit for the endpoints someone could guess at.
const hits = new Map();
function limited(req, key, max) {
  const ip = String(req.headers['x-forwarded-for'] || req.socket.remoteAddress).split(',')[0].trim();
  const k = key + ip, now = Date.now(), h = (hits.get(k) || []).filter(t => now - t < 60e3);
  h.push(now); hits.set(k, h);
  return h.length > max;
}
// The device behind this request (made on first sight) and whether the owner is logged in on it.
function who(req, res) {
  const c = cookies(req), out = [];
  let key = c.ld && db.devices[sha(c.ld)] ? sha(c.ld) : null;
  if (!key) {
    const t = token(); key = sha(t);
    db.devices[key] = { created: Date.now(), seen: 0, label: '', boards: [], clients: [] };
    out.push(setCookie('ld', t, DEVICE_DAYS));
  } else if (Date.now() - db.devices[key].seen > DAY) out.push(setCookie('ld', c.ld, DEVICE_DAYS));   // renew
  const d = db.devices[key];
  d.seen = Date.now();
  const o = c.lo && db.owners[sha(c.lo)];
  const owner = !!(o && o.exp > Date.now());
  if (owner && o.exp - Date.now() < (OWNER_DAYS - 1) * DAY) { o.exp = Date.now() + OWNER_DAYS * DAY; out.push(setCookie('lo', c.lo, OWNER_DAYS)); }
  if (out.length) res.setHeader('Set-Cookie', out);
  save();
  return { key, d, owner, c };
}
const OWNER_HASH = AUTH ? crypto.scryptSync(env.OWNER_PASS, 'ledify-owner', 32) : null;

async function api(req, res, url) {
  if (req.method !== 'GET' && req.headers['x-ledify'] !== '1') return json(res, 403, { error: 'missing header' });   // no cross-site posts
  const { key, d, owner, c } = who(req, res);
  const body = req.method === 'POST' ? await readBody(req) : {};
  const label = s => String(s || '').replace(/[^\p{L}\p{N} '._-]/gu, '').slice(0, 40);
  switch (url.pathname) {
    case '/api/me':
      return json(res, 200, { owner, boards: owner ? [...new Set([...Object.keys(db.boards), ...d.boards])] : d.boards, label: d.label });
    case '/api/login': {
      if (limited(req, 'login', 5)) return json(res, 429, { error: 'too many tries, wait a minute' });
      const ok = crypto.timingSafeEqual(crypto.scryptSync(String(body.password || ''), 'ledify-owner', 32), OWNER_HASH);
      if (!ok) return json(res, 401, { error: 'wrong password' });
      const t = token();
      db.owners[sha(t)] = { exp: Date.now() + (body.remember === false ? 1 : OWNER_DAYS) * DAY };
      dropClients(d); save();
      return json(res, 200, { ok: true }, { 'Set-Cookie': setCookie('lo', t, body.remember === false ? 1 : OWNER_DAYS) });
    }
    case '/api/logout':
      if (c.lo) delete db.owners[sha(c.lo)];
      dropClients(d); save();
      return json(res, 200, { ok: true }, { 'Set-Cookie': setCookie('lo', '', 0) });
    case '/api/label':
      d.label = label(body.label); save();
      return json(res, 200, { ok: true });
    case '/api/broker': {                       // a fresh broker login for this tab, limited to this browser's boards
      const boards = owner ? null : d.boards;
      if (!owner && !boards.length) return json(res, 200, { boards: [] });
      const user = (owner ? 'owner_' : 'web_') + key.slice(0, 12) + '_' + token(3), pass = token(24), role = 'web_' + key.slice(0, 16);
      try {
        if (!owner) await putRole(role, webAcls(boards));
        await putClient(user, pass, owner ? 'ledify_owner' : role);
      } catch (e) { return json(res, 503, { error: e.message }); }
      d.clients = [...(d.clients || []), { user, at: Date.now() }].slice(-8);
      save();
      return json(res, 200, { url: env.MQTT_WS_URL, user, pass, owner, boards: owner ? [...new Set([...Object.keys(db.boards), ...d.boards])] : boards });
    }
    case '/api/link': {                         // in person: the code the board shows while its setup window is open
      if (limited(req, 'link', 10)) return json(res, 429, { error: 'too many tries, wait a minute' });
      const id = String(body.board || ''), p = pairs.get(id);
      if (!BOARD_ID.test(id) || !p || Date.now() - p.at > PAIR_MS || !same(p.code, String(body.code || '').toLowerCase()))
        return json(res, 403, { error: 'that code is wrong or expired: hold the board\'s button 3 s and try again' });
      pairs.delete(id);
      if (body.label) d.label = label(body.label);
      const fresh = !db.boards[id];
      try { await ensureBoard(id); } catch (e) { return json(res, 503, { error: e.message }); }
      if (fresh) admin.publish(`ledify/${id}/cmd`, boardLine(id));    // still on the old shared login: give it its own
      grant(d, id);
      log('linked', id, 'to device', key.slice(0, 8), fresh ? '(new board account)' : '');
      return json(res, 200, { ok: true, board: id });
    }
    case '/api/provision': {                    // owner, over Bluetooth: a new board's broker line before it is online
      if (!owner) return json(res, 403, { error: 'log in first' });
      const id = String(body.board || '');
      if (!BOARD_ID.test(id)) return json(res, 400, { error: 'bad board id' });
      try { await ensureBoard(id); } catch (e) { return json(res, 503, { error: e.message }); }
      grant(d, id);
      return json(res, 200, { line: boardLine(id) });
    }
    case '/api/share': {
      if (!owner) return json(res, 403, { error: 'log in first' });
      const id = String(body.board || '');
      if (!db.boards[id]) return json(res, 404, { error: 'link that board first' });
      const t = token(18);
      db.shares[sha(t)] = { board: id, exp: Date.now() + SHARE_HOURS * 3600e3 };
      save();
      return json(res, 200, { token: t, hours: SHARE_HOURS });
    }
    case '/api/join': {
      if (limited(req, 'join', 10)) return json(res, 429, { error: 'too many tries, wait a minute' });
      const k = sha(String(body.token || '')), s = db.shares[k];
      if (!s || s.exp < Date.now()) return json(res, 403, { error: 'that share link is used up or expired' });
      delete db.shares[k];
      if (body.label) d.label = label(body.label);
      grant(d, s.board);
      return json(res, 200, { ok: true, board: s.board });
    }
    case '/api/access': {                       // owner: who can use what
      if (!owner) return json(res, 403, { error: 'log in first' });
      const boards = Object.keys(db.boards).map(id => ({
        id, name: names.get(id) || '', created: db.boards[id].created,
        devices: Object.entries(db.devices).filter(([, v]) => v.boards.includes(id))
          .map(([k, v]) => ({ device: k.slice(0, 16), label: v.label, seen: v.seen, you: k === key })),
      }));
      return json(res, 200, { boards });
    }
    case '/api/revoke': {                       // owner (or a device leaving a board itself)
      const id = String(body.board || ''), dk = body.device ? String(body.device) : key.slice(0, 16);
      if (!owner && dk !== key.slice(0, 16)) return json(res, 403, { error: 'log in first' });
      for (const [k, v] of Object.entries(db.devices)) {
        if (k.startsWith(dk) && dk.length === 16) { v.boards = v.boards.filter(b => b !== id); dropClients(v); }
      }
      save();
      return json(res, 200, { ok: true });
    }
    case '/api/forget': {                       // owner: delete a board's account; it must be linked again in person
      if (!owner) return json(res, 403, { error: 'log in first' });
      const id = String(body.board || '');
      if (!db.boards[id]) return json(res, 404, { error: 'no such board' });
      delete db.boards[id];
      for (const v of Object.values(db.devices)) if (v.boards.includes(id)) { v.boards = v.boards.filter(b => b !== id); dropClients(v); }
      save();
      run([{ command: 'deleteClient', username: id }, { command: 'deleteRole', rolename: 'board_' + id }]).catch(() => {});
      return json(res, 200, { ok: true });
    }
  }
  return json(res, 404, { error: 'no such endpoint' });
}

// ---------- static files ----------
const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml',
  '.png': 'image/png', '.json': 'application/json', '.webmanifest': 'application/manifest+json', '.bin': 'application/octet-stream' };
const gz = new Map();
function configJs() {
  const unset = v => (v ? String(v) : '');
  const cfg = AUTH
    ? { auth: true, mqttUrl: env.MQTT_WS_URL || '', boardMqtt: env.BOARD_MQTT_URL || env.MQTT_WS_URL || '', spotifyClientId: unset(env.SPOTIFY_CLIENT_ID) }
    : { mqttUrl: env.MQTT_WS_URL || 'wss://mosquitto-production-595e.up.railway.app', mqttUser: env.MQTT_USER || 'ledify', mqttPass: unset(env.MQTT_PASS),
      boardMqtt: env.BOARD_MQTT_URL || 'mqtt://zephyr.proxy.rlwy.net:52505', spotifyClientId: unset(env.SPOTIFY_CLIENT_ID) };
  return 'window.LEDIFY_CONFIG = ' + JSON.stringify(cfg) + ';\n';
}
function serveFile(req, res, url) {
  let p = decodeURIComponent(url.pathname);
  if (p === '/') p = '/index.html';
  const fresh = ['/index.html', '/config.js', '/sw.js'].includes(p) || p.startsWith('/firmware/');
  const headers = { 'Cache-Control': p.startsWith('/assets/') ? 'public, max-age=31536000, immutable' : fresh ? 'no-cache' : 'public, max-age=3600',
    'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer' };
  if (p === '/config.js') {
    res.writeHead(200, { ...headers, 'Content-Type': 'text/javascript' });
    return res.end(configJs());
  }
  const file = path.join(STATIC, path.normalize(p));
  if (!file.startsWith(STATIC + path.sep)) { res.writeHead(400); return res.end(); }
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) { res.writeHead(404, { 'Content-Type': 'text/plain' }); return res.end('not found'); }
    const type = TYPES[path.extname(file)] || 'application/octet-stream';
    headers['Content-Type'] = type;
    if (/text|javascript|json|svg|manifest/.test(type) && /gzip/.test(req.headers['accept-encoding'] || '') && st.size < 4e6) {
      const k = file + st.mtimeMs;
      if (!gz.has(k)) gz.set(k, zlib.gzipSync(fs.readFileSync(file)));
      res.writeHead(200, { ...headers, 'Content-Encoding': 'gzip', Vary: 'Accept-Encoding' });
      return res.end(req.method === 'HEAD' ? undefined : gz.get(k));
    }
    res.writeHead(200, { ...headers, 'Content-Length': st.size });
    if (req.method === 'HEAD') return res.end();
    fs.createReadStream(file).pipe(res);
  });
}
// Old mode's page login (everything except /firmware/*, which boards fetch without credentials).
function basicOk(req) {
  if (!env.PANEL_PASS) return true;
  const m = (req.headers.authorization || '').match(/^Basic (.+)$/);
  if (!m) return false;
  const [u, ...rest] = Buffer.from(m[1], 'base64').toString().split(':');
  return same(u, env.PANEL_USER || 'ledify') && same(rest.join(':'), env.PANEL_PASS);
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  try {
    if (AUTH && url.pathname.startsWith('/api/')) return await api(req, res, url);
    if (!AUTH && !url.pathname.startsWith('/firmware/') && !basicOk(req)) {
      res.writeHead(401, { 'WWW-Authenticate': 'Basic realm="ledify"' });
      return res.end();
    }
    if (req.method !== 'GET' && req.method !== 'HEAD') { res.writeHead(405); return res.end(); }
    serveFile(req, res, url);
  } catch (e) {
    log('error', url.pathname, e.message);
    if (!res.headersSent) json(res, 400, { error: 'bad request' });
  }
});

if (AUTH) { startAdmin(); setInterval(sweep, 3600e3); }
server.listen(Number(env.PORT) || 3000, () => log(`panel on :${env.PORT || 3000}, accounts ${AUTH ? 'on' : 'off (old shared login)'}`));
