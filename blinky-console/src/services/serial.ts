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
import {
  Transport,
  TransportError,
  TransportErrorCode,
  TransportEvent,
  WebSerialTransport,
} from './transport';

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

// Map a transport-level error code onto the narrower serial-protocol code
// set that public consumers observe via SerialError.code.
function transportCodeToSerialCode(code: TransportErrorCode): SerialErrorCode {
  switch (code) {
    case TransportErrorCode.NOT_SUPPORTED:
      return SerialErrorCode.NOT_SUPPORTED;
    case TransportErrorCode.NOT_CONNECTED:
      return SerialErrorCode.NOT_CONNECTED;
    case TransportErrorCode.CONNECTION_FAILED:
      return SerialErrorCode.CONNECTION_FAILED;
    case TransportErrorCode.DISCONNECTED:
      return SerialErrorCode.DISCONNECTED;
    case TransportErrorCode.PERMISSION_DENIED:
      return SerialErrorCode.PERMISSION_DENIED;
    case TransportErrorCode.DEVICE_LOST:
      return SerialErrorCode.DEVICE_LOST;
    case TransportErrorCode.PORT_IN_USE:
      return SerialErrorCode.PORT_IN_USE;
    case TransportErrorCode.IO_ERROR:
      return SerialErrorCode.CONNECTION_FAILED;
  }
}

function toSerialError(error: unknown): SerialError {
  if (error instanceof SerialError) return error;
  if (error instanceof TransportError) {
    return new SerialError(error.message, transportCodeToSerialCode(error.code));
  }
  const message = error instanceof Error ? error.message : 'Unknown error';
  return new SerialError(message, SerialErrorCode.CONNECTION_FAILED);
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
  'audiovis',
  'agc',
  'transient',
  'detection',
  'rhythm',
] as const;

type SettingsCategory = (typeof VALID_CATEGORIES)[number];

function isValidCategory(category: string): category is SettingsCategory {
  return VALID_CATEGORIES.includes(category as SettingsCategory);
}

/**
 * SerialService — the protocol layer atop a byte {@link Transport}.
 *
 * Responsibilities (what the firmware and console speak about):
 *  - Command validation and send
 *  - Per-line JSON dispatch: typed stream events (audio / battery / transient
 *    / rhythm / status) plus generic `data` events for everything else
 *  - `sendAndReceiveJson` request/response with timeout
 *
 * Byte-level concerns (WebSerial port lifecycle, line framing, error
 * classification) live in {@link WebSerialTransport}. In later milestones
 * additional transports (Web Bluetooth, server-proxied WebSocket) will
 * satisfy the same {@link Transport} contract and this layer will
 * continue to work unmodified.
 */
class SerialService {
  private transport: Transport;
  private listeners: SerialEventCallback[] = [];

  constructor(transport?: Transport) {
    this.transport = transport ?? new WebSerialTransport();
    this.transport.addEventListener(this.handleTransportEvent);
  }

  isSupported(): boolean {
    return this.transport.isSupported();
  }

  isConnected(): boolean {
    return this.transport.isConnected();
  }

  addEventListener(callback: SerialEventCallback): void {
    this.listeners.push(callback);
  }

  removeEventListener(callback: SerialEventCallback): void {
    this.listeners = this.listeners.filter(l => l !== callback);
  }

  private emit(event: SerialEvent): void {
    this.listeners.forEach(callback => callback(event));
  }

  async connect(baudRate: number = 115200): Promise<boolean> {
    logger.info('Attempting serial connection', { baudRate });

    // If a different baud rate was requested, swap in a fresh transport.
    // Only WebSerialTransport carries a baudRate; other transport types
    // simply ignore this path.
    if (this.transport instanceof WebSerialTransport && this.transport.baudRate !== baudRate) {
      this.transport.removeEventListener(this.handleTransportEvent);
      this.transport = new WebSerialTransport(baudRate);
      this.transport.addEventListener(this.handleTransportEvent);
    }

    try {
      await this.transport.connect();
      // `connected` event already emitted by the transport and relayed.
      return true;
    } catch (error) {
      logger.error('Connection failed', { error });
      this.emit({ type: 'error', error: toSerialError(error) });
      return false;
    }
  }

