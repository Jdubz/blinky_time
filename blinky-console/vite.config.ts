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
        // Precache essential shell assets including index.html for offline PWA
        globPatterns: ['index.html', '**/*.{ico,woff2}'],
        // Navigation fallback for SPA
        navigateFallback: '/index.html',
        navigateFallbackDenylist: [/^\/api\//],
        // Runtime caching strategies
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
          {
            // Cache Google Fonts stylesheets
            urlPattern: /^https:\/\/fonts\.googleapis\.com\/.*/i,
            handler: 'CacheFirst',
            options: {
              cacheName: 'google-fonts-cache',
              expiration: {
                maxEntries: 10,
                maxAgeSeconds: 60 * 60 * 24 * 365, // 1 year
              },
              cacheableResponse: {
                statuses: [0, 200],
              },
            },
          },
          {
            // Cache Google Fonts webfonts
            urlPattern: /^https:\/\/fonts\.gstatic\.com\/.*/i,
            handler: 'CacheFirst',
            options: {
              cacheName: 'gstatic-fonts-cache',
              expiration: {
                maxEntries: 10,
                maxAgeSeconds: 60 * 60 * 24 * 365, // 1 year
              },
              cacheableResponse: {
                statuses: [0, 200],
              },
            },
          },
        ],
        // Clean up old caches
        cleanupOutdatedCaches: true,
        // Immediately activate new service worker
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
