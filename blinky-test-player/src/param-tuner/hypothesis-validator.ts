/**
 * Hypothesis Validation Runner - Tests multi-hypothesis tempo tracking
 *
 * Validates:
 * - Hypothesis creation from autocorrelation peaks
 * - Promotion logic (confidence-based, min beats requirement)
 * - Tempo change tracking (gradual and abrupt)
 * - Half-time/double-time ambiguity resolution
 * - Decay behavior during silence and phrases
 */

import { SerialPort } from 'serialport';
import { ReadlineParser } from '@serialport/parser-readline';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const TEST_PLAYER_PATH = join(__dirname, '..', '..', 'dist', 'index.js');
const BAUD_RATE = 115200;
const COMMAND_TIMEOUT_MS = 2000;

// =============================================================================
// HYPOTHESIS VALIDATION TYPES
// =============================================================================

/**
 * Single hypothesis state snapshot
 */
export interface HypothesisSnapshot {
  slot: number;
  active: boolean;
  bpm: number;
  phase: number;
  strength: number;
  confidence: number;
  beatCount: number;
  avgPhaseError: number;
  priority: number;
}

/**
 * Complete hypothesis tracker state at a point in time
 */
export interface HypothesisState {
  timestampMs: number;
  hypotheses: HypothesisSnapshot[];
  primaryIndex: number;
}

/**
 * Hypothesis validation test result
 */
export interface HypothesisValidationResult {
  pattern: string;
  durationMs: number;
  expectedBpm: number | null;

  // Hypothesis tracking
  hypotheses: {
    /** Maximum number of concurrent hypotheses */
    maxConcurrent: number;
    /** Total hypotheses created during test */
    totalCreated: number;
    /** Number of promotions to primary */
    promotions: number;
    /** Time to first hypothesis creation (ms) */
    timeToFirstMs: number | null;
  };

  // Primary hypothesis tracking
  primary: {
    /** Average BPM of primary hypothesis */
    avgBpm: number;
    /** BPM error vs expected (null if no expected) */
    bpmError: number | null;
    /** Average confidence of primary */
    avgConfidence: number;
    /** Confidence growth rate (conf/second) */
    confidenceGrowth: number;
    /** Average phase error */
    avgPhaseError: number;
  };

  // Tempo change tracking (for tempo-ramp, tempo-sudden patterns)
  tempoChanges?: {
    /** Number of tempo changes detected */
    changesDetected: number;
    /** Average lag to detect change (ms) */
    avgLagMs: number;
    /** Successful transitions */
    successfulTransitions: number;
  };

  // Ambiguity resolution (for half-time-ambiguity pattern)
  ambiguity?: {
    /** Both 60 and 120 BPM hypotheses created */
    bothCreated: boolean;
    /** Correct BPM won (120 BPM) */
    correctWon: boolean;
    /** Time to resolve ambiguity (ms) */
    resolutionTimeMs: number | null;
  };

  // Silence decay (for silence-gaps pattern)
  silenceDecay?: {
    /** Hypotheses survived silence gaps */
    survived: boolean;
    /** Confidence decay rate during silence */
    decayRate: number;
    /** Grace period observed */
    gracePeriodMs: number | null;
  };

  // Raw state snapshots
  states: HypothesisState[];
}

// =============================================================================
// HYPOTHESIS VALIDATOR
// =============================================================================

export class HypothesisValidator {
  private port: SerialPort | null = null;
  private parser: ReadlineParser | null = null;
  private responseBuffer: string[] = [];

  /**
   * Connect to device
   */
  async connect(portPath: string): Promise<void> {
    this.port = new SerialPort({
      path: portPath,
      baudRate: BAUD_RATE,
    });

    this.parser = this.port.pipe(new ReadlineParser({ delimiter: '\n' }));

    // Buffer responses
    this.parser.on('data', (line: string) => {
      this.responseBuffer.push(line.trim());
    });

    // Wait for connection
    await new Promise((resolve) => setTimeout(resolve, 2000));
  }

