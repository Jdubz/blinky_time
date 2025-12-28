/**
 * Test runner - executes patterns and measures detection performance
 */
import { EventEmitter } from 'events';
import type { TestResult, DetectionMode, TunerOptions } from './types.js';
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
     * Set detection mode
     */
    setMode(mode: DetectionMode): Promise<void>;
    /**
     * Set a single parameter
     */
    setParameter(name: string, value: number): Promise<void>;
    /**
     * Set multiple parameters
     */
    setParameters(params: Record<string, number>): Promise<void>;
    /**
     * Reset parameters to defaults for a mode
     */
    resetDefaults(mode: DetectionMode): Promise<void>;
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
}
