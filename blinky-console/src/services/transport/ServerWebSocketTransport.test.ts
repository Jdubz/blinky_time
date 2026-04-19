import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ServerWebSocketTransport } from './ServerWebSocketTransport';
import type { TransportEvent } from './types';
import { TransportErrorCode } from './types';

// --- Mock WebSocket ---

type MockWSHandler = ((event: { data: string }) => void) | null;

class MockWebSocket {
  static CONNECTING = 0;
  static OPEN = 1;
  static CLOSING = 2;
  static CLOSED = 3;

  readyState = MockWebSocket.CONNECTING;
  onopen: (() => void) | null = null;
  onclose: ((event: { code: number; reason: string }) => void) | null = null;
  onerror: (() => void) | null = null;
  onmessage: MockWSHandler = null;
  sent: string[] = [];

  constructor(public url: string) {
    // Auto-open on next tick unless test intervenes
    setTimeout(() => {
      if (this.readyState === MockWebSocket.CONNECTING) {
        this.readyState = MockWebSocket.OPEN;
        this.onopen?.();
      }
    }, 0);
  }

  send(data: string): void {
    this.sent.push(data);
  }

  close(): void {
    this.readyState = MockWebSocket.CLOSED;
    this.onclose?.({ code: 1000, reason: 'Normal closure' });
  }

  // Test helpers
  simulateMessage(data: unknown): void {
    this.onmessage?.({ data: JSON.stringify(data) });
  }

  simulateError(): void {
    this.onerror?.();
  }

  simulateClose(code = 1006, reason = ''): void {
    this.readyState = MockWebSocket.CLOSED;
    this.onclose?.({ code, reason });
  }
}

let mockWsInstance: MockWebSocket | null = null;

function collectEvents(transport: ServerWebSocketTransport): TransportEvent[] {
  const events: TransportEvent[] = [];
  transport.addEventListener(e => events.push(e));
  return events;
}

beforeEach(() => {
  mockWsInstance = null;
  vi.stubGlobal('WebSocket', class extends MockWebSocket {
    constructor(url: string) {
      super(url);
      mockWsInstance = this;
    }
  });
});

afterEach(() => {
  vi.restoreAllMocks();
});

