/**
 * Phase 3: Parameter Interactions
 * Tests interactions between parameters, especially for Hybrid mode
 */
import type { TunerOptions } from './types.js';
import { StateManager } from './state.js';
export declare function runInteractions(options: TunerOptions, stateManager: StateManager): Promise<void>;
export declare function showInteractionSummary(stateManager: StateManager): Promise<void>;
