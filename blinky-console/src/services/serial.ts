/**
 * Legacy `serialService` singleton — a backward-compat facade over the
 * new {@link DeviceProtocol} + {@link Transport} layering. New code should
 * construct DeviceProtocol instances directly (one per device) via a
 * Source (M7+); this singleton is retained so existing components and
 * the useSerial hook keep working unchanged.
 *
 * Event listeners survive protocol swaps: the service maintains its own
 * listener list and re-subscribes them when bindProtocol is called.
 */

import { logger } from '../lib/logger';
import { WebSerialTransport } from './transport';
import { DeviceProtocol, SerialEventCallback } from './protocol';
import type { SerialEvent } from './protocol';

// Re-export the protocol-layer types so the historical
// `from '../services/serial'` import path keeps resolving.
export { SerialError, SerialErrorCode } from './protocol';
export type {
  BatteryStatusData,
  SerialEvent,
  SerialEventCallback,
  SerialEventType,
} from './protocol';

/**
 * Thin wrapper around a single DeviceProtocol bound to a WebSerialTransport.
 * Keeps the legacy `connect(baudRate)` signature: changing the baud rate
 * swaps the underlying transport (which is only meaningful for WebSerial).
 */
class SerialService {
  private protocol: DeviceProtocol;
  private listeners: SerialEventCallback[] = [];
  /** The handler we attach to the protocol — forwards all events to our listeners. */
  private protocolHandler: (event: SerialEvent) => void;

  constructor() {
    this.protocol = new DeviceProtocol(new WebSerialTransport());
    this.protocolHandler = (event: SerialEvent) => {
      for (const cb of this.listeners) {
        try { cb(event); } catch { /* listener errors are their problem */ }
      }
    };
    this.protocol.addEventListener(this.protocolHandler);
  }

  /**
   * Bind an external DeviceProtocol (e.g., from the DeviceRegistry).
   * Listeners are migrated automatically — callers don't need to re-subscribe.
   *
   * If the current protocol is connected, it is disconnected first.
   */
  async bindProtocol(protocol: DeviceProtocol): Promise<void> {
    if (this.protocol === protocol) return; // no-op

    // Tear down old protocol
    this.protocol.removeEventListener(this.protocolHandler);
    if (this.protocol.isConnected()) {
      await this.protocol.disconnect();
    }

    // Bind new protocol
    this.protocol = protocol;
    this.protocol.addEventListener(this.protocolHandler);
    logger.info('SerialService: protocol rebound');
  }

  /** The currently-bound protocol instance. */
  get currentProtocol(): DeviceProtocol {
    return this.protocol;
  }

  isSupported(): boolean {
    return this.protocol.isSupported();
  }

  isConnected(): boolean {
    return this.protocol.isConnected();
  }

  addEventListener(callback: SerialEventCallback): void {
    this.listeners.push(callback);
  }

  removeEventListener(callback: SerialEventCallback): void {
    this.listeners = this.listeners.filter(cb => cb !== callback);
  }

  async connect(baudRate: number = 115200): Promise<boolean> {
    logger.info('Attempting serial connection', { baudRate });

    // Idempotency: tear down any prior connection before opening a new one.
    // Without this, calling connect() while already connected would leak
    // the old port handle when the WebSerial port picker grabs a new one
    // (and would also trip setTransport's connection guard below).
    if (this.protocol.isConnected()) {
      logger.warn('connect() called while already connected — disconnecting first');
      await this.protocol.disconnect();
    }

    // Swap in a fresh WebSerialTransport if a different baud rate was
    // requested. Listener subscriptions on the protocol are preserved.
    const t = this.protocol.currentTransport;
    if (t instanceof WebSerialTransport && t.baudRate !== baudRate) {
      this.protocol.setTransport(new WebSerialTransport(baudRate));
    }

    return this.protocol.connect();
  }

  async disconnect(): Promise<void> {
    logger.info('Disconnecting from serial port');
    await this.protocol.disconnect();
  }

  async send(command: string): Promise<void> {
    return this.protocol.send(command);
  }

  async sendAndReceiveJson<T>(command: string, timeoutMs?: number) {
    return this.protocol.sendAndReceiveJson<T>(command, timeoutMs);
  }

  async sendAndReceiveJsonWithError<T>(command: string, timeoutMs?: number) {
    return this.protocol.sendAndReceiveJsonWithError<T>(command, timeoutMs);
  }

  // High-level RPC convenience methods — all delegate to the protocol.
  getDeviceInfo() {
    return this.protocol.getDeviceInfo();
  }
  getSettings() {
    return this.protocol.getSettings();
  }
  getSettingsByCategory(category: string) {
    return this.protocol.getSettingsByCategory(category);
  }
  setSetting(name: string, value: number | boolean) {
    return this.protocol.setSetting(name, value);
  }
  setStreamEnabled(enabled: boolean) {
    return this.protocol.setStreamEnabled(enabled);
  }
  saveSettings() {
    return this.protocol.saveSettings();
  }
  loadSettings() {
    return this.protocol.loadSettings();
  }
  resetDefaults() {
    return this.protocol.resetDefaults();
  }
  requestBatteryStatus() {
    return this.protocol.requestBatteryStatus();
  }
  setGenerator(name: Parameters<DeviceProtocol['setGenerator']>[0]) {
    return this.protocol.setGenerator(name);
  }
  setEffect(name: Parameters<DeviceProtocol['setEffect']>[0]) {
    return this.protocol.setEffect(name);
  }

  getConnectionState(): { connected: boolean; readable: boolean; writable: boolean } {
    const connected = this.protocol.isConnected();
    return { connected, readable: connected, writable: connected };
  }
}

// Singleton instance — retained for backward compatibility.
export const serialService = new SerialService();
