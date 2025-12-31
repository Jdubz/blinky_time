/**
 * State management for resumable tuning sessions
 *
 * EXTENSIBILITY: Supports per-pattern incremental saves for:
 * - Interruptible tests (Ctrl+C recovery)
 * - Progress tracking during long-running sweeps
 * - Auto-save at configurable intervals
 */

import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'fs';
import { join } from 'path';
import type { TuningState, BaselineResult, SweepResult, SweepPoint, InteractionResult, ValidationResult, DetectionMode, TestResult } from './types.js';

/**
 * Incremental sweep progress - tracks per-pattern results within a sweep
 */
export interface IncrementalSweepProgress {
  parameter: string;
  valueIndex: number;
  patternIndex: number;
  currentValue: number;
  partialResults: SweepPoint[];
  currentValueResults: TestResult[];  // Results for current value being tested
}

/**
 * Incremental baseline progress - tracks per-pattern results within a baseline test
 */
export interface IncrementalBaselineProgress {
  mode: DetectionMode;
  patternIndex: number;
  completedPatterns: string[];
  partialResults: Record<string, TestResult>;
}

export class StateManager {
  private outputDir: string;
  private statePath: string;
  private state: TuningState;

  constructor(outputDir: string) {
    this.outputDir = outputDir;
    this.statePath = join(outputDir, 'state.json');

    // Ensure output directory exists
    this.ensureDir(outputDir);
    this.ensureDir(join(outputDir, 'baseline'));
    this.ensureDir(join(outputDir, 'sweeps'));
    this.ensureDir(join(outputDir, 'incremental'));  // NEW: Per-pattern saves
    this.ensureDir(join(outputDir, 'interactions'));
    this.ensureDir(join(outputDir, 'validation'));
    this.ensureDir(join(outputDir, 'reports'));

    // Load or create state
    this.state = this.loadState();
  }

  private ensureDir(dir: string): void {
    if (!existsSync(dir)) {
      mkdirSync(dir, { recursive: true });
    }
  }

  private loadState(): TuningState {
    if (existsSync(this.statePath)) {
      try {
        return JSON.parse(readFileSync(this.statePath, 'utf-8'));
      } catch {
        // If corrupted, start fresh
      }
    }

    return {
      lastUpdated: new Date().toISOString(),
      currentPhase: 'baseline',
      phasesCompleted: [],
      baseline: {
        completed: [],
        results: {} as Record<DetectionMode, BaselineResult>,
      },
      sweeps: {
        completed: [],
        results: {},
      },
      interactions: {
        completed: [],
        results: {},
      },
      validation: {
        completed: [],
        results: {} as Record<DetectionMode, ValidationResult>,
      },
    };
  }

  save(): void {
    this.state.lastUpdated = new Date().toISOString();
    writeFileSync(this.statePath, JSON.stringify(this.state, null, 2));
  }

  getState(): TuningState {
    return this.state;
  }

  hasResumableState(): boolean {
    return this.state.phasesCompleted.length > 0 ||
      (this.state.baseline?.completed?.length ?? 0) > 0 ||
      (this.state.sweeps?.completed?.length ?? 0) > 0;
  }

  // Baseline methods
  isBaselineComplete(mode: DetectionMode): boolean {
    return this.state.baseline?.completed?.includes(mode) ?? false;
  }

  setBaselineInProgress(mode: DetectionMode): void {
    if (!this.state.baseline) {
      this.state.baseline = { completed: [], results: {} as Record<DetectionMode, BaselineResult> };
    }
    this.state.baseline.current = mode;
    this.state.currentPhase = 'baseline';
    this.save();
  }

  saveBaselineResult(mode: DetectionMode, result: BaselineResult): void {
    if (!this.state.baseline) {
      this.state.baseline = { completed: [], results: {} as Record<DetectionMode, BaselineResult> };
    }
    this.state.baseline.results[mode] = result;
    if (!this.state.baseline.completed.includes(mode)) {
      this.state.baseline.completed.push(mode);
    }
    delete this.state.baseline.current;

    // Save to file
    writeFileSync(
      join(this.outputDir, 'baseline', `${mode}.json`),
      JSON.stringify(result, null, 2)
    );
    this.save();
  }

