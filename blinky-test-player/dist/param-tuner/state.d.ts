/**
 * State management for resumable tuning sessions
 */
import type { TuningState, BaselineResult, SweepResult, InteractionResult, ValidationResult, DetectionMode } from './types.js';
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
    isBaselineComplete(mode: DetectionMode): boolean;
    setBaselineInProgress(mode: DetectionMode): void;
    saveBaselineResult(mode: DetectionMode, result: BaselineResult): void;
    getBaselineResult(mode: DetectionMode): BaselineResult | undefined;
    markBaselinePhaseComplete(): void;
    isSweepComplete(param: string): boolean;
    setSweepInProgress(param: string, index: number): void;
    saveSweepResult(param: string, result: SweepResult): void;
    getSweepResult(param: string): SweepResult | undefined;
    getSweepResumeIndex(param: string): number;
    markSweepPhaseComplete(): void;
    isInteractionComplete(name: string): boolean;
    setInteractionInProgress(name: string, index: number): void;
    saveInteractionResult(name: string, result: InteractionResult): void;
    getInteractionResult(name: string): InteractionResult | undefined;
    getInteractionResumeIndex(name: string): number;
    markInteractionPhaseComplete(): void;
    isValidationComplete(mode: DetectionMode): boolean;
    setValidationInProgress(mode: DetectionMode): void;
    saveValidationResult(mode: DetectionMode, result: ValidationResult): void;
    getValidationResult(mode: DetectionMode): ValidationResult | undefined;
    markValidationPhaseComplete(): void;
    setOptimalParams(mode: DetectionMode, params: Record<string, number>): void;
    getOptimalParams(mode: DetectionMode): Record<string, number> | undefined;
    markDone(): void;
    reset(): void;
}
