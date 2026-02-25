/**
 * Device connection - manages serial I/O with a single blinky device
 *
 * Extracted from runner.ts to enable multi-device support.
 * Handles serial port lifecycle, command sending, streaming, and transient buffering.
 */

import { SerialPort } from 'serialport';
import { ReadlineParser } from '@serialport/parser-readline';
import { EventEmitter } from 'events';
import type { DetectorType } from './types.js';
import { PARAMETERS } from './types.js';

const BAUD_RATE = 115200;
const COMMAND_TIMEOUT_MS = 2000;

export interface AudioSample {
  l: number;
  raw: number;
  t: number;
}

export interface TransientEvent {
  timestampMs: number;
  type: string;
  strength: number;
}

export interface RecordingResult {
  transients: TransientEvent[];
  audioSamples: Array<{
    timestampMs: number;
    level: number;
    raw: number;
    transient: number;
  }> | null;
  startTime: number;
  stopTime: number;
}

export class DeviceConnection extends EventEmitter {
  private port: SerialPort | null = null;
  private parser: ReadlineParser | null = null;
  private streaming = false;
  private pendingCommand: {
    resolve: (value: string) => void;
    reject: (error: Error) => void;
    timeout: NodeJS.Timeout;
  } | null = null;

  // Test recording state
  private testStartTime: number | null = null;
  private transientBuffer: TransientEvent[] = [];

  // Audio sample recording (debugging only)
  private audioSampleBuffer: Array<{
    timestampMs: number;
    level: number;
    raw: number;
    transient: number;
  }> | null = null;

  private recordAudio: boolean;

  constructor(
    public readonly portPath: string,
    options?: { recordAudio?: boolean }
  ) {
    super();
    this.recordAudio = options?.recordAudio ?? false;

    if (this.recordAudio) {
      this.audioSampleBuffer = [];
    }
  }

  async connect(): Promise<void> {
    if (this.port) {
      return;
    }

    return new Promise((resolve, reject) => {
      this.port = new SerialPort({
        path: this.portPath,
        baudRate: BAUD_RATE,
      });

      this.parser = this.port.pipe(new ReadlineParser({ delimiter: '\n' }));

      this.port.on('error', (err) => {
        this.emit('error', err);
        reject(err);
      });

      this.port.on('close', () => {
        this.port = null;
        this.parser = null;
        this.streaming = false;
      });

      this.parser.on('data', (line: string) => {
        this.handleLine(line.trim());
      });

      this.port.on('open', async () => {
        // Small delay for device to be ready
        await new Promise(r => setTimeout(r, 500));
        resolve();
      });
    });
  }

  async disconnect(): Promise<void> {
    if (this.streaming) {
      await this.stopStream();
    }

    if (this.port && this.port.isOpen) {
      return new Promise((resolve) => {
        this.port!.close(() => {
          this.port = null;
          this.parser = null;
          resolve();
        });
      });
    }
  }

