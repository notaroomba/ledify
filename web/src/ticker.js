// One animation loop for every small preview, paused while the tab is hidden.
const subs = new Set();
let raf = 0;
const t0 = performance.now();
const loop = () => { raf = subs.size ? requestAnimationFrame(loop) : 0; if (!document.hidden) { const t = (performance.now() - t0) / 1000; subs.forEach(f => f(t)); } };
export function onTick(f) { subs.add(f); if (!raf) raf = requestAnimationFrame(loop); return () => subs.delete(f); }