  async disconnect(): Promise<void> {
    logger.info('Disconnecting from serial port');
    await this.transport.disconnect();
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
    if (!this.isConnected()) {
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
      await this.transport.send(sanitized);
    } catch (error) {
      logger.error('Command send failed', { command: sanitized, error });
      throw toSerialError(error);
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
          // Each 'data' event is already a single line from the transport
          // layer. Check this line directly (no buffering — line boundaries
          // are preserved by the transport).
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

  /** Relay transport events into the public SerialEvent stream. */
  private handleTransportEvent = (event: TransportEvent): void => {
    switch (event.type) {
      case 'connected':
        logger.info('Serial connection established');
        this.emit({ type: 'connected' });
        break;
      case 'disconnected':
        logger.info('Serial port disconnected');
        this.emit({ type: 'disconnected' });
        break;
      case 'error':
        if (event.error) {
          this.emit({ type: 'error', error: toSerialError(event.error) });
        }
        break;
      case 'line':
        if (event.line) this.handleLine(event.line);
        break;
    }
  };

  /**
   * Dispatch a single decoded line from the transport.
   *
   * Known stream message shapes are parsed and emitted as typed events;
   * everything else — command responses, diagnostic strings — goes out as
   * a generic `data` event for `sendAndReceiveJson` and the raw console.
   */
  private handleLine(line: string): void {
    // Audio streaming
    if (line.startsWith('{"a":')) {
      try {
        const parsed = JSON.parse(line);
        const validation = AudioMessageSchema.safeParse(parsed);
        if (validation.success) {
          this.emit({ type: 'audio', audio: validation.data });
        } else {
          logger.debug('Audio message validation warning', { errors: validation.error.issues });
          this.emit({ type: 'audio', audio: parsed as AudioMessage });
        }
        return;
      } catch {
        // Not valid audio JSON
      }
    }

    // Battery streaming
    if (line.startsWith('{"b":')) {
      try {
        const parsed = JSON.parse(line);
        const validation = BatteryMessageSchema.safeParse(parsed);
        if (validation.success) {
          this.emit({ type: 'battery', battery: validation.data });
        } else {
          logger.debug('Battery message validation warning', { errors: validation.error.issues });
          this.emit({ type: 'battery', battery: parsed as BatteryMessage });
        }
        return;
      } catch {
        // Not valid battery JSON
      }
    }

    // Battery status response
    if (line.startsWith('{"battery":')) {
      try {
        const parsed = JSON.parse(line);
        const validation = BatteryStatusResponseSchema.safeParse(parsed);
        if (validation.success) {
          this.emit({ type: 'batteryStatus', batteryStatus: validation.data.battery });
        } else {
          logger.debug('Battery status validation warning', {
            errors: validation.error.issues,
          });
          this.emit({ type: 'batteryStatus', batteryStatus: parsed.battery });
        }
        return;
      } catch {
        // Not valid battery status JSON
      }
    }

    // Transient detection
    if (line.startsWith('{"type":"TRANSIENT"')) {
      try {
        const parsed = JSON.parse(line);
        const validation = TransientMessageSchema.safeParse(parsed);
        if (validation.success) {
          this.emit({ type: 'transient', transient: validation.data });
        } else {
          logger.debug('Transient message validation warning', {
            errors: validation.error.issues,
          });
          this.emit({ type: 'transient', transient: parsed as TransientMessage });
        }
        return;
      } catch {
        // Not valid transient JSON
      }
    }

    // Rhythm analyzer
    if (line.startsWith('{"type":"RHYTHM"')) {
      try {
        const parsed = JSON.parse(line);
        const validation = RhythmMessageSchema.safeParse(parsed);
        if (validation.success) {
          this.emit({ type: 'rhythm', rhythm: validation.data });
        } else {
          logger.debug('Rhythm message validation warning', {
            errors: validation.error.issues,
          });
          this.emit({ type: 'rhythm', rhythm: parsed as RhythmMessage });
        }
        return;
      } catch {
        // Not valid rhythm JSON
      }
    }

    // Status message
    if (line.startsWith('{"type":"STATUS"')) {
      try {
        const parsed = JSON.parse(line);
        const validation = StatusMessageSchema.safeParse(parsed);
        if (validation.success) {
          this.emit({ type: 'status', status: validation.data });
        } else {
          logger.debug('Status message validation warning', {
            errors: validation.error.issues,
          });
          this.emit({ type: 'status', status: parsed as StatusMessage });
        }
        return;
      } catch {
        // Not valid status JSON
      }
    }

    // Regular data — command responses, debug output, raw console lines.
    this.emit({ type: 'data', data: line });
  }

  // Debug snapshot of connection state
  getConnectionState(): { connected: boolean; readable: boolean; writable: boolean } {
    const connected = this.transport.isConnected();
    return {
      connected,
      readable: connected,
      writable: connected,
    };
  }
}

// Singleton instance
export const serialService = new SerialService();
