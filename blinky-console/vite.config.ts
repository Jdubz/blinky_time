import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { VitePWA } from 'vite-plugin-pwa';

export default defineConfig({
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
    // Proxy API + WebSocket traffic to a local blinky-server during dev.
    // Keeps the console at :3000 (with HMR + WebSerial) while routing
    // server-backed requests through to :8420. Skip the proxies if you
    // run the console without a local blinky-server — WebSerial paths
    // still work.
    proxy: {
      '/api': 'http://localhost:8420',
      '/ws': { target: 'ws://localhost:8420', ws: true },
    },
  },
});
