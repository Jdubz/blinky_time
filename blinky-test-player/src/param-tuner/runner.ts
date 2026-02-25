/**
 * Test runner - executes patterns and measures detection performance
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * Legacy detection mode switching has been removed.
 *
 * Delegates serial I/O to DeviceConnection and scoring to scoreDetections().
 */

import { spawn } from 'child_process';
import { EventEmitter } from 'events';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { promises as fs } from 'fs';
import type { TestResult, TunerOptions, DetectorType } from './types.js';
import { DeviceConnection } from './device-connection.js';
import { scoreDetections } from './scoring.js';
import type { GroundTruthData } from './scoring.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Path to test player CLI (use dist/index.js compiled version)
export const TEST_PLAYER_PATH = join(__dirname, '..', '..', 'dist', 'index.js');

export class TestRunner extends EventEmitter {
  private connection: DeviceConnection;

  constructor(private options: TunerOptions) {
    super();
    this.connection = new DeviceConnection(options.port, {
      recordAudio: options.recordAudio,
    });

    // Forward errors from connection
    this.connection.on('error', (err) => this.emit('error', err));
  }

  async connect(): Promise<void> {
    await this.connection.connect();
  }

  async disconnect(): Promise<void> {
    await this.connection.disconnect();
  }

  /**
   * Set a single parameter using the new ensemble command format
   */
  async setParameter(name: string, value: number): Promise<void> {
    await this.connection.setParameter(name, value);
  }

  /**
   * Set multiple parameters
   */
  async setParameters(params: Record<string, number>): Promise<void> {
    await this.connection.setParameters(params);
  }

  /**
   * Set detector enabled state
   */
  async setDetectorEnabled(detector: DetectorType, enabled: boolean): Promise<void> {
    await this.connection.setDetectorEnabled(detector, enabled);
  }

  /**
   * Set detector weight
   */
  async setDetectorWeight(detector: DetectorType, weight: number): Promise<void> {
    await this.connection.setDetectorWeight(detector, weight);
  }

  /**
   * Set detector threshold
   */
  async setDetectorThreshold(detector: DetectorType, threshold: number): Promise<void> {
    await this.connection.setDetectorThreshold(detector, threshold);
  }

  /**
   * Set agreement boost value
   */
  async setAgreementBoost(level: number, boost: number): Promise<void> {
    await this.connection.setAgreementBoost(level, boost);
  }

  /**
   * Reset parameters to defaults for ensemble
   */
  async resetDefaults(): Promise<void> {
    await this.connection.resetDefaults();
  }

  /**
   * Save current settings to device flash memory
   */
  async saveToFlash(): Promise<void> {
    await this.connection.saveToFlash();
  }

  /**
   * Get current parameter value from device
   */
  async getParameter(name: string): Promise<number> {
    return this.connection.getParameter(name);
  }

  /**
   * Run a single test pattern and return results
   */
  async runPattern(patternId: string): Promise<TestResult> {
    // Lock gain if specified
    if (this.options.gain !== undefined) {
      await this.connection.sendCommand(`test lock hwgain ${this.options.gain}`);
    }

    // Start streaming and recording
    await this.connection.startStream();
    const recordStartTime = this.connection.startRecording();

    // Run the test player CLI
    const result = await new Promise<{ success: boolean; groundTruth?: GroundTruthData; error?: string }>((resolve) => {
      const child = spawn('node', [TEST_PLAYER_PATH, 'play', patternId, '--quiet'], {
        stdio: ['ignore', 'pipe', 'pipe'],
      });

      let stdout = '';
      let stderr = '';

      child.stdout.on('data', (data) => {
        stdout += data.toString();
      });

      child.stderr.on('data', (data) => {
        stderr += data.toString();
      });

      child.on('close', (code) => {
        if (code === 0) {
          try {
            const groundTruth = JSON.parse(stdout);
            resolve({ success: true, groundTruth });
          } catch {
            resolve({ success: false, error: 'Failed to parse ground truth: ' + stdout });
          }
        } else {
          resolve({ success: false, error: stderr || `Process exited with code ${code}` });
        }
      });

      child.on('error', (err) => {
        resolve({ success: false, error: err.message });
      });
    });

    // Stop recording
    const recording = this.connection.stopRecording();
    const rawDuration = recording.stopTime - recording.startTime;

    // Save audio recording if debugging was enabled
    if (recording.audioSamples && recording.audioSamples.length > 0) {
      await this.saveAudioRecording(patternId, recording.audioSamples);
    }

    await this.connection.stopStream();

    // Unlock gain
    if (this.options.gain !== undefined) {
      await this.connection.sendCommand('test unlock hwgain');
    }

    if (!result.success || !result.groundTruth) {
      throw new Error(result.error || 'Test failed');
    }

    // Score detections against ground truth
    return scoreDetections(
      recording.transients,
      recordStartTime,
      result.groundTruth,
      patternId,
      rawDuration,
    );
  }

  /**
   * Run multiple patterns and return aggregated results
   */
  async runPatterns(patterns: string[]): Promise<{
    byPattern: Record<string, TestResult>;
    avgF1: number;
    avgPrecision: number;
    avgRecall: number;
  }> {
    const byPattern: Record<string, TestResult> = {};
    let totalF1 = 0;
    let totalPrecision = 0;
    let totalRecall = 0;

    for (const pattern of patterns) {
      const result = await this.runPattern(pattern);
      byPattern[pattern] = result;
      totalF1 += result.f1;
      totalPrecision += result.precision;
      totalRecall += result.recall;
    }

    const n = Object.keys(byPattern).length;
    return {
      byPattern,
      avgF1: n > 0 ? Math.round((totalF1 / n) * 1000) / 1000 : 0,
      avgPrecision: n > 0 ? Math.round((totalPrecision / n) * 1000) / 1000 : 0,
      avgRecall: n > 0 ? Math.round((totalRecall / n) * 1000) / 1000 : 0,
    };
  }

  /**
   * Save audio recording to file (debugging only)
   * Saves raw audio samples as JSON for offline analysis
   */
  private async saveAudioRecording(
    patternId: string,
    samples: Array<{ timestampMs: number; level: number; raw: number; transient: number }>
  ): Promise<void> {
    const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
    const filename = `audio-${patternId}-${timestamp}.json`;
    const outputDir = this.options.outputDir || join(__dirname, '..', '..', 'tuning-results');
    const audioDir = join(outputDir, 'audio-recordings');
    const filepath = join(audioDir, filename);

    // Ensure directory exists
    await fs.mkdir(audioDir, { recursive: true });

    // Prepare data
    const data = {
      version: '1.0',
      pattern: patternId,
      timestamp: new Date().toISOString(),
      sampleCount: samples.length,
      durationMs: samples.length > 0 ? samples[samples.length - 1].timestampMs : 0,
      sampleRate: samples.length > 1
        ? Math.round(samples.length / (samples[samples.length - 1].timestampMs / 1000))
        : 0,
      samples: samples.map(s => ({
        t: s.timestampMs,
        l: Math.round(s.level * 1000) / 1000,  // Round to 3 decimals
        r: s.raw,
        tr: Math.round(s.transient * 1000) / 1000,
      })),
    };

    await fs.writeFile(filepath, JSON.stringify(data, null, 2), 'utf-8');
    console.log(`   Audio recording saved: ${filename} (${samples.length} samples)`);
  }
}
