import { logger } from '../../lib/logger';
import {
  Transport,
  TransportError,
  TransportErrorCode,
  TransportEvent,
  TransportEventCallback,
} from './types';

// Max buffer size (16KB) before forced truncation — protects against
// runaway firmware output that never emits a newline.
const MAX_BUFFER_SIZE = 16384;

function classifyError(error: unknown): TransportErrorCode {
  if (error instanceof DOMException) {
    switch (error.name) {
      case 'NotFoundError':
        return TransportErrorCode.NOT_CONNECTED;
      case 'SecurityError':
        return TransportErrorCode.PERMISSION_DENIED;
      case 'InvalidStateError':
        return TransportErrorCode.PORT_IN_USE;
      case 'NetworkError':
        return TransportErrorCode.DEVICE_LOST;
      case 'AbortError':
        return TransportErrorCode.DISCONNECTED;
    }
  }
  return TransportErrorCode.IO_ERROR;
}

/**
 * Transport backed by the WebSerial API — talks directly to a USB serial
 * device from the browser. Requires a user gesture to open the port picker.
 */
export class WebSerialTransport implements Transport {
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private listeners: TransportEventCallback[] = [];
  private buffer = '';
  private isReading = false;

  constructor(public readonly baudRate: number = 115200) {}

  isSupported(): boolean {
    return 'serial' in navigator;
  }

  isConnected(): boolean {
    return this.port !== null && this.writer !== null;
  }

  addEventListener(callback: TransportEventCallback): void {
    this.listeners.push(callback);
  }

  removeEventListener(callback: TransportEventCallback): void {
    this.listeners = this.listeners.filter(l => l !== callback);
  }

  private emit(event: TransportEvent): void {
    this.listeners.forEach(cb => cb(event));
  }

  async connect(): Promise<void> {
    logger.info('Opening WebSerial port', { baudRate: this.baudRate });

    if (!this.isSupported()) {
      throw new TransportError(
        'WebSerial API not supported in this browser',
        TransportErrorCode.NOT_SUPPORTED
      );
    }

    try {
      this.port = await navigator.serial.requestPort();
      await this.port.open({ baudRate: this.baudRate });

      if (!this.port.readable) {
        throw new TransportError('Port is not readable', TransportErrorCode.CONNECTION_FAILED);
      }
      this.reader = this.port.readable.getReader();
      this.startReading();

      if (!this.port.writable) {
        throw new TransportError('Port is not writable', TransportErrorCode.CONNECTION_FAILED);
      }
      this.writer = this.port.writable.getWriter();

      logger.info('WebSerial connected');
      this.emit({ type: 'connected' });
    } catch (error) {
      // Clean up any partial state before re-raising.
      await this.disconnect().catch(() => {});
      if (error instanceof TransportError) throw error;
      const code = classifyError(error);
      const message = error instanceof Error ? error.message : 'Connection failed';
      throw new TransportError(message, code);
    }
  }

  async disconnect(): Promise<void> {
    const wasConnected = this.isConnected();
    this.isReading = false;

    if (this.reader) {
      try {
        await this.reader.cancel();
      } catch (e) {
        logger.warn('reader.cancel failed', { error: e });
      }
      try {
        this.reader.releaseLock();
      } catch (e) {
        logger.warn('reader.releaseLock failed', { error: e });
      }
      this.reader = null;
    }

    if (this.writer) {
      try {
        this.writer.releaseLock();
      } catch (e) {
        logger.warn('writer.releaseLock failed', { error: e });
      }
      this.writer = null;
    }

    if (this.port) {
      try {
        await this.port.close();
      } catch (e) {
        logger.warn('port.close failed', { error: e });
      }
      this.port = null;
    }

    this.buffer = '';

    if (wasConnected) {
      logger.info('WebSerial disconnected');
    }
    this.emit({ type: 'disconnected' });
  }

  async send(text: string): Promise<void> {
    if (!this.writer) {
      throw new TransportError('Cannot send: not connected', TransportErrorCode.NOT_CONNECTED);
    }
    try {
      const encoder = new TextEncoder();
      await this.writer.write(encoder.encode(text + '\n'));
    } catch (error) {
      const code = classifyError(error);
      const message = error instanceof Error ? error.message : 'Send failed';
      throw new TransportError(message, code);
    }
  }

  /** Background loop: drain the ReadableStream, decode, split on newlines. */
  private async startReading(): Promise<void> {
    if (!this.reader) return;
    this.isReading = true;
    const decoder = new TextDecoder();

    try {
      while (this.isReading) {
        const { value, done } = await this.reader.read();
        if (done) break;

        this.buffer += decoder.decode(value);

        if (this.buffer.length > MAX_BUFFER_SIZE) {
          // Keep the tail — the latest partial line is what we care about.
          this.buffer = this.buffer.substring(this.buffer.length - MAX_BUFFER_SIZE / 2);
        }

        const lines = this.buffer.split('\n');
        this.buffer = lines.pop() ?? '';

        for (const line of lines) {
          const trimmed = line.trim();
          if (trimmed) {
            this.emit({ type: 'line', line: trimmed });
          }
        }
      }
    } catch (error) {
      if (this.isReading) {
        const code = classifyError(error);
        const message = error instanceof Error ? error.message : 'Read error';
        this.emit({ type: 'error', error: new TransportError(message, code) });
        if (code === TransportErrorCode.DEVICE_LOST || code === TransportErrorCode.DISCONNECTED) {
          // Proactively tear down — the port is gone.
          this.disconnect().catch(() => {});
        }
      }
    }
  }
}
