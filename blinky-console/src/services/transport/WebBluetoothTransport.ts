import { logger } from '../../lib/logger';
import { NUS_RX_CHAR, NUS_SERVICE, NUS_TX_CHAR, RECORD_SERVICE } from '../record/recordProtocol';
import {
  Transport,
  TransportError,
  TransportErrorCode,
  TransportEvent,
  TransportEventCallback,
} from './types';

// Same guard as WebSerialTransport: firmware output that never emits a
// newline must not grow the buffer without bound.
const MAX_BUFFER_SIZE = 16384;
// Conservative write chunk: fits the minimum ATT MTU (23 - 3), so commands
// go through even before (or without) an MTU upgrade. Commands are short.
const WRITE_CHUNK = 20;

/**
 * Transport over the Nordic UART Service via Web Bluetooth — talks directly
 * to one device from the browser, no blinky-server. Android Chrome (and
 * desktop Chrome/Edge); NOT iOS Safari.
 *
 * The device is chosen with the browser's picker on `connect()` (needs a
 * user gesture), or reused across reconnects via `reconnect()`. The record
 * service is requested as an optional service so `getServer()` callers
 * (RecordSession) can reach it on the same GATT connection.
 */
export class WebBluetoothTransport implements Transport {
  private device: BluetoothDevice | null = null;
  private server: BluetoothRemoteGATTServer | null = null;
  private rx: BluetoothRemoteGATTCharacteristic | null = null;
  private tx: BluetoothRemoteGATTCharacteristic | null = null;
  private listeners: TransportEventCallback[] = [];
  private buffer = '';
  private decoder = new TextDecoder();
  private encoder = new TextEncoder();
  private writeChain: Promise<void> = Promise.resolve();
  private writeChunk = WRITE_CHUNK;
  private writeWithoutResponse = false;

  isSupported(): boolean {
    return typeof navigator !== 'undefined' && 'bluetooth' in navigator;
  }

  isConnected(): boolean {
    return !!this.server?.connected && this.rx !== null;
  }

  getDevice(): BluetoothDevice | null {
    return this.device;
  }

