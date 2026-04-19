import { beforeEach, describe, expect, it, vi } from 'vitest';
import { DeviceRegistry } from './DeviceRegistry';
import { Device, Source, TransportBinding } from './types';
import type { Transport } from '../transport';
import type { DeviceProtocol } from '../protocol';

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

const fakeSource = (kind: Source['kind'] = 'webserial'): Source => ({
  kind,
  displayName: `${kind} fake`,
  isSupported: () => true,
});

const fakeTransport = (): Transport =>
  ({
    isSupported: () => true,
    isConnected: () => false,
    connect: vi.fn(),
    disconnect: vi.fn(),
    send: vi.fn(),
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
  }) as unknown as Transport;

const fakeBinding = (kind: Source['kind'] = 'webserial'): TransportBinding => ({
  source: fakeSource(kind),
  transport: fakeTransport(),
});

const fakeProtocol = (connected = false): DeviceProtocol =>
  ({
    isConnected: () => connected,
  }) as unknown as DeviceProtocol;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

describe('DeviceRegistry', () => {
  let registry: DeviceRegistry;

  beforeEach(() => {
    registry = new DeviceRegistry();
  });

  it('starts empty', () => {
    expect(registry.list()).toEqual([]);
    expect(registry.get('any')).toBeUndefined();
    expect(registry.has('any')).toBe(false);
  });

  it('adds a new device', () => {
    const dev = new Device('SN-AAA', 'Bucket', [fakeBinding()]);
    const result = registry.upsert(dev);

    expect(result).toBe(dev);
    expect(registry.list()).toEqual([dev]);
    expect(registry.get('SN-AAA')).toBe(dev);
    expect(registry.has('SN-AAA')).toBe(true);
  });

  it('merges transports into existing entry on duplicate id', () => {
    const original = new Device('SN-AAA', 'Bucket', [fakeBinding('webserial')]);
    registry.upsert(original);

    const update = new Device('SN-AAA', 'Bucket', [fakeBinding('blinky-server')]);
    const result = registry.upsert(update);

    // Returns the canonical (existing) entry, not the new one.
    expect(result).toBe(original);
    expect(result.transports).toHaveLength(2);
    expect(result.transports.map(b => b.source.kind)).toEqual(['webserial', 'blinky-server']);
    // Registry size still 1.
    expect(registry.list()).toHaveLength(1);
  });

  it('binds protocol to existing entry when existing has none', () => {
    const existing = new Device('SN-AAA', 'Bucket', [fakeBinding('webserial')], null);
    registry.upsert(existing);

    const newProtocol = fakeProtocol();
    const update = new Device('SN-AAA', 'Bucket', [fakeBinding('blinky-server')], newProtocol);
    registry.upsert(update);

    expect(existing.protocol).toBe(newProtocol);
  });

  it('preserves an actively-connected protocol on merge', () => {
    const originalProtocol = fakeProtocol(true);
    const existing = new Device('SN-AAA', 'Bucket', [fakeBinding('webserial')], originalProtocol);
    registry.upsert(existing);

    const update = new Device('SN-AAA', 'Bucket', [fakeBinding('blinky-server')], fakeProtocol());
    registry.upsert(update);

    expect(existing.protocol).toBe(originalProtocol);
  });

  it('replaces a dead protocol on merge', () => {
    // A reconnect produces a fresh DeviceProtocol; the registry shouldn't
    // hold onto the prior dead one and silently drop the live one.
    const deadProtocol = fakeProtocol(false);
    const existing = new Device('SN-AAA', 'Bucket', [fakeBinding('webserial')], deadProtocol);
    registry.upsert(existing);

    const liveProtocol = fakeProtocol(true);
    const update = new Device('SN-AAA', 'Bucket', [fakeBinding('blinky-server')], liveProtocol);
    registry.upsert(update);

    expect(existing.protocol).toBe(liveProtocol);
  });

  it('does not duplicate identical transport bindings', () => {
    const binding = fakeBinding();
    const dev = new Device('SN-AAA', 'Bucket', [binding]);
    registry.upsert(dev);

    const dup = new Device('SN-AAA', 'Bucket', [binding]);
    registry.upsert(dup);

    expect(dev.transports).toHaveLength(1);
  });

  it('preserves the registered displayName when subsequent upsert has none', () => {
    const original = new Device('SN-AAA', 'Bucket Totem', [fakeBinding()]);
    registry.upsert(original);

    const update = new Device('SN-AAA', '', [fakeBinding('blinky-server')]);
    registry.upsert(update);

    expect(original.displayName).toBe('Bucket Totem');
  });

  it('overwrites the displayName when subsequent upsert has a non-empty value', () => {
    // A device first registered as "Unconfigured Device" (placeholder)
    // should pick up its real name on a later upsert.
    const original = new Device('SN-AAA', 'Unconfigured Device', [fakeBinding()]);
    registry.upsert(original);

    const update = new Device('SN-AAA', 'Bucket Totem', [fakeBinding('blinky-server')]);
    registry.upsert(update);

    expect(original.displayName).toBe('Bucket Totem');
  });

  it('removes a device by id', () => {
    const dev = new Device('SN-AAA', 'Bucket', [fakeBinding()]);
    registry.upsert(dev);

    expect(registry.remove('SN-AAA')).toBe(true);
    expect(registry.remove('SN-AAA')).toBe(false);
    expect(registry.list()).toEqual([]);
  });

  it('clears all devices', () => {
    registry.upsert(new Device('SN-A', 'a', [fakeBinding()]));
    registry.upsert(new Device('SN-B', 'b', [fakeBinding()]));
    expect(registry.list()).toHaveLength(2);

    registry.clear();
    expect(registry.list()).toEqual([]);
  });

  it('notifies subscribers on add, merge, remove, and clear', () => {
    const cb = vi.fn();
    registry.subscribe(cb);

    registry.upsert(new Device('SN-A', 'a', [fakeBinding()]));
    expect(cb).toHaveBeenCalledTimes(1);

    registry.upsert(new Device('SN-A', 'a', [fakeBinding('blinky-server')]));
    expect(cb).toHaveBeenCalledTimes(2);

    registry.remove('SN-A');
    expect(cb).toHaveBeenCalledTimes(3);

    registry.upsert(new Device('SN-B', 'b', [fakeBinding()]));
    registry.clear();
    expect(cb).toHaveBeenCalledTimes(5);
  });

  it('does not notify when remove targets a missing id', () => {
    const cb = vi.fn();
    registry.subscribe(cb);

    registry.remove('NOPE');
    expect(cb).not.toHaveBeenCalled();
  });

  it('does not notify when clear runs on an empty registry', () => {
    const cb = vi.fn();
    registry.subscribe(cb);

    registry.clear();
    expect(cb).not.toHaveBeenCalled();
  });

  it('returns an unsubscribe function from subscribe', () => {
    const cb = vi.fn();
    const unsubscribe = registry.subscribe(cb);

    registry.upsert(new Device('SN-A', 'a', [fakeBinding()]));
    expect(cb).toHaveBeenCalledTimes(1);

    unsubscribe();
    registry.upsert(new Device('SN-B', 'b', [fakeBinding()]));
    expect(cb).toHaveBeenCalledTimes(1);
  });

  it('sends the current device list (post-mutation) to subscribers', () => {
    const cb = vi.fn();
    registry.subscribe(cb);

    const dev = new Device('SN-A', 'a', [fakeBinding()]);
    registry.upsert(dev);

    expect(cb).toHaveBeenCalledWith([dev]);
  });
});
