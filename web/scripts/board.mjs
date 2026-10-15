// After "vite build --mode board": gzip what the firmware embeds and add the icon and manifest.
import { readFileSync, writeFileSync, unlinkSync, copyFileSync, readdirSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
const dir = new URL('../../firmware/main/panel/', import.meta.url).pathname;
for (const f of ['index.html', 'app.js', 'app.css']) {
  writeFileSync(dir + f + '.gz', gzipSync(readFileSync(dir + f), { level: 9 }));
  unlinkSync(dir + f);
}
for (const f of ['icon.svg', 'manifest.webmanifest']) copyFileSync(new URL('../public/' + f, import.meta.url).pathname, dir + f);
console.log('board panel:', readdirSync(dir).join(', '));
