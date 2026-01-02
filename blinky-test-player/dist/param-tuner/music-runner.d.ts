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
import { EventEmitter } from 'events';
import type { TunerOptions } from './types.js';
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
    phase: {
        /** Phase standard deviation (lower = more stable) */
        stability: number;
        /** Number of phase resets detected */
        resets: number;
    };
    lock: {
        /** Time in ms to first stable BPM lock (null if never locked) */
        timeToLockMs: number | null;
        /** Percentage of time in locked state */
        lockedPct: number;
        /** Average confidence when locked */
        avgConfidence: number;
    };
    activation: {
        /** Time in ms until music mode activated */
        timeToActivateMs: number | null;
        /** Percentage of time with music mode active */
        activePct: number;
        /** Number of activations/deactivations */
        toggleCount: number;
    };
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
export declare class MusicModeRunner extends EventEmitter {
    private options;
    private port;
    private parser;
    private portPath;
    private streaming;
    private pendingCommand;
    private testStartTime;
    private bpmSampleBuffer;
    private lastMusicActive;
    private toggleCount;
    constructor(options: TunerOptions);
    connect(): Promise<void>;
    disconnect(): Promise<void>;
    private sendCommand;
    private startStream;
    private stopStream;
    private handleLine;
    /**
     * Set a single parameter
     */
    setParameter(name: string, value: number): Promise<void>;
    /**
     * Set multiple parameters
     */
    setParameters(params: Record<string, number>): Promise<void>;
    /**
     * Run a music mode test pattern and measure BPM/phase metrics
     */
    runPattern(patternId: string, expectedBpm: number | null): Promise<MusicModeResult>;
    /**
     * Analyze collected BPM samples and compute metrics
     */
    private analyzeResults;
    /**
     * Calculate median of an array
     */
    private median;
    /**
     * Run multiple patterns and aggregate results
     */
    runPatterns(patterns: Array<{
        id: string;
        expectedBpm: number | null;
    }>): Promise<{
        byPattern: Record<string, MusicModeResult>;
        avgBpmError: number | null;
        avgPhaseStability: number;
        avgLockTime: number | null;
        avgActivePct: number;
    }>;
}
/**
 * Run a parameter sweep specifically for music mode metrics
 */
export declare function runMusicModeSweep(options: TunerOptions, parameter: string, values: number[], patterns: Array<{
    id: string;
    expectedBpm: number | null;
}>): Promise<MusicModeSweepResult[]>;
/**
 * Find optimal parameter value for music mode metrics
 */
export declare function findOptimalMusicModeValue(results: MusicModeSweepResult[], optimizeFor?: 'bpm_accuracy' | 'phase_stability' | 'lock_time'): {
    value: number;
    score: number;
};
/**
 * Print music mode sweep results
 */
export declare function printMusicModeSweepResults(parameter: string, results: MusicModeSweepResult[]): void;
