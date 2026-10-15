// Listen: the board streams 16 kHz 16-bit PCM while a lease is active.
// Feed: analyse a file or this device's mic and send "loud bass mid treble pulse" to the board 20 times a second.
import { createSignal } from 'solid-js';
import { send, publishRaw, setUi, toast, hooks } from './store';

const [on, setOn] = createSignal(false), [lstate, setLstate] = createSignal('off'), [vol, setVol] = createSignal(40);
let lctx, lnext = 0, ltimer;
hooks.audio = buf => {
  if (!on()) return;
  const pcm = new Int16Array(buf.buffer, buf.byteOffset, buf.length >> 1);
  const ab = lctx.createBuffer(1, pcm.length, 16000), ch = ab.getChannelData(0), g = vol() / 32768;
  for (let i = 0; i < pcm.length; i++) ch[i] = Math.max(-1, Math.min(1, pcm[i] * g));
  const src = lctx.createBufferSource(); src.buffer = ab; src.connect(lctx.destination);
  const t = Math.max(lnext, lctx.currentTime + 0.08);
  src.start(t); lnext = t + ab.duration;
  setLstate('playing');
};
function toggle() {
  if (on()) { setOn(false); clearInterval(ltimer); send('listen 0'); setLstate('off'); return; }
  if (!lctx) lctx = new (window.AudioContext || window.webkitAudioContext)();
  if (lctx.state === 'suspended') lctx.resume();
  setOn(true); lnext = 0; send('listen 60'); ltimer = setInterval(() => send('listen 60'), 45000);
  setLstate('waiting for audio');
}
export const listen = { on, state: lstate, vol, setVol, toggle };

const [active, setActive] = createSignal(false), [sens, setSens] = createSignal(4);
let actx, analyser, asrc, atimer, aelem, apeak = 1, bpeak = [1, 1, 1], aavg = 0, apulse = 0;
function actxInit() { if (!actx) actx = new (window.AudioContext || window.webkitAudioContext)(); if (actx.state === 'suspended') actx.resume(); }
function astart(node) {
  analyser = actx.createAnalyser(); analyser.fftSize = 1024; analyser.smoothingTimeConstant = 0.4;
  node.connect(analyser);
  const td = new Uint8Array(analyser.fftSize), fd = new Uint8Array(analyser.frequencyBinCount);
  const hz = actx.sampleRate / analyser.fftSize, edges = [20, 250, 2000, 8000].map(f => Math.round(f / hz));
  clearInterval(atimer);
  atimer = setInterval(() => {
    analyser.getByteTimeDomainData(td); analyser.getByteFrequencyData(fd);
    let acc = 0; for (const x of td) { const v = (x - 128) / 128; acc += v * v; }
    const rms = Math.sqrt(acc / td.length) * sens();
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
    publishRaw('audio/set', loud + ' ' + bands.join(' ') + ' ' + apulse);
    setUi('live', { loud, bands, pulse: apulse });
  }, 50);
  setActive(true);
}
function stop() {
  clearInterval(atimer); atimer = null;
  if (asrc) { try { asrc.disconnect(); if (asrc.stream) asrc.stream.getTracks().forEach(t => t.stop()); } catch (e) { /* already gone */ } asrc = null; }
  if (aelem) aelem.pause();
  setActive(false);
}
function playFile(file) {
  if (!file) { toast('Pick an audio file first', 'warn'); return; }
  actxInit(); stop();
  aelem.src = URL.createObjectURL(file);
  if (!aelem.node) aelem.node = actx.createMediaElementSource(aelem);   // one source per element, ever
  asrc = aelem.node; asrc.connect(actx.destination); astart(asrc); aelem.play();
}
async function mic() {
  try { actxInit(); stop(); const st = await navigator.mediaDevices.getUserMedia({ audio: true }); asrc = actx.createMediaStreamSource(st); asrc.stream = st; astart(asrc); }
  catch (e) { toast('Microphone: ' + e.message, 'warn'); }
}
export const feed = { active, sens, setSens, playFile, mic, stop, attach: el => { aelem = el; } };
