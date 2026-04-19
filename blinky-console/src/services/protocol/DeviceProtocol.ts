import {
  AudioMessage,
  AudioMessageSchema,
  BatteryMessage,
  BatteryMessageSchema,
  BatteryStatusResponseSchema,
  DeviceInfo,
  DeviceInfoSchema,
  EffectType,
  GeneratorType,
  RhythmMessage,
  RhythmMessageSchema,
  SettingsResponse,
  SettingsResponseSchema,
  StatusMessage,
  StatusMessageSchema,
  TransientMessage,
  TransientMessageSchema,
} from '../../types';
import { logger } from '../../lib/logger';
import { Transport, TransportError, TransportErrorCode, TransportEvent } from '../transport';
import { SerialError, SerialErrorCode, SerialEvent, SerialEventCallback } from './types';

// Limits applied to commands sent by this protocol layer.
const MAX_COMMAND_LENGTH = 128;
const ALLOWED_COMMAND_PATTERN = /^[a-zA-Z0-9_\-.\s]+$/;

// Categories the firmware's SettingsRegistry recognises. Used to validate
// `getSettingsByCategory` parameters before they hit the wire.
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

// Translate transport-level errors onto the protocol error code set so
// public consumers see a single error vocabulary regardless of underlying
// transport.
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

/**
 * DeviceProtocol — the firmware-aware layer that sits on top of any
 * {@link Transport}. Owns command sanitization, request/response with
 * timeout, JSON line dispatch into typed stream events, and the high-level
 * RPC helpers (getDeviceInfo, getSettings, ...).
 *
 * One DeviceProtocol per physical device. The transport can be replaced
 * via {@link setTransport} when the protocol is disconnected — useful for
 * the singleton legacy facade that supports a configurable WebSerial baud
 * rate, and for future per-device transport-switching from the UI.
 */
export class DeviceProtocol {
  private transport: Transport;
  private listeners: SerialEventCallback[] = [];

  constructor(transport: Transport) {
    this.transport = transport;
    this.transport.addEventListener(this.handleTransportEvent);
  }

  /** Read-only view of the current transport. */
  get currentTransport(): Transport {
    return this.transport;
  }

  /**
   * Swap the underlying transport. The protocol must be disconnected
   * first — replacing a live transport would leak its connection handle.
   * Throws SerialError(NOT_CONNECTED-shaped) if currently connected.
   */
  setTransport(transport: Transport): void {
    if (this.transport.isConnected()) {
      throw new SerialError(
        'Cannot replace transport while connected — disconnect first',
        SerialErrorCode.PORT_IN_USE
      );
    }
    this.transport.removeEventListener(this.handleTransportEvent);
    this.transport = transport;
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

  async connect(): Promise<boolean> {
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
    await this.transport.disconnect();
  }

  // Validate command before sending — strip control chars, cap length.
  private validateCommand(command: string): string {
    let sanitized = command.trim().substring(0, MAX_COMMAND_LENGTH);
    if (!ALLOWED_COMMAND_PATTERN.test(sanitized)) {
      sanitized = sanitized.replace(/[^\w\s.-]/g, '');
    }
    return sanitized;
  }

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

  private sendJsonResult<T>(
    data: T | null,
    error?: SerialError
  ): { data: T | null; error?: SerialError } {
    return { data, error };
  }

  async sendAndReceiveJson<T>(command: string, timeoutMs: number = 2000): Promise<T | null> {
    const result = await this.sendAndReceiveJsonWithError<T>(command, timeoutMs);
    return result.data;
  }

  async sendAndReceiveJsonWithError<T>(
    command: string,
    timeoutMs: number = 2000
  ): Promise<{ data: T | null; error?: SerialError }> {
    return new Promise(resolve => {
      let resolved = false;
      let parseAttempts = 0;
      const maxParseAttempts = 10; // Bound how many garbage lines we'll try parsing

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
          // Each `data` event is already a single line. Check it directly —
          // line boundaries are guaranteed by the transport.
          const trimmed = event.data.trim();

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
                // Not valid JSON — wait for the next event.
              }
            }
          }
        }

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

  // === High-level RPCs ======================================================

  async getDeviceInfo(): Promise<DeviceInfo | null> {
    logger.debug('Requesting device info');
    const result = await this.sendAndReceiveJsonWithError<DeviceInfo>('json info');
    if (result.error || !result.data) {
      logger.error('Failed to get device info', { error: result.error?.message });
      return null;
    }

    const validation = DeviceInfoSchema.safeParse(result.data);
    if (!validation.success) {
      logger.warn('Device info validation failed', {
        errors: validation.error.issues,
        data: result.data,
      });
      // Graceful degradation — return the raw data.
      return result.data;
    }

    logger.debug('Device info received', { device: validation.data.device });
    return validation.data;
  }

  async getSettings(): Promise<SettingsResponse | null> {
    logger.debug('Requesting settings');
    const result = await this.sendAndReceiveJsonWithError<SettingsResponse>('json settings');
    if (result.error || !result.data) {
      logger.error('Failed to get settings', { error: result.error?.message });
      return null;
    }

    const validation = SettingsResponseSchema.safeParse(result.data);
    if (!validation.success) {
      logger.warn('Settings validation failed', {
        errors: validation.error.issues,
        settingsCount: result.data?.settings?.length,
      });
      // Graceful degradation.
      return result.data;
    }

    logger.debug('Settings received', { count: validation.data.settings.length });
    return validation.data;
  }

  async getSettingsByCategory(category: string): Promise<SettingsResponse | null> {
    if (!isValidCategory(category)) {
      logger.error('Invalid category requested', {
        category,
        validCategories: VALID_CATEGORIES,
      });
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

    const validation = SettingsResponseSchema.safeParse(result.data);
    if (!validation.success) {
      logger.error('Category settings validation failed, returning null', {
        category,
        errors: validation.error.issues,
        settingsCount: result.data?.settings?.length,
      });
      // Strict for category-scoped requests — caller asked for a specific shape.
      return null;
    }

    logger.debug('Category settings received', {
      category,
      count: validation.data.settings.length,
    });
    return validation.data;
  }

  async setSetting(name: string, value: number | boolean): Promise<void> {
    await this.send(`set ${name} ${value}`);
  }

  async setStreamEnabled(enabled: boolean): Promise<void> {
    await this.send(enabled ? 'stream on' : 'stream off');
  }

  async saveSettings(): Promise<void> {
    await this.send('save');
  }

  async loadSettings(): Promise<void> {
    await this.send('load');
  }

  async resetDefaults(): Promise<void> {
    await this.send('defaults');
  }

  async requestBatteryStatus(): Promise<void> {
    await this.send('battery');
  }

  async setGenerator(name: GeneratorType): Promise<void> {
    await this.send(`gen ${name}`);
  }

  async setEffect(name: EffectType): Promise<void> {
    await this.send(`effect ${name}`);
  }

  // === Internal: transport event handling =================================

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
   * Dispatch a single decoded line into typed stream events when it matches
   * a known shape; otherwise fall through as a generic `data` event so
   * `sendAndReceiveJson` and the raw console can pick it up.
   */
  private handleLine(line: string): void {
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
        /* not JSON — fall through */
      }
    }

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
        /* not JSON — fall through */
      }
    }

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
        /* not JSON — fall through */
      }
    }

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
        /* not JSON — fall through */
      }
    }

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
        /* not JSON — fall through */
      }
    }

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
        /* not JSON — fall through */
      }
    }

    // Anything else — command responses, debug strings, raw console output.
    this.emit({ type: 'data', data: line });
  }
}
