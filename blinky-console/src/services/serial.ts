import { DeviceInfo, SettingsResponse, AudioMessage, BatteryMessage } from '../types';

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
  | 'batteryDebug';

export interface BatteryDebugData {
  rawCount: number;
  adcBits: number;
  maxCount: number;
  vRef: number;
  vAdc: number;
  dividerRatio: number;
  vBattCalculated: number;
  vBattActual: number;
}

export interface SerialEvent {
  type: SerialEventType;
  data?: string;
  audio?: AudioMessage;
  battery?: BatteryMessage;
  batteryDebug?: BatteryDebugData;
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
  private batteryDebugBuffer: string[] = [];
  private collectingBatteryDebug: boolean = false;

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
    this.batteryDebugBuffer = [];
    this.collectingBatteryDebug = false;

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
      throw new Error('Not connected');
    }

    const sanitized = this.validateCommand(command);
    if (!sanitized) {
      throw new Error('Invalid command');
    }

    const encoder = new TextEncoder();
    const data = encoder.encode(sanitized + '\n');
    await this.writer.write(data);
  }

  // Send command and wait for JSON response
  async sendAndReceiveJson<T>(command: string, timeoutMs: number = 2000): Promise<T | null> {
    return new Promise(resolve => {
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

  // Request battery debug data
  async requestBatteryDebug(): Promise<void> {
    await this.send('battery raw');
  }

  // Parse battery debug output
  private parseBatteryDebugOutput(): void {
    try {
      const debugData: BatteryDebugData = {
        rawCount: 0,
        adcBits: 0,
        maxCount: 0,
        vRef: 0,
        vAdc: 0,
        dividerRatio: 0,
        vBattCalculated: 0,
        vBattActual: 0,
      };

      for (const line of this.batteryDebugBuffer) {
        if (line.startsWith('Raw ADC count:')) {
          debugData.rawCount = parseInt(line.split(':')[1].trim());
        } else if (line.startsWith('ADC bits:')) {
          debugData.adcBits = parseInt(line.split(':')[1].trim());
        } else if (line.startsWith('Max count:')) {
          debugData.maxCount = parseFloat(line.split(':')[1].trim());
        } else if (line.startsWith('V_ref:')) {
          debugData.vRef = parseFloat(line.split(':')[1].replace('V', '').trim());
        } else if (line.startsWith('V_adc (pin):')) {
          debugData.vAdc = parseFloat(line.split(':')[1].replace('V', '').trim());
        } else if (line.startsWith('Divider ratio:')) {
          debugData.dividerRatio = parseFloat(line.split(':')[1].trim());
        } else if (line.startsWith('V_batt (calculated):')) {
          debugData.vBattCalculated = parseFloat(line.split(':')[1].replace('V', '').trim());
        } else if (line.startsWith('V_batt (from getVoltage()):')) {
          debugData.vBattActual = parseFloat(line.split(':')[1].replace('V', '').trim());
        }
      }

      this.emit({ type: 'batteryDebug', batteryDebug: debugData });
    } catch (error) {
      console.error('Failed to parse battery debug output:', error);
    }
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

          // Check for battery debug output
          if (trimmed === '=== Battery Raw ADC Debug ===') {
            this.collectingBatteryDebug = true;
            this.batteryDebugBuffer = [];
            continue;
          }

          if (this.collectingBatteryDebug) {
            if (trimmed === '===========================') {
              // End of battery debug output - parse it
              this.parseBatteryDebugOutput();
              this.collectingBatteryDebug = false;
              this.batteryDebugBuffer = [];
              continue;
            }
            this.batteryDebugBuffer.push(trimmed);
            continue;
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
