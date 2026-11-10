// The LED matrix player: draws a video, a picture, a screen or tab, the camera, scrolling text or a test pattern
// into a canvas the size of the board's matrix and sends each frame as raw pixels. Only pixels go to the board, never
// sound: a video's audio plays on this device alone.
//
// A frame is "LM", width, height, then width x height RGB bytes row by row from the top left (firmware/main/matrix.h);
// the board puts each pixel where its wiring says. On the board's own page frames go over its WebSocket on the LAN;
// on the hosted panel they go through the broker, which is slower and adds latency, so the default rate is lower.
import { createSignal } from 'solid-js';
import { state, publishFrame, toast, LOCAL, opt } from './store';
import { act } from './actions';

// Layout bits, as in matrix.h.
export const SERP = 1, COLS = 2, RIGHT = 4, BOTTOM = 8;
// Strip index of (x, y), the same arithmetic as matrix_index() in matrix.c.
export function matrixIndex(w, h, lay, x, y) {
  if (lay & RIGHT) x = w - 1 - x;
  if (lay & BOTTOM) y = h - 1 - y;
  if (lay & COLS) { if ((lay & SERP) && (x & 1)) y = h - 1 - y; return x * h + y; }
  if ((lay & SERP) && (y & 1)) x = w - 1 - x;
  return y * w + x;
}

// The picture size: the board's matrix, or without one a single row as long as the strip (up to 255 pixels).
export const size = () => {
  const s = state();
  if (s.mw > 0 && s.mh > 0) return { w: s.mw, h: s.mh, matrix: true };
  return { w: Math.max(1, Math.min(255, s.count || 60)), h: 1, matrix: false };
};

export const MAX_FPS = 30;
const [source, setSource] = createSignal(null);       // 'video' | 'image' | 'screen' | 'camera' | 'text' | 'test'
const [fps, setFps] = createSignal(LOCAL ? 30 : 12);
const [gain, setGain] = createSignal(60);             // percent of full: the strip's level and power cap still apply
const [gamma, setGamma] = createSignal(true);
const [fit, setFit] = createSignal('cover');          // cover (crop to fill) | contain (whole picture) | stretch
const [mirror, setMirror] = createSignal(false);
const [text, setText] = createSignal('LEDIFY');
const [ink, setInk] = createSignal('#ff8a00');
const [sent, setSent] = createSignal(0);              // frames per second actually sent
const [skipped, setSkipped] = createSignal(0);        // frames held back because the connection was still busy

let stream = null, url = null, timer = null, view = null, textX = 0, frames = 0, skips = 0, countAt = 0;
// The <video> and <img> outlive the lens, so playback carries on with it closed; the lens shows the video's controls.
const video = Object.assign(document.createElement('video'), { controls: true, playsInline: true, className: 'mt-2 w-full rounded-xl' });
const img = Object.assign(document.createElement('img'), { alt: '' });
img.style.cssText = 'position:fixed;left:0;top:0;width:1px;height:1px;opacity:0;pointer-events:none';   // in the page, so GIFs animate
document.body.appendChild(img);
const mid = document.createElement('canvas'), out = document.createElement('canvas');
const midCtx = mid.getContext('2d'), outCtx = out.getContext('2d', { willReadFrequently: true });

// Brightness and gamma in one table: LEDs are linear, pictures are not, so without the curve video looks washed out.
let lut = new Uint8Array(256), lutKey = '';
function table() {
  const key = gain() + '/' + gamma();
  if (key === lutKey) return lut;
  lutKey = key;
  for (let v = 0; v < 256; v++) lut[v] = Math.round(255 * (gamma() ? Math.pow(v / 255, 2.2) : v / 255) * gain() / 100);
  return lut;
}

// Where the source goes: cover crops it to fill, contain letterboxes it, stretch ignores its shape.
function place(sw, sh, dw, dh) {
  if (fit() === 'stretch' || !sw || !sh) return [0, 0, sw, sh, 0, 0, dw, dh];
  const s = fit() === 'cover' ? Math.max(dw / sw, dh / sh) : Math.min(dw / sw, dh / sh);
  const cw = dw / s, ch = dh / s;
  if (fit() === 'cover') return [(sw - cw) / 2, (sh - ch) / 2, cw, ch, 0, 0, dw, dh];
  return [0, 0, sw, sh, (dw - sw * s) / 2, (dh - sh * s) / 2, sw * s, sh * s];
}

// Red along the top, green down the left side, white in the top left corner, blue on the diagonal: whichever way the
// panel is wired, this shows at a glance whether the layout is right.
function testPattern(c, w, h) {
  c.fillStyle = '#000'; c.fillRect(0, 0, w, h);
  c.fillStyle = '#00f'; for (let i = 0; i < Math.min(w, h); i++) c.fillRect(i, i, 1, 1);
  c.fillStyle = '#f00'; c.fillRect(0, 0, w, 1);
  c.fillStyle = '#0f0'; c.fillRect(0, 0, 1, h);
  c.fillStyle = '#fff'; c.fillRect(0, 0, 1, 1);
}

