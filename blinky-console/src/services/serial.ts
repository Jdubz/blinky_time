import {
  DeviceInfo,
  SettingsResponse,
  AudioMessage,
  BatteryMessage,
  TransientMessage,
  RhythmMessage,
  StatusMessage,
  GeneratorType,
  EffectType,
} from '../types';

// Custom error classes for better error handling
export class SerialError extends Error {
  constructor(
    message: string,
    public readonly code: SerialErrorCode
  ) {
    super(message);
    this.name = 'SerialError';
  }
}

export enum SerialErrorCode {
  NOT_SUPPORTED = 'NOT_SUPPORTED',
  NOT_CONNECTED = 'NOT_CONNECTED',
  CONNECTION_FAILED = 'CONNECTION_FAILED',
  DISCONNECTED = 'DISCONNECTED',
  COMMAND_INVALID = 'COMMAND_INVALID',
  COMMAND_FAILED = 'COMMAND_FAILED',
  TIMEOUT = 'TIMEOUT',
  PARSE_ERROR = 'PARSE_ERROR',
  PORT_IN_USE = 'PORT_IN_USE',
  PERMISSION_DENIED = 'PERMISSION_DENIED',
  DEVICE_LOST = 'DEVICE_LOST',
}

// Map native error types to our error codes
function classifyError(error: unknown): SerialErrorCode {
  if (error instanceof DOMException) {
    switch (error.name) {
      case 'NotFoundError':
        return SerialErrorCode.NOT_CONNECTED;
      case 'SecurityError':
        return SerialErrorCode.PERMISSION_DENIED;
      case 'InvalidStateError':
        return SerialErrorCode.PORT_IN_USE;
      case 'NetworkError':
        return SerialErrorCode.DEVICE_LOST;
      case 'AbortError':
        return SerialErrorCode.DISCONNECTED;
    }
  }
  return SerialErrorCode.CONNECTION_FAILED;
}

// WebSerial type declarations
declare global {
  interface Navigator {
    serial: Serial;
  }
  interface Serial {
    requestPort(options?: SerialPortRequestOptions): Promise<SerialPort>;
    getPorts(): Promise<SerialPort[]>;
  }
  interface SerialPortRequestOptions {
    filters?: SerialPortFilter[];
  }
  interface SerialPortFilter {
    usbVendorId?: number;
    usbProductId?: number;
  }
  interface SerialPort {
    open(options: SerialOptions): Promise<void>;
    close(): Promise<void>;
    readable: ReadableStream<Uint8Array> | null;
    writable: WritableStream<Uint8Array> | null;
    getInfo(): SerialPortInfo;
  }
  interface SerialOptions {
    baudRate: number;
    dataBits?: number;
    stopBits?: number;
    parity?: 'none' | 'even' | 'odd';
    bufferSize?: number;
    flowControl?: 'none' | 'hardware';
  }
  interface SerialPortInfo {
    usbVendorId?: number;
    usbProductId?: number;
  }
}

export type SerialEventType =
  | 'connected'
  | 'disconnected'
  | 'data'
  | 'error'
  | 'audio'
  | 'battery'
  | 'batteryStatus'
  | 'transient'
  | 'rhythm'
  | 'status';

export interface BatteryStatusData {
  voltage: number; // Battery voltage in volts
  percent: number; // Battery percentage (0-100)
  charging: boolean; // True if currently charging
  connected: boolean; // True if battery is connected
}

export interface SerialEvent {
  type: SerialEventType;
  data?: string;
  audio?: AudioMessage;
  battery?: BatteryMessage;
  batteryStatus?: BatteryStatusData;
  transient?: TransientMessage;
  rhythm?: RhythmMessage;
  status?: StatusMessage;
  error?: Error;
}

export type SerialEventCallback = (event: SerialEvent) => void;

// Constants for safety limits
const MAX_BUFFER_SIZE = 4096; // Max buffer size before truncation
const MAX_COMMAND_LENGTH = 128; // Max command length to send
const ALLOWED_COMMAND_PATTERN = /^[a-zA-Z0-9_\-.\s]+$/; // Alphanumeric + basic chars