  getBaselineResult(mode: DetectionMode): BaselineResult | undefined {
    return this.state.baseline?.results[mode];
  }

  markBaselinePhaseComplete(): void {
    if (!this.state.phasesCompleted.includes('baseline')) {
      this.state.phasesCompleted.push('baseline');
    }
    this.save();
  }

  // Sweep methods
  isSweepComplete(param: string): boolean {
    return this.state.sweeps?.completed?.includes(param) ?? false;
  }

  setSweepInProgress(param: string, index: number): void {
    if (!this.state.sweeps) {
      this.state.sweeps = { completed: [], results: {} };
    }
    this.state.sweeps.current = param;
    this.state.sweeps.currentIndex = index;
    this.state.currentPhase = 'sweep';
    this.save();
  }

  saveSweepResult(param: string, result: SweepResult): void {
    if (!this.state.sweeps) {
      this.state.sweeps = { completed: [], results: {} };
    }
    this.state.sweeps.results[param] = result;
    if (!this.state.sweeps.completed.includes(param)) {
      this.state.sweeps.completed.push(param);
    }
    delete this.state.sweeps.current;
    delete this.state.sweeps.currentIndex;

    // Save to file
    writeFileSync(
      join(this.outputDir, 'sweeps', `${param}.json`),
      JSON.stringify(result, null, 2)
    );
    this.save();
  }

  getSweepResult(param: string): SweepResult | undefined {
    return this.state.sweeps?.results[param];
  }

  getSweepResumeIndex(param: string): number {
    if (this.state.sweeps?.current === param && this.state.sweeps.currentIndex !== undefined) {
      return this.state.sweeps.currentIndex;
    }
    return 0;
  }

  markSweepPhaseComplete(): void {
    if (!this.state.phasesCompleted.includes('sweep')) {
      this.state.phasesCompleted.push('sweep');
    }
    this.save();
  }

  // =============================================================================
  // INCREMENTAL SAVE METHODS - Per-pattern saves for interruptible tests
  // =============================================================================

  /**
   * Get path for incremental sweep file
   */
  private getIncrementalPath(param: string): string {
    return join(this.outputDir, 'incremental', `${param}.json`);
  }

  /**
   * Start or resume a sweep with incremental saves
   * Returns the progress to resume from (or fresh start)
   */
  getIncrementalSweepProgress(param: string): IncrementalSweepProgress | null {
    const path = this.getIncrementalPath(param);
    if (existsSync(path)) {
      try {
        return JSON.parse(readFileSync(path, 'utf-8'));
      } catch {
        // Corrupted, start fresh
      }
    }
    return null;
  }

  /**
   * Save progress after each pattern test completes
   * Called after each individual pattern in a sweep
   */
  saveIncrementalProgress(progress: IncrementalSweepProgress): void {
    const path = this.getIncrementalPath(progress.parameter);
    writeFileSync(path, JSON.stringify(progress, null, 2));

    // Also update the main state with current position
    if (!this.state.sweeps) {
      this.state.sweeps = { completed: [], results: {} };
    }
    this.state.sweeps.current = progress.parameter;
    this.state.sweeps.currentIndex = progress.valueIndex;
    this.state.currentPhase = 'sweep';
    this.save();
  }

  /**
   * Clear incremental progress after sweep completes
   */
  clearIncrementalProgress(param: string): void {
    const path = this.getIncrementalPath(param);
    if (existsSync(path)) {
      const { unlinkSync } = require('fs');
      try {
        unlinkSync(path);
      } catch {
        // Ignore deletion errors
      }
    }
  }

  /**
   * Save a single pattern result to the incremental file
   * This allows recovery even if interrupted mid-pattern-set
   */
  appendPatternResult(param: string, valueIndex: number, value: number, patternResult: TestResult): void {
    let progress = this.getIncrementalSweepProgress(param);

    if (!progress || progress.valueIndex !== valueIndex) {
      // Starting a new value, reset current value results
      progress = {
        parameter: param,
        valueIndex,
        patternIndex: 0,
        currentValue: value,
        partialResults: progress?.partialResults || [],
        currentValueResults: [],
      };
    }

    // Add the pattern result
    progress.currentValueResults.push(patternResult);
    progress.patternIndex++;

    this.saveIncrementalProgress(progress);
  }

