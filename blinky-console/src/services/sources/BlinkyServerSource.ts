/**
 * BlinkyServerSource — discovers devices from a blinky-server instance.
 *
 * Polls `GET /api/devices` periodically and registers discovered devices
 * with the DeviceRegistry. Each device gets a ServerWebSocketTransport
 * binding so it can be connected via the server's WebSocket proxy.
 *
 * Devices are NOT auto-connected — they appear in the registry with transport
 * bindings, and the user selects which to connect (same flow as WebSerial
 * after port selection).
 */

import { logger } from '../../lib/logger';
import { ServerWebSocketTransport } from '../transport';
import { Device } from './types';
import type { DeviceRegistry } from './DeviceRegistry';
import type { ServerDevice, Source, SourceKind } from './types';

const DEFAULT_POLL_INTERVAL_MS = 10_000;
const POLL_TIMEOUT_MS = 5_000;

export class BlinkyServerSource implements Source {
  readonly kind: SourceKind = 'blinky-server';
  readonly displayName: string;
  private pollTimer: ReturnType<typeof setInterval> | null = null;
  private pollInFlight = false;
  private knownDeviceIds = new Set<string>();

  constructor(
    private readonly serverUrl: string,
    private readonly registry: DeviceRegistry,
    private readonly pollIntervalMs = DEFAULT_POLL_INTERVAL_MS,
  ) {
    try {
      this.displayName = new URL(serverUrl).host;
    } catch {
      this.displayName = serverUrl;
    }
  }

  isSupported(): boolean {
    return true; // HTTP + WebSocket available everywhere
  }

  /** Start polling for devices. Performs an initial fetch immediately.
   *  Idempotent — calling while already started is a no-op. */
  async start(): Promise<void> {
    if (this.pollTimer !== null) return;  // Already running
    logger.info('BlinkyServerSource: starting', { serverUrl: this.serverUrl });
    await this.poll();
    this.pollTimer = setInterval(() => {
      if (!this.pollInFlight) {
        this.pollInFlight = true;
        this.poll()
          .catch(() => {})
          .finally(() => { this.pollInFlight = false; });
      }
    }, this.pollIntervalMs);
  }

  /** Stop polling. Does not disconnect devices or remove them from registry. */
  stop(): void {
    if (this.pollTimer) {
      clearInterval(this.pollTimer);
      this.pollTimer = null;
    }
    logger.info('BlinkyServerSource: stopped');
  }

  /** Fetch device list from server and update registry. */
  async poll(): Promise<void> {
    try {
      const resp = await fetch(`${this.serverUrl}/api/devices`, {
        signal: AbortSignal.timeout(POLL_TIMEOUT_MS),
      });
      if (!resp.ok) return;

      // Check content-type if present — reject HTML error pages but accept
      // missing header (some proxies strip it).
      const contentType = resp.headers?.get?.('content-type');
      if (contentType && !contentType.includes('json')) {
        logger.warn('BlinkyServerSource: non-JSON response', { contentType });
        return;
      }

      const devices: ServerDevice[] = await resp.json();
      const currentIds = new Set<string>();

      for (const d of devices) {
        // Only register connected devices — disconnected ones can't be reached
        if (d.state !== 'connected') continue;

        const id = d.hardware_sn || d.id;
        currentIds.add(id);

        const displayName = d.device_name || d.device_type || 'Unknown';
        const existing = this.registry.get(id);

        if (existing) {
          // Update display name if server has a better one
          if (displayName !== 'Unknown' && existing.displayName !== displayName) {
            existing.displayName = displayName;
          }
        } else if (!this.knownDeviceIds.has(id)) {
          // New device — create transport binding and register
          const transport = new ServerWebSocketTransport(this.serverUrl, d.id);
          const device = new Device(id, displayName, [{ source: this, transport }]);
          this.registry.upsert(device);
          logger.debug('BlinkyServerSource: discovered device', {
            id,
            name: displayName,
            serverTransport: d.transport,
          });
        }
      }

      this.knownDeviceIds = currentIds;
    } catch {
      // Server unreachable or non-JSON response — silently skip.
      // This is expected when running from Firebase (no local server).
    }
  }
}

/**
 * Probe the current origin for a blinky-server and auto-create a source.
 *
 * Called once at app startup. If the page is served from blinky-server
 * (same-origin), the probe succeeds and devices are auto-discovered.
 * If served from Firebase or localhost:3000 (Vite dev), the probe fails
 * fast (1s timeout) and the app runs in WebSerial-only mode.
 *
 * Note: with Vite dev proxy (localhost:3000 → localhost:8420), the probe
 * succeeds because the proxy forwards /api/devices to the server.
 */
export async function detectSameOriginServer(
  registry: DeviceRegistry,
): Promise<BlinkyServerSource | null> {
  try {
    const resp = await fetch('/api/devices', {
      signal: AbortSignal.timeout(1_000),
    });
    if (!resp.ok) return null;

    const contentType = resp.headers?.get?.('content-type');
    if (contentType && !contentType.includes('json')) return null;

    const source = new BlinkyServerSource(window.location.origin, registry);
    await source.start();
    logger.info('Same-origin blinky-server detected', {
      origin: window.location.origin,
    });
    return source;
  } catch {
    // Not served from blinky-server — WebSerial-only mode
  }
  return null;
}
