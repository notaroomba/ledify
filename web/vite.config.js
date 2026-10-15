import { defineConfig } from 'vite';
import solid from 'vite-plugin-solid';

// Two builds: the hosted panel (hashed assets in dist/) and the copy the board carries itself (fixed names,
// written into the firmware tree and gzipped by scripts/board.mjs).
export default defineConfig(({ mode }) => mode === 'board'
  ? {
    plugins: [solid()],
    publicDir: false,
    build: {
      target: 'es2020', sourcemap: false, outDir: '../firmware/main/panel', emptyOutDir: true,
      rollupOptions: { output: { entryFileNames: 'app.js', assetFileNames: 'app.[ext]', inlineDynamicImports: true } },
    },
  }
  : { plugins: [solid()], build: { target: 'es2020', sourcemap: false } });
