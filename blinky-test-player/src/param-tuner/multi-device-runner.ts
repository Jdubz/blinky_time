/**
 * Multi-device runner - manages N DeviceConnection instances with shared audio playback
 *
 * Key insight: audio plays through speakers, so ALL devices hear the same audio
 * from a single playback. We only need ONE audio player subprocess while N devices
 * record concurrently.
 *
 * Uses ffplay for audio playback (no browser/Playwright needed).
 */

import { spawn } from 'child_process';
import { readFileSync } from 'fs';
import { EventEmitter } from 'events';
import type { TestResult, MultiDeviceTestResult, PerDeviceTestResult } from './types.js';
import { DeviceConnection } from './device-connection.js';
import { scoreDetections } from './scoring.js';
import type { GroundTruthData } from './scoring.js';

export interface MusicTestFile {
  audioFile: string;
  groundTruthFile: string;
  id: string;  // Short identifier (e.g., "techno-minimal-01")
}

function computeVariationStats(values: number[]): { mean: number; stddev: number; min: number; max: number; spread: number } {
  const n = values.length;
  if (n === 0) return { mean: 0, stddev: 0, min: 0, max: 0, spread: 0 };
  const mean = values.reduce((a, b) => a + b, 0) / n;
  const variance = values.reduce((sum, v) => sum + (v - mean) ** 2, 0) / n;
  const min = Math.min(...values);
  const max = Math.max(...values);
  return {
    mean: Math.round(mean * 1000) / 1000,
    stddev: Math.round(Math.sqrt(variance) * 1000) / 1000,
    min: Math.round(min * 1000) / 1000,
    max: Math.round(max * 1000) / 1000,
    spread: Math.round((max - min) * 1000) / 1000,
  };
}

/**
 * Load a .beats.json ground truth file and convert to GroundTruthData format.
 */
function loadGroundTruth(gtFile: string, audioStartTime: Date): GroundTruthData {
  const raw = JSON.parse(readFileSync(gtFile, 'utf-8'));
  return {
    pattern: raw.pattern || 'unknown',
    durationMs: raw.durationMs || 0,
    startedAt: audioStartTime.toISOString(),
    hits: (raw.hits || []).map((h: { time: number; type?: string; strength?: number; expectTrigger?: boolean }) => ({
      timeMs: Math.round(h.time * 1000),
      type: h.type || 'low',
      strength: h.strength ?? 1.0,
      expectTrigger: h.expectTrigger,
    })),
  };
}

export class MultiDeviceRunner extends EventEmitter {
  private connections: Map<string, DeviceConnection> = new Map();
  private gain?: number;

  constructor(
    private ports: string[],
    options?: { gain?: number; recordAudio?: boolean }
  ) {
    super();
    for (const port of ports) {
      const conn = new DeviceConnection(port, { recordAudio: options?.recordAudio });
      conn.on('error', (err) => this.emit('error', { port, error: err }));
      this.connections.set(port, conn);
    }
    this.gain = options?.gain;
  }

  get deviceCount(): number {
    return this.connections.size;
  }

  get portList(): string[] {
    return [...this.connections.keys()];
  }

  /**
   * Connect all devices sequentially. On failure, disconnects any already-connected devices.
   */
  async connectAll(): Promise<void> {
    const connected: DeviceConnection[] = [];
    try {
      for (const [port, conn] of this.connections) {
        await conn.connect();
        connected.push(conn);
      }
    } catch (err) {
      // Clean up any devices that connected before the failure
      for (const conn of connected) {
        await conn.disconnect().catch(() => {});
      }
      throw err;
    }
  }

  /**
   * Disconnect all devices gracefully.
   */
  async disconnectAll(): Promise<void> {
    const promises = [...this.connections.values()].map(conn =>
      conn.disconnect().catch(() => {})
    );
    await Promise.all(promises);
  }

  /**
   * Set the same parameter value on all devices.
   */
  async setParameterAll(name: string, value: number): Promise<void> {
    for (const conn of this.connections.values()) {
      await conn.setParameter(name, value);
    }
  }

  /**
   * Set different parameter values per device.
   */
  async setParameterPerDevice(name: string, assignments: Map<string, number>): Promise<void> {
    for (const [port, value] of assignments) {
      const conn = this.connections.get(port);
      if (!conn) throw new Error(`Unknown port: ${port}`);
      await conn.setParameter(name, value);
    }
  }

