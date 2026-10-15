// Installable panel: hashed assets are cached on first use, the page and config come from the network with the
// cache as a fallback, so the app opens offline (the strip still needs the broker).
const CACHE = 'ledify-v1';
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', e => {
  const u = new URL(e.request.url);
  if (e.request.method !== 'GET' || u.origin !== location.origin) return;
  if (u.pathname.startsWith('/assets/')) {
    e.respondWith(caches.open(CACHE).then(async c => (await c.match(e.request)) || fetch(e.request).then(r => { c.put(e.request, r.clone()); return r; })));
  } else if (['/', '/index.html', '/config.js', '/manifest.webmanifest'].includes(u.pathname) || u.pathname.startsWith('/icon')) {
    e.respondWith(fetch(e.request).then(r => { caches.open(CACHE).then(c => c.put(e.request, r.clone())); return r; }).catch(() => caches.match(e.request)));
  }
});
