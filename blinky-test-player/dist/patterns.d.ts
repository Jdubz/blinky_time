/**
 * Pre-defined transient test patterns
 *
 * Each pattern defines a sequence of transient hits with exact timing.
 * Ground truth is automatically derived from the pattern definition.
 *
 * Two-band system:
 * - 'low': Bass transients (50-200 Hz)
 * - 'high': Brightness transients (2-8 kHz)
 */
import type { TestPattern } from './types.js';
/**
 * Simple alternating pattern (120 BPM, 8 bars)
 * Low transients on 1 and 3, high transients on 2 and 4
 */
export declare const SIMPLE_BEAT: TestPattern;
/**
 * Low band barrage - rapid bass transients at varying intervals
 * Tests low-band detection accuracy and timing precision
 */
export declare const LOW_BARRAGE: TestPattern;
/**
 * High band burst - rapid high-frequency transients
 * Tests high-band detection with rapid consecutive hits
 */
export declare const HIGH_BURST: TestPattern;
/**
 * Mixed pattern - interleaved low and high with varying dynamics
 * Tests classification when both bands are active
 */
export declare const MIXED_PATTERN: TestPattern;
/**
 * Timing precision test - hits at precise intervals
 * Tests timing accuracy with various intervals: 100ms, 150ms, 200ms, 250ms
 */
export declare const TIMING_TEST: TestPattern;
/**
 * Simultaneous hits - low and high at exact same time
 * Tests detection when both bands trigger simultaneously
 */
export declare const SIMULTANEOUS_TEST: TestPattern;
/**
 * Realistic electronic track simulation
 * Background: sub-bass drone + mid pad + noise floor
 * This tests detection in the presence of continuous audio, like real music
 */
export declare const REALISTIC_TRACK: TestPattern;
/**
 * Heavy background test
 * High background levels to test detection sensitivity in loud environments
 */
export declare const HEAVY_BACKGROUND: TestPattern;
/**
 * Dynamic background test
 * No transients, just background - for observing baseline adaptation
 */
export declare const BASELINE_ONLY: TestPattern;
/**
 * Quiet section test
 * Simulates a breakdown/quiet section in a track
 */
export declare const QUIET_SECTION: TestPattern;
/**
 * All available test patterns
 */
export declare const TEST_PATTERNS: TestPattern[];
/**
 * Get pattern by ID
 */
export declare function getPatternById(id: string): TestPattern | undefined;
