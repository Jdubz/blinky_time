/**
 * Transport interface — byte-level communication with a device.
 *
 * Transports speak to one device at a time via a wire-format-specific channel
 * (WebSerial USB, Web Bluetooth characteristic, blinky-server WebSocket, ...).
 * They expose a uniform interface so higher layers (DeviceProtocol, Source)
 * don't care which channel is in use.
 *
 * Transport is byte-oriented at its edges but delivers complete newline-
 * delimited text lines via the `line` event — each transport handles whatever
 * framing its wire format needs (UTF-8 decode + line split for raw bytes,
 * envelope unwrap for server WS, ...). Higher layers parse lines as JSON.
 *
 * Design rules:
 * - Events are the ONLY way to receive inbound data. No polling, no pull.
 * - `connect()` throws on failure; it does NOT emit an error event. The
 *   caller decides how to surface the failure.
 * - `disconnect()` is idempotent and always emits a `disconnected` event.
 * - Inbound read failures emit `error` AND fire a `disconnected` event.
 * - `send()` throws on failure (synchronous error path).
 */

export enum TransportErrorCode {
  NOT_SUPPORTED = 'NOT_SUPPORTED',
  NOT_CONNECTED = 'NOT_CONNECTED',
  CONNECTION_FAILED = 'CONNECTION_FAILED',
  DISCONNECTED = 'DISCONNECTED',
  PERMISSION_DENIED = 'PERMISSION_DENIED',
  DEVICE_LOST = 'DEVICE_LOST',
  PORT_IN_USE = 'PORT_IN_USE',
  IO_ERROR = 'IO_ERROR',
}

export class TransportError extends Error {
  constructor(
    message: string,
    public readonly code: TransportErrorCode
  ) {
    super(message);
    this.name = 'TransportError';
  }
}

export type TransportEventType = 'connected' | 'disconnected' | 'line' | 'error';

export interface TransportEvent {
  type: TransportEventType;
  /** Present on `line` events. Already UTF-8-decoded, trimmed, and non-empty. */
  line?: string;
  /** Present on `error` events. */
  error?: TransportError;
}

export type TransportEventCallback = (event: TransportEvent) => void;

export interface Transport {
  /** Whether this transport is usable in the current environment. */
  isSupported(): boolean;

  /** Whether currently connected to a peer. */
  isConnected(): boolean;

  /**
   * Open the transport. May prompt the user (e.g. WebSerial port picker).
   * Throws {@link TransportError} on failure; emits `connected` on success.
   */
  connect(): Promise<void>;

  /** Close the transport gracefully. Idempotent. Emits `disconnected`. */
  disconnect(): Promise<void>;

  /**
   * Send a single line of text. Any framing/newline the wire requires is
   * added by the transport. Throws {@link TransportError} on failure.
   */
  send(text: string): Promise<void>;

  addEventListener(callback: TransportEventCallback): void;
  removeEventListener(callback: TransportEventCallback): void;
}
