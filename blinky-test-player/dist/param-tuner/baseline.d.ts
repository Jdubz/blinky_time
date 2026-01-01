/**
 * Phase 1: Baseline Testing
 * Establishes baseline performance for ensemble detection with default parameters
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * Legacy per-mode testing has been removed.
 */
import type { TunerOptions } from './types.js';
import { StateManager } from './state.js';
export declare function runBaseline(options: TunerOptions, stateManager: StateManager): Promise<void>;
export declare function showBaselineSummary(stateManager: StateManager): Promise<void>;