function draw() {
  const { w, h } = size();
  if (out.width !== w || out.height !== h) { out.width = w; out.height = h; }
  outCtx.setTransform(1, 0, 0, 1, 0, 0);
  outCtx.fillStyle = '#000'; outCtx.fillRect(0, 0, w, h);
  const src = source();
  if (src === 'test') testPattern(outCtx, w, h);
  else if (src === 'text') {
    const t = text() || ' ';
    outCtx.font = `bold ${Math.max(6, Math.round(h * 0.9))}px ui-sans-serif, system-ui, sans-serif`;
    outCtx.textBaseline = 'middle';
    const tw = outCtx.measureText(t).width;
    outCtx.fillStyle = ink();
    outCtx.fillText(t, w - textX, h / 2 + 0.5);
    textX += Math.max(1, Math.round(w / 32));                      // about a matrix width every second at 30 fps
    if (textX > w + tw) textX = 0;
  } else {
    const el = src === 'image' ? img : video;
    const sw = el ? (el.videoWidth || el.naturalWidth || 0) : 0, sh = el ? (el.videoHeight || el.naturalHeight || 0) : 0;
    if (!sw || !sh) return outCtx.getImageData(0, 0, w, h);
    // Two steps: a large picture squeezed straight down to a few pixels shimmers, since only a few of its pixels get
    // sampled; going via 8x the matrix averages properly.
    const mw = Math.min(sw, w * 8), mh = Math.min(sh, h * 8);
    if (mid.width !== mw || mid.height !== mh) { mid.width = mw; mid.height = mh; }
    const [sx, sy, sW, sH, dx, dy, dW, dH] = place(sw, sh, mw, mh);
    midCtx.fillStyle = '#000'; midCtx.fillRect(0, 0, mw, mh);
    midCtx.imageSmoothingQuality = 'high';
    midCtx.drawImage(el, sx, sy, sW, sH, dx, dy, dW, dH);
    outCtx.imageSmoothingEnabled = true; outCtx.imageSmoothingQuality = 'high';
    if (mirror()) outCtx.setTransform(-1, 0, 0, 1, w, 0);
    outCtx.drawImage(mid, 0, 0, mw, mh, 0, 0, w, h);
    outCtx.setTransform(1, 0, 0, 1, 0, 0);
  }
  return outCtx.getImageData(0, 0, w, h);
}

function tick() {
  const px = draw(), { width: w, height: h, data } = px, t = table();
  if (view) {                                                        // the preview shows what the strip is sent
    if (view.width !== w || view.height !== h) { view.width = w; view.height = h; }
    view.getContext('2d').putImageData(px, 0, 0);
  }
  const f = new Uint8Array(4 + w * h * 3);
  f[0] = 76; f[1] = 77; f[2] = w; f[3] = h;                          // "LM"
  for (let i = 0, o = 4; i < w * h; i++, o += 3) { f[o] = t[data[i * 4]]; f[o + 1] = t[data[i * 4 + 1]]; f[o + 2] = t[data[i * 4 + 2]]; }
  if (publishFrame(f)) frames++; else skips++;
  const now = performance.now();
  if (now - countAt >= 1000) { setSent(Math.round(frames * 1000 / (now - countAt))); setSkipped(skips); frames = 0; skips = 0; countAt = now; }
}

function run() {
  clearInterval(timer);
  countAt = performance.now(); frames = 0; skips = 0;
  timer = setInterval(tick, Math.round(1000 / fps()));
}

function release() {
  clearInterval(timer); timer = null;
  if (stream) { stream.getTracks().forEach(t => t.stop()); stream = null; }
  if (video) { video.pause(); video.removeAttribute('src'); video.srcObject = null; video.load(); }
  if (url) { URL.revokeObjectURL(url); url = null; }
  if (img) img.removeAttribute('src');
}

function start(kind) {
  if (opt.power() === 0) act.power(true);                           // frames only show while the output is on
  setSource(kind); textX = 0;
  run();
}

export const player = {
  source, fps, gain, gamma, fit, mirror, text, ink, sent, skipped,
  setGain, setGamma, setFit, setMirror, setText, setInk,
  setFps(v) { setFps(Math.max(1, Math.min(MAX_FPS, v))); if (timer) run(); },
  // The lens: where the video's controls go, and the preview canvas (null when it closes).
  attach(host, c) { if (host) host.appendChild(video); view = c; },
  stop() { release(); setSource(null); setSent(0); },
  playFile(file) {
    if (!file) return;
    release();
    url = URL.createObjectURL(file);
    if (file.type.startsWith('image/')) { img.src = url; start('image'); return; }
    video.srcObject = null; video.src = url; video.loop = true; video.muted = false;
    video.play().catch(() => { video.muted = true; video.play().catch(() => {}); });
    start('video');
  },
  async screen() {
    try {
      release();
      stream = await navigator.mediaDevices.getDisplayMedia({ video: { frameRate: MAX_FPS }, audio: false });
      stream.getVideoTracks()[0].addEventListener('ended', () => player.stop());   // the browser's own "Stop sharing"
      video.srcObject = stream; video.muted = true; await video.play();
      setMirror(false); start('screen');
    } catch (e) { if (e.name !== 'NotAllowedError') toast('Screen: ' + e.message, 'warn'); player.stop(); }
  },
  async camera() {
    try {
      release();
      stream = await navigator.mediaDevices.getUserMedia({ video: { width: 640, height: 480, frameRate: MAX_FPS }, audio: false });
      video.srcObject = stream; video.muted = true; await video.play();
      setMirror(true); start('camera');
    } catch (e) { toast('Camera: ' + e.message, 'warn'); player.stop(); }
  },
  showText() { release(); start('text'); },
  test() { release(); start('test'); },
};
