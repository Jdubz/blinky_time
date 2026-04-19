import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { VitePWA } from 'vite-plugin-pwa';

export default defineConfig({
  plugins: [
    react(),
    VitePWA({
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
      workbox: {
        // Precache essential shell assets including index.html for offline PWA.
        // Includes PNG icons (PWA icons, favicons) so first offline load doesn't 404.
        globPatterns: ['index.html', '**/*.{ico,png,woff2}'],
        // Navigation fallback for SPA
        navigateFallback: '/index.html',
        navigateFallbackDenylist: [/^\/api\//],
        runtimeCaching: [
          {
            // Hashed app assets - CacheFirst since new deploys get new URLs
            // Only match same-origin assets in /assets/ with content hashes
            urlPattern: ({ url }) =>
              url.origin === self.location.origin &&
              /\/assets\/.*\.[a-f0-9]+\.(?:js|css)$/i.test(url.pathname),
            handler: 'CacheFirst',
            options: {
              cacheName: 'app-assets',
              expiration: {
                maxEntries: 50,
                maxAgeSeconds: 60 * 60 * 24 * 7, // 7 days
              },
            },
          },
        ],
        cleanupOutdatedCaches: true,
        skipWaiting: true,
        clientsClaim: true,
      },
      // Dev options for testing
      devOptions: {
        enabled: false, // Set to true to test PWA in dev mode
      },
    }),
  ],
  server: {
    port: 3000,
  },
});
