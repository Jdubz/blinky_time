/**
 * State management for resumable tuning sessions
 */

import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'fs';
import { join } from 'path';
import type { TuningState, BaselineResult, SweepResult, InteractionResult, ValidationResult, DetectionMode } from './types.js';

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
