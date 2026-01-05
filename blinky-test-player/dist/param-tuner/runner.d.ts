/**
 * Test runner - executes patterns and measures detection performance
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * Legacy detection mode switching has been removed.
 */
import { EventEmitter } from 'events';
import type { TestResult, TunerOptions, DetectorType } from './types.js';
export declare class TestRunner extends EventEmitter {
    private options;
    private port;
    private parser;
    private portPath;
    private streaming;
    private pendingCommand;
    private testStartTime;
    private transientBuffer;
    private audioSampleBuffer;
    constructor(options: TunerOptions);
    connect(): Promise<void>;
    disconnect(): Promise<void>;
    private sendCommand;
    private startStream;
    private stopStream;
    private handleLine;
    /**
     * Set a single parameter using the new ensemble command format
     */
    setParameter(name: string, value: number): Promise<void>;
    /**
     * Set multiple parameters
     */
    setParameters(params: Record<string, number>): Promise<void>;
    /**
     * Set detector enabled state
     */
    setDetectorEnabled(detector: DetectorType, enabled: boolean): Promise<void>;
    /**
     * Set detector weight
     */
    setDetectorWeight(detector: DetectorType, weight: number): Promise<void>;
    /**
     * Set detector threshold
     */
    setDetectorThreshold(detector: DetectorType, threshold: number): Promise<void>;
    /**
     * Set agreement boost value
     */
    setAgreementBoost(level: number, boost: number): Promise<void>;
    /**
     * Reset parameters to defaults for ensemble
     */
    resetDefaults(): Promise<void>;
    /**
     * Save current settings to device flash memory
     */
    saveToFlash(): Promise<void>;
    /**
     * Get current parameter value from device
     */
    getParameter(name: string): Promise<number>;
    /**
     * Run a single test pattern and return results
     */
    runPattern(patternId: string): Promise<TestResult>;
    /**
     * Run multiple patterns and return aggregated results
     */
    runPatterns(patterns: string[]): Promise<{
        byPattern: Record<string, TestResult>;
        avgF1: number;
        avgPrecision: number;
        avgRecall: number;
    }>;
    /**
     * Save audio recording to file (debugging only)
     * Saves raw audio samples as JSON for offline analysis
     */
    private saveAudioRecording;
}