  getServer(): BluetoothRemoteGATTServer | null {
    return this.server;
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

  /** Show the device picker and connect. */
  async connect(): Promise<void> {
    if (!this.isSupported()) {
      throw new TransportError(
        'Web Bluetooth not supported in this browser (use Chrome on Android)',
        TransportErrorCode.NOT_SUPPORTED
      );
    }
    let device: BluetoothDevice;
    try {
      device = await navigator.bluetooth.requestDevice({
        filters: [{ namePrefix: 'Blinky' }],
        optionalServices: [NUS_SERVICE, RECORD_SERVICE],
      });
    } catch (err) {
      const code =
        err instanceof DOMException && err.name === 'NotFoundError'
          ? TransportErrorCode.DISCONNECTED // user cancelled the picker
          : TransportErrorCode.PERMISSION_DENIED;
      throw new TransportError(`Bluetooth device selection failed: ${String(err)}`, code);
    }
    this.device = device;
    device.addEventListener('gattserverdisconnected', this.onGattDisconnected);
    await this.openGatt();
  }

  /** Reconnect to the previously picked device (no picker, no gesture). */
  async reconnect(): Promise<void> {
    if (!this.device) {
      throw new TransportError('No device to reconnect to', TransportErrorCode.NOT_CONNECTED);
    }
    await this.openGatt();
  }

  private async openGatt(): Promise<void> {
    const device = this.device!;
    // Fresh line state: a partial UTF-8 sequence or line from the previous
    // link must not be glued onto the first bytes of this one.
    this.buffer = '';
    this.decoder = new TextDecoder();
    // A new link may negotiate a different MTU: bulk mode must be re-armed.
    this.defaultWrites();
    try {
      logger.info('Connecting GATT', { name: device.name });
      this.server = await device.gatt!.connect();
      const nus = await this.server.getPrimaryService(NUS_SERVICE);
      const rx = await nus.getCharacteristic(NUS_RX_CHAR);
      const tx = await nus.getCharacteristic(NUS_TX_CHAR);
      tx.addEventListener('characteristicvaluechanged', this.onTxNotify);
      this.tx = tx;
      await tx.startNotifications();
      // Only now is the transport usable (isConnected keys off rx).
      this.rx = rx;
    } catch (err) {
      this.teardown();
      // Don't leave a GATT link open behind a failed setup (e.g. no NUS).
      if (device.gatt?.connected) device.gatt.disconnect();
      throw new TransportError(
        `Bluetooth connect failed: ${String(err)}`,
        TransportErrorCode.CONNECTION_FAILED
      );
    }
    this.emit({ type: 'connected' });
  }

  async disconnect(): Promise<void> {
    if (this.device) {
      // Explicit disconnect: drop the listener first so it's not reported
      // as an unexpected loss.
      this.device.removeEventListener('gattserverdisconnected', this.onGattDisconnected);
      if (this.device.gatt?.connected) this.device.gatt.disconnect();
    }
    this.teardown();
    this.device = null;
    this.emit({ type: 'disconnected' });
  }

  private teardown(): void {
    this.tx?.removeEventListener('characteristicvaluechanged', this.onTxNotify);
    this.tx = null;
    this.rx = null;
    this.server = null;
    this.buffer = '';
  }

  private onGattDisconnected = (): void => {
    logger.warn('Bluetooth link lost');
    this.teardown();
    this.emit({
      type: 'error',
      error: new TransportError('Bluetooth link lost', TransportErrorCode.DEVICE_LOST),
    });
    this.emit({ type: 'disconnected' });
  };

  private onTxNotify = (event: Event): void => {
    const value = (event.target as BluetoothRemoteGATTCharacteristic).value;
    if (!value) return;
    this.buffer += this.decoder.decode(value, { stream: true });
    if (this.buffer.length > MAX_BUFFER_SIZE) {
      logger.warn('NUS line buffer overflow — truncating', { size: this.buffer.length });
      this.buffer = this.buffer.slice(-MAX_BUFFER_SIZE / 2);
    }
    let nl: number;
    while ((nl = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, nl).trim();
      this.buffer = this.buffer.slice(nl + 1);
      if (line) this.emit({ type: 'line', line });
    }
  };

  /**
   * Bulk mode for long transfers (signed OTA): split each line into
   * `chunk`-byte write-without-response packets instead of 20-byte acked
   * writes. Size `chunk` from the link's ATT MTU minus 3, which only the
   * device can report (Web Bluetooth doesn't expose it); the OTA protocol
   * CRC-checks every line, so a packet the OS truncates is detected and the
   * caller drops back to defaultWrites().
   */
  bulkWrites(chunk: number): void {
    if (!Number.isInteger(chunk) || chunk < WRITE_CHUNK || chunk > 512) {
      throw new TransportError(`bad BLE write size ${chunk}`, TransportErrorCode.IO_ERROR);
    }
    this.writeChunk = chunk;
    this.writeWithoutResponse = true;
  }

  /** Back to 20-byte acknowledged writes (fits any MTU). */
  defaultWrites(): void {
    this.writeChunk = WRITE_CHUNK;
    this.writeWithoutResponse = false;
  }

  /** Send one command line. Writes are serialized and chunked. */
  send(text: string): Promise<void> {
    if (!this.rx) {
      return Promise.reject(new TransportError('Not connected', TransportErrorCode.NOT_CONNECTED));
    }
    const rx = this.rx;
    const bytes = this.encoder.encode(text + '\n');
    const chunk = this.writeChunk;
    const noResponse = this.writeWithoutResponse;
    const job = this.writeChain.then(async () => {
      for (let i = 0; i < bytes.length; i += chunk) {
        try {
          const part = bytes.slice(i, i + chunk);
          if (noResponse) await rx.writeValueWithoutResponse(part);
          else await rx.writeValueWithResponse(part);
        } catch (err) {
          throw new TransportError(`NUS write failed: ${String(err)}`, TransportErrorCode.IO_ERROR);
        }
      }
    });
    // Keep the chain alive after a failure; the caller still sees the rejection.
    this.writeChain = job.catch(() => undefined);
    return job;
  }
}