  /**
   * Disconnect from device
   */
  async disconnect(): Promise<void> {
    if (this.port) {
      await new Promise<void>((resolve, reject) => {
        this.port!.close((err) => {
          if (err) reject(err);
          else resolve();
        });
      });
      this.port = null;
      this.parser = null;
    }
  }

  /**
   * Send command and get response
   */
  private async sendCommand(cmd: string): Promise<string> {
    if (!this.port) {
      throw new Error('Not connected to device');
    }

    this.responseBuffer = [];
    this.port.write(cmd + '\n');

    // Wait for response
    await new Promise((resolve) => setTimeout(resolve, 200));

    return this.responseBuffer.join('\n');
  }

  /**
   * Get current hypothesis state
   */
  async getHypothesisState(): Promise<HypothesisState> {
    const response = await this.sendCommand('json hypotheses');

    // Find JSON in response
    const jsonMatch = response.match(/\{[\s\S]*\}/);
    if (!jsonMatch) {
      throw new Error(`Failed to parse hypothesis state: ${response}`);
    }

    const data = JSON.parse(jsonMatch[0]);

    return {
      timestampMs: Date.now(),
      hypotheses: data.hypotheses || [],
      primaryIndex: data.primaryIndex || 0,
    };
  }

  /**
   * Run hypothesis validation test
   */
  async runTest(pattern: string, expectedBpm: number | null = null, gain?: number): Promise<HypothesisValidationResult> {
    const states: HypothesisState[] = [];
    const startTime = Date.now();

    // Start audio streaming to enable hypothesis tracking
    await this.sendCommand('stream start');
    await new Promise((resolve) => setTimeout(resolve, 500));

    // Lock hardware gain if specified
    if (gain !== undefined) {
      await this.sendCommand(`set hwgain ${gain}`);
    }

    // Start pattern playback
    const player = spawn('node', [TEST_PLAYER_PATH, 'play', pattern], {
      stdio: 'ignore',
    });

    // Poll hypothesis state every 500ms
    const pollInterval = setInterval(async () => {
      try {
        const state = await this.getHypothesisState();
        states.push(state);
      } catch (err) {
        console.error('Failed to poll hypothesis state:', err);
      }
    }, 500);

    // Wait for pattern to complete
    await new Promise<void>((resolve) => {
      player.on('exit', () => resolve());
    });

    clearInterval(pollInterval);

    // Get final state
    const finalState = await this.getHypothesisState();
    states.push(finalState);

    // Unlock hardware gain
    if (gain !== undefined) {
      await this.sendCommand('set hwgain 255');
    }

    // Stop streaming
    await this.sendCommand('stream stop');

    const durationMs = Date.now() - startTime;

    // Analyze results
    return this.analyzeResults(pattern, states, durationMs, expectedBpm);
  }