class SerialService {
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private listeners: SerialEventCallback[] = [];
  private buffer: string = '';
  private isReading: boolean = false;

  // Check if WebSerial is supported
  isSupported(): boolean {
    return 'serial' in navigator;
  }

  // Add event listener
  addEventListener(callback: SerialEventCallback): void {
    this.listeners.push(callback);
  }

  // Remove event listener
  removeEventListener(callback: SerialEventCallback): void {
    this.listeners = this.listeners.filter(l => l !== callback);
  }

  // Emit event to all listeners
  private emit(event: SerialEvent): void {
    this.listeners.forEach(callback => callback(event));
  }

  // Request and connect to a serial port
  async connect(baudRate: number = 115200): Promise<boolean> {
    if (!this.isSupported()) {
      const error = new SerialError(
        'WebSerial API not supported in this browser',
        SerialErrorCode.NOT_SUPPORTED
      );
      this.emit({ type: 'error', error });
      return false;
    }

    try {
      // Request port from user
      this.port = await navigator.serial.requestPort();

      // Open with specified baud rate
      await this.port.open({ baudRate });

      // Set up reader and writer
      if (this.port.readable) {
        this.reader = this.port.readable.getReader();
        this.startReading();
      } else {
        throw new SerialError('Port is not readable', SerialErrorCode.CONNECTION_FAILED);
      }

      if (this.port.writable) {
        this.writer = this.port.writable.getWriter();
      } else {
        throw new SerialError('Port is not writable', SerialErrorCode.CONNECTION_FAILED);
      }

      this.emit({ type: 'connected' });
      return true;
    } catch (error) {
      // Clean up partial connection state
      await this.disconnect().catch(() => {});

      // Classify and emit the error
      if (error instanceof SerialError) {
        this.emit({ type: 'error', error });
      } else {
        const code = classifyError(error);
        const message = error instanceof Error ? error.message : 'Connection failed';
        const serialError = new SerialError(message, code);
        this.emit({ type: 'error', error: serialError });
      }
      return false;
    }
  }

  // Disconnect from serial port
  async disconnect(): Promise<void> {
    this.isReading = false;

    // Release reader
    if (this.reader) {
      try {
        await this.reader.cancel();
      } catch (e) {
        console.warn('Error canceling reader:', e);
      }
      try {
        this.reader.releaseLock();
      } catch (e) {
        console.warn('Error releasing reader lock:', e);
      }
      this.reader = null;
    }

    // Release writer
    if (this.writer) {
      try {
        this.writer.releaseLock();
      } catch (e) {
        console.warn('Error releasing writer lock:', e);
      }
      this.writer = null;
    }

    // Close port
    if (this.port) {
      try {
        await this.port.close();
      } catch (e) {
        console.warn('Error closing port:', e);
      }
      this.port = null;
    }

    // Clear buffers
    this.buffer = '';

    this.emit({ type: 'disconnected' });
  }

  // Check if connected
  isConnected(): boolean {
    return this.port !== null && this.writer !== null;
  }

  // Validate command before sending
  private validateCommand(command: string): string {
    // Trim and limit length
    let sanitized = command.trim().substring(0, MAX_COMMAND_LENGTH);

    // Check for allowed characters (alphanumeric, spaces, basic punctuation)
    if (!ALLOWED_COMMAND_PATTERN.test(sanitized)) {
      // Remove any control characters or special chars
      sanitized = sanitized.replace(/[^\w\s.-]/g, '');
    }

    return sanitized;
  }

  // Send a command (with validation)
  async send(command: string): Promise<void> {
    if (!this.writer) {
      throw new SerialError('Cannot send: not connected to device', SerialErrorCode.NOT_CONNECTED);
    }

    const sanitized = this.validateCommand(command);
    if (!sanitized) {
      throw new SerialError(
        `Invalid command: "${command.substring(0, 20)}"`,
        SerialErrorCode.COMMAND_INVALID
      );
    }

    try {
      const encoder = new TextEncoder();
      const data = encoder.encode(sanitized + '\n');
      await this.writer.write(data);
    } catch (error) {
      const code = classifyError(error);
      const message = error instanceof Error ? error.message : 'Failed to send command';
      throw new SerialError(message, code);
    }
  }

