import { describe, it, expect, vi, beforeEach } from 'vitest';
import { renderHook, act, waitFor } from '@testing-library/react';
import { useDeviceAudioStream } from './useDeviceAudioStream';
import { Device } from '../services/sources/types';
import type { Source, TransportBinding } from '../services/sources/types';
import type { Transport } from '../services/transport';
import type { DeviceProtocol, SerialEvent, SerialEventCallback } from '../services/protocol';

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

const fakeSource = (kind: Source['kind'] = 'blinky-server'): Source => ({
  kind,
  displayName: `${kind} fake`,
  isSupported: () => true,
});

const fakeTransport = (): Transport =>
  ({
    isSupported: () => true,
    isConnected: () => false,
    connect: vi.fn(() => Promise.resolve()),
    disconnect: vi.fn(),
    send: vi.fn(),
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
  }) as unknown as Transport;

const fakeBinding = (kind: Source['kind'] = 'blinky-server'): TransportBinding => ({
  source: fakeSource(kind),
  transport: fakeTransport(),
});

interface FakeProtocol {
  protocol: DeviceProtocol;
  emit: (event: SerialEvent) => void;
  setConnected: (v: boolean) => void;
  connect: ReturnType<typeof vi.fn>;
  setStreamEnabled: ReturnType<typeof vi.fn>;
  listeners: SerialEventCallback[];
}

function makeProtocol({ connected = false }: { connected?: boolean } = {}): FakeProtocol {
  const listeners: SerialEventCallback[] = [];
  let isConnected = connected;

  const protocol = {
    isConnected: () => isConnected,
    addEventListener: (cb: SerialEventCallback) => {
      listeners.push(cb);
    },
    removeEventListener: (cb: SerialEventCallback) => {
      const i = listeners.indexOf(cb);
      if (i >= 0) listeners.splice(i, 1);
    },
    connect: vi.fn(() => {
      isConnected = true;
      return Promise.resolve(true);
    }),
    setStreamEnabled: vi.fn(() => Promise.resolve()),
  } as unknown as DeviceProtocol;

  return {
    protocol,
    emit: event => listeners.forEach(cb => cb(event)),
    setConnected: v => {
      isConnected = v;
    },
    connect: protocol.connect as unknown as ReturnType<typeof vi.fn>,
    setStreamEnabled: protocol.setStreamEnabled as unknown as ReturnType<typeof vi.fn>,
    listeners,
  };
}