  async sendCommand(command: string): Promise<string> {
    if (!this.port || !this.port.isOpen) {
      throw new Error(`Not connected to ${this.portPath}`);
    }

    const wasStreaming = this.streaming;
    if (wasStreaming) {
      await this.stopStream();
    }

    return new Promise<string>((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.pendingCommand = null;
        reject(new Error(`Command timeout: ${command}`));
      }, COMMAND_TIMEOUT_MS);

      this.pendingCommand = { resolve, reject, timeout };
      this.port!.write(command + '\n');
    }).then(async (result) => {
      if (wasStreaming) {
        await this.startStream();
      }
      return result;
    });
  }

  async startStream(): Promise<void> {
    if (!this.port || !this.port.isOpen) {
      throw new Error(`Not connected to ${this.portPath}`);
    }

    this.port.write('stream fast\n');
    this.streaming = true;
  }

  async stopStream(): Promise<void> {
    if (!this.port || !this.port.isOpen) {
      return;
    }

    this.port.write('stream off\n');
    this.streaming = false;
  }

  /**
   * Start recording transients and audio samples.
   * Returns the recording start timestamp.
   */
  startRecording(): number {
    this.transientBuffer = [];
    if (this.recordAudio) {
      this.audioSampleBuffer = [];
    }
    this.testStartTime = Date.now();
    return this.testStartTime;
  }

  /**
   * Stop recording and return captured data.
   */
  stopRecording(): RecordingResult {
    const stopTime = Date.now();
    const result: RecordingResult = {
      transients: [...this.transientBuffer],
      audioSamples: this.audioSampleBuffer ? [...this.audioSampleBuffer] : null,
      startTime: this.testStartTime || stopTime,
      stopTime,
    };

    this.testStartTime = null;
    this.transientBuffer = [];
    if (this.audioSampleBuffer) {
      this.audioSampleBuffer = [];
    }

    return result;
  }

  get isRecording(): boolean {
    return this.testStartTime !== null;
  }

  get isConnected(): boolean {
    return this.port !== null && this.port.isOpen;
  }

  get isStreaming(): boolean {
    return this.streaming;
  }

  // --- Parameter helpers ---

  async setParameter(name: string, value: number): Promise<void> {
    const param = PARAMETERS[name];
    if (param && param.command) {
      await this.sendCommand(`set ${param.command} ${value}`);
    } else {
      await this.sendCommand(`set ${name} ${value}`);
    }
  }

  async setParameters(params: Record<string, number>): Promise<void> {
    for (const [name, value] of Object.entries(params)) {
      await this.setParameter(name, value);
    }
  }

  async setDetectorEnabled(detector: DetectorType, enabled: boolean): Promise<void> {
    await this.sendCommand(`set detector_enable ${detector} ${enabled ? 1 : 0}`);
  }

  async setDetectorWeight(detector: DetectorType, weight: number): Promise<void> {
    await this.sendCommand(`set detector_weight ${detector} ${weight}`);
  }

  async setDetectorThreshold(detector: DetectorType, threshold: number): Promise<void> {
    await this.sendCommand(`set detector_thresh ${detector} ${threshold}`);
  }

  async setAgreementBoost(level: number, boost: number): Promise<void> {
    if (level < 0 || level > 7) {
      throw new Error('Agreement level must be 0-7');
    }
    await this.sendCommand(`set agree_${level} ${boost}`);
  }

  async resetDefaults(): Promise<void> {
    const ensembleParams = Object.values(PARAMETERS).filter(p => p.mode === 'ensemble');
    for (const param of ensembleParams) {
      await this.setParameter(param.name, param.default);
    }
  }

  async saveToFlash(): Promise<void> {
    await this.sendCommand('save');
    await new Promise(r => setTimeout(r, 500));
  }

  async getParameter(name: string): Promise<number> {
    const response = await this.sendCommand(`show ${name}`);
    const match = response.match(/:\s*([\d.]+)/);
    if (match) {
      return parseFloat(match[1]);
    }
    throw new Error(`Failed to parse parameter value: ${response}`);
  }

  // --- Private ---

  private handleLine(line: string): void {
    // Check for JSON audio data
    if (line.startsWith('{"a":')) {
      try {
        const parsed = JSON.parse(line);
        const audio: AudioSample = parsed.a;

        // If in test mode, record transients and audio samples
        if (this.testStartTime !== null) {
          const timestampMs = Date.now() - this.testStartTime;

          if (this.audioSampleBuffer) {
            this.audioSampleBuffer.push({
              timestampMs,
              level: audio.l,
              raw: audio.raw,
              transient: audio.t || 0,
            });
          }

          if (audio.t > 0) {
            this.transientBuffer.push({
              timestampMs,
              type: 'unified',
              strength: audio.t,
            });
          }
        }
      } catch {
        // Ignore parse errors
      }
      return;
    }

    // Check for pending command response
    if (this.pendingCommand) {
      clearTimeout(this.pendingCommand.timeout);
      this.pendingCommand.resolve(line);
      this.pendingCommand = null;
    }
  }
}