  // Result type for sendAndReceiveJson with error details
  private sendJsonResult<T>(
    data: T | null,
    error?: SerialError
  ): { data: T | null; error?: SerialError } {
    return { data, error };
  }

  // Send command and wait for JSON response
  async sendAndReceiveJson<T>(command: string, timeoutMs: number = 2000): Promise<T | null> {
    const result = await this.sendAndReceiveJsonWithError<T>(command, timeoutMs);
    return result.data;
  }

  // Send command and wait for JSON response, with detailed error info
  async sendAndReceiveJsonWithError<T>(
    command: string,
    timeoutMs: number = 2000
  ): Promise<{ data: T | null; error?: SerialError }> {
    return new Promise(resolve => {
      let resolved = false;
      let parseAttempts = 0;
      const maxParseAttempts = 10; // Prevent infinite parsing attempts

      const cleanup = () => {
        this.removeEventListener(handler);
      };

      const timeout = setTimeout(() => {
        if (!resolved) {
          resolved = true;
          cleanup();
          resolve(
            this.sendJsonResult<T>(
              null,
              new SerialError(
                `Timeout waiting for response to "${command}" (${timeoutMs}ms)`,
                SerialErrorCode.TIMEOUT
              )
            )
          );
        }
      }, timeoutMs);

      const handler = (event: SerialEvent) => {
        if (event.type === 'data' && event.data) {
          // Each 'data' event is already a single line from startReading()
          // Check this line directly instead of buffering (which loses line boundaries)
          const trimmed = event.data.trim();

          // Check if this line is a complete JSON object
          if (trimmed.startsWith('{') && trimmed.endsWith('}')) {
            parseAttempts++;
            if (parseAttempts <= maxParseAttempts) {
              try {
                const parsed = JSON.parse(trimmed) as T;
                if (!resolved) {
                  resolved = true;
                  clearTimeout(timeout);
                  cleanup();
                  resolve(this.sendJsonResult(parsed));
                }
                return;
              } catch {
                // Not valid JSON, continue waiting for next event
              }
            }
          }
        }

        // Handle disconnect during wait
        if (event.type === 'disconnected' || event.type === 'error') {
          if (!resolved) {
            resolved = true;
            clearTimeout(timeout);
            cleanup();
            resolve(
              this.sendJsonResult<T>(
                null,
                new SerialError(
                  'Device disconnected while waiting for response',
                  SerialErrorCode.DISCONNECTED
                )
              )
            );
          }
        }
      };

      this.addEventListener(handler);

      this.send(command).catch((error: Error) => {
        if (!resolved) {
          resolved = true;
          clearTimeout(timeout);
          cleanup();
          const serialError =
            error instanceof SerialError
              ? error
              : new SerialError(error.message, SerialErrorCode.COMMAND_FAILED);
          resolve(this.sendJsonResult<T>(null, serialError));
        }
      });
    });
  }

  // Get device info
  async getDeviceInfo(): Promise<DeviceInfo | null> {
    return this.sendAndReceiveJson<DeviceInfo>('json info');
  }

  // Get all settings
  async getSettings(): Promise<SettingsResponse | null> {
    return this.sendAndReceiveJson<SettingsResponse>('json settings');
  }

  // Set a setting value
  async setSetting(name: string, value: number | boolean): Promise<void> {
    await this.send(`set ${name} ${value}`);
  }

  // Enable/disable audio streaming
  async setStreamEnabled(enabled: boolean): Promise<void> {
    await this.send(enabled ? 'stream on' : 'stream off');
  }

  // Save settings to flash
  async saveSettings(): Promise<void> {
    await this.send('save');
  }

  // Load settings from flash
  async loadSettings(): Promise<void> {
    await this.send('load');
  }

  // Reset to defaults
  async resetDefaults(): Promise<void> {
    await this.send('defaults');
  }

  // Request battery status data
  async requestBatteryStatus(): Promise<void> {
    await this.send('battery');
  }

  // Apply a preset by name
  async applyPreset(name: string): Promise<void> {
    await this.send(`preset ${name}`);
  }