function makeDevice(opts: { id?: string; protocol?: DeviceProtocol | null } = {}): Device {
  return new Device(opts.id ?? 'SN-AAA', 'Test Device', [fakeBinding()], opts.protocol ?? null);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

describe('useDeviceAudioStream', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('returns empty state when device is null', () => {
    const { result } = renderHook(() => useDeviceAudioStream(null));

    expect(result.current.audioData).toBeNull();
    expect(result.current.musicModeData).toBeNull();
    expect(result.current.isStreaming).toBe(false);
    expect(result.current.isConnected).toBe(false);
  });

  it('returns empty state when device has no protocol', () => {
    const device = makeDevice({ protocol: null });
    const { result } = renderHook(() => useDeviceAudioStream(device));

    expect(result.current.isConnected).toBe(false);
    expect(result.current.audioData).toBeNull();
  });

  it('subscribes to protocol events on mount', () => {
    const fp = makeProtocol({ connected: true });
    const device = makeDevice({ protocol: fp.protocol });

    renderHook(() => useDeviceAudioStream(device));

    expect(fp.listeners).toHaveLength(1);
  });

  it('unsubscribes on unmount', () => {
    const fp = makeProtocol({ connected: true });
    const device = makeDevice({ protocol: fp.protocol });

    const { unmount } = renderHook(() => useDeviceAudioStream(device));
    unmount();

    expect(fp.listeners).toHaveLength(0);
  });

  it('reports isConnected from protocol on mount', () => {
    const fp = makeProtocol({ connected: true });
    const device = makeDevice({ protocol: fp.protocol });

    const { result } = renderHook(() => useDeviceAudioStream(device));
    expect(result.current.isConnected).toBe(true);
  });

  it('calls protocol.connect() when not already connected', async () => {
    const fp = makeProtocol({ connected: false });
    const device = makeDevice({ protocol: fp.protocol });

    renderHook(() => useDeviceAudioStream(device));

    await waitFor(() => expect(fp.connect).toHaveBeenCalledTimes(1));
  });

  it('does NOT call protocol.connect() when already connected', () => {
    const fp = makeProtocol({ connected: true });
    const device = makeDevice({ protocol: fp.protocol });

    renderHook(() => useDeviceAudioStream(device));
    expect(fp.connect).not.toHaveBeenCalled();
  });

  it('updates audioData when an audio event arrives', () => {
    const fp = makeProtocol({ connected: true });
    const device = makeDevice({ protocol: fp.protocol });

    const { result } = renderHook(() => useDeviceAudioStream(device));

    act(() => {
      fp.emit({
        type: 'audio',
        audio: {
          a: { l: 0.5, t: 0.1, pk: 0.6, vl: 0.05, raw: 0.3, h: 32, alive: 1 },
        },
      });
    });

    expect(result.current.audioData?.l).toBe(0.5);
  });

  it('updates musicModeData when audio event includes m field', () => {
    const fp = makeProtocol({ connected: true });
    const device = makeDevice({ protocol: fp.protocol });

    const { result } = renderHook(() => useDeviceAudioStream(device));

    act(() => {
      fp.emit({
        type: 'audio',
        audio: {
          a: { l: 0.5, t: 0, pk: 0.6, vl: 0.05, raw: 0.3, h: 32, alive: 1 },
          m: { a: 1, bpm: 120, ph: 0.5, str: 0.7, q: 0, e: 0.5, p: 0.5 },
        },
      });
    });

    expect(result.current.musicModeData?.bpm).toBe(120);
  });

  it('clears state on disconnected event', () => {
    const fp = makeProtocol({ connected: true });
    const device = makeDevice({ protocol: fp.protocol });

    const { result } = renderHook(() => useDeviceAudioStream(device));

    act(() => {
      fp.emit({
        type: 'audio',
        audio: { a: { l: 0.5, t: 0, pk: 0.6, vl: 0, raw: 0.3, h: 32, alive: 1 } },
      });
    });
    expect(result.current.audioData).not.toBeNull();

    act(() => {
      fp.emit({ type: 'disconnected' });
    });

    expect(result.current.isConnected).toBe(false);
    expect(result.current.isStreaming).toBe(false);
    expect(result.current.audioData).toBeNull();
    expect(result.current.musicModeData).toBeNull();
  });

  describe('toggleStreaming', () => {
    it('sends stream on and updates state when starting', async () => {
      const fp = makeProtocol({ connected: true });
      const device = makeDevice({ protocol: fp.protocol });

      const { result } = renderHook(() => useDeviceAudioStream(device));

      await act(async () => {
        await result.current.toggleStreaming();
      });

      expect(fp.setStreamEnabled).toHaveBeenCalledWith(true);
      expect(result.current.isStreaming).toBe(true);
    });

    it('sends stream off and clears data when stopping', async () => {
      const fp = makeProtocol({ connected: true });
      const device = makeDevice({ protocol: fp.protocol });

      const { result } = renderHook(() => useDeviceAudioStream(device));

      await act(async () => {
        await result.current.toggleStreaming(); // on
      });

      // Push data while streaming.
      act(() => {
        fp.emit({
          type: 'audio',
          audio: { a: { l: 0.5, t: 0, pk: 0.6, vl: 0, raw: 0.3, h: 32, alive: 1 } },
        });
      });
      expect(result.current.audioData).not.toBeNull();

      await act(async () => {
        await result.current.toggleStreaming(); // off
      });

      expect(fp.setStreamEnabled).toHaveBeenLastCalledWith(false);
      expect(result.current.isStreaming).toBe(false);
      expect(result.current.audioData).toBeNull();
    });

    it('is a no-op when device is not connected', async () => {
      const fp = makeProtocol({ connected: false });
      // Make connect() fail so isConnected stays false through toggle.
      fp.connect.mockImplementationOnce(() => Promise.reject(new Error('boom')));
      const device = makeDevice({ protocol: fp.protocol });

      const { result } = renderHook(() => useDeviceAudioStream(device));

      await act(async () => {
        await result.current.toggleStreaming();
      });

      expect(fp.setStreamEnabled).not.toHaveBeenCalled();
      expect(result.current.isStreaming).toBe(false);
    });
  });

  it('stops the stream on unmount', () => {
    const fp = makeProtocol({ connected: true });
    const device = makeDevice({ protocol: fp.protocol });

    const { unmount } = renderHook(() => useDeviceAudioStream(device));
    unmount();

    expect(fp.setStreamEnabled).toHaveBeenCalledWith(false);
  });

  it('clears state and resubscribes when device changes', () => {
    const fpA = makeProtocol({ connected: true });
    const fpB = makeProtocol({ connected: true });
    const deviceA = makeDevice({ id: 'SN-AAA', protocol: fpA.protocol });
    const deviceB = makeDevice({ id: 'SN-BBB', protocol: fpB.protocol });

    const { result, rerender } = renderHook(
      ({ device }: { device: Device }) => useDeviceAudioStream(device),
      { initialProps: { device: deviceA } }
    );

    act(() => {
      fpA.emit({
        type: 'audio',
        audio: { a: { l: 0.9, t: 0, pk: 0.9, vl: 0.1, raw: 0.5, h: 40, alive: 1 } },
      });
    });
    expect(result.current.audioData?.l).toBe(0.9);

    rerender({ device: deviceB });

    // Old subscription torn down, new one in place, state reset.
    expect(fpA.listeners).toHaveLength(0);
    expect(fpB.listeners).toHaveLength(1);
    expect(result.current.audioData).toBeNull();

    // Events from the new device flow through; old device's events are ignored.
    act(() => {
      fpA.emit({
        type: 'audio',
        audio: { a: { l: 0.1, t: 0, pk: 0.2, vl: 0, raw: 0.05, h: 20, alive: 1 } },
      });
    });
    expect(result.current.audioData).toBeNull();

    act(() => {
      fpB.emit({
        type: 'audio',
        audio: { a: { l: 0.42, t: 0, pk: 0.5, vl: 0, raw: 0.2, h: 30, alive: 1 } },
      });
    });
    expect(result.current.audioData?.l).toBe(0.42);
  });

  describe('onTransientEvent', () => {
    it('fires registered callbacks on transient events', () => {
      const fp = makeProtocol({ connected: true });
      const device = makeDevice({ protocol: fp.protocol });
      const cb = vi.fn();

      const { result } = renderHook(() => useDeviceAudioStream(device));

      act(() => {
        result.current.onTransientEvent(cb);
      });

      act(() => {
        fp.emit({
          type: 'transient',
          transient: { type: 'TRANSIENT', timestampMs: 100, strength: 0.8 },
        });
      });

      expect(cb).toHaveBeenCalledTimes(1);
      expect(cb.mock.calls[0][0].strength).toBe(0.8);
    });

    it('returns an unsubscribe function', () => {
      const fp = makeProtocol({ connected: true });
      const device = makeDevice({ protocol: fp.protocol });
      const cb = vi.fn();

      const { result } = renderHook(() => useDeviceAudioStream(device));

      let unsubscribe!: () => void;
      act(() => {
        unsubscribe = result.current.onTransientEvent(cb);
      });

      act(() => {
        unsubscribe();
      });

      act(() => {
        fp.emit({
          type: 'transient',
          transient: { type: 'TRANSIENT', timestampMs: 100, strength: 0.8 },
        });
      });

      expect(cb).not.toHaveBeenCalled();
    });
  });
});
