import { describe, expect, it, vi } from 'vitest';
import { DeviceProtocol } from './DeviceProtocol';
import { SerialError, SerialErrorCode } from './types';
import type { Transport } from '../transport';

const mockTransport = (connected = false): Transport =>
  ({
    isSupported: () => true,
    isConnected: () => connected,
    connect: vi.fn(),
    disconnect: vi.fn(),
    send: vi.fn(),
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
  }) as unknown as Transport;

describe('DeviceProtocol.setTransport', () => {
  it('swaps transport when disconnected', () => {
    const initial = mockTransport(false);
    const next = mockTransport(false);
    const protocol = new DeviceProtocol(initial);

    protocol.setTransport(next);
    expect(protocol.currentTransport).toBe(next);
  });

  it('detaches its listener from the old transport on swap', () => {
    const initial = mockTransport(false);
    const next = mockTransport(false);
    const protocol = new DeviceProtocol(initial);

    protocol.setTransport(next);
    expect(initial.removeEventListener).toHaveBeenCalledTimes(1);
    expect(next.addEventListener).toHaveBeenCalledTimes(1);
  });

  it('throws SerialError(PORT_IN_USE) when called while connected', () => {
    // Replacing a live transport would leak its open port handle —
    // setTransport must refuse and force the caller to disconnect first.
    const live = mockTransport(true);
    const next = mockTransport(false);
    const protocol = new DeviceProtocol(live);

    expect(() => protocol.setTransport(next)).toThrow(SerialError);
    try {
      protocol.setTransport(next);
    } catch (e) {
      expect((e as SerialError).code).toBe(SerialErrorCode.PORT_IN_USE);
    }
    // Still bound to the original transport.
    expect(protocol.currentTransport).toBe(live);
  });
});
