import { beforeEach, describe, expect, it, vi } from 'vitest';
import { WebSerialTransport, TransportErrorCode } from '.';
import type { TransportEvent } from '.';

/**
 * Builds a fake SerialPort and wires it into navigator.serial.requestPort.
 * The reader's `read()` is sequenced from the supplied generator function so
 * each test can shape the byte stream / error timing it cares about.
 */
function setupMockSerial(readQueue: ReadResult[]): {
  port: { open: ReturnType<typeof vi.fn>; close: ReturnType<typeof vi.fn> };
  reader: {
    read: ReturnType<typeof vi.fn>;
    cancel: ReturnType<typeof vi.fn>;
    releaseLock: ReturnType<typeof vi.fn>;
  };
  writer: { write: ReturnType<typeof vi.fn>; releaseLock: ReturnType<typeof vi.fn> };
} {
  const reader = {
    read: vi.fn(),
    cancel: vi.fn().mockResolvedValue(undefined),
    releaseLock: vi.fn(),
  };
  // Each read() shifts off the queue. When the queue empties, the port is
  // treated as closed (done: true) so loops exit cleanly.
  reader.read.mockImplementation(() => {
    const next = readQueue.shift();
    if (!next) return Promise.resolve({ value: new Uint8Array(), done: true });
    if (next.kind === 'data') return Promise.resolve({ value: next.value, done: false });
    if (next.kind === 'done') return Promise.resolve({ value: new Uint8Array(), done: true });
    return Promise.reject(next.error);
  });

  const writer = {
    write: vi.fn().mockResolvedValue(undefined),
    releaseLock: vi.fn(),
  };

  const port = {
    open: vi.fn().mockResolvedValue(undefined),
    close: vi.fn().mockResolvedValue(undefined),
    readable: { getReader: () => reader } as unknown as ReadableStream<Uint8Array>,
    writable: { getWriter: () => writer } as unknown as WritableStream<Uint8Array>,
    getInfo: vi.fn().mockReturnValue({}),
  };

  (navigator.serial.requestPort as ReturnType<typeof vi.fn>).mockResolvedValue(port);
  return { port, reader, writer };
}

type ReadResult =
  | { kind: 'data'; value: Uint8Array }
  | { kind: 'done' }
  | { kind: 'error'; error: Error };

/** Resolves once at least N events of the given types have been emitted. */
function awaitEvents(
  transport: WebSerialTransport,
  types: TransportEvent['type'][]
): Promise<TransportEvent[]> {
  return new Promise(resolve => {
    const collected: TransportEvent[] = [];
    const remaining = [...types];
    const handler = (event: TransportEvent) => {
      collected.push(event);
      const idx = remaining.indexOf(event.type);
      if (idx >= 0) remaining.splice(idx, 1);
      if (remaining.length === 0) {
        transport.removeEventListener(handler);
        resolve(collected);
      }
    };
    transport.addEventListener(handler);
  });
}

describe('WebSerialTransport.startReading auto-disconnect', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('emits both `error` and `disconnected` on a generic read failure', async () => {
    // Reproduces the zombie-state bug: any read error must tear down the
    // port — not only DEVICE_LOST / DISCONNECTED. Without the fix, the
    // reader thread exits but the port stays open and isConnected()
    // returns true forever.
    setupMockSerial([
      { kind: 'data', value: new TextEncoder().encode('hello\n') },
      { kind: 'error', error: new Error('I/O failure on USB bus') },
    ]);

    const transport = new WebSerialTransport();
    const events = awaitEvents(transport, ['error', 'disconnected']);

    await transport.connect();
    const collected = await events;

    const errorEvent = collected.find(e => e.type === 'error');
    expect(errorEvent).toBeDefined();
    expect(errorEvent?.error?.code).toBe(TransportErrorCode.IO_ERROR);

    expect(collected.map(e => e.type)).toContain('disconnected');
    expect(transport.isConnected()).toBe(false);
  });

  it('still tears down on classified DEVICE_LOST errors', async () => {
    // Sanity check: the prior behavior (auto-disconnect on DEVICE_LOST)
    // still holds. NetworkError DOMException → DEVICE_LOST.
    const networkError = Object.assign(new Error('device gone'), { name: 'NetworkError' });
    Object.setPrototypeOf(networkError, DOMException.prototype);
    setupMockSerial([{ kind: 'error', error: networkError }]);

    const transport = new WebSerialTransport();
    const events = awaitEvents(transport, ['error', 'disconnected']);

    await transport.connect();
    const collected = await events;

    expect(collected.map(e => e.type)).toContain('disconnected');
    expect(transport.isConnected()).toBe(false);
  });

  it('does NOT emit error/disconnect when isReading was cleared first (clean shutdown)', async () => {
    // If the consumer called disconnect() and that races with the read
    // loop seeing a cancellation exception, we must not double-fire.
    setupMockSerial([{ kind: 'data', value: new TextEncoder().encode('hi\n') }, { kind: 'done' }]);

    const transport = new WebSerialTransport();
    const events: TransportEvent[] = [];
    transport.addEventListener(e => events.push(e));

    await transport.connect();
    await transport.disconnect();

    // Exactly one disconnected from disconnect() — not a stray error.
    const errorCount = events.filter(e => e.type === 'error').length;
    expect(errorCount).toBe(0);
  });
});
