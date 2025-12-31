/**
 * Music Mode Runner - Tests BPM tracking, phase stability, and rhythm detection
 *
 * Unlike the transient TestRunner which measures F1/precision/recall,
 * this runner focuses on:
 * - BPM accuracy: How close detected BPM is to expected
 * - Phase stability: How consistent the phase tracking is
 * - Lock time: How quickly the system locks onto the tempo
 * - Confidence tracking: How confidence evolves during playback
 */

import { SerialPort } from 'serialport';
import { ReadlineParser } from '@serialport/parser-readline';
import { EventEmitter } from 'events';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import type { TunerOptions } from './types.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Path to test player CLI
const TEST_PLAYER_PATH = join(__dirname, '..', '..', 'dist', 'index.js');

const BAUD_RATE = 115200;
const COMMAND_TIMEOUT_MS = 2000;

// =============================================================================
// MUSIC MODE RESULT TYPES
// =============================================================================

/**
 * Single BPM sample during test
 */
export interface BpmSample {
  timestampMs: number;
  bpm: number;
  phase: number;
  confidence: number;
  rhythmStrength: number;
  musicActive: boolean;
}

/**
 * Aggregated music mode test result
 */
export interface MusicModeResult {
  pattern: string;
  durationMs: number;
  expectedBpm: number | null;

  // BPM tracking
  bpm: {
    /** Average detected BPM */
    avg: number;
    /** BPM standard deviation */
    stdDev: number;
    /** Minimum detected BPM */
    min: number;
    /** Maximum detected BPM */
    max: number;
    /** BPM error vs expected (null if no expected BPM) */
    error: number | null;
    /** Percentage of time within 3% of expected BPM */
    accuracyPct: number | null;
  };

  // Phase tracking
  phase: {
    /** Phase standard deviation (lower = more stable) */
    stability: number;
    /** Number of phase resets detected */
    resets: number;
  };

  // Lock behavior
  lock: {
    /** Time in ms to first stable BPM lock (null if never locked) */
    timeToLockMs: number | null;
    /** Percentage of time in locked state */
    lockedPct: number;
    /** Average confidence when locked */
    avgConfidence: number;
  };

  // Music mode activation
  activation: {
    /** Time in ms until music mode activated */
    timeToActivateMs: number | null;
    /** Percentage of time with music mode active */
    activePct: number;
    /** Number of activations/deactivations */
    toggleCount: number;
  };

  // Raw samples for detailed analysis
  samples: BpmSample[];
}

/**
 * Aggregated result for multiple patterns
 */
export interface MusicModeSweepResult {
  paramValue: number;
  avgBpmError: number | null;
  avgPhaseStability: number;
  avgLockTime: number | null;
  avgActivePct: number;
  byPattern: Record<string, MusicModeResult>;
}

// =============================================================================
// MUSIC MODE RUNNER
// =============================================================================

export class MusicModeRunner extends EventEmitter {
  private port: SerialPort | null = null;
  private parser: ReadlineParser | null = null;
  private portPath: string;
  private streaming = false;
  private pendingCommand: {
    resolve: (value: string) => void;
    reject: (error: Error) => void;
    timeout: NodeJS.Timeout;
  } | null = null;

  // Test recording state
  private testStartTime: number | null = null;
  private bpmSampleBuffer: BpmSample[] = [];
  private lastMusicActive = false;
  private toggleCount = 0;

