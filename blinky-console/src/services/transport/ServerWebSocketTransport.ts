/**
 * ServerWebSocketTransport — communicates with a device through a blinky-server
 * WebSocket proxy at `/ws/{device_id}`.
 *
 * The server relays device serial lines as JSON envelopes:
 *   Inbound streaming: { type: "audio"|"battery"|..., device_id, data: {...} }
 *   Inbound response:  { type: "response", device_id, command, response }
 *   Outbound command:   { type: "command", command: "..." }
 *   Outbound stream:    { type: "stream_control", enabled, mode }
 *
 * This transport unwraps envelopes and emits `line` events containing the
 * inner data/response text, so DeviceProtocol sees the same format it would
 * from WebSerial (e.g., `{"a":...}` for audio, raw text for command responses).
 */

import { logger } from '../../lib/logger';
import type { Transport, TransportEvent, TransportEventCallback } from './types';
import { TransportError, TransportErrorCode } from './types';

/** Message envelope from the server's WebSocket stream. */
interface ServerMessage {
  type: string;
  device_id?: string;
  data?: unknown;
  command?: string;
  response?: string;
}

export class ServerWebSocketTransport implements Transport {
  private ws: WebSocket | null = null;
  private listeners: TransportEventCallback[] = [];

  constructor(
    public readonly serverUrl: string,
    public readonly deviceId: string,
  ) {}

  isSupported(): boolean {
    return typeof WebSocket !== 'undefined';
  }

  isConnected(): boolean {
    return this.ws?.readyState === WebSocket.OPEN;
  }

  async connect(): Promise<void> {
    if (this.isConnected()) return;

    const wsUrl = this.serverUrl.replace(/^http/, 'ws') + `/ws/${this.deviceId}`;
    logger.info('Connecting to server WebSocket', { url: wsUrl });

    return new Promise<void>((resolve, reject) => {
      const ws = new WebSocket(wsUrl);
      let settled = false;

      ws.onopen = () => {
        if (settled) return;
        settled = true;
        this.ws = ws;
        logger.info('Server WebSocket connected', { deviceId: this.deviceId });
        this.emit({ type: 'connected' });
        resolve();
      };

      ws.onerror = () => {
        if (!settled) {
          settled = true;
          reject(new TransportError(
            `WebSocket connection failed: ${wsUrl}`,
            TransportErrorCode.CONNECTION_FAILED,
          ));
        }
      };

      ws.onclose = (event) => {
        // Prevent the onclose from firing a disconnect during a failed connect
        // (the reject above handles that case).
        if (!settled) {
          settled = true;
          reject(new TransportError(
            `WebSocket closed during connect: code ${event.code}`,
            TransportErrorCode.CONNECTION_FAILED,
          ));
          return;
        }
        logger.info('Server WebSocket closed', { code: event.code, reason: event.reason });
        this.ws = null;
        this.emit({ type: 'disconnected' });
      };

      ws.onmessage = (event) => this.handleMessage(event.data);
    });
  }

  async disconnect(): Promise<void> {
    const ws = this.ws;
    this.ws = null;

    if (ws) {
      // Remove handlers to prevent double-fire of disconnected
      ws.onclose = null;
      ws.onmessage = null;
      ws.onerror = null;
      ws.close();
      logger.info('Server WebSocket disconnected', { deviceId: this.deviceId });
    }

    this.emit({ type: 'disconnected' });
  }

  async send(text: string): Promise<void> {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      throw new TransportError(
        'Cannot send: not connected to server',
        TransportErrorCode.NOT_CONNECTED,
      );
    }

    // Convert firmware stream commands to server envelope format.
    // DeviceProtocol sends "stream on", "stream fast", "stream off", etc.
    const streamMatch = text.match(/^stream\s+(\S+)/);
    if (streamMatch) {
      const mode = streamMatch[1];
      this.ws.send(JSON.stringify({
        type: 'stream_control',
        enabled: mode !== 'off',
        mode,
      }));
      return;
    }

    // All other commands go through the command envelope
    this.ws.send(JSON.stringify({ type: 'command', command: text }));
  }

  addEventListener(callback: TransportEventCallback): void {
    this.listeners.push(callback);
  }

  removeEventListener(callback: TransportEventCallback): void {
    this.listeners = this.listeners.filter(cb => cb !== callback);
  }

  // --- Private ---

  private emit(event: TransportEvent): void {
    for (const cb of this.listeners) {
      try {
        cb(event);
      } catch (e) {
        logger.warn('Transport listener threw', { error: e });
      }
    }
  }

  private handleMessage(raw: string): void {
    let msg: ServerMessage;
    try {
      msg = JSON.parse(raw);
    } catch {
      logger.warn('ServerWS: unparseable message', { raw: raw.slice(0, 200) });
      return;
    }

    if (msg.type === 'response' && msg.response != null) {
      // Command response — emit response text as a line so DeviceProtocol's
      // request/response pairing works. The response is the raw firmware text
      // (e.g., JSON for `json info`, or "OK" for `stream off`).
      // Response may be multi-line; split and emit each line separately.
      const lines = String(msg.response).split('\n');
      for (const line of lines) {
        const trimmed = line.trim();
        if (trimmed) {
          this.emit({ type: 'line', line: trimmed });
        }
      }
    } else if (msg.data != null) {
      // Streaming data (audio, battery, status, etc.) — re-stringify the
      // inner data field to produce the same JSON format DeviceProtocol
      // expects from WebSerial (e.g., {"a":...} for audio frames).
      const line = typeof msg.data === 'string' ? msg.data : JSON.stringify(msg.data);
      if (line) {
        this.emit({ type: 'line', line });
      }
    }
  }
}