  /**
   * Reset all devices to defaults.
   */
  async resetDefaultsAll(): Promise<void> {
    for (const conn of this.connections.values()) {
      await conn.resetDefaults();
    }
  }

  /**
   * Run a music file with all devices recording simultaneously from one audio playback.
   * Returns per-device results with variation stats.
   */
  async runMusicTestAllDevices(musicTest: MusicTestFile, durationMs?: number): Promise<MultiDeviceTestResult> {
    // Lock gain on all devices if specified
    if (this.gain !== undefined) {
      for (const conn of this.connections.values()) {
        await conn.sendCommand(`set hwgainlock ${this.gain}`);
      }
    }

    // Start streaming on all devices
    for (const conn of this.connections.values()) {
      await conn.startStream();
    }

    // Start recording on all devices (synchronized timestamp)
    const recordStartTimes = new Map<string, number>();
    for (const [port, conn] of this.connections) {
      recordStartTimes.set(port, conn.startRecording());
    }

    // Play audio file with ffplay
    const audioStartTime = new Date();
    await this.playAudioFile(musicTest.audioFile, durationMs);

    // Stop recording on all devices
    const recordings = new Map<string, ReturnType<DeviceConnection['stopRecording']>>();
    for (const [port, conn] of this.connections) {
      recordings.set(port, conn.stopRecording());
    }

    // Stop streaming on all devices
    for (const conn of this.connections.values()) {
      await conn.stopStream();
    }

    // Unlock gain on all devices
    if (this.gain !== undefined) {
      for (const conn of this.connections.values()) {
        await conn.sendCommand('set hwgainlock 255');
      }
    }

    // Load ground truth
    const groundTruth = loadGroundTruth(musicTest.groundTruthFile, audioStartTime);

    // Score each device independently
    const perDevice: PerDeviceTestResult[] = [];
    for (const [port, recording] of recordings) {
      const startTime = recordStartTimes.get(port)!;
      const rawDuration = recording.stopTime - recording.startTime;

      const testResult = scoreDetections(
        recording.transients,
        startTime,
        groundTruth,
        musicTest.id,
        rawDuration,
      );

      perDevice.push({
        port,
        label: this.getDeviceLabel(port),
        result: testResult,
      });
    }

    // Compute variation stats
    const variation = perDevice.length > 1 ? {
      f1: computeVariationStats(perDevice.map(d => d.result.f1)),
      precision: computeVariationStats(perDevice.map(d => d.result.precision)),
      recall: computeVariationStats(perDevice.map(d => d.result.recall)),
    } : undefined;

    return { pattern: musicTest.id, perDevice, variation };
  }

  /**
   * Run a music file with different parameter values assigned per device.
   * Returns results keyed by the parameter value each device was assigned.
   */
  async runMusicTestWithAssignments(
    musicTest: MusicTestFile,
    paramName: string,
    portToValue: Map<string, number>,
    durationMs?: number,
  ): Promise<Map<number, TestResult>> {
    // Set per-device parameter values
    await this.setParameterPerDevice(paramName, portToValue);

    // Run the test on all devices
    const multiResult = await this.runMusicTestAllDevices(musicTest, durationMs);

    // Key results by parameter value
    const resultByValue = new Map<number, TestResult>();
    for (const deviceResult of multiResult.perDevice) {
      const value = portToValue.get(deviceResult.port);
      if (value !== undefined) {
        resultByValue.set(value, deviceResult.result);
      }
    }

    return resultByValue;
  }

  private getDeviceLabel(port: string): string {
    const idx = this.ports.indexOf(port);
    return `D${idx + 1}`;
  }

  private playAudioFile(audioFile: string, durationMs?: number): Promise<void> {
    return new Promise((resolve, reject) => {
      const args = ['-nodisp', '-autoexit', '-loglevel', 'error', audioFile];
      if (durationMs) {
        args.push('-t', (durationMs / 1000).toString());
      }

      const child = spawn('ffplay', args, {
        stdio: ['ignore', 'ignore', 'pipe'],
      });

      let stderr = '';
      child.stderr.on('data', (data) => {
        stderr += data.toString();
      });

      child.on('close', (code) => {
        if (code === 0) {
          resolve();
        } else {
          reject(new Error(stderr || `ffplay exited with code ${code}`));
        }
      });

      child.on('error', (err) => {
        reject(err);
      });
    });
  }
}
