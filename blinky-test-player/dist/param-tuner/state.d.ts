/**
 * State management for resumable tuning sessions
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * Legacy per-mode state tracking has been removed.
 *
 * EXTENSIBILITY: Supports per-pattern incremental saves for:
 * - Interruptible tests (Ctrl+C recovery)
 * - Progress tracking during long-running sweeps
 * - Auto-save at configurable intervals
 */
import type { TuningState, BaselineResult, SweepResult, SweepPoint, InteractionResult, ValidationResult, TestResult } from './types.js';
/**
 * Incremental sweep progress - tracks per-pattern results within a sweep
 */
export interface IncrementalSweepProgress {
    parameter: string;
    valueIndex: number;
    patternIndex: number;
    currentValue: number;
    partialResults: SweepPoint[];
    currentValueResults: TestResult[];
}
/**
 * Incremental baseline progress - tracks per-pattern results within a baseline test
 */
export interface IncrementalBaselineProgress {
    patternIndex: number;
    completedPatterns: string[];
    partialResults: Record<string, TestResult>;
}
export declare class StateManager {
    private outputDir;
    private statePath;
    private state;
    constructor(outputDir: string);
    private ensureDir;
    private loadState;
    save(): void;
    getState(): TuningState;
    hasResumableState(): boolean;
    isBaselineComplete(): boolean;
    setBaselineInProgress(): void;
    saveBaselineResult(result: BaselineResult): void;
    getBaselineResult(): BaselineResult | undefined;
    markBaselinePhaseComplete(): void;
    isSweepComplete(param: string): boolean;
    setSweepInProgress(param: string, index: number): void;
    saveSweepResult(param: string, result: SweepResult): void;
    getSweepResult(param: string): SweepResult | undefined;
    getSweepResumeIndex(param: string): number;
    markSweepPhaseComplete(): void;
    /**
     * Get path for incremental sweep file
     */
    private getIncrementalPath;
    /**
     * Start or resume a sweep with incremental saves
     * Returns the progress to resume from (or fresh start)
     */
    getIncrementalSweepProgress(param: string): IncrementalSweepProgress | null;
    /**
     * Save progress after each pattern test completes
     * Called after each individual pattern in a sweep
     */
    saveIncrementalProgress(progress: IncrementalSweepProgress): void;
    /**
     * Clear incremental progress after sweep completes
     */
    clearIncrementalProgress(param: string): void;
    /**
     * Save a single pattern result to the incremental file
     * This allows recovery even if interrupted mid-pattern-set
     */
    appendPatternResult(param: string, valueIndex: number, value: number, patternResult: TestResult): void;
    /**
     * Finalize a sweep value (all patterns tested for this value)
     * Adds to partialResults and resets currentValueResults
     */
    finalizeSweepValue(param: string, sweepPoint: SweepPoint): void;
    /**
     * Check if a specific pattern in a sweep has already been completed
     * Used to skip already-tested patterns when resuming
     */
    isPatternCompletedInSweep(param: string, valueIndex: number, patternIndex: number): boolean;
    /**
     * Get partial results for a param (for resuming)
     */
    getPartialSweepResults(param: string): SweepPoint[];
    /**
     * Get path for incremental baseline file
     */
    private getIncrementalBaselinePath;
    /**
     * Get incremental baseline progress
     */
    getIncrementalBaselineProgress(): IncrementalBaselineProgress | null;
    /**
     * Save baseline progress after each pattern completes
     */
    saveIncrementalBaselineProgress(progress: IncrementalBaselineProgress): void;
    /**
     * Append a pattern result to baseline progress
     */
    appendBaselinePatternResult(pattern: string, result: TestResult): void;
    /**
     * Check if a pattern has been completed in baseline
     */
    isBaselinePatternCompleted(pattern: string): boolean;
    /**
     * Get partial baseline results for resuming
     */
    getPartialBaselineResults(): Record<string, TestResult>;
    /**
     * Clear incremental baseline progress after baseline completes
     */
    clearIncrementalBaselineProgress(): void;
    isInteractionComplete(name: string): boolean;
    setInteractionInProgress(name: string, index: number): void;
    saveInteractionResult(name: string, result: InteractionResult): void;
    getInteractionResult(name: string): InteractionResult | undefined;
    getInteractionResumeIndex(name: string): number;
    /**
     * Save an incremental interaction point result
     * Used to resume interrupted interaction tests
     */
    saveInteractionPoint(name: string, point: any): void;
    /**
     * Load partial interaction results for resumption
     */
    getPartialInteractionResults(name: string): any[];
    /**
     * Clear partial interaction results after completion
     */
    clearPartialInteractionResults(name: string): void;
    markInteractionPhaseComplete(): void;
    isValidationComplete(): boolean;
    setValidationInProgress(): void;
    saveValidationResult(result: ValidationResult): void;
    getValidationResult(): ValidationResult | undefined;
    markValidationPhaseComplete(): void;
    setOptimalParams(params: Record<string, number>): void;
    setOptimalParam(param: string, value: number): void;
    getOptimalParams(): Record<string, number>;
    markDone(): void;
    reset(): void;
}
