import {
  DeviceInfo,
  DeviceInfoSchema,
  SettingsResponse,
  SettingsResponseSchema,
  AudioMessage,
  AudioMessageSchema,
  BatteryMessage,
  BatteryMessageSchema,
  BatteryStatusResponseSchema,
  TransientMessage,
  TransientMessageSchema,
  RhythmMessage,
  RhythmMessageSchema,
  StatusMessage,
  StatusMessageSchema,
  GeneratorType,
  EffectType,
} from '../types';
import { logger } from '../lib/logger';

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
const MAX_BUFFER_SIZE = 16384; // Max buffer size before truncation (16KB for large JSON responses)
const MAX_COMMAND_LENGTH = 128; // Max command length to send
const ALLOWED_COMMAND_PATTERN = /^[a-zA-Z0-9_\-.\s]+$/; // Alphanumeric + basic chars

// Valid category names for settings (matches firmware SettingsRegistry categories)
const VALID_CATEGORIES = [
  'fire',
  'firemusic',
  'fireorganic',
  'water',
  'lightning',
  'audio',
  'agc',
  'transient',
  'detection',
  'rhythm',
] as const;

type SettingsCategory = (typeof VALID_CATEGORIES)[number];

function isValidCategory(category: string): category is SettingsCategory {
  return VALID_CATEGORIES.includes(category as SettingsCategory);
}

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
    logger.info('Attempting serial connection', { baudRate });

    if (!this.isSupported()) {
      const error = new SerialError(
        'WebSerial API not supported in this browser',
        SerialErrorCode.NOT_SUPPORTED
      );
      logger.error('WebSerial not supported');
      this.emit({ type: 'error', error });
      return false;
    }

    try {
      // Request port from user
      logger.debug('Requesting serial port from user');
      this.port = await navigator.serial.requestPort();

      // Open with specified baud rate
      logger.debug('Opening port', { baudRate });
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

      logger.info('Serial connection established');
      this.emit({ type: 'connected' });
      return true;
    } catch (error) {
      // Clean up partial connection state
      logger.error('Connection failed', { error });
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
    logger.info('Disconnecting from serial port');
    this.isReading = false;

    // Release reader
    if (this.reader) {
      try {
        await this.reader.cancel();
      } catch (e) {
        logger.warn('Error canceling reader', { error: e });
      }
      try {
        this.reader.releaseLock();
      } catch (e) {
        logger.warn('Error releasing reader lock', { error: e });
      }
      this.reader = null;
    }

    // Release writer
    if (this.writer) {
      try {
        this.writer.releaseLock();
      } catch (e) {
        logger.warn('Error releasing writer lock', { error: e });
      }
      this.writer = null;
    }

    // Close port
    if (this.port) {
      try {
        await this.port.close();
      } catch (e) {
        logger.warn('Error closing port', { error: e });
      }
      this.port = null;
    }

    // Clear buffers
    this.buffer = '';

    logger.info('Serial port disconnected');
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
      logger.error('Invalid command rejected', { command: command.substring(0, 20) });
      throw new SerialError(
        `Invalid command: "${command.substring(0, 20)}"`,
        SerialErrorCode.COMMAND_INVALID
      );
    }

    try {
      logger.debug('Sending command', { command: sanitized });
      const encoder = new TextEncoder();
      const data = encoder.encode(sanitized + '\n');
      await this.writer.write(data);
    } catch (error) {
      const code = classifyError(error);
      const message = error instanceof Error ? error.message : 'Failed to send command';
      logger.error('Command send failed', { command: sanitized, error: message });
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

  // Get device info (with Zod validation)
  async getDeviceInfo(): Promise<DeviceInfo | null> {
    logger.debug('Requesting device info');
    const result = await this.sendAndReceiveJsonWithError<DeviceInfo>('json info');

    if (result.error || !result.data) {
      logger.error('Failed to get device info', { error: result.error?.message });
      return null;
    }

    // Validate response against schema
    const validation = DeviceInfoSchema.safeParse(result.data);
    if (!validation.success) {
      logger.warn('Device info validation failed', {
        errors: validation.error.issues,
        data: result.data,
      });
      // Return data anyway for graceful degradation
      return result.data;
    }

    logger.debug('Device info received', { device: validation.data.device });
    return validation.data;
  }

  // Get all settings (with Zod validation)
  async getSettings(): Promise<SettingsResponse | null> {
    logger.debug('Requesting settings');
    const result = await this.sendAndReceiveJsonWithError<SettingsResponse>('json settings');

    if (result.error || !result.data) {
      logger.error('Failed to get settings', { error: result.error?.message });
      return null;
    }

    // Validate response against schema
    const validation = SettingsResponseSchema.safeParse(result.data);
    if (!validation.success) {
      logger.warn('Settings validation failed', {
        errors: validation.error.issues,
        settingsCount: result.data?.settings?.length,
      });
      // Return data anyway for graceful degradation
      return result.data;
    }

    logger.debug('Settings received', { count: validation.data.settings.length });
    return validation.data;
  }

  // Get settings for a specific category (with Zod validation)
  async getSettingsByCategory(category: string): Promise<SettingsResponse | null> {
    // Validate category parameter before sending to device
    if (!isValidCategory(category)) {
      logger.error('Invalid category requested', { category, validCategories: VALID_CATEGORIES });
      return null;
    }

    logger.debug('Requesting settings for category', { category });
    const result = await this.sendAndReceiveJsonWithError<SettingsResponse>(
      `json settings ${category}`
    );

    if (result.error || !result.data) {
      logger.error('Failed to get settings for category', {
        category,
        error: result.error?.message,
      });
      return null;
    }

    // Validate response against schema
    const validation = SettingsResponseSchema.safeParse(result.data);
    if (!validation.success) {
      logger.error('Category settings validation failed, returning null', {
        category,
        errors: validation.error.issues,
        settingsCount: result.data?.settings?.length,
      });
      // Return null instead of invalid data (strict validation)
      return null;
    }

    logger.debug('Category settings received', {
      category,
      count: validation.data.settings.length,
    });
    return validation.data;
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
              const parsed = JSON.parse(trimmed);
              const validation = AudioMessageSchema.safeParse(parsed);
              if (validation.success) {
                this.emit({ type: 'audio', audio: validation.data });
              } else {
                // Emit anyway for graceful degradation, but log warning
                logger.debug('Audio message validation warning', {
                  errors: validation.error.issues,
                });
                this.emit({ type: 'audio', audio: parsed as AudioMessage });
              }
              continue;
            } catch {
              // Not valid audio JSON
            }
          }

          // Check if it's a battery streaming message
          if (trimmed.startsWith('{"b":')) {
            try {
              const parsed = JSON.parse(trimmed);
              const validation = BatteryMessageSchema.safeParse(parsed);
              if (validation.success) {
                this.emit({ type: 'battery', battery: validation.data });
              } else {
                logger.debug('Battery message validation warning', {
                  errors: validation.error.issues,
                });
                this.emit({ type: 'battery', battery: parsed as BatteryMessage });
              }
              continue;
            } catch {
              // Not valid battery JSON
            }
          }

          // Check if it's a battery status message
          if (trimmed.startsWith('{"battery":')) {
            try {
              const parsed = JSON.parse(trimmed);
              const validation = BatteryStatusResponseSchema.safeParse(parsed);
              if (validation.success) {
                this.emit({ type: 'batteryStatus', batteryStatus: validation.data.battery });
              } else {
                logger.debug('Battery status validation warning', {
                  errors: validation.error.issues,
                });
                this.emit({ type: 'batteryStatus', batteryStatus: parsed.battery });
              }
              continue;
            } catch {
              // Not valid battery status JSON
            }
          }

          // Check if it's a transient detection message
          if (trimmed.startsWith('{"type":"TRANSIENT"')) {
            try {
              const parsed = JSON.parse(trimmed);
              const validation = TransientMessageSchema.safeParse(parsed);
              if (validation.success) {
                this.emit({ type: 'transient', transient: validation.data });
              } else {
                logger.debug('Transient message validation warning', {
                  errors: validation.error.issues,
                });
                this.emit({ type: 'transient', transient: parsed as TransientMessage });
              }
              continue;
            } catch {
              // Not valid transient JSON
            }
          }

          // Check if it's a rhythm analyzer message
          if (trimmed.startsWith('{"type":"RHYTHM"')) {
            try {
              const parsed = JSON.parse(trimmed);
              const validation = RhythmMessageSchema.safeParse(parsed);
              if (validation.success) {
                this.emit({ type: 'rhythm', rhythm: validation.data });
              } else {
                logger.debug('Rhythm message validation warning', {
                  errors: validation.error.issues,
                });
                this.emit({ type: 'rhythm', rhythm: parsed as RhythmMessage });
              }
              continue;
            } catch {
              // Not valid rhythm JSON
            }
          }

          // Check if it's a status message
          if (trimmed.startsWith('{"type":"STATUS"')) {
            try {
              const parsed = JSON.parse(trimmed);
              const validation = StatusMessageSchema.safeParse(parsed);
              if (validation.success) {
                this.emit({ type: 'status', status: validation.data });
                continue;
              } else {
                logger.debug('Status message validation warning', {
                  errors: validation.error.issues,
                });
              }
              const statusMsg = parsed as StatusMessage;
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
