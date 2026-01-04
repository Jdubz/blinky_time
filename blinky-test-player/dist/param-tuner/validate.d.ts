/**
 * Phase 4: Validation
 * Validates optimal parameters across ALL patterns
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * Legacy per-mode validation has been removed.
 */
import type { TunerOptions } from './types.js';
import { StateManager } from './state.js';
export declare function runValidation(options: TunerOptions, stateManager: StateManager): Promise<void>;
export declare function showValidationSummary(stateManager: StateManager): Promise<void>;