  /**
   * Finalize a sweep value (all patterns tested for this value)
   * Adds to partialResults and resets currentValueResults
   */
  finalizeSweepValue(param: string, sweepPoint: SweepPoint): void {
    let progress = this.getIncrementalSweepProgress(param);

    if (!progress) {
      progress = {
        parameter: param,
        valueIndex: 0,
        patternIndex: 0,
        currentValue: sweepPoint.value,
        partialResults: [],
        currentValueResults: [],
      };
    }

    progress.partialResults.push(sweepPoint);
    progress.valueIndex++;
    progress.patternIndex = 0;
    progress.currentValueResults = [];

    this.saveIncrementalProgress(progress);
  }

  /**
   * Check if a specific pattern in a sweep has already been completed
   * Used to skip already-tested patterns when resuming
   */
  isPatternCompletedInSweep(param: string, valueIndex: number, patternIndex: number): boolean {
    const progress = this.getIncrementalSweepProgress(param);
    if (!progress) return false;

    // If we're past this value, it's completed
    if (progress.valueIndex > valueIndex) return true;

    // If we're at this value, check pattern index
    if (progress.valueIndex === valueIndex) {
      return progress.patternIndex > patternIndex;
    }

    return false;
  }

  /**
   * Get partial results for a param (for resuming)
   */
  getPartialSweepResults(param: string): SweepPoint[] {
    const progress = this.getIncrementalSweepProgress(param);
    return progress?.partialResults || [];
  }

  // =============================================================================
  // INCREMENTAL BASELINE METHODS - Per-pattern saves for interruptible baseline tests
  // =============================================================================

  /**
   * Get path for incremental baseline file
   */
  private getIncrementalBaselinePath(mode: DetectionMode): string {
    return join(this.outputDir, 'incremental', `baseline-${mode}.json`);
  }

  /**
   * Get incremental baseline progress for a mode
   */
  getIncrementalBaselineProgress(mode: DetectionMode): IncrementalBaselineProgress | null {
    const path = this.getIncrementalBaselinePath(mode);
    if (existsSync(path)) {
      try {
        return JSON.parse(readFileSync(path, 'utf-8'));
      } catch {
        // Corrupted, start fresh
      }
    }
    return null;
  }

  /**
   * Save baseline progress after each pattern completes
   */
  saveIncrementalBaselineProgress(progress: IncrementalBaselineProgress): void {
    const path = this.getIncrementalBaselinePath(progress.mode);
    writeFileSync(path, JSON.stringify(progress, null, 2));

    // Update main state
    this.state.currentPhase = 'baseline';
    this.save();
  }

  /**
   * Append a pattern result to baseline progress
   */
  appendBaselinePatternResult(mode: DetectionMode, pattern: string, result: TestResult): void {
    let progress = this.getIncrementalBaselineProgress(mode);

    if (!progress) {
      progress = {
        mode,
        patternIndex: 0,
        completedPatterns: [],
        partialResults: {},
      };
    }

    progress.partialResults[pattern] = result;
    progress.completedPatterns.push(pattern);
    progress.patternIndex = progress.completedPatterns.length;

    this.saveIncrementalBaselineProgress(progress);
  }

  /**
   * Check if a pattern has been completed in baseline for a mode
   */
  isBaselinePatternCompleted(mode: DetectionMode, pattern: string): boolean {
    const progress = this.getIncrementalBaselineProgress(mode);
    return progress?.completedPatterns?.includes(pattern) ?? false;
  }

  /**
   * Get partial baseline results for resuming
   */
  getPartialBaselineResults(mode: DetectionMode): Record<string, TestResult> {
    const progress = this.getIncrementalBaselineProgress(mode);
    return progress?.partialResults || {};
  }

  /**
   * Clear incremental baseline progress after mode baseline completes
   */
  clearIncrementalBaselineProgress(mode: DetectionMode): void {
    const path = this.getIncrementalBaselinePath(mode);
    if (existsSync(path)) {
      const { unlinkSync } = require('fs');
      try {
        unlinkSync(path);
      } catch {
        // Ignore deletion errors
      }
    }
  }

