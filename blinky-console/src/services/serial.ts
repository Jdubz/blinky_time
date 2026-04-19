/**
 * Legacy `serialService` singleton — a backward-compat facade over the
 * new {@link DeviceProtocol} + {@link Transport} layering.
 *
 * Event listeners survive protocol swaps: the service maintains its own
 * listener list and proxies events, so listeners survive protocol swaps.
 * When bindProtocol() is called, a synthetic 'disconnected' event is emitted
 * BEFORE the swap so useSerial resets its state cleanly.
 */

import { logger } from '../lib/logger';
import { WebSerialTransport } from './transport';
import { DeviceProtocol } from './protocol';
import type { SerialEvent, SerialEventCallback } from './protocol';

// Re-export the protocol-layer types so the historical
// `from '../services/serial'` import path keeps resolving.
export { SerialError, SerialErrorCode } from './protocol';
export type {
  BatteryStatusData,
  SerialEvent,
  SerialEventCallback,
  SerialEventType,
} from './protocol';

class SerialService {
  private protocol: DeviceProtocol;
  private listeners: SerialEventCallback[] = [];
  private protocolHandler: (event: SerialEvent) => void;
  /** True when the protocol was bound from a registry Device (not the default). */
  private boundToRegistryDevice = false;

  constructor() {
    this.protocol = new DeviceProtocol(new WebSerialTransport());
    this.protocolHandler = (event: SerialEvent) => {
      for (const cb of this.listeners) {
        try {
          cb(event);
        } catch (e) {
          logger.warn('Serial event listener threw', { error: e });
        }
      }
    };
    this.protocol.addEventListener(this.protocolHandler);
  }

  /**
   * Bind an external DeviceProtocol (e.g., from the DeviceRegistry).
   * Listeners are migrated automatically — callers don't need to re-subscribe.
   *
   * A synthetic 'disconnected' event is emitted BEFORE the swap so useSerial
   * resets its state (device info, settings, streaming) before the new
   * protocol starts emitting.
   */
  async bindProtocol(protocol: DeviceProtocol): Promise<void> {
    if (this.protocol === protocol) return; // no-op

    const wasConnected = this.protocol.isConnected();

    // Emit synthetic disconnect BEFORE removing the handler, so useSerial
    // sees the state transition and resets. This prevents stale device info
    // from showing when navigating between devices.
    if (wasConnected) {
      this.protocolHandler({ type: 'disconnected' });
    }

    // Tear down old protocol
    this.protocol.removeEventListener(this.protocolHandler);
    if (wasConnected) {
      // Fire-and-forget — the synthetic disconnect already notified listeners
      this.protocol.disconnect().catch(() => {});
    }

    // Bind new protocol
    this.protocol = protocol;
    this.protocol.addEventListener(this.protocolHandler);
    this.boundToRegistryDevice = true;
    logger.info('SerialService: protocol rebound');
  }

  /**
   * Unbind the current protocol and reset to a fresh default.
   * Called when DeviceDetail unmounts to avoid keeping a stale binding.
   * Disconnects the current protocol and emits a synthetic disconnected event.
   */
  unbind(): void {
    if (!this.boundToRegistryDevice) return;
    const wasConnected = this.protocol.isConnected();
    if (wasConnected) {
      this.protocolHandler({ type: 'disconnected' });
    }
    this.protocol.removeEventListener(this.protocolHandler);
    if (wasConnected) {
      this.protocol.disconnect().catch(() => {});
    }
    this.protocol = new DeviceProtocol(new WebSerialTransport());
    this.protocol.addEventListener(this.protocolHandler);
    this.boundToRegistryDevice = false;
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

    if (this.protocol.isConnected()) {
      logger.warn('connect() called while already connected — disconnecting first');
      await this.protocol.disconnect();
    }

    // Only swap transport for WebSerial (direct USB). Registry-bound protocols
    // (e.g., ServerWebSocketTransport) manage their own transport — don't touch it.
    if (!this.boundToRegistryDevice) {
      const t = this.protocol.currentTransport;
      if (t instanceof WebSerialTransport && t.baudRate !== baudRate) {
        this.protocol.setTransport(new WebSerialTransport(baudRate));
      }
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

export const serialService = new SerialService();
