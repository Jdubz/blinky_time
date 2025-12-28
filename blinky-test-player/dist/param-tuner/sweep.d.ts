/**
 * Phase 2: Parameter Sweeps
 * Sweeps each parameter independently to find optimal values
 */
import type { TunerOptions } from './types.js';
import { StateManager } from './state.js';
export declare function runSweeps(options: TunerOptions, stateManager: StateManager): Promise<void>;
export declare function showSweepSummary(stateManager: StateManager): Promise<void>;