  // Interaction methods
  isInteractionComplete(name: string): boolean {
    return this.state.interactions?.completed?.includes(name) ?? false;
  }

  setInteractionInProgress(name: string, index: number): void {
    if (!this.state.interactions) {
      this.state.interactions = { completed: [], results: {} };
    }
    this.state.interactions.current = name;
    this.state.interactions.currentIndex = index;
    this.state.currentPhase = 'interact';
    this.save();
  }

  saveInteractionResult(name: string, result: InteractionResult): void {
    if (!this.state.interactions) {
      this.state.interactions = { completed: [], results: {} };
    }
    this.state.interactions.results[name] = result;
    if (!this.state.interactions.completed.includes(name)) {
      this.state.interactions.completed.push(name);
    }
    delete this.state.interactions.current;
    delete this.state.interactions.currentIndex;

    // Save to file
    writeFileSync(
      join(this.outputDir, 'interactions', `${name}.json`),
      JSON.stringify(result, null, 2)
    );
    this.save();
  }

  getInteractionResult(name: string): InteractionResult | undefined {
    return this.state.interactions?.results[name];
  }

  getInteractionResumeIndex(name: string): number {
    if (this.state.interactions?.current === name && this.state.interactions.currentIndex !== undefined) {
      return this.state.interactions.currentIndex;
    }
    return 0;
  }

  markInteractionPhaseComplete(): void {
    if (!this.state.phasesCompleted.includes('interact')) {
      this.state.phasesCompleted.push('interact');
    }
    this.save();
  }

  // Validation methods
  isValidationComplete(mode: DetectionMode): boolean {
    return this.state.validation?.completed?.includes(mode) ?? false;
  }

  setValidationInProgress(mode: DetectionMode): void {
    if (!this.state.validation) {
      this.state.validation = { completed: [], results: {} as Record<DetectionMode, ValidationResult> };
    }
    this.state.validation.current = mode;
    this.state.currentPhase = 'validate';
    this.save();
  }

  saveValidationResult(mode: DetectionMode, result: ValidationResult): void {
    if (!this.state.validation) {
      this.state.validation = { completed: [], results: {} as Record<DetectionMode, ValidationResult> };
    }
    this.state.validation.results[mode] = result;
    if (!this.state.validation.completed.includes(mode)) {
      this.state.validation.completed.push(mode);
    }
    delete this.state.validation.current;

    // Save to file
    writeFileSync(
      join(this.outputDir, 'validation', `${mode}.json`),
      JSON.stringify(result, null, 2)
    );
    this.save();
  }

  getValidationResult(mode: DetectionMode): ValidationResult | undefined {
    return this.state.validation?.results[mode];
  }

  markValidationPhaseComplete(): void {
    if (!this.state.phasesCompleted.includes('validate')) {
      this.state.phasesCompleted.push('validate');
    }
    this.save();
  }

  // Optimal parameters
  setOptimalParams(mode: DetectionMode, params: Record<string, number>): void {
    if (!this.state.optimalParams) {
      this.state.optimalParams = {
        drummer: {},
        bass: {},
        hfc: {},
        spectral: {},
        hybrid: {},
      };
    }
    this.state.optimalParams[mode] = params;
    this.save();
  }

  getOptimalParams(mode: DetectionMode): Record<string, number> | undefined {
    return this.state.optimalParams?.[mode];
  }

  // Complete marking
  markDone(): void {
    this.state.currentPhase = 'done';
    if (!this.state.phasesCompleted.includes('report')) {
      this.state.phasesCompleted.push('report');
    }
    this.save();
  }

  // Reset
  reset(): void {
    this.state = {
      lastUpdated: new Date().toISOString(),
      currentPhase: 'baseline',
      phasesCompleted: [],
      baseline: {
        completed: [],
        results: {} as Record<DetectionMode, BaselineResult>,
      },
      sweeps: {
        completed: [],
        results: {},
      },
      interactions: {
        completed: [],
        results: {},
      },
      validation: {
        completed: [],
        results: {} as Record<DetectionMode, ValidationResult>,
      },
    };
    this.save();
  }
}
