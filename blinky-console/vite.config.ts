import { defineConfig, loadEnv } from 'vite';
import react from '@vitejs/plugin-react';
import { VitePWA } from 'vite-plugin-pwa';

export default defineConfig(({ mode }) => {
  // The dev proxy's blinky-server: a local one unless BLINKY_DEV_PROXY_TARGET (the environment, or
  // the gitignored .env.local; template .env.example) names another. This repo is public, so a
  // real host never goes in this file. The BLINKY_ prefix keeps it out of the client bundle.
  const env = loadEnv(mode, process.cwd(), 'BLINKY_');
  const proxyTarget = (env.BLINKY_DEV_PROXY_TARGET || 'http://localhost:8420').replace(/\/+$/, '');
  return {
    plugins: [
      react(),
      VitePWA({
        // Single-installation kiosk stack — we never want the console cached.
        // `selfDestroying` generates a service worker that unregisters itself
        // and wipes every cache on the next visit, so any kiosk that installed
        // an older precaching SW cleans itself up without manual intervention.
        // Manifest is still emitted for "Add to Home Screen" metadata only.
        selfDestroying: true,
        registerType: 'autoUpdate',
        includeAssets: [
          'favicon.ico',
          'favicon-16x16.png',
          'favicon-32x32.png',
          'apple-touch-icon.png',
        ],
        manifest: {
          name: 'Blinky Console',
          short_name: 'Blinky',
          description: 'WebSerial control interface for Blinky Time LED controller',
          theme_color: '#1a1a2e',
          background_color: '#1a1a2e',
          display: 'standalone',
          orientation: 'portrait',
          scope: '/',
          start_url: '/',
          categories: ['utilities', 'productivity'],
          icons: [
            {
              src: 'pwa-192x192.png',
              sizes: '192x192',
              type: 'image/png',
            },
            {
              src: 'pwa-512x512.png',
              sizes: '512x512',
              type: 'image/png',
            },
            {
              src: 'pwa-maskable-192x192.png',
              sizes: '192x192',
              type: 'image/png',
              purpose: 'maskable',
            },
            {
              src: 'pwa-maskable-512x512.png',
              sizes: '512x512',
              type: 'image/png',
              purpose: 'maskable',
            },
          ],
        },
        devOptions: {
          enabled: false,
        },
      }),
    ],
    // Served same-origin behind the LemonCart Caddy at /devices/*, which
    // reverse-proxies to blinky-server. Assets are referenced as /devices/assets/…
    // so Caddy's handle_path strip resolves them back to blinky-server's root.
    base: '/devices/',
    // Build output goes into blinky-server/web/ so a single server process
    // serves both the API (port 8420) and the SPA. See docs/DEVELOPMENT.md
    // for the dev workflow.
    build: {
      outDir: '../blinky-server/web',
      emptyOutDir: true,
    },
    server: {
      port: 3000,
      // Proxy API + WebSocket traffic to blinky-server during dev (local by
      // default, BLINKY_DEV_PROXY_TARGET above). Keeps the console at :3000
      // (with HMR + WebSerial) while routing server-backed requests through.
      // Skip the proxies if you run the console without a blinky-server —
      // WebSerial paths still work.
      proxy: {
        '/api': proxyTarget,
        '/ws': { target: proxyTarget.replace(/^http/, 'ws'), ws: true },
      },
    },
  };
});
