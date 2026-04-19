import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { BlinkyServerSource, detectSameOriginServer } from './BlinkyServerSource';
import { DeviceRegistry } from './DeviceRegistry';
import { Device } from './types';
import type { ServerDevice } from './types';

function makeServerDevice(overrides: Partial<ServerDevice> = {}): ServerDevice {
  return {
    id: 'ABCD1234',
    port: '/dev/ttyACM0',
    platform: 'nrf52840',
    transport: 'serial',
    state: 'connected',
    version: 'b129',
    device_type: 'hat',
    device_name: 'Test Hat',
    configured: true,
    hardware_sn: 'ABCD1234',
    ble_address: null,
    ...overrides,
  };
}

let registry: DeviceRegistry;
let fetchMock: ReturnType<typeof vi.fn>;

beforeEach(() => {
  registry = new DeviceRegistry();
  fetchMock = vi.fn();
  vi.stubGlobal('fetch', fetchMock);
});

afterEach(() => {
  vi.restoreAllMocks();
});

describe('BlinkyServerSource', () => {
  const SERVER_URL = 'http://blinkyhost.local:8420';

  describe('poll', () => {
    it('registers connected devices in the registry', async () => {
      fetchMock.mockResolvedValue({
        ok: true,
        json: () => Promise.resolve([makeServerDevice()]),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.poll();

      expect(registry.list()).toHaveLength(1);
      expect(registry.list()[0].id).toBe('ABCD1234');
      expect(registry.list()[0].displayName).toBe('Test Hat');
    });

    it('skips disconnected devices', async () => {
      fetchMock.mockResolvedValue({
        ok: true,
        json: () => Promise.resolve([
          makeServerDevice({ state: 'connected' }),
          makeServerDevice({ id: 'DEAD', hardware_sn: 'DEAD', state: 'disconnected' }),
        ]),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.poll();

      expect(registry.list()).toHaveLength(1);
      expect(registry.list()[0].id).toBe('ABCD1234');
    });

    it('uses hardware_sn as device ID for cross-source dedup', async () => {
      fetchMock.mockResolvedValue({
        ok: true,
        json: () => Promise.resolve([
          makeServerDevice({ id: 'server-internal-id', hardware_sn: 'HW_SN_123' }),
        ]),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.poll();

      expect(registry.list()[0].id).toBe('HW_SN_123');
    });

    it('falls back to server id when hardware_sn is null', async () => {
      fetchMock.mockResolvedValue({
        ok: true,
        json: () => Promise.resolve([
          makeServerDevice({ id: 'FALLBACK', hardware_sn: null }),
        ]),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.poll();

      expect(registry.list()[0].id).toBe('FALLBACK');
    });

    it('does not duplicate devices on repeated polls', async () => {
      fetchMock.mockResolvedValue({
        ok: true,
        json: () => Promise.resolve([makeServerDevice()]),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.poll();
      await source.poll();
      await source.poll();

      expect(registry.list()).toHaveLength(1);
    });

    it('handles server unreachable gracefully', async () => {
      fetchMock.mockRejectedValue(new Error('Network error'));

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.poll(); // should not throw

      expect(registry.list()).toHaveLength(0);
    });

    it('handles non-ok response gracefully', async () => {
      fetchMock.mockResolvedValue({ ok: false, status: 500 });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.poll(); // should not throw

      expect(registry.list()).toHaveLength(0);
    });

    it('registers multiple devices', async () => {
      fetchMock.mockResolvedValue({
        ok: true,
        json: () => Promise.resolve([
          makeServerDevice({ id: 'A', hardware_sn: 'A', device_name: 'Device A' }),
          makeServerDevice({ id: 'B', hardware_sn: 'B', device_name: 'Device B' }),
          makeServerDevice({ id: 'C', hardware_sn: 'C', device_name: 'Device C' }),
        ]),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.poll();

      expect(registry.list()).toHaveLength(3);
    });
  });

  describe('start/stop', () => {
    it('performs initial poll on start', async () => {
      fetchMock.mockResolvedValue({
        ok: true,
        json: () => Promise.resolve([makeServerDevice()]),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.start();

      expect(registry.list()).toHaveLength(1);
      source.stop();
    });

    it('stop clears the poll timer', async () => {
      fetchMock.mockResolvedValue({
        ok: true,
        json: () => Promise.resolve([]),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.start();
      source.stop();
      // No assertion needed — just verifying no crash and cleanup
    });
  });

  describe('properties', () => {
    it('has kind blinky-server', () => {
      const source = new BlinkyServerSource(SERVER_URL, registry);
      expect(source.kind).toBe('blinky-server');
    });

    it('uses host as display name', () => {
      const source = new BlinkyServerSource(SERVER_URL, registry);
      expect(source.displayName).toBe('blinkyhost.local:8420');
    });

    it('is always supported', () => {
      const source = new BlinkyServerSource(SERVER_URL, registry);
      expect(source.isSupported()).toBe(true);
    });
  });

  describe('cross-source dedup', () => {
    it('merges with WebSerial-discovered device by hardware_sn', async () => {
      // Simulate WebSerial discovering a device first
      const existingDevice = new Device('ABCD1234', 'USB Device', []);
      registry.upsert(existingDevice);
      expect(registry.list()).toHaveLength(1); // sanity check

      // Now server discovers the same device
      fetchMock.mockResolvedValue({
        ok: true,
        json: () => Promise.resolve([
          makeServerDevice({ hardware_sn: 'ABCD1234' }),
        ]),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.poll();

      // Should still be one device (not duplicated)
      const devices = registry.list();
      expect(devices).toHaveLength(1);
      expect(devices[0].id).toBe('ABCD1234');
      // Display name updated from server
      expect(devices[0].displayName).toBe('Test Hat');
    });
  });

  describe('edge cases', () => {
    it('start() is idempotent (no timer leak)', async () => {
      fetchMock.mockResolvedValue({
        ok: true,
        json: () => Promise.resolve([]),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.start();
      await source.start(); // second call should be no-op
      source.stop();
    });

    it('handles server returning non-JSON response', async () => {
      fetchMock.mockResolvedValue({
        ok: true,
        headers: new Headers({ 'content-type': 'text/html' }),
        json: () => Promise.reject(new Error('not json')),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.poll(); // should not throw
      expect(registry.list()).toHaveLength(0);
    });

    it('handles null device_name with fallback', async () => {
      fetchMock.mockResolvedValue({
        ok: true,
        json: () => Promise.resolve([
          makeServerDevice({ device_name: null, device_type: 'tube_light' }),
        ]),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.poll();
      expect(registry.list()[0].displayName).toBe('tube_light');
    });

    it('handles null device_name and null device_type', async () => {
      fetchMock.mockResolvedValue({
        ok: true,
        json: () => Promise.resolve([
          makeServerDevice({ device_name: null, device_type: null }),
        ]),
      });

      const source = new BlinkyServerSource(SERVER_URL, registry, 60_000);
      await source.poll();
      expect(registry.list()[0].displayName).toBe('Unknown');
    });
  });
});

describe('detectSameOriginServer', () => {
  it('creates source when server responds', async () => {
    fetchMock.mockResolvedValue({
      ok: true,
      json: () => Promise.resolve([]),
    });

    const source = await detectSameOriginServer(registry);
    expect(source).not.toBeNull();
    source?.stop();
  });

  it('returns null when no server present', async () => {
    fetchMock.mockRejectedValue(new Error('No server'));

    const source = await detectSameOriginServer(registry);
    expect(source).toBeNull();
  });
});
