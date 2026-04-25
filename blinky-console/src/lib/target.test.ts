/**
 * Tests for lib/target.ts — the Target abstraction that unifies fleet HTTP
 * dispatch and single-device protocol RPCs behind one interface.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import type { DeviceProtocol } from '../services/protocol';
import { type Target, targetSetGenerator, targetSetEffect, targetSetSetting } from './target';

function makeProtocolMock(): DeviceProtocol {
  return {
    setGenerator: vi.fn().mockResolvedValue(undefined),
    setEffect: vi.fn().mockResolvedValue(undefined),
    setSetting: vi.fn().mockResolvedValue(undefined),
  } as unknown as DeviceProtocol;
}

function fleetTarget(): Target {
  return { kind: 'fleet' };
}

function deviceTarget(protocol: DeviceProtocol): Target {
  return { kind: 'device', id: 'device-abc', protocol };
}

describe('target dispatch — fleet mode', () => {
  let fetchSpy: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    fetchSpy = vi.fn().mockResolvedValue({ ok: true } as Response);
    vi.stubGlobal('fetch', fetchSpy);
  });

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('targetSetGenerator POSTs /api/fleet/generator/<name>', async () => {
    await targetSetGenerator(fleetTarget(), 'fire');
    expect(fetchSpy).toHaveBeenCalledOnce();
    const [url, init] = fetchSpy.mock.calls[0];
    expect(url).toBe('/api/fleet/generator/fire');
    expect(init?.method).toBe('POST');
  });

  it('targetSetGenerator URL-encodes the generator name', async () => {
    await targetSetGenerator(fleetTarget(), 'fire/variant' as never);
    const [url] = fetchSpy.mock.calls[0];
    expect(url).toBe('/api/fleet/generator/fire%2Fvariant');
  });

  it('targetSetEffect POSTs /api/fleet/effect/<name>', async () => {
    await targetSetEffect(fleetTarget(), 'hue');
    const [url, init] = fetchSpy.mock.calls[0];
    expect(url).toBe('/api/fleet/effect/hue');
    expect(init?.method).toBe('POST');
  });

  it('targetSetSetting PUTs /api/fleet/settings/<name> with {value}', async () => {
    await targetSetSetting(fleetTarget(), 'huespeed', 0.5);
    const [url, init] = fetchSpy.mock.calls[0];
    expect(url).toBe('/api/fleet/settings/huespeed');
    expect(init?.method).toBe('PUT');
    expect(init?.headers).toMatchObject({ 'Content-Type': 'application/json' });
    expect(JSON.parse(init?.body as string)).toEqual({ value: 0.5 });
  });

  it('throws when fleet responds non-ok', async () => {
    fetchSpy.mockResolvedValueOnce({ ok: false, status: 503 } as Response);
    await expect(targetSetGenerator(fleetTarget(), 'fire')).rejects.toThrow(/503/);
  });
});

describe('target dispatch — device mode', () => {
  it('targetSetGenerator calls protocol.setGenerator and NOT fetch', async () => {
    const proto = makeProtocolMock();
    const fetchSpy = vi.fn();
    vi.stubGlobal('fetch', fetchSpy);
    try {
      await targetSetGenerator(deviceTarget(proto), 'water');
      expect(proto.setGenerator).toHaveBeenCalledWith('water');
      expect(fetchSpy).not.toHaveBeenCalled();
    } finally {
      vi.unstubAllGlobals();
    }
  });

  it('targetSetEffect routes to protocol.setEffect', async () => {
    const proto = makeProtocolMock();
    await targetSetEffect(deviceTarget(proto), 'none');
    expect(proto.setEffect).toHaveBeenCalledWith('none');
  });

  it('targetSetSetting routes to protocol.setSetting with (name, value)', async () => {
    const proto = makeProtocolMock();
    await targetSetSetting(deviceTarget(proto), 'hueshift', 0.25);
    expect(proto.setSetting).toHaveBeenCalledWith('hueshift', 0.25);
  });

  it('targetSetSetting passes boolean values through unchanged', async () => {
    const proto = makeProtocolMock();
    await targetSetSetting(deviceTarget(proto), 'enabled', true);
    expect(proto.setSetting).toHaveBeenCalledWith('enabled', true);
  });
});