  /**
   * Analyze hypothesis validation results
   */
  private analyzeResults(
    pattern: string,
    states: HypothesisState[],
    durationMs: number,
    expectedBpm: number | null
  ): HypothesisValidationResult {
    // Track hypothesis creation/promotion
    const seenHypotheses = new Set<number>();
    let maxConcurrent = 0;
    let promotions = 0;
    let firstHypothesisTime: number | null = null;

    // Track primary hypothesis metrics
    const primaryBpms: number[] = [];
    const primaryConfidences: number[] = [];
    const primaryPhaseErrors: number[] = [];

    for (let i = 0; i < states.length; i++) {
      const state = states[i];
      const activeHypos = state.hypotheses.filter(h => h.active);

      maxConcurrent = Math.max(maxConcurrent, activeHypos.length);

      // Track first hypothesis
      if (firstHypothesisTime === null && activeHypos.length > 0) {
        firstHypothesisTime = state.timestampMs;
      }

      // Track hypothesis creation
      for (const h of activeHypos) {
        const key = h.slot * 1000 + Math.round(h.bpm);
        if (!seenHypotheses.has(key)) {
          seenHypotheses.add(key);
        }
      }

      // Track promotions (when a non-slot-0 hypothesis moves to slot 0)
      if (i > 0 && activeHypos.length > 0) {
        const prevPrimary = states[i - 1].hypotheses[0];
        const currPrimary = state.hypotheses[0];
        if (prevPrimary.active && currPrimary.active &&
            Math.abs(prevPrimary.bpm - currPrimary.bpm) > 5) {
          promotions++;
        }
      }

      // Track primary metrics
      const primary = state.hypotheses[state.primaryIndex];
      if (primary && primary.active) {
        primaryBpms.push(primary.bpm);
        primaryConfidences.push(primary.confidence);
        primaryPhaseErrors.push(primary.avgPhaseError);
      }
    }

    // Calculate averages
    const avgBpm = primaryBpms.length > 0
      ? primaryBpms.reduce((a, b) => a + b, 0) / primaryBpms.length
      : 0;
    const bpmError = expectedBpm !== null && avgBpm > 0
      ? Math.abs(avgBpm - expectedBpm)
      : null;
    const avgConfidence = primaryConfidences.length > 0
      ? primaryConfidences.reduce((a, b) => a + b, 0) / primaryConfidences.length
      : 0;
    const avgPhaseError = primaryPhaseErrors.length > 0
      ? primaryPhaseErrors.reduce((a, b) => a + b, 0) / primaryPhaseErrors.length
      : 0;

    // Calculate confidence growth rate
    const confidenceGrowth = primaryConfidences.length >= 2
      ? (primaryConfidences[primaryConfidences.length - 1] - primaryConfidences[0]) / (durationMs / 1000)
      : 0;

    return {
      pattern,
      durationMs,
      expectedBpm,
      hypotheses: {
        maxConcurrent,
        totalCreated: seenHypotheses.size,
        promotions,
        timeToFirstMs: firstHypothesisTime,
      },
      primary: {
        avgBpm,
        bpmError,
        avgConfidence,
        confidenceGrowth,
        avgPhaseError,
      },
      states,
    };
  }
}

/**
 * Run hypothesis validation for a single pattern
 */
export async function validateHypothesis(
  portPath: string,
  pattern: string,
  expectedBpm: number | null = null,
  gain?: number
): Promise<HypothesisValidationResult> {
  const validator = new HypothesisValidator();

  try {
    await validator.connect(portPath);
    return await validator.runTest(pattern, expectedBpm, gain);
  } finally {
    await validator.disconnect();
  }
}

/**
 * Run hypothesis validation suite
 */
export async function runHypothesisValidationSuite(
  portPath: string,
  gain?: number
): Promise<HypothesisValidationResult[]> {
  const patterns = [
    { id: 'tempo-ramp', expectedBpm: null }, // Variable BPM
    { id: 'tempo-sudden', expectedBpm: null }, // Multiple BPMs
    { id: 'half-time-ambiguity', expectedBpm: 120 },
    { id: 'silence-gaps', expectedBpm: 120 },
    { id: 'steady-120bpm', expectedBpm: 120 },
  ];

  const results: HypothesisValidationResult[] = [];

  for (const { id, expectedBpm } of patterns) {
    console.log(`\nValidating hypothesis tracking: ${id}...`);
    const result = await validateHypothesis(portPath, id, expectedBpm, gain);
    results.push(result);

    // Print summary
    console.log(`  Hypotheses: ${result.hypotheses.totalCreated} created, ${result.hypotheses.maxConcurrent} concurrent`);
    console.log(`  Primary BPM: ${result.primary.avgBpm.toFixed(1)} (error: ${result.primary.bpmError?.toFixed(1) || 'N/A'})`);
    console.log(`  Confidence: ${result.primary.avgConfidence.toFixed(3)} (growth: ${result.primary.confidenceGrowth.toFixed(3)}/s)`);
    console.log(`  Promotions: ${result.hypotheses.promotions}`);
  }

  return results;
}
