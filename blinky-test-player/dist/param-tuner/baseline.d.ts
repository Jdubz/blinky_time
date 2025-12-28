/**
 * Phase 1: Baseline Testing
 * Establishes baseline performance for each algorithm with default parameters
 */
import type { TunerOptions } from './types.js';
import { StateManager } from './state.js';
export declare function runBaseline(options: TunerOptions, stateManager: StateManager): Promise<void>;
export declare function showBaselineSummary(stateManager: StateManager): Promise<void>;