  constructor(private options: TunerOptions) {
    super();
    this.portPath = options.port;
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

  private async sendCommand(command: string): Promise<string> {
    if (!this.port || !this.port.isOpen) {
      throw new Error('Not connected');
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

  private async startStream(): Promise<void> {
    if (!this.port || !this.port.isOpen) {
      throw new Error('Not connected');
    }

    this.port.write('stream fast\n');
    this.streaming = true;
  }

  private async stopStream(): Promise<void> {
    if (!this.port || !this.port.isOpen) {
      return;
    }

    this.port.write('stream off\n');
    this.streaming = false;
  }

  private handleLine(line: string): void {
    // Check for JSON audio data with music mode info
    if (line.startsWith('{')) {
      try {
        const parsed = JSON.parse(line);

        // Check for music mode data in the stream
        // The firmware sends: {"a":{"l":level,"raw":raw,"t":transient},"m":{"a":active,"bpm":bpm,"ph":phase,"str":strength,"conf":confidence}}
        if (parsed.m && this.testStartTime !== null) {
          const timestampMs = Date.now() - this.testStartTime;
          const musicData = parsed.m;

          const musicActive = Boolean(musicData.a);

          // Track toggles
          if (musicActive !== this.lastMusicActive) {
            this.toggleCount++;
            this.lastMusicActive = musicActive;
          }

          this.bpmSampleBuffer.push({
            timestampMs,
            bpm: musicData.bpm ?? 0,
            phase: musicData.ph ?? 0,
            confidence: musicData.conf ?? musicData.str ?? 0,
            rhythmStrength: musicData.str ?? 0,
            musicActive,
          });
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

  /**
   * Set a single parameter
   */
  async setParameter(name: string, value: number): Promise<void> {
    await this.sendCommand(`set ${name} ${value}`);
  }

  /**
   * Set multiple parameters
   */
  async setParameters(params: Record<string, number>): Promise<void> {
    for (const [name, value] of Object.entries(params)) {
      await this.setParameter(name, value);
    }
  }

  /**
   * Run a music mode test pattern and measure BPM/phase metrics
   */
  async runPattern(patternId: string, expectedBpm: number | null): Promise<MusicModeResult> {
    // Lock gain if specified
    if (this.options.gain !== undefined) {
      await this.sendCommand(`set hwgainlock ${this.options.gain}`);
    }

    // Clear buffers and start streaming
    this.bpmSampleBuffer = [];
    this.toggleCount = 0;
    this.lastMusicActive = false;
    await this.startStream();

    // Run the test player CLI
    const result = await new Promise<{ success: boolean; groundTruth?: unknown; error?: string }>((resolve) => {
      const child = spawn('node', [TEST_PLAYER_PATH, 'play', patternId, '--quiet'], {
        stdio: ['ignore', 'pipe', 'pipe'],
      });

      let stdout = '';
      let stderr = '';

      // Start recording when the process starts
      this.testStartTime = Date.now();

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
    const recordStopTime = Date.now();
    const rawDuration = recordStopTime - (this.testStartTime || recordStopTime);
    const samples = [...this.bpmSampleBuffer];
    this.testStartTime = null;
    this.bpmSampleBuffer = [];

    await this.stopStream();

    // Unlock gain
    if (this.options.gain !== undefined) {
      await this.sendCommand('set hwgainlock 255');
    }

    if (!result.success) {
      throw new Error(result.error || 'Test failed');
    }

    const groundTruth = result.groundTruth as {
      pattern: string;
      durationMs: number;
      bpm?: number;
    };

    // Use pattern's BPM if available and no explicit expected BPM
    const actualExpectedBpm = expectedBpm ?? groundTruth.bpm ?? null;

    // Analyze the samples
    return this.analyzeResults(patternId, rawDuration, actualExpectedBpm, samples);
  }

  /**
   * Analyze collected BPM samples and compute metrics
   */
  private analyzeResults(
    pattern: string,
    durationMs: number,
    expectedBpm: number | null,
    samples: BpmSample[]
  ): MusicModeResult {
    if (samples.length === 0) {
      return {
        pattern,
        durationMs,
        expectedBpm,
        bpm: { avg: 0, stdDev: 0, min: 0, max: 0, error: null, accuracyPct: null },
        phase: { stability: 0, resets: 0 },
        lock: { timeToLockMs: null, lockedPct: 0, avgConfidence: 0 },
        activation: { timeToActivateMs: null, activePct: 0, toggleCount: this.toggleCount },
        samples,
      };
    }

    // Filter to samples where music mode is active
    const activeSamples = samples.filter(s => s.musicActive && s.bpm > 0);

    // BPM analysis
    const bpmValues = activeSamples.map(s => s.bpm);
    const avgBpm = bpmValues.length > 0
      ? bpmValues.reduce((a, b) => a + b, 0) / bpmValues.length
      : 0;
    const bpmVariance = bpmValues.length > 0
      ? bpmValues.reduce((sum, v) => sum + Math.pow(v - avgBpm, 2), 0) / bpmValues.length
      : 0;
    const bpmStdDev = Math.sqrt(bpmVariance);
    const bpmMin = bpmValues.length > 0 ? Math.min(...bpmValues) : 0;
    const bpmMax = bpmValues.length > 0 ? Math.max(...bpmValues) : 0;

    let bpmError: number | null = null;
    let accuracyPct: number | null = null;
    if (expectedBpm !== null && bpmValues.length > 0) {
      bpmError = Math.abs(avgBpm - expectedBpm);
      // Count samples within 3% of expected BPM
      const tolerance = expectedBpm * 0.03;
      const accurateCount = bpmValues.filter(b => Math.abs(b - expectedBpm) <= tolerance).length;
      accuracyPct = Math.round((accurateCount / bpmValues.length) * 100);
    }

    // Phase stability analysis
    const phaseValues = activeSamples.map(s => s.phase);
    let phaseStability = 0;
    let phaseResets = 0;
    if (phaseValues.length > 1) {
      // Calculate phase differences (accounting for wrap-around at 0/1)
      const phaseDiffs: number[] = [];
      for (let i = 1; i < phaseValues.length; i++) {
        let diff = phaseValues[i] - phaseValues[i - 1];
        // Normalize phase difference to handle wrap-around at 0/1 boundary
        // A jump from 0.9 to 0.1 should be +0.2, not -0.8
        if (diff > 0.5) {
          diff -= 1;
        } else if (diff < -0.5) {
          diff += 1;
        }
        // Detect phase reset (large discontinuity after normalization)
        // A normalized diff > 0.3 indicates a significant phase jump
        if (Math.abs(diff) > 0.3) {
          phaseResets++;
        }
        phaseDiffs.push(Math.abs(diff));
      }
      // Stability = inverse of average phase change
      const avgPhaseDiff = phaseDiffs.reduce((a, b) => a + b, 0) / phaseDiffs.length;
      phaseStability = avgPhaseDiff;
    }

    // Lock time analysis
    // "Locked" = confidence > 0.6 and BPM within 5% of median BPM
    const medianBpm = this.median(bpmValues);
    const LOCK_CONF_THRESH = 0.6;
    const LOCK_BPM_TOLERANCE = medianBpm * 0.05;

    let timeToLockMs: number | null = null;
    let lockedCount = 0;
    let totalConfidence = 0;

    for (const sample of activeSamples) {
      const isLocked = sample.confidence >= LOCK_CONF_THRESH &&
        Math.abs(sample.bpm - medianBpm) <= LOCK_BPM_TOLERANCE;

      if (isLocked) {
        lockedCount++;
        totalConfidence += sample.confidence;
        if (timeToLockMs === null) {
          timeToLockMs = sample.timestampMs;
        }
      }
    }

    const lockedPct = activeSamples.length > 0
      ? Math.round((lockedCount / activeSamples.length) * 100)
      : 0;
    const avgConfidence = lockedCount > 0
      ? Math.round((totalConfidence / lockedCount) * 100) / 100
      : 0;

    // Activation analysis
    let timeToActivateMs: number | null = null;
    let activeCount = 0;
    for (const sample of samples) {
      if (sample.musicActive) {
        activeCount++;
        if (timeToActivateMs === null) {
          timeToActivateMs = sample.timestampMs;
        }
      }
    }
    const activePct = samples.length > 0
      ? Math.round((activeCount / samples.length) * 100)
      : 0;

    return {
      pattern,
      durationMs,
      expectedBpm,
      bpm: {
        avg: Math.round(avgBpm * 10) / 10,
        stdDev: Math.round(bpmStdDev * 100) / 100,
        min: Math.round(bpmMin * 10) / 10,
        max: Math.round(bpmMax * 10) / 10,
        error: bpmError !== null ? Math.round(bpmError * 10) / 10 : null,
        accuracyPct,
      },
      phase: {
        stability: Math.round(phaseStability * 1000) / 1000,
        resets: phaseResets,
      },
      lock: {
        timeToLockMs,
        lockedPct,
        avgConfidence,
      },
      activation: {
        timeToActivateMs,
        activePct,
        toggleCount: this.toggleCount,
      },
      samples,
    };
  }

  /**
   * Calculate median of an array
   */
  private median(values: number[]): number {
    if (values.length === 0) return 0;
    const sorted = [...values].sort((a, b) => a - b);
    const mid = Math.floor(sorted.length / 2);
    return sorted.length % 2 === 0
      ? (sorted[mid - 1] + sorted[mid]) / 2
      : sorted[mid];
  }

  /**
   * Run multiple patterns and aggregate results
   */
  async runPatterns(
    patterns: Array<{ id: string; expectedBpm: number | null }>
  ): Promise<{
    byPattern: Record<string, MusicModeResult>;
    avgBpmError: number | null;
    avgPhaseStability: number;
    avgLockTime: number | null;
    avgActivePct: number;
  }> {
    const byPattern: Record<string, MusicModeResult> = {};
    let totalBpmError = 0;
    let bpmErrorCount = 0;
    let totalPhaseStability = 0;
    let totalLockTime = 0;
    let lockTimeCount = 0;
    let totalActivePct = 0;

    for (const { id, expectedBpm } of patterns) {
      const result = await this.runPattern(id, expectedBpm);
      byPattern[id] = result;

      if (result.bpm.error !== null) {
        totalBpmError += result.bpm.error;
        bpmErrorCount++;
      }
      totalPhaseStability += result.phase.stability;
      if (result.lock.timeToLockMs !== null) {
        totalLockTime += result.lock.timeToLockMs;
        lockTimeCount++;
      }
      totalActivePct += result.activation.activePct;
    }

    const n = Object.keys(byPattern).length;
    return {
      byPattern,
      avgBpmError: bpmErrorCount > 0 ? Math.round((totalBpmError / bpmErrorCount) * 10) / 10 : null,
      avgPhaseStability: n > 0 ? Math.round((totalPhaseStability / n) * 1000) / 1000 : 0,
      avgLockTime: lockTimeCount > 0 ? Math.round(totalLockTime / lockTimeCount) : null,
      avgActivePct: n > 0 ? Math.round(totalActivePct / n) : 0,
    };
  }
}

// =============================================================================
// MUSIC MODE SWEEP UTILITIES
// =============================================================================

/**
 * Run a parameter sweep specifically for music mode metrics
 */
export async function runMusicModeSweep(
  options: TunerOptions,
  parameter: string,
  values: number[],
  patterns: Array<{ id: string; expectedBpm: number | null }>
): Promise<MusicModeSweepResult[]> {
  const runner = new MusicModeRunner(options);
  await runner.connect();

  const results: MusicModeSweepResult[] = [];

  try {
    for (const value of values) {
      console.log(`  Testing ${parameter}=${value}...`);

      await runner.setParameter(parameter, value);

      const patternResults = await runner.runPatterns(patterns);

      results.push({
        paramValue: value,
        avgBpmError: patternResults.avgBpmError,
        avgPhaseStability: patternResults.avgPhaseStability,
        avgLockTime: patternResults.avgLockTime,
        avgActivePct: patternResults.avgActivePct,
        byPattern: patternResults.byPattern,
      });
    }
  } finally {
    await runner.disconnect();
  }

  return results;
}

/**
 * Find optimal parameter value for music mode metrics
 */
export function findOptimalMusicModeValue(
  results: MusicModeSweepResult[],
  optimizeFor: 'bpm_accuracy' | 'phase_stability' | 'lock_time' = 'bpm_accuracy'
): { value: number; score: number } {
  if (results.length === 0) {
    throw new Error('No results to analyze');
  }

  let bestValue = results[0].paramValue;
  let bestScore = Infinity;

  for (const result of results) {
    let score: number;

    switch (optimizeFor) {
      case 'bpm_accuracy':
        // Lower BPM error is better
        score = result.avgBpmError ?? Infinity;
        break;
      case 'phase_stability':
        // Lower phase stability (less jitter) is better
        score = result.avgPhaseStability;
        break;
      case 'lock_time':
        // Lower lock time is better
        score = result.avgLockTime ?? Infinity;
        break;
    }

    if (score < bestScore) {
      bestScore = score;
      bestValue = result.paramValue;
    }
  }

  return { value: bestValue, score: bestScore };
}

/**
 * Print music mode sweep results
 */
export function printMusicModeSweepResults(
  parameter: string,
  results: MusicModeSweepResult[]
): void {
  console.log('\n' + '─'.repeat(70));
  console.log(`Music Mode Sweep: ${parameter}`);
  console.log('─'.repeat(70));
  console.log('Value     | BPM Error | Phase Stab | Lock Time | Active %');
  console.log('─'.repeat(70));

  for (const result of results) {
    const bpmErr = result.avgBpmError !== null ? result.avgBpmError.toFixed(1).padStart(8) : '     N/A';
    const phaseSt = result.avgPhaseStability.toFixed(3).padStart(9);
    const lockT = result.avgLockTime !== null ? (result.avgLockTime + 'ms').padStart(9) : '      N/A';
    const activePct = (result.avgActivePct + '%').padStart(8);

    console.log(
      `${String(result.paramValue).padStart(8)} | ${bpmErr} | ${phaseSt} | ${lockT} | ${activePct}`
    );
  }

  console.log('─'.repeat(70));
}
