/**
 * Serial connection manager for blinky device
 */

import { SerialPort } from 'serialport';
import { ReadlineParser } from '@serialport/parser-readline';
import { EventEmitter } from 'events';
import type { DeviceInfo, AudioSample, BatteryStatus, Setting, ConnectionState, MusicModeState, LedTelemetry } from './types.js';

const BAUD_RATE = 115200;
const COMMAND_TIMEOUT_MS = 2000;

export class BlinkySerial extends EventEmitter {
  private port: SerialPort | null = null;
  private parser: ReadlineParser | null = null;
  private portPath: string | null = null;
  private deviceInfo: DeviceInfo | null = null;
  private streaming = false;
  private pendingCommand: {
    resolve: (value: string) => void;
    reject: (error: Error) => void;
    timeout: NodeJS.Timeout;
  } | null = null;
  // Multi-line response accumulation
  private responseBuffer: string[] = [];
  private responseTimeout: NodeJS.Timeout | null = null;
  private static readonly RESPONSE_LINE_TIMEOUT_MS = 100; // Wait for more lines

  /**
   * List available serial ports
   */
  async listPorts(): Promise<string[]> {
    const ports = await SerialPort.list();
    return ports.map(p => p.path);
  }

  /**
   * Connect to a serial port
   */
  async connect(portPath: string): Promise<DeviceInfo> {
    if (this.port) {
      await this.disconnect();
    }

    return new Promise((resolve, reject) => {
      this.port = new SerialPort({
        path: portPath,
        baudRate: BAUD_RATE,
      });

      this.parser = this.port.pipe(new ReadlineParser({ delimiter: '\n' }));
      this.portPath = portPath;

      this.port.on('error', (err) => {
        this.emit('error', err);
        reject(err);
      });

      this.port.on('close', () => {
        this.port = null;
        this.parser = null;
        this.portPath = null;
        this.deviceInfo = null;
        this.streaming = false;
        this.emit('disconnected');
      });

      this.parser.on('data', (line: string) => {
        this.handleLine(line.trim());
      });

      // Wait for port to open, then get device info
      this.port.on('open', async () => {
        try {
          // Small delay for device to be ready
          await new Promise(r => setTimeout(r, 500));

          // Get device info
          const infoJson = await this.sendCommand('json info');
          const deviceInfo: DeviceInfo = JSON.parse(infoJson);
          this.deviceInfo = deviceInfo;

          this.emit('connected', deviceInfo);
          resolve(deviceInfo);
        } catch (err) {
          reject(err);
        }
      });
    });
  }

  /**
   * Disconnect from serial port
   */
  async disconnect(): Promise<void> {
    // Always send stream off before closing, regardless of tracked state.
    // This prevents port lock if streaming was enabled via sendCommand('stream fast')
    // without going through startStream().
    if (this.port && this.port.isOpen) {
      try {
        this.port.write('stream off\n');
        this.streaming = false;
        // Brief pause to let the device process the command before port close
        await new Promise(r => setTimeout(r, 50));
      } catch {
        // Ignore write errors - port may already be closing
      }
    }

    // Clear any pending response state
    if (this.responseTimeout) {
      clearTimeout(this.responseTimeout);
      this.responseTimeout = null;
    }
    this.responseBuffer = [];

    if (this.port && this.port.isOpen) {
      return new Promise((resolve) => {
        this.port!.close(() => {
          this.port = null;
          this.parser = null;
          this.portPath = null;
          this.deviceInfo = null;
          resolve();
        });
      });
    }
  }

  /**
   * Get current connection state
   */
  getState(): ConnectionState {
    return {
      connected: this.port !== null && this.port.isOpen,
      port: this.portPath,
      deviceInfo: this.deviceInfo,
      streaming: this.streaming,
    };
  }

