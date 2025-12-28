/**
 * Phase 4: Validation
 * Validates optimal parameters across ALL patterns
 */
import type { TunerOptions } from './types.js';
import { StateManager } from './state.js';
export declare function runValidation(options: TunerOptions, stateManager: StateManager): Promise<void>;
export declare function showValidationSummary(stateManager: StateManager): Promise<void>;
