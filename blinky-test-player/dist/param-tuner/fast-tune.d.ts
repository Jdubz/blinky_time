/**
 * Fast Parameter Tuning
 * Uses binary search and targeted validation instead of exhaustive sweeps
 * Completes in ~30 min instead of 4-6 hours
 */
import type { TunerOptions } from './types.js';
export declare function runFastTune(options: TunerOptions): Promise<void>;