describe('ServerWebSocketTransport', () => {
  const SERVER_URL = 'http://blinkyhost.local:8420';
  const DEVICE_ID = 'ABC123';

  describe('connect', () => {
    it('connects to the correct WebSocket URL', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      await transport.connect();
      expect(mockWsInstance?.url).toBe('ws://blinkyhost.local:8420/ws/ABC123');
    });

    it('converts https to wss', async () => {
      const transport = new ServerWebSocketTransport('https://example.com', DEVICE_ID);
      await transport.connect();
      expect(mockWsInstance?.url).toBe('wss://example.com/ws/ABC123');
    });

    it('emits connected event', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      const events = collectEvents(transport);
      await transport.connect();
      expect(events).toContainEqual({ type: 'connected' });
    });

    it('reports isConnected() after connect', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      expect(transport.isConnected()).toBe(false);
      await transport.connect();
      expect(transport.isConnected()).toBe(true);
    });

    it('throws on connection failure', async () => {
      vi.stubGlobal('WebSocket', class {
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;
        readyState = 0;
        url: string;
        onopen: (() => void) | null = null;
        onclose: ((e: unknown) => void) | null = null;
        onerror: (() => void) | null = null;
        onmessage: ((e: unknown) => void) | null = null;
        sent: string[] = [];
        constructor(url: string) {
          this.url = url;
          mockWsInstance = this as unknown as MockWebSocket;
          // Fire error WITHOUT opening — simulates connection refused
          setTimeout(() => this.onerror?.(), 0);
        }
        send(d: string) { this.sent.push(d); }
        close() { this.readyState = 3; }
      });

      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      await expect(transport.connect()).rejects.toThrow('WebSocket connection failed');
    });
  });

  describe('disconnect', () => {
    it('emits disconnected event', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      const events = collectEvents(transport);
      await transport.connect();
      events.length = 0;

      await transport.disconnect();
      expect(events).toContainEqual({ type: 'disconnected' });
    });

    it('is idempotent', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      await transport.connect();
      await transport.disconnect();
      await transport.disconnect(); // no throw
    });

    it('reports isConnected() false after disconnect', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      await transport.connect();
      await transport.disconnect();
      expect(transport.isConnected()).toBe(false);
    });
  });

  describe('send', () => {
    it('sends commands as JSON envelope', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      await transport.connect();
      await transport.send('json info');
      expect(JSON.parse(mockWsInstance!.sent[0])).toEqual({
        type: 'command',
        command: 'json info',
      });
    });

    it('converts stream commands to stream_control envelope', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      await transport.connect();

      await transport.send('stream on');
      expect(JSON.parse(mockWsInstance!.sent[0])).toEqual({
        type: 'stream_control',
        enabled: true,
        mode: 'on',
      });

      await transport.send('stream off');
      expect(JSON.parse(mockWsInstance!.sent[1])).toEqual({
        type: 'stream_control',
        enabled: false,
        mode: 'off',
      });

      await transport.send('stream fast');
      expect(JSON.parse(mockWsInstance!.sent[2])).toEqual({
        type: 'stream_control',
        enabled: true,
        mode: 'fast',
      });
    });

    it('throws when not connected', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      await expect(transport.send('hello')).rejects.toThrow('not connected');
    });
  });

  describe('incoming messages', () => {
    it('emits streaming data as line events', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      const events = collectEvents(transport);
      await transport.connect();
      events.length = 0;

      mockWsInstance!.simulateMessage({
        type: 'audio',
        device_id: DEVICE_ID,
        data: { a: 42, e: 0.5 },
      });

      expect(events).toHaveLength(1);
      expect(events[0].type).toBe('line');
      expect(JSON.parse(events[0].line!)).toEqual({ a: 42, e: 0.5 });
    });

    it('emits command responses as line events', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      const events = collectEvents(transport);
      await transport.connect();
      events.length = 0;

      mockWsInstance!.simulateMessage({
        type: 'response',
        device_id: DEVICE_ID,
        command: 'json info',
        response: '{"device":{"type":"hat","name":"Test"}}',
      });

      expect(events).toHaveLength(1);
      expect(events[0].type).toBe('line');
      expect(events[0].line).toBe('{"device":{"type":"hat","name":"Test"}}');
    });

    it('handles multi-line responses', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      const events = collectEvents(transport);
      await transport.connect();
      events.length = 0;

      mockWsInstance!.simulateMessage({
        type: 'response',
        device_id: DEVICE_ID,
        command: 'show nn',
        response: 'onset=0.42\nready=yes\n',
      });

      expect(events).toHaveLength(2);
      expect(events[0].line).toBe('onset=0.42');
      expect(events[1].line).toBe('ready=yes');
    });

    it('handles battery messages', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      const events = collectEvents(transport);
      await transport.connect();
      events.length = 0;

      mockWsInstance!.simulateMessage({
        type: 'battery',
        device_id: DEVICE_ID,
        data: { b: 85, c: false },
      });

      expect(events[0].line).toBe('{"b":85,"c":false}');
    });

    it('ignores unparseable messages', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      const events = collectEvents(transport);
      await transport.connect();
      events.length = 0;

      // Simulate raw string that isn't JSON
      mockWsInstance!.onmessage?.({ data: 'not json' });
      expect(events).toHaveLength(0);
    });
  });

  describe('server-initiated disconnect', () => {
    it('emits disconnected when server closes WebSocket', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      const events = collectEvents(transport);
      await transport.connect();
      events.length = 0;

      mockWsInstance!.simulateClose(1006, 'Connection lost');
      expect(events).toContainEqual({ type: 'disconnected' });
      expect(transport.isConnected()).toBe(false);
    });
  });

  describe('isSupported', () => {
    it('returns true when WebSocket is available', () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      expect(transport.isSupported()).toBe(true);
    });
  });

  describe('edge cases', () => {
    it('throws if connect() called while connecting', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      const p1 = transport.connect();
      await expect(transport.connect()).rejects.toThrow('already in progress');
      await p1;
    });

    it('connect() is no-op when already connected', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      const events = collectEvents(transport);
      await transport.connect();
      events.length = 0;
      await transport.connect(); // no-op
      expect(events).toHaveLength(0); // no second connected event
    });

    it('disconnect() before connect emits disconnected without error', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      const events = collectEvents(transport);
      await transport.disconnect();
      expect(events).toContainEqual({ type: 'disconnected' });
    });

    it('send() after disconnect throws', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      await transport.connect();
      await transport.disconnect();
      await expect(transport.send('hello')).rejects.toThrow('not connected');
    });

    it('silently drops messages with neither data nor response', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, DEVICE_ID);
      const events = collectEvents(transport);
      await transport.connect();
      events.length = 0;

      mockWsInstance!.simulateMessage({ type: 'internal', device_id: DEVICE_ID });
      expect(events).toHaveLength(0);
    });

    it('URL-encodes device ID with special characters', async () => {
      const transport = new ServerWebSocketTransport(SERVER_URL, 'device with spaces');
      await transport.connect();
      expect(mockWsInstance?.url).toBe('ws://blinkyhost.local:8420/ws/device%20with%20spaces');
    });
  });
});
