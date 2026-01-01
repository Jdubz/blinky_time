/**
 * Fast Parameter Tuning for Ensemble Detector
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * This tuner optimizes detector thresholds, weights, and agreement boosts.
 *
 * Tuning Strategy:
 * Phase 1: Find optimal threshold for each detector
 * Phase 2: Optimize detector weights
 * Phase 3: Tune agreement boost values
 * Phase 4: Final validation
 */
import type { TunerOptions } from './types.js';
export declare function runFastTune(options: TunerOptions): Promise<void>;
