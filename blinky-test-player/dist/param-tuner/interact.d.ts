/**
 * Phase 3: Parameter Interactions
 * Tests interactions between ensemble parameters
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * Tests weight and threshold interactions for the ensemble detector.
 * Legacy hybrid mode testing has been removed.
 */
import type { TunerOptions } from './types.js';
import { StateManager } from './state.js';
export declare function runInteractions(options: TunerOptions, stateManager: StateManager): Promise<void>;
export declare function showInteractionSummary(stateManager: StateManager): Promise<void>;
