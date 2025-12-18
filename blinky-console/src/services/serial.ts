import { DeviceInfo, SettingsResponse, AudioMessage } from '../types';

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

export type SerialEventType = 'connected' | 'disconnected' | 'data' | 'error' | 'audio';

export interface SerialEvent {
  type: SerialEventType;
  data?: string;
  audio?: AudioMessage;
  error?: Error;
}

export type SerialEventCallback = (event: SerialEvent) => void;

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
      this.emit({ type: 'error', error: new Error('WebSerial not supported') });
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
      }
      if (this.port.writable) {
        this.writer = this.port.writable.getWriter();
      }

      this.emit({ type: 'connected' });
      return true;
    } catch (error) {
      this.emit({ type: 'error', error: error as Error });
      return false;
    }
  }

  // Disconnect from serial port
  async disconnect(): Promise<void> {
    this.isReading = false;

    try {
      if (this.reader) {
        await this.reader.cancel();
        this.reader.releaseLock();
        this.reader = null;
      }
      if (this.writer) {
        this.writer.releaseLock();
        this.writer = null;
      }
      if (this.port) {
        await this.port.close();
        this.port = null;
      }
    } catch (error) {
      console.error('Error disconnecting:', error);
    }

    this.emit({ type: 'disconnected' });
  }

  // Check if connected
  isConnected(): boolean {
    return this.port !== null && this.writer !== null;
  }

  // Send a command
  async send(command: string): Promise<void> {
    if (!this.writer) {
      throw new Error('Not connected');
    }

    const encoder = new TextEncoder();
    const data = encoder.encode(command + '\n');
    await this.writer.write(data);
  }

  // Send command and wait for JSON response
  async sendAndReceiveJson<T>(command: string, timeoutMs: number = 2000): Promise<T | null> {
    return new Promise((resolve) => {
      let jsonBuffer = '';
      let resolved = false;

      const timeout = setTimeout(() => {
        if (!resolved) {
          resolved = true;
          this.removeEventListener(handler);
          resolve(null);
        }
      }, timeoutMs);

      const handler = (event: SerialEvent) => {
        if (event.type === 'data' && event.data) {
          jsonBuffer += event.data;

          // Try to find a complete JSON object
          const lines = jsonBuffer.split('\n');
          for (const line of lines) {
            const trimmed = line.trim();
            if (trimmed.startsWith('{') && trimmed.endsWith('}')) {
              try {
                const parsed = JSON.parse(trimmed) as T;
                if (!resolved) {
                  resolved = true;
                  clearTimeout(timeout);
                  this.removeEventListener(handler);
                  resolve(parsed);
                }
                return;
              } catch {
                // Not valid JSON, continue
              }
            }
          }
        }
      };

      this.addEventListener(handler);
      this.send(command).catch(() => {
        if (!resolved) {
          resolved = true;
          clearTimeout(timeout);
          this.removeEventListener(handler);
          resolve(null);
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

          // Regular data
          this.emit({ type: 'data', data: trimmed });
        }
      }
    } catch (error) {
      if (this.isReading) {
        this.emit({ type: 'error', error: error as Error });
      }
    }
  }
}

// Singleton instance
export const serialService = new SerialService();