  // Get list of available presets
  async getPresets(): Promise<string[] | null> {
    const response = await this.sendAndReceiveJson<{ presets: string[] }>('json presets');
    return response?.presets || null;
  }

  // Set active generator
  async setGenerator(name: GeneratorType): Promise<void> {
    await this.send(`gen ${name}`);
  }

  // Set active effect
  async setEffect(name: EffectType): Promise<void> {
    await this.send(`effect ${name}`);
  }

  // Start reading from serial port
  private async startReading(): Promise<void> {
    if (!this.reader) return;

    this.isReading = true;
    const decoder = new TextDecoder();

    try {
      while (this.isReading) {
        const { value, done } = await this.reader.read();
        if (done) break;

        const text = decoder.decode(value);
        this.buffer += text;

        // Prevent unbounded buffer growth - truncate if too large
        if (this.buffer.length > MAX_BUFFER_SIZE) {
          // Keep only the last portion that might contain complete data
          this.buffer = this.buffer.substring(this.buffer.length - MAX_BUFFER_SIZE / 2);
        }

        // Process complete lines
        const lines = this.buffer.split('\n');
        this.buffer = lines.pop() || ''; // Keep incomplete line in buffer

        for (const line of lines) {
          const trimmed = line.trim();
          if (!trimmed) continue;

          // Check if it's an audio streaming message
          if (trimmed.startsWith('{"a":')) {
            try {
              const audioMsg = JSON.parse(trimmed) as AudioMessage;
              this.emit({ type: 'audio', audio: audioMsg });
              continue;
            } catch {
              // Not valid audio JSON
            }
          }

          // Check if it's a battery streaming message
          if (trimmed.startsWith('{"b":')) {
            try {
              const batteryMsg = JSON.parse(trimmed) as BatteryMessage;
              this.emit({ type: 'battery', battery: batteryMsg });
              continue;
            } catch {
              // Not valid battery JSON
            }
          }

          // Check if it's a battery status message
          if (trimmed.startsWith('{"battery":')) {
            try {
              const parsed = JSON.parse(trimmed) as { battery: BatteryStatusData };
              this.emit({ type: 'batteryStatus', batteryStatus: parsed.battery });
              continue;
            } catch {
              // Not valid battery status JSON
            }
          }

          // Check if it's a transient detection message
          if (trimmed.startsWith('{"type":"TRANSIENT"')) {
            try {
              const transMsg = JSON.parse(trimmed) as TransientMessage;
              // Support legacy timestampMs field
              if (!transMsg.ts && transMsg.timestampMs) {
                transMsg.ts = transMsg.timestampMs;
              }
              this.emit({ type: 'transient', transient: transMsg });
              continue;
            } catch {
              // Not valid transient JSON
            }
          }

          // Check if it's a rhythm analyzer message
          if (trimmed.startsWith('{"type":"RHYTHM"')) {
            try {
              const rhythmMsg = JSON.parse(trimmed) as RhythmMessage;
              this.emit({ type: 'rhythm', rhythm: rhythmMsg });
              continue;
            } catch {
              // Not valid rhythm JSON
            }
          }

          // Check if it's a status message
          if (trimmed.startsWith('{"type":"STATUS"')) {
            try {
              const statusMsg = JSON.parse(trimmed) as StatusMessage;
              this.emit({ type: 'status', status: statusMsg });
              continue;
            } catch {
              // Not valid status JSON
            }
          }

          // Regular data
          this.emit({ type: 'data', data: trimmed });
        }
      }
    } catch (error) {
      if (this.isReading) {
        const code = classifyError(error);
        const message = error instanceof Error ? error.message : 'Read error';
        const serialError = new SerialError(message, code);
        this.emit({ type: 'error', error: serialError });

        // If device was lost, trigger disconnection
        if (code === SerialErrorCode.DEVICE_LOST || code === SerialErrorCode.DISCONNECTED) {
          this.disconnect().catch(() => {});
        }
      }
    }
  }

  // Get last error if any - useful for debugging
  getConnectionState(): { connected: boolean; readable: boolean; writable: boolean } {
    return {
      connected: this.port !== null,
      readable: this.reader !== null,
      writable: this.writer !== null,
    };
  }
}

// Singleton instance
export const serialService = new SerialService();