  /**
   * Send a command and wait for response
   */
  async sendCommand(command: string): Promise<string> {
    if (!this.port || !this.port.isOpen) {
      throw new Error('Not connected');
    }

    // Track streaming commands so this.streaming stays in sync with firmware
    const isStreamEnable = /^stream\s+(on|fast|debug|normal)$/i.test(command.trim());
    const isStreamDisable = /^stream\s+off$/i.test(command.trim());

    // If streaming, temporarily stop to send command (unless this command enables streaming)
    const wasStreaming = this.streaming;
    if (wasStreaming && !isStreamEnable) {
      await this.stopStream();
    }

    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.pendingCommand = null;
        reject(new Error(`Command timeout: ${command}`));
      }, COMMAND_TIMEOUT_MS);

      this.pendingCommand = { resolve, reject, timeout };
      this.port!.write(command + '\n');
    }).then(async (result) => {
      // Update streaming state to match what we just told the firmware
      if (isStreamEnable) {
        this.streaming = true;
      } else if (isStreamDisable) {
        this.streaming = false;
      } else if (wasStreaming) {
        // Resume streaming if it was active before a non-stream command
        await this.startStream();
      }
      return result as string;
    });
  }

  /**
   * Start audio streaming
   */
  async startStream(): Promise<void> {
    if (!this.port || !this.port.isOpen) {
      throw new Error('Not connected');
    }

    this.port.write('stream on\n');
    this.streaming = true;
    this.emit('streamStarted');
  }

  /**
   * Stop audio streaming
   */
  async stopStream(): Promise<void> {
    if (!this.port || !this.port.isOpen) {
      return;
    }

    this.port.write('stream off\n');
    this.streaming = false;
    this.emit('streamStopped');
  }

  /**
   * Get all settings as JSON
   */
  async getSettings(): Promise<Setting[]> {
    const json = await this.sendCommand('json settings');
    const parsed = JSON.parse(json);
    return parsed.settings || [];
  }

  /**
   * Set a setting value
   */
  async setSetting(name: string, value: number): Promise<string> {
    return this.sendCommand(`set ${name} ${value}`);
  }

  /**
   * Save settings to flash
   */
  async saveSettings(): Promise<string> {
    return this.sendCommand('save');
  }

  /**
   * Load settings from flash
   */
  async loadSettings(): Promise<string> {
    return this.sendCommand('load');
  }

  /**
   * Reset to defaults
   */
  async resetDefaults(): Promise<string> {
    return this.sendCommand('defaults');
  }

  /**
   * Handle incoming line from serial
   */
  private handleLine(line: string): void {
    // Check for JSON audio data (may include music mode: {"a":{...},"m":{...}})
    if (line.startsWith('{"a":')) {
      try {
        const parsed = JSON.parse(line);
        const audio: AudioSample = parsed.a;
        this.emit('audio', audio);

        // Emit transient events (unified detection using 't' field)
        // The simplified "Drummer's Algorithm" outputs a single transient strength
        if (audio.t > 0) {
          this.emit('transient', { type: 'unified', strength: audio.t });
        }

        // Emit music mode state if present
        if (parsed.m) {
          const music: MusicModeState = parsed.m;
          this.emit('music', music);

          // Emit beat event (phase wrap detection)
          if (music.q === 1) {
            this.emit('beat', { type: 'quarter', bpm: music.bpm, predicted: music.bp === 1 });
          }
        }

        // Emit LED telemetry if present
        if (parsed.led) {
          const led: LedTelemetry = parsed.led;
          this.emit('led', led);
        }
      } catch {
        // Ignore parse errors
      }
      return;
    }

    // Check for JSON battery data
    if (line.startsWith('{"b":')) {
      try {
        const parsed = JSON.parse(line);
        const battery: BatteryStatus = parsed.b;
        this.emit('battery', battery);
      } catch {
        // Ignore parse errors
      }
      return;
    }

    // Check for pending command response
    if (this.pendingCommand) {
      // Clear any existing line timeout
      if (this.responseTimeout) {
        clearTimeout(this.responseTimeout);
        this.responseTimeout = null;
      }

      // Add line to buffer
      this.responseBuffer.push(line);

      // Set a short timeout to wait for more lines
      // If no more lines arrive within timeout, finalize the response
      this.responseTimeout = setTimeout(() => {
        if (this.pendingCommand) {
          clearTimeout(this.pendingCommand.timeout);
          const response = this.responseBuffer.join('\n');
          this.responseBuffer = [];
          this.responseTimeout = null;
          this.pendingCommand.resolve(response);
          this.pendingCommand = null;
        }
      }, BlinkySerial.RESPONSE_LINE_TIMEOUT_MS);

      return;
    }

    // Emit raw line for debugging
    this.emit('line', line);
  }
}
