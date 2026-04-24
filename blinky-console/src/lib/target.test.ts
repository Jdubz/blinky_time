/**
 * Tests for lib/target.ts — the Target abstraction that unifies fleet HTTP
 * dispatch and single-device protocol RPCs behind one interface.
 *
 * These tests mock both fetch (fleet mode) and a DeviceProtocol instance
 * (device mode) to verify each dispatch function routes correctly. Added
 * 2026-04-24 per PR 131 review feedback.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import type { DeviceProtocol } from '../services/protocol';
import {
  type Target,
  targetLabel,
  targetSetGenerator,
  targetSetEffect,
  targetSetSetting,
  targetSendCommand,
  targetSave,
  targetLoad,
  targetResetDefaults,
} from './target';

function makeProtocolMock(): DeviceProtocol {
  return {
    setGenerator: vi.fn().mockResolvedValue(undefined),
    setEffect: vi.fn().mockResolvedValue(undefined),
    setSetting: vi.fn().mockResolvedValue(undefined),
    send: vi.fn().mockResolvedValue('ok'),
    saveSettings: vi.fn().mockResolvedValue(undefined),
    loadSettings: vi.fn().mockResolvedValue(undefined),
    resetDefaults: vi.fn().mockResolvedValue(undefined),
  } as unknown as DeviceProtocol;
}

function fleetTarget(): Target {
  return { kind: 'fleet' };
}

function deviceTarget(protocol: DeviceProtocol): Target {
  return { kind: 'device', id: 'device-abc', protocol };
}

describe('targetLabel', () => {
  it('returns "All devices" for fleet target', () => {
    expect(targetLabel(fleetTarget())).toBe('All devices');
  });

  it('returns provided device name when given', () => {
    const proto = makeProtocolMock();
    expect(targetLabel(deviceTarget(proto), 'Kitchen Blinky')).toBe('Kitchen Blinky');
  });

  it('falls back to truncated device id when no name provided', () => {
    const proto = makeProtocolMock();
    const t: Target = { kind: 'device', id: '0123456789abcdef', protocol: proto };
    expect(targetLabel(t)).toBe('01234567');
  });
});

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

  it('targetSendCommand POSTs /api/fleet/command with {command}', async () => {
    await targetSendCommand(fleetTarget(), 'gen fire');
    const [url, init] = fetchSpy.mock.calls[0];
    expect(url).toBe('/api/fleet/command');
    expect(JSON.parse(init?.body as string)).toEqual({ command: 'gen fire' });
  });

  it.each([
    ['targetSave', targetSave, '/api/fleet/settings/save'],
    ['targetLoad', targetLoad, '/api/fleet/settings/load'],
    ['targetResetDefaults', targetResetDefaults, '/api/fleet/settings/defaults'],
  ] as const)('%s POSTs %s', async (_name, fn, expectedUrl) => {
    await fn(fleetTarget());
    const [url, init] = fetchSpy.mock.calls[0];
    expect(url).toBe(expectedUrl);
    expect(init?.method).toBe('POST');
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

  it('targetSendCommand routes to protocol.send', async () => {
    const proto = makeProtocolMock();
    await targetSendCommand(deviceTarget(proto), 'status');
    expect(proto.send).toHaveBeenCalledWith('status');
  });

  it.each([
    ['targetSave', targetSave, 'saveSettings'],
    ['targetLoad', targetLoad, 'loadSettings'],
    ['targetResetDefaults', targetResetDefaults, 'resetDefaults'],
  ] as const)('%s calls protocol.%s', async (_name, fn, method) => {
    const proto = makeProtocolMock();
    await fn(deviceTarget(proto));
    // Double-cast via unknown because DeviceProtocol's type has no index
    // signature and tsc rejects the direct Record cast.
    expect((proto as unknown as Record<string, ReturnType<typeof vi.fn>>)[method]).toHaveBeenCalledOnce();
  });
});
