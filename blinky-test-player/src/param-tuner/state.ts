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

import { existsSync, mkdirSync, readFileSync, writeFileSync, unlinkSync } from 'fs';
import { join } from 'path';
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
  currentValueResults: TestResult[];  // Results for current value being tested
}

/**
 * Incremental baseline progress - tracks per-pattern results within a baseline test
 */
export interface IncrementalBaselineProgress {
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
    this.ensureDir(join(outputDir, 'incremental'));  // Per-pattern saves
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
      sweeps: {
        completed: [],
        results: {},
      },
      interactions: {
        completed: [],
        results: {},
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
      this.state.baseline !== undefined ||
      (this.state.sweeps?.completed?.length ?? 0) > 0;
  }

  // =============================================================================
  // BASELINE METHODS (Ensemble - single baseline, not per-mode)
  // =============================================================================

  isBaselineComplete(): boolean {
    return this.state.phasesCompleted.includes('baseline');
  }

  setBaselineInProgress(): void {
    this.state.currentPhase = 'baseline';
    this.save();
  }

  saveBaselineResult(result: BaselineResult): void {
    this.state.baseline = result;

    // Save to file
    writeFileSync(
      join(this.outputDir, 'baseline', 'ensemble.json'),
      JSON.stringify(result, null, 2)
    );
    this.save();
  }

  getBaselineResult(): BaselineResult | undefined {
    return this.state.baseline;
  }

  markBaselinePhaseComplete(): void {
    if (!this.state.phasesCompleted.includes('baseline')) {
      this.state.phasesCompleted.push('baseline');
    }
    this.save();
  }

  // =============================================================================
  // SWEEP METHODS
  // =============================================================================

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
  // INCREMENTAL SWEEP METHODS - Per-pattern saves for interruptible tests
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
  private getIncrementalBaselinePath(): string {
    return join(this.outputDir, 'incremental', 'baseline-ensemble.json');
  }

  /**
   * Get incremental baseline progress
   */
  getIncrementalBaselineProgress(): IncrementalBaselineProgress | null {
    const path = this.getIncrementalBaselinePath();
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
    const path = this.getIncrementalBaselinePath();
    writeFileSync(path, JSON.stringify(progress, null, 2));

    // Update main state
    this.state.currentPhase = 'baseline';
    this.save();
  }

  /**
   * Append a pattern result to baseline progress
   */
  appendBaselinePatternResult(pattern: string, result: TestResult): void {
    let progress = this.getIncrementalBaselineProgress();

    if (!progress) {
      progress = {
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
   * Check if a pattern has been completed in baseline
   */
  isBaselinePatternCompleted(pattern: string): boolean {
    const progress = this.getIncrementalBaselineProgress();
    return progress?.completedPatterns?.includes(pattern) ?? false;
  }

  /**
   * Get partial baseline results for resuming
   */
  getPartialBaselineResults(): Record<string, TestResult> {
    const progress = this.getIncrementalBaselineProgress();
    return progress?.partialResults || {};
  }

  /**
   * Clear incremental baseline progress after baseline completes
   */
  clearIncrementalBaselineProgress(): void {
    const path = this.getIncrementalBaselinePath();
    if (existsSync(path)) {
      try {
        unlinkSync(path);
      } catch {
        // Ignore deletion errors
      }
    }
  }

  // =============================================================================
  // INTERACTION METHODS
  // =============================================================================

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

  /**
   * Save an incremental interaction point result
   * Used to resume interrupted interaction tests
   */
  saveInteractionPoint(name: string, point: any): void {
    const partialPath = join(this.outputDir, 'interactions', `${name}.partial.json`);
    let partialResults: any[] = [];

    try {
      if (existsSync(partialPath)) {
        partialResults = JSON.parse(readFileSync(partialPath, 'utf-8'));
      }
    } catch {
      partialResults = [];
    }

    partialResults.push(point);
    writeFileSync(partialPath, JSON.stringify(partialResults, null, 2));
  }

  /**
   * Load partial interaction results for resumption
   */
  getPartialInteractionResults(name: string): any[] {
    const partialPath = join(this.outputDir, 'interactions', `${name}.partial.json`);
    try {
      if (existsSync(partialPath)) {
        return JSON.parse(readFileSync(partialPath, 'utf-8'));
      }
    } catch {
      // Ignore errors
    }
    return [];
  }

  /**
   * Clear partial interaction results after completion
   */
  clearPartialInteractionResults(name: string): void {
    const partialPath = join(this.outputDir, 'interactions', `${name}.partial.json`);
    try {
      if (existsSync(partialPath)) {
        unlinkSync(partialPath);
      }
    } catch {
      // Ignore errors
    }
  }

  markInteractionPhaseComplete(): void {
    if (!this.state.phasesCompleted.includes('interact')) {
      this.state.phasesCompleted.push('interact');
    }
    this.save();
  }

  // =============================================================================
  // VALIDATION METHODS (Ensemble - single validation, not per-mode)
  // =============================================================================

  isValidationComplete(): boolean {
    return this.state.phasesCompleted.includes('validate');
  }

  setValidationInProgress(): void {
    this.state.currentPhase = 'validate';
    this.save();
  }

  saveValidationResult(result: ValidationResult): void {
    this.state.validation = result;

    // Save to file
    writeFileSync(
      join(this.outputDir, 'validation', 'ensemble.json'),
      JSON.stringify(result, null, 2)
    );
    this.save();
  }

  getValidationResult(): ValidationResult | undefined {
    return this.state.validation;
  }

  markValidationPhaseComplete(): void {
    if (!this.state.phasesCompleted.includes('validate')) {
      this.state.phasesCompleted.push('validate');
    }
    this.save();
  }

  // =============================================================================
  // OPTIMAL PARAMETERS
  // =============================================================================

  setOptimalParams(params: Record<string, number>): void {
    this.state.optimalParams = params;
    this.save();
  }

  setOptimalParam(param: string, value: number): void {
    if (!this.state.optimalParams) {
      this.state.optimalParams = {};
    }
    this.state.optimalParams[param] = value;
    this.save();
  }

  getOptimalParams(): Record<string, number> {
    return this.state.optimalParams || {};
  }

  // =============================================================================
  // COMPLETE / RESET
  // =============================================================================

  markDone(): void {
    this.state.currentPhase = 'done';
    if (!this.state.phasesCompleted.includes('report')) {
      this.state.phasesCompleted.push('report');
    }
    this.save();
  }

  reset(): void {
    this.state = {
      lastUpdated: new Date().toISOString(),
      currentPhase: 'baseline',
      phasesCompleted: [],
      sweeps: {
        completed: [],
        results: {},
      },
      interactions: {
        completed: [],
        results: {},
      },
    };
    this.save();
  }
}
