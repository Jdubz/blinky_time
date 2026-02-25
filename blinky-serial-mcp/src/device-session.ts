/**
 * Per-device session encapsulating a BlinkySerial connection and all its state.
 * Replaces the global variables pattern from the old index.ts.
 */

import { BlinkySerial } from './serial.js';
import type {
  AudioSample, MusicModeState, LedTelemetry, TransientEvent,
  BeatEvent, ConnectionState, DeviceInfo, TimestampedSample, TimestampedMusicState,
} from './types.js';

export class DeviceSession {
  readonly serial: BlinkySerial;
  readonly port: string;

  // Per-device state (previously global in index.ts)
  lastAudioSample: AudioSample | null = null;
  lastLedSample: LedTelemetry | null = null;
  audioSampleCount = 0;
  lastMusicState: MusicModeState | null = null;

  // Test recording buffers
  transientBuffer: TransientEvent[] = [];
  audioSampleBuffer: TimestampedSample[] = [];
  musicStateBuffer: TimestampedMusicState[] = [];
  beatEventBuffer: BeatEvent[] = [];
  testStartTime: number | null = null;

  constructor(port: string) {
    this.port = port;
    this.serial = new BlinkySerial();
    this.setupListeners();
  }

  private setupListeners(): void {
    this.serial.on('audio', (sample: AudioSample) => {
      this.lastAudioSample = sample;
      this.audioSampleCount++;

      if (this.testStartTime !== null) {
        const timestampMs = Date.now() - this.testStartTime;

        this.audioSampleBuffer.push({
          timestampMs,
          level: sample.l,
          raw: sample.raw,
          transient: sample.t || 0,
        });

        if (sample.t > 0) {
          this.transientBuffer.push({
            timestampMs,
            type: 'unified',
            strength: sample.t,
          });
        }
      }
    });

    this.serial.on('error', (err: Error) => {
      console.error(`Serial error on ${this.port}:`, err.message);
    });

    this.serial.on('music', (state: MusicModeState) => {
      this.lastMusicState = state;

      if (this.testStartTime !== null) {
        const timestampMs = Date.now() - this.testStartTime;
        this.musicStateBuffer.push({
          timestampMs,
          active: state.a === 1,
          bpm: state.bpm,
          phase: state.ph,
          confidence: state.conf,
          oss: state.oss,
          cbss: state.cb,
        });
      }
    });

    this.serial.on('beat', (beat: { type: string; bpm: number; predicted?: boolean }) => {
      if (this.testStartTime !== null) {
        const timestampMs = Date.now() - this.testStartTime;
        this.beatEventBuffer.push({
          timestampMs,
          bpm: beat.bpm,
          type: beat.type as 'quarter',
          predicted: beat.predicted,
        });
      }
    });

    this.serial.on('led', (led: LedTelemetry) => {
      this.lastLedSample = led;
    });
  }

  async connect(): Promise<DeviceInfo> {
    return this.serial.connect(this.port);
  }

  async disconnect(): Promise<void> {
    return this.serial.disconnect();
  }

  getState(): ConnectionState {
    return this.serial.getState();
  }

  /** Reset streaming-related counters (called by stream_start) */
  resetStreamState(): void {
    this.lastAudioSample = null;
    this.lastLedSample = null;
    this.audioSampleCount = 0;
  }

  /** Start test recording — clears all buffers and sets start time */
  startTestRecording(): void {
    this.transientBuffer = [];
    this.audioSampleBuffer = [];
    this.musicStateBuffer = [];
    this.beatEventBuffer = [];
    this.testStartTime = Date.now();
  }

  /** Stop test recording and return captured data */
  stopTestRecording(): {
    duration: number;
    startTime: number;
    transients: TransientEvent[];
    audioSamples: TimestampedSample[];
    musicStates: TimestampedMusicState[];
    beatEvents: BeatEvent[];
  } {
    const duration = this.testStartTime !== null ? Date.now() - this.testStartTime : 0;
    const startTime = this.testStartTime || Date.now();
    const result = {
      duration,
      startTime,
      transients: [...this.transientBuffer],
      audioSamples: [...this.audioSampleBuffer],
      musicStates: [...this.musicStateBuffer],
      beatEvents: [...this.beatEventBuffer],
    };
    this.testStartTime = null;
    this.transientBuffer = [];
    this.audioSampleBuffer = [];
    this.musicStateBuffer = [];
    this.beatEventBuffer = [];
    return result;
  }
}
